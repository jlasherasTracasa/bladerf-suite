/*
 * mod_dmr.c — Modulador DMR (solo señalización, SIN VOZ) a 48 ksps.
 *
 * Genera un flujo continuo de estación base (BS) con las dos ranuras TDMA
 * alternadas (tramas de 30 ms = CACH de 24 bits + ráfaga de 264 bits):
 *   - CACH: TACT = AT 1, TC = ranura − 1, LCSS 0 + Hamming(7,4); carga a cero.
 *   - ráfagas de datos con sincronía "BS datos": cabecera de voz LC y terminador
 *     con LC (BPTC(196,96) de LC(9) + RS(12,9) enmascarado 0x969696/0x999999),
 *     CSBK y cabecera de datos (CRC-CCITT enmascarado 0xA5A5 / 0xCCCC), reposo.
 *   - supertramas de "voz" (6 tramas por ranura): A con sincronía "BS voz"; B..E
 *     con EMB (QR(16,7)) y el LC incrustado en 4 fragmentos; F con incrustado
 *     nulo. La carga de voz de 2×108 bits es un patrón pseudoaleatorio fijo:
 *     NO hay codificador de voz ni audio.
 * Modulación 4FSK: dibit 01 → +3 (+1944 Hz), 00 → +1, 10 → −1, 11 → −3; pulso
 * RRC β = 0,2 (§10.2.2.2), 10 muestras por símbolo (4800 símb/s a 48 kHz).
 *
 * Parámetros JSON (opcionales):
 *   {"cc":1, "calls":[{"slot":1,"group":true,"dst":91,"src":2001234,"svc":0,"superframes":2,"header":true},
 *                     {"slot":2,"group":false,"dst":4321,"src":1234567,"svc":128,"superframes":1}],
 *    "csbk":true, "data_header":true}
 * Por omisión, los dos de arriba (la llamada privada con bit de emergencia),
 * un CSBK y una cabecera de datos en la ranura 2.
 *
 * air_ok = false (no está en la lista blanca de §6).
 */
#include "mod.h"

#include "../decoders/dmr.h"
#include "../decoders/dmr_fec.h"
#include "../dsp/pulse.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define SPS 10
#define NIDLE 4
#define MAXQ 64

typedef enum { B_IDLE, B_HDR, B_TERM, B_CSBK, B_DH, B_VOICE } btype_t;

typedef struct {
    btype_t t;
    int k;         /* B_VOICE: 0..5 (A..F) */
    uint8_t lc[9]; /* LC del encabezado/terminador/incrustado */
} bdesc_t;

typedef struct {
    int slot, svc, superframes;
    bool group, header;
    uint32_t dst, src;
} call_cfg_t;

static void bits_from_bytes(const uint8_t *b, int n, uint8_t *bits)
{
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 8; k++)
            bits[8 * i + k] = (uint8_t)((b[i] >> (7 - k)) & 1);
}

/* Información BPTC de 12 octetos → 196 bits */
static void bptc_from_bytes(const uint8_t *b12, uint8_t *out196)
{
    uint8_t info[96];
    bits_from_bytes(b12, 12, info);
    dmr_bptc_encode(info, out196);
}

static void put_sync(uint8_t *b, int kind)
{
    for (int i = 0; i < 48; i++)
        b[108 + i] = (uint8_t)((DMR_SYNC[kind] >> (47 - i)) & 1);
}

static void put_slottype(uint8_t *b, int cc, int dt)
{
    uint32_t w = dmr_slottype_encode(cc, dt);
    for (int i = 0; i < 10; i++) {
        b[98 + i] = (uint8_t)((w >> (19 - i)) & 1);
        b[156 + i] = (uint8_t)((w >> (9 - i)) & 1);
    }
}

static void lc_block(const uint8_t *lc9, uint8_t mask, uint8_t *b12)
{
    memcpy(b12, lc9, 9);
    dmr_rs129_parity(lc9, b12 + 9);
    for (int i = 9; i < 12; i++)
        b12[i] ^= mask;
}

static void crc_block(const uint8_t *b10, uint16_t mask, uint8_t *b12)
{
    memcpy(b12, b10, 10);
    uint16_t c = (uint16_t)(dmr_crc_ccitt(b10, 10) ^ mask);
    b12[10] = (uint8_t)(c >> 8);
    b12[11] = (uint8_t)c;
}

static uint32_t rnd_state = 0x1234567u;
static uint8_t rnd_bit(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return (uint8_t)(rnd_state & 1);
}

/* Construye la trama de 288 bits (CACH + ráfaga) de una ranura. */
static void build_frame(const bdesc_t *bd, int slot, int cc, uint8_t *f)
{
    uint8_t *b = f + 24, pay17[17] = {0};
    memset(b, 0, 264);
    uint8_t blk[12], info[196], b10[10];
    switch (bd->t) {
    case B_IDLE:
        memset(blk, 0, 12);
        bptc_from_bytes(blk, info);
        memcpy(b, info, 98);
        memcpy(b + 166, info + 98, 98);
        put_sync(b, DMR_SYNC_BS_DATA);
        put_slottype(b, cc, DMR_DT_IDLE);
        break;
    case B_HDR:
    case B_TERM:
        lc_block(bd->lc, bd->t == B_HDR ? 0x96 : 0x99, blk);
        bptc_from_bytes(blk, info);
        memcpy(b, info, 98);
        memcpy(b + 166, info + 98, 98);
        put_sync(b, DMR_SYNC_BS_DATA);
        put_slottype(b, cc, bd->t == B_HDR ? DMR_DT_VOICE_LC_HEADER : DMR_DT_TERMINATOR_LC);
        break;
    case B_CSBK:
        b10[0] = 0x80 | 0x04; /* LB = 1, PF = 0, CSBKO = 4 (valor de prueba) */
        b10[1] = 0x00;
        for (int i = 2; i < 10; i++)
            b10[i] = (uint8_t)(0x10 * i + 1);
        crc_block(b10, 0xA5A5, blk);
        bptc_from_bytes(blk, info);
        memcpy(b, info, 98);
        memcpy(b + 166, info + 98, 98);
        put_sync(b, DMR_SYNC_BS_DATA);
        put_slottype(b, cc, DMR_DT_CSBK);
        break;
    case B_DH:
        b10[0] = 0x02;          /* G/I = 0 (privada), A = 0, DPF = 2 (sin confirmar) */
        b10[1] = 0x40;          /* SAP = 4 (IP) */
        b10[2] = 0x01, b10[3] = 0x2F, b10[4] = 0x91; /* destino 77713 */
        b10[5] = 0x01, b10[6] = 0x5A, b10[7] = 0xB8; /* origen 88760 */
        b10[8] = 0x81;
        b10[9] = 0x00;
        crc_block(b10, 0xCCCC, blk);
        bptc_from_bytes(blk, info);
        memcpy(b, info, 98);
        memcpy(b + 166, info + 98, 98);
        put_sync(b, DMR_SYNC_BS_DATA);
        put_slottype(b, cc, DMR_DT_DATA_HEADER);
        break;
    case B_VOICE: {
        for (int i = 0; i < 108; i++) { /* carga ficticia, NO es voz */
            b[i] = rnd_bit();
            b[156 + i] = rnd_bit();
        }
        if (bd->k == 0) {
            put_sync(b, DMR_SYNC_BS_VOICE);
        } else {
            uint32_t frag[4], emb;
            uint32_t fr = 0;
            int lcss = 0;
            if (bd->k >= 1 && bd->k <= 4) {
                dmr_emblc_encode(bd->lc, frag);
                fr = frag[bd->k - 1];
                lcss = bd->k == 1 ? 1 : bd->k == 4 ? 2 : 3;
            }
            emb = dmr_emb_encode(cc, 0, lcss);
            for (int i = 0; i < 8; i++) {
                b[108 + i] = (uint8_t)((emb >> (15 - i)) & 1);
                b[148 + i] = (uint8_t)((emb >> (7 - i)) & 1);
            }
            for (int i = 0; i < 32; i++)
                b[116 + i] = (uint8_t)((fr >> (31 - i)) & 1);
        }
        break;
    }
    }
    dmr_cach_build(dmr_tact_encode(1, slot - 1, 0), pay17, f);
}

static void make_lc(const call_cfg_t *c, uint8_t *lc)
{
    lc[0] = c->group ? 0x00 : 0x03; /* PF = 0, FLCO */
    lc[1] = 0x00;                   /* FID: conjunto estándar */
    lc[2] = (uint8_t)c->svc;
    lc[3] = (uint8_t)(c->dst >> 16), lc[4] = (uint8_t)(c->dst >> 8), lc[5] = (uint8_t)c->dst;
    lc[6] = (uint8_t)(c->src >> 16), lc[7] = (uint8_t)(c->src >> 8), lc[8] = (uint8_t)c->src;
}

static int push_q(bdesc_t *q, int n, btype_t t, int k, const uint8_t *lc)
{
    if (n >= MAXQ)
        return n;
    q[n].t = t;
    q[n].k = k;
    if (lc)
        memcpy(q[n].lc, lc, 9);
    else
        memset(q[n].lc, 0, 9);
    return n + 1;
}

static int dmr_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    dmr_fec_init();
    call_cfg_t calls[2] = {{1, 0, 2, true, true, 91, 2001234}, {2, 0x80, 1, false, true, 4321, 1234567}};
    int ncalls = 2, cc = 1;
    bool csbk = true, dh = true;
    const char *js = params ? params : "{}";
    jtok_t t[128];
    int nt = json_parse(js, strlen(js), t, 128);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "parámetros JSON no válidos");
        return -1;
    }
    double v;
    bool bv;
    int k;
    if ((k = json_obj_get(js, t, 0, "cc")) >= 0 && json_num(js, t, k, &v))
        cc = (int)v & 15;
    if ((k = json_obj_get(js, t, 0, "csbk")) >= 0 && json_bool(js, t, k, &bv))
        csbk = bv;
    if ((k = json_obj_get(js, t, 0, "data_header")) >= 0 && json_bool(js, t, k, &bv))
        dh = bv;
    int arr = json_obj_get(js, t, 0, "calls");
    if (arr >= 0 && t[arr].type == J_ARR) {
        ncalls = 0;
        for (int i = 0; i < t[arr].size && ncalls < 2; i++) {
            int e = json_arr_get(t, arr, i);
            if (e < 0 || t[e].type != J_OBJ)
                continue;
            call_cfg_t c = {1, 0, 1, true, true, 1, 1};
            if ((k = json_obj_get(js, t, e, "slot")) >= 0 && json_num(js, t, k, &v))
                c.slot = (int)v;
            if ((k = json_obj_get(js, t, e, "group")) >= 0 && json_bool(js, t, k, &bv))
                c.group = bv;
            if ((k = json_obj_get(js, t, e, "dst")) >= 0 && json_num(js, t, k, &v))
                c.dst = (uint32_t)v & 0xFFFFFF;
            if ((k = json_obj_get(js, t, e, "src")) >= 0 && json_num(js, t, k, &v))
                c.src = (uint32_t)v & 0xFFFFFF;
            if ((k = json_obj_get(js, t, e, "svc")) >= 0 && json_num(js, t, k, &v))
                c.svc = (int)v & 0xFF;
            if ((k = json_obj_get(js, t, e, "header")) >= 0 && json_bool(js, t, k, &bv))
                c.header = bv;
            if ((k = json_obj_get(js, t, e, "superframes")) >= 0 && json_num(js, t, k, &v))
                c.superframes = (int)v < 1 ? 1 : (int)v > 6 ? 6 : (int)v;
            if (c.slot != 1 && c.slot != 2) {
                snprintf(err, errlen, "slot debe ser 1 o 2");
                return -1;
            }
            calls[ncalls++] = c;
        }
    }

    /* Colas por ranura */
    bdesc_t q[2][MAXQ];
    int nq[2] = {0, 0};
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < NIDLE; i++)
            nq[s] = push_q(q[s], nq[s], B_IDLE, 0, NULL);
    for (int i = 0; i < ncalls; i++) {
        int s = calls[i].slot - 1;
        uint8_t lc[9];
        make_lc(&calls[i], lc);
        if (s == 1 && csbk)
            nq[s] = push_q(q[s], nq[s], B_CSBK, 0, NULL);
        if (s == 1 && dh)
            nq[s] = push_q(q[s], nq[s], B_DH, 0, NULL);
        if (calls[i].header) /* header:false = el receptor llega tarde a la llamada */
            nq[s] = push_q(q[s], nq[s], B_HDR, 0, lc);
        for (int sf = 0; sf < calls[i].superframes; sf++)
            for (int f = 0; f < 6; f++)
                nq[s] = push_q(q[s], nq[s], B_VOICE, f, lc);
        nq[s] = push_q(q[s], nq[s], B_TERM, 0, lc);
    }
    if (ncalls == 0 && csbk)
        nq[1] = push_q(q[1], nq[1], B_CSBK, 0, NULL);
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < NIDLE; i++)
            nq[s] = push_q(q[s], nq[s], B_IDLE, 0, NULL);
    int nframes = 2 * (nq[0] > nq[1] ? nq[0] : nq[1]);

    /* Símbolos */
    int nsym = nframes * 144;
    int8_t *lv = malloc((size_t)nsym);
    uint8_t f[288];
    for (int fr = 0; fr < nframes; fr++) {
        int s = fr & 1, idx = fr >> 1;
        bdesc_t bd = idx < nq[s] ? q[s][idx] : (bdesc_t){.t = B_IDLE};
        build_frame(&bd, s + 1, cc, f);
        for (int i = 0; i < 144; i++) {
            int b1 = f[2 * i], b0 = f[2 * i + 1];
            lv[fr * 144 + i] = (int8_t)(b1 ? (b0 ? -3 : -1) : (b0 ? 3 : 1));
        }
    }

    /* Pulso RRC β = 0,2, pico unidad */
    int nh = pulse_ntaps(SPS, 8);
    float *h = malloc(sizeof(float) * (size_t)nh);
    pulse_rrc(h, 0.2, SPS, 8, PULSE_NORM_DC);
    float hmax = 0;
    for (int i = 0; i < nh; i++)
        hmax = fmaxf(hmax, fabsf(h[i]));
    for (int i = 0; i < nh; i++)
        h[i] /= hmax;

    out->rate = FS;
    int lead = (int)(0.05 * FS), nsamp = nsym * SPS + nh;
    iqbuf_reserve(out, lead + nsamp + 4800);
    for (int i = 0; i < lead; i++)
        iqbuf_push(out, (cf32){0.8f, 0});
    double ph = 0;
    for (int n = 0; n < nsamp; n++) {
        double s = 0;
        int k0 = (n - nh + SPS) / SPS; /* símbolos cuyo pulso cubre n */
        if (k0 < 0)
            k0 = 0;
        for (int kk = k0; kk < nsym && kk * SPS <= n; kk++) {
            int j = n - kk * SPS;
            if (j >= 0 && j < nh)
                s += lv[kk] * h[j];
        }
        ph += 2 * M_PI * (s * (DMR_DEV_HZ / 3.0)) / FS;
        if (ph > M_PI)
            ph -= 2 * M_PI;
        else if (ph < -M_PI)
            ph += 2 * M_PI;
        iqbuf_push(out, (cf32){(float)(0.8 * cos(ph)), (float)(0.8 * sin(ph))});
    }
    for (int i = 0; i < 4800; i++)
        iqbuf_push(out, (cf32){(float)(0.8 * cos(ph)), (float)(0.8 * sin(ph))});
    free(h);
    free(lv);
    return 0;
}

const modulator_desc_t MOD_DMR = {
    .id = "dmr",
    .name = "DMR",
    .air_ok = false,
    .modulate = dmr_modulate,
};
