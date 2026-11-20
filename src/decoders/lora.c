/*
 * lora.c — Decodificador LoRa (capa física CSS) y análisis LoRaWAN sin descifrar.
 *
 * La capa física está en lora_phy.{h,c} (receptor de Tapparel et al., arXiv:
 * 2002.08208, con la sincronía fina y la corrección de la deriva de reloj de
 * arXiv:2502.08485). Aquí se configura (opts), se entrega cada trama con CRC
 * correcto como evento JSON y se interpreta la cabecera de LoRaWAN:
 *
 *   LoRaWAN 1.0.x/1.1, capa MAC (LoRa Alliance, "LoRaWAN L2 1.0.4", cap. 4):
 *     PHYPayload = MHDR(1) | MACPayload | MIC(4)
 *     MHDR  = MType(3 bits, 7..5) | RFU(3) | Major(2 bits, 1..0 = 00)
 *     MType 0 JoinRequest, 1 JoinAccept, 2/3 UnconfirmedData Up/Down,
 *           4/5 ConfirmedData Up/Down, 6 RejoinRequest, 7 Proprietary
 *     MACPayload (datos) = DevAddr(4, LE) | FCtrl(1) | FCnt(2, LE) |
 *                          FOpts(0..15) | [FPort(1) | FRMPayload]
 *     FCtrl uplink:   ADR(7) ADRACKReq(6) ACK(5) ClassB(4) FOptsLen(3..0)
 *     FCtrl downlink: ADR(7) RFU(6) ACK(5) FPending(4) FOptsLen(3..0)
 *     JoinRequest = AppEUI(8, LE) | DevEUI(8, LE) | DevNonce(2, LE) | MIC(4)
 *   La carga (FRMPayload) va cifrada con AppSKey/NwkSKey y el MIC es un
 *   AES-CMAC: NO se descifra ni se verifica (AGENTS.md §0.12).
 *
 * opts (JSON): {"bw":125000|250000, "sf":7..12 | [7,9] | "all", "sync":52 |
 *   "0x34" | [..] | "any", "ldro":true|false}. Por defecto BW 125 kHz, todos
 * los SF y sync ∈ {0x34 LoRaWAN público, 0x12 privado, 0x2B Meshtastic}.
 *
 * state_json: contadores y tabla de dispositivos LoRaWAN (DevAddr), con
 * caducidad de 15 min.
 */
#include "decoder.h"
#include "lora_phy.h"

#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEV 64
#define DEV_TTL 900.0

typedef struct {
    uint32_t devaddr;
    uint64_t deveui;
    double last;
    unsigned count;
    int fcnt, fport, sf;
    double rssi, snr;
    bool used;
} lw_dev_t;

typedef struct {
    lora_rx_t *rx;
    int bw_hz;
    dec_emit_fn emit;
    void *ectx;
    double now;
    double fs;      /* tasa de entrada (muestras/s) */
    int64_t n_in;   /* muestras de entrada consumidas antes de la llamada en curso */
    sb_t sb;
    unsigned long frames, lorawan;
    lw_dev_t dev[MAX_DEV];
} lora_t;

static const char *const MTYPE[8] = {"JoinRequest",    "JoinAccept",         "UnconfirmedDataUp",
                                     "UnconfirmedDataDown", "ConfirmedDataUp", "ConfirmedDataDown",
                                     "RejoinRequest",  "Proprietary"};

/* Parsea las opciones. Devuelve 0 o −1 con err. */
static int parse_opts(const char *js, lora_rx_cfg_t *c, char *err, size_t errlen)
{
    c->bw_hz = 125000;
    c->sf_mask = 0x1F80; /* SF7..12 */
    c->ldro = -1;
    c->sync_any = false;
    c->nsync = 3;
    c->sync[0] = 0x34;
    c->sync[1] = 0x12;
    c->sync[2] = 0x2B;
    if (!js || !js[0])
        return 0;
    jtok_t t[64];
    int nt = json_parse(js, strlen(js), t, 64);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "opts no es un objeto JSON válido");
        return -1;
    }
    int i;
    double v;
    char s[32];
    if ((i = json_obj_get(js, t, 0, "bw")) >= 0 && json_num(js, t, i, &v)) {
        if (v != 125000 && v != 250000) {
            snprintf(err, errlen, "bw debe ser 125000 o 250000");
            return -1;
        }
        c->bw_hz = (int)v;
    }
    if ((i = json_obj_get(js, t, 0, "sf")) >= 0) {
        unsigned mask = 0;
        if (json_num(js, t, i, &v)) {
            if (v >= 7 && v <= 12)
                mask = 1u << (int)v;
        } else if (json_str(js, t, i, s, sizeof s)) {
            if (!strcmp(s, "all"))
                mask = 0x1F80;
            else if (atoi(s) >= 7 && atoi(s) <= 12)
                mask = 1u << atoi(s);
        } else if (t[i].type == J_ARR) {
            for (int k = 0; k < t[i].size; k++) {
                int e = json_arr_get(t, i, k);
                if (e >= 0 && json_num(js, t, e, &v) && v >= 7 && v <= 12)
                    mask |= 1u << (int)v;
            }
        }
        if (!mask) {
            snprintf(err, errlen, "sf no válido (7..12, lista o \"all\")");
            return -1;
        }
        c->sf_mask = mask;
    }
    if ((i = json_obj_get(js, t, 0, "sync")) >= 0) {
        c->nsync = 0;
        if (json_num(js, t, i, &v))
            c->sync[c->nsync++] = (int)v & 0xFF;
        else if (json_str(js, t, i, s, sizeof s)) {
            if (!strcmp(s, "any"))
                c->sync_any = true;
            else
                c->sync[c->nsync++] = (int)strtol(s, NULL, 0) & 0xFF;
        } else if (t[i].type == J_ARR) {
            for (int k = 0; k < t[i].size && c->nsync < 8; k++) {
                int e = json_arr_get(t, i, k);
                if (e >= 0 && json_num(js, t, e, &v))
                    c->sync[c->nsync++] = (int)v & 0xFF;
            }
        }
        if (!c->nsync && !c->sync_any) {
            snprintf(err, errlen, "sync no válido");
            return -1;
        }
    }
    bool b;
    if ((i = json_obj_get(js, t, 0, "ldro")) >= 0 && json_bool(js, t, i, &b))
        c->ldro = b;
    return 0;
}

static void *lora_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    lora_rx_cfg_t c;
    memset(&c, 0, sizeof c);
    if (parse_opts(cfg->opts, &c, err, errlen) < 0)
        return NULL;
    c.fs = cfg->rate;
    lora_t *l = calloc(1, sizeof *l);
    if (!l) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    l->fs = cfg->rate;
    l->bw_hz = c.bw_hz;
    l->rx = lora_rx_create(&c, err, errlen);
    if (!l->rx) {
        free(l);
        return NULL;
    }
    return l;
}

static void lora_destroy(void *st)
{
    lora_t *l = st;
    if (!l)
        return;
    lora_rx_destroy(l->rx);
    sb_free(&l->sb);
    free(l);
}

static void add_hex(sb_t *sb, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        sb_printf(sb, "%02X", p[i]);
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static lw_dev_t *dev_get(lora_t *l, uint32_t addr, uint64_t eui, double now)
{
    lw_dev_t *free_slot = NULL, *oldest = &l->dev[0];
    for (int i = 0; i < MAX_DEV; i++) {
        lw_dev_t *d = &l->dev[i];
        if (d->used && now - d->last > DEV_TTL)
            d->used = false;
        if (d->used && d->devaddr == addr && d->deveui == eui)
            return d;
        if (!d->used && !free_slot)
            free_slot = d;
        if (d->last < oldest->last)
            oldest = d;
    }
    lw_dev_t *d = free_slot ? free_slot : oldest;
    memset(d, 0, sizeof *d);
    d->used = true;
    d->devaddr = addr;
    d->deveui = eui;
    return d;
}

/* Análisis de LoRaWAN (sin descifrar). Escribe ,"lorawan":{…} y rellena text. */
static bool lorawan_parse(lora_t *l, const lora_frame_t *f, sb_t *sb, char *text, size_t tl)
{
    const uint8_t *p = f->data;
    int n = f->len;
    if (n < 12 || (p[0] & 3) != 0)
        return false;
    int mt = p[0] >> 5;
    if (mt == 0) { /* JoinRequest: 1+8+8+2+4 */
        if (n != 23)
            return false;
        uint64_t appeui = 0, deveui = 0;
        for (int i = 7; i >= 0; i--) {
            appeui = (appeui << 8) | p[1 + i];
            deveui = (deveui << 8) | p[9 + i];
        }
        unsigned nonce = p[17] | (p[18] << 8);
        sb_printf(sb, ",\"lorawan\":{\"mtype\":\"JoinRequest\",\"join_eui\":\"%016llX\",\"dev_eui\":\"%016llX\","
                      "\"dev_nonce\":%u,\"mic\":\"",
                  (unsigned long long)appeui, (unsigned long long)deveui, nonce);
        add_hex(sb, p + 19, 4);
        sb_puts(sb, "\"}");
        snprintf(text, tl, "LoRaWAN JoinRequest DevEUI %016llX nonce %u", (unsigned long long)deveui, nonce);
        lw_dev_t *d = dev_get(l, 0, deveui, l->now);
        d->last = l->now;
        d->count++;
        d->sf = f->sf;
        d->rssi = f->rssi_dbfs;
        d->snr = f->snr_db;
        d->fcnt = -1;
        d->fport = -1;
        return true;
    }
    if (mt == 1) { /* JoinAccept: cifrado */
        sb_printf(sb, ",\"lorawan\":{\"mtype\":\"JoinAccept\",\"encrypted\":true}");
        snprintf(text, tl, "LoRaWAN JoinAccept (cifrado, %d B)", n);
        return n == 17 || n == 33;
    }
    if (mt < 2 || mt > 5 || (p[0] & 0x1C))
        return false; /* MType de datos y RFU = 000 */
    uint32_t addr = le32(p + 1);
    unsigned fctrl = p[5], fcnt = p[6] | (p[7] << 8);
    int foptslen = (int)(fctrl & 15);
    int hdr = 8 + foptslen;
    if (n < hdr + 4)
        return false;
    bool up = (mt == 2 || mt == 4);
    int fport = -1, frm = 0;
    if (n > hdr + 4) {
        fport = p[hdr];
        frm = n - hdr - 1 - 4;
    }
    sb_printf(sb, ",\"lorawan\":{\"mtype\":\"%s\",\"confirmed\":%s,\"uplink\":%s,\"devaddr\":\"%08X\",\"fctrl\":%u,"
                  "\"adr\":%s,\"ack\":%s,",
              MTYPE[mt], (mt == 4 || mt == 5) ? "true" : "false", up ? "true" : "false", addr, fctrl,
              (fctrl & 0x80) ? "true" : "false", (fctrl & 0x20) ? "true" : "false");
    if (up)
        sb_printf(sb, "\"adr_ack_req\":%s,\"class_b\":%s,", (fctrl & 0x40) ? "true" : "false",
                  (fctrl & 0x10) ? "true" : "false");
    else
        sb_printf(sb, "\"fpending\":%s,", (fctrl & 0x10) ? "true" : "false");
    sb_printf(sb, "\"fopts_len\":%d,\"fcnt\":%u", foptslen, fcnt);
    if (foptslen) {
        sb_puts(sb, ",\"fopts\":\"");
        add_hex(sb, p + 8, foptslen);
        sb_puts(sb, "\"");
    }
    if (fport >= 0)
        sb_printf(sb, ",\"fport\":%d,\"frm_len\":%d", fport, frm);
    sb_puts(sb, ",\"mic\":\"");
    add_hex(sb, p + n - 4, 4);
    sb_puts(sb, "\"}");
    snprintf(text, tl, "LoRaWAN %s %08X FCnt %u%s SF%d", MTYPE[mt], addr, fcnt, fport >= 0 ? "" : " (sin datos)", f->sf);
    if (fport >= 0) {
        size_t m = strlen(text);
        snprintf(text + m, tl - m, " FPort %d (%d B)", fport, frm);
    }
    lw_dev_t *d = dev_get(l, addr, 0, l->now);
    d->last = l->now;
    d->count++;
    d->fcnt = (int)fcnt;
    d->fport = fport;
    d->sf = f->sf;
    d->rssi = f->rssi_dbfs;
    d->snr = f->snr_db;
    return true;
}

static void on_frame(void *ctx, const lora_frame_t *f)
{
    lora_t *l = ctx;
    l->frames++;
    if (!l->emit)
        return;
    sb_t *sb = &l->sb;
    sb_clear(sb);
    char text[200], lw_text[160] = "";
    sb_printf(sb, "{\"sf\":%d,\"bw\":%d,\"cr\":\"4/%d\",\"crc_ok\":%s,\"has_crc\":%s,\"ldro\":%s,\"sync\":\"0x%02X\","
                  "\"len\":%d,\"snr\":%.1f,\"rssi\":%.1f,\"cfo\":%.0f,\"sfo_ppm\":%.0f,\"fec_fixed\":%d,\"payload\":\"",
              f->sf, f->bw_hz, f->cr + 4, f->crc_ok ? "true" : "false", f->has_crc ? "true" : "false",
              f->ldro ? "true" : "false", f->sync, f->len, f->snr_db, f->rssi_dbfs, f->cfo_hz, f->sfo_ppm,
              f->fec_fixed);
    add_hex(sb, f->data, f->len);
    sb_puts(sb, "\"");
    if (lorawan_parse(l, f, sb, lw_text, sizeof lw_text)) {
        l->lorawan++;
        snprintf(text, sizeof text, "%s, SNR %.1f dB", lw_text, f->snr_db);
    } else
        snprintf(text, sizeof text, "LoRa SF%d BW%d CR4/%d %d B%s, SNR %.1f dB", f->sf, f->bw_hz / 1000, f->cr + 4,
                 f->len, f->crc_ok ? "" : " (sin CRC)", f->snr_db);
    sb_puts(sb, ",\"text\":");
    sb_json_str(sb, text);
    sb_puts(sb, "}");
    /* Posición: la capa PHY da el inicio del preámbulo y el fin de la trama en segundos de señal de
     * entrada; se pasan a muestras relativas al inicio de esta llamada. */
    dec_emit_span(l->emit, l->ectx, (int64_t)llround(f->t_start * l->fs) - l->n_in,
                  (int64_t)llround(f->t_end * l->fs) - l->n_in, sb->data, sb->len);
}

static void lora_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    lora_t *l = st;
    l->emit = emit;
    l->ectx = ctx;
    l->now = now;
    lora_rx_process(l->rx, iq, n, on_frame, l);
    l->n_in += n;
}

static void lora_state(void *st, sb_t *sb, double now)
{
    lora_t *l = st;
    unsigned long a, b, c, d;
    lora_rx_stats(l->rx, &a, &b, &c, &d);
    sb_printf(sb, "{\"bw\":%d,\"frames\":%lu,\"lorawan\":%lu,\"preambles\":%lu,\"hdr_bad\":%lu,\"crc_bad\":%lu,"
                  "\"devices\":[",
              l->bw_hz, d, l->lorawan, a, b, c);
    int cnt = 0;
    for (int i = 0; i < MAX_DEV; i++) {
        lw_dev_t *dv = &l->dev[i];
        if (!dv->used || now - dv->last > DEV_TTL)
            continue;
        if (cnt++)
            sb_puts(sb, ",");
        if (dv->devaddr || !dv->deveui)
            sb_printf(sb, "{\"id\":\"%08X\"", dv->devaddr);
        else
            sb_printf(sb, "{\"id\":\"%016llX\"", (unsigned long long)dv->deveui);
        sb_printf(sb, ",\"count\":%u,\"sf\":%d,\"rssi\":%.1f,\"snr\":%.1f,\"age\":%.0f", dv->count, dv->sf, dv->rssi,
                  dv->snr, now - dv->last);
        if (dv->fcnt >= 0)
            sb_printf(sb, ",\"fcnt\":%d", dv->fcnt);
        if (dv->fport >= 0)
            sb_printf(sb, ",\"fport\":%d", dv->fport);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_LORA = {
    .id = "lora",
    .name = "LoRa / LoRaWAN",
    .group = "IoT",
    .desc = "LoRa PHY (CSS SF7–12) y capa LoRaWAN.",
    .kind = DEC_NARROW,
    .rate = 500000,
    .bw_hz = 300000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {868100000LL, 868300000LL, 868500000LL, 0LL},
    .create = lora_create,
    .destroy = lora_destroy,
    .process = lora_process,
    .state_json = lora_state,
};
