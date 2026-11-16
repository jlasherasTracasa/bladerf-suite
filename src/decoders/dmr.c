/*
 * dmr.c — Decodificador DMR SOLO DE METADATOS (ETSI TS 102 361-1 V2.5.1).
 * NO se decodifica, extrae ni reproduce voz (AGENTS.md §0.12): las tramas de
 * voz solo sirven para saber que hay una llamada activa y para leer su LC.
 * Ver docs/decoders/dmr.md.
 *
 * Cadena: IQ 48 kHz → FIR de canal → discriminador FM (±1944 Hz = ±1) →
 * filtro adaptado RRC (β = 0,2, §10.2.2.2) → reloj de Gardner a 4800 símb/s
 * (sps = 10; pocsag_fsk.h) → dibits (umbrales 0 y ±2/3 de la amplitud externa).
 *
 * Sincronía y ráfaga. Cada símbolo entra en una ventana de 24 símbolos
 * (48 bits) que se compara con los 8 patrones de la tabla 9.2 (≤ 5 bits de
 * diferencia). Los patrones de voz y de datos son complementarios símbolo a
 * símbolo, de modo que una señal con polaridad invertida parecería "datos"
 * (limitación: no se corrige la polaridad). La sincronía está en el centro de
 * la ráfaga de 132 símbolos (símbolos 54..77); cuando llegan los 54 símbolos
 * posteriores se procesa la ráfaga entera:
 *   - CACH (12 símbolos antes de la ráfaga): TACT = AT, TC, LCSS + Hamming(7,4);
 *     TC da la ranura (0 → 1, 1 → 2) en transmisiones de estación base.
 *   - Datos: tipo de ranura (20 bits repartidos 10 + 10 alrededor de la
 *     sincronía) con Golay(20,8): CC y tipo de dato. La carga (98 + 98 bits)
 *     es BPTC(196,96); según el tipo:
 *       1 cabecera de voz / 2 terminador con LC: 12 octetos = LC(9) +
 *         RS(12,9); la paridad lleva la máscara 0x969696 / 0x999999.
 *         LC: PF, FLCO(6), FID, y para FLCO 0 (grupo) y 3 (privada):
 *         opciones de servicio, destino(24), origen(24).
 *       3 CSBK: 12 octetos, CRC-CCITT con máscara 0xA5A5.
 *       6 cabecera de datos: CRC-CCITT con máscara 0xCCCC.
 *   - Voz: la sincronía de voz marca la trama A del supertrama (6 tramas de
 *     la misma ranura, cada 288 símbolos). Las tramas B..E llevan EMB
 *     (QR(16,7): CC, LCSS) y 32 bits de LC incrustado; cuatro fragmentos
 *     (LCSS 01, 11, 11, 10) forman el LC de 72 bits (Hamming(16,11) por filas
 *     + paridad por columnas + suma de 5 bits). Así se identifica una llamada
 *     a la que se llegó tarde (sin cabecera).
 *
 * Eventos: uno por cabecera/terminador/CSBK/cabecera de datos/LC incrustado
 * validados por FEC y CRC; repeticiones idénticas (< 3 s) se descartan.
 */
#include "decoder.h"
#include "dmr.h"
#include "dmr_fec.h"
#include "pocsag_fsk.h"

#include "../dsp/bits.h"
#include "../dsp/pulse.h"
#include "../util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 1024
#define SPS 10
#define SYNC_MAX_ERR 5
#define NSCHED 16
#define CALL_TTL 3.0
#define RECENT_MAX 24
#define RECENT_TTL 600.0
#define MASK48 0xFFFFFFFFFFFFull
#define FH_SIZE 2048
#define LOCK_SYMS 400 /* sin sincronía durante tantos símbolos = reloj no enganchado */

typedef struct {
    bool active, known; /* known = tiene IDs (LC leído) */
    int cc, flco, fid, svc;
    bool group;
    uint32_t src, dst;
    double t0, last;
    char via[12];
} call_t;

typedef struct {
    int slot, cc, flco, svc;
    bool group, known;
    uint32_t src, dst;
    double t0, t1;
    char via[12];
} done_t;

typedef struct {
    int64_t end;
    int slot;
} sched_t;

typedef struct {
    uint64_t h;
    double t;
} dd_t;

typedef struct {
    fskfe_t fe;
    fskch_t ch;
    uint8_t ring[RING];
    int64_t nsym;
    uint64_t win;
    int64_t pend_end;
    int pend_kind;
    sched_t sched[NSCHED];
    int nsched;
    /* adquisición de reloj dirigida por sincronía (ver acq_*) */
    float fh[FH_SIZE];       /* salida del filtro adaptado, anillo de muestras */
    int64_t ns;              /* nº de muestras procesadas */
    uint32_t sreg[SPS];      /* por fase mod SPS: signos de los últimos 24 símbolos */
    int64_t last_sync_sym;
    bool cand;
    int cand_best, cand_kind, cand_wait;
    int64_t cand_first, cand_last;
    call_t call[3];
    uint32_t frag[3][4];
    int nfrag[3];
    int slot_cc[3];
    double rate, now;
    unsigned long n_bursts, n_syncs, n_bad_slot, n_lc_ok, n_lc_bad, n_csbk, n_dh, n_emb, n_emb_bad;
    unsigned long dt_count[16];
    int last_cc;
    dd_t dd[8];
    int ndd;
    done_t rec[RECENT_MAX];
    int nrec;
    sb_t sb;
    dec_emit_fn emit;
    void *emit_ctx;
    float *fm;
    int fm_cap;
} dmr_t;

/* ------------------------------------------------------------------------- */
static void get_bits(const dmr_t *d, int64_t first, int nsym, uint8_t *bits)
{
    for (int i = 0; i < nsym; i++) {
        uint8_t v = d->ring[(first + i) & (RING - 1)];
        bits[2 * i] = (uint8_t)(v >> 1);
        bits[2 * i + 1] = (uint8_t)(v & 1);
    }
}

static void bits_to_bytes(const uint8_t *bits, int nbytes, uint8_t *out)
{
    for (int i = 0; i < nbytes; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++)
            v = (uint8_t)((v << 1) | bits[8 * i + b]);
        out[i] = v;
    }
}

/* ¿Evento repetido? (clave hash, ventana de 3 s). */
static bool is_dup(dmr_t *d, uint64_t h)
{
    for (int i = 0; i < d->ndd; i++)
        if (d->dd[i].h == h) {
            bool dup = d->now - d->dd[i].t < 3.0;
            d->dd[i].t = d->now;
            return dup;
        }
    int idx = d->ndd < 8 ? d->ndd++ : 0;
    if (d->ndd == 8)
        for (int i = 1; i < 8; i++)
            if (d->dd[i].t < d->dd[idx].t)
                idx = i;
    d->dd[idx].h = h;
    d->dd[idx].t = d->now;
    return false;
}

static uint64_t hash_bytes(int tag, int slot, const uint8_t *p, int n)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(tag * 16 + slot);
    for (int i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void finish_call(dmr_t *d, int slot)
{
    call_t *c = &d->call[slot];
    if (!c->active)
        return;
    c->active = false;
    if (!c->known)
        return;
    done_t r = {.slot = slot, .cc = c->cc, .flco = c->flco, .svc = c->svc, .group = c->group, .known = c->known,
                .src = c->src, .dst = c->dst, .t0 = c->t0, .t1 = c->last};
    snprintf(r.via, sizeof r.via, "%s", c->via);
    if (d->nrec == RECENT_MAX) {
        memmove(d->rec, d->rec + 1, sizeof(done_t) * (RECENT_MAX - 1));
        d->nrec--;
    }
    d->rec[d->nrec++] = r;
}

static void send(dmr_t *d)
{
    if (d->emit)
        d->emit(d->emit_ctx, d->sb.data, d->sb.len);
}

static void slot_str(char *s, size_t n, int slot)
{
    if (slot)
        snprintf(s, n, "TS%d", slot);
    else
        snprintf(s, n, "TS?");
}

/* LC de 9 octetos → evento. burst = "voice_header" | "terminator" | "embedded_lc" */
static void emit_lc(dmr_t *d, const char *burst, int slot, int cc, const uint8_t *lc, int corrected)
{
    int pf = lc[0] >> 7, flco = lc[0] & 0x3F, fid = lc[1];
    bool ids = flco == 0 || flco == 3;
    uint32_t dst = (uint32_t)lc[3] << 16 | (uint32_t)lc[4] << 8 | lc[5];
    uint32_t src = (uint32_t)lc[6] << 16 | (uint32_t)lc[7] << 8 | lc[8];
    int svc = lc[2];
    bool group = flco == 0;
    sb_t *s = &d->sb;
    sb_clear(s);
    sb_printf(s, "{\"burst\":\"%s\",\"slot\":%d,\"cc\":%d,\"pf\":%d,\"flco\":%d,\"fid\":%d", burst, slot, cc, pf, flco, fid);
    char ts[8], line[200];
    slot_str(ts, sizeof ts, slot);
    const char *what = !strcmp(burst, "voice_header") ? "inicio" : !strcmp(burst, "terminator") ? "fin" : "LC incrustado";
    if (ids) {
        sb_printf(s, ",\"call\":\"%s\",\"dst\":%u,\"src\":%u,\"svc\":%d,\"emergency\":%s,\"privacy\":%s", group ? "group" : "private",
                  dst, src, svc, (svc & 0x80) ? "true" : "false", (svc & 0x40) ? "true" : "false");
        snprintf(line, sizeof line, "DMR %s CC%d %s %s %u <- %u (%s)%s", ts, cc, group ? "grupo" : "privada",
                 group ? "TG" : "ID", dst, src, what, (svc & 0x80) ? " EMERGENCIA" : "");
    } else {
        char hex[20];
        for (int i = 0; i < 7; i++)
            snprintf(hex + 2 * i, 3, "%02X", lc[2 + i]);
        sb_printf(s, ",\"data\":\"%s\"", hex);
        snprintf(line, sizeof line, "DMR %s CC%d LC FLCO %d FID %d datos %s (%s)", ts, cc, flco, fid, hex, what);
    }
    sb_printf(s, ",\"corrected\":%d,\"text\":", corrected);
    sb_json_str(s, line);
    sb_puts(s, "}");
    send(d);
}

/* Registra el LC en la tabla de llamadas. Devuelve true si es un evento nuevo. */
static bool note_lc(dmr_t *d, const char *burst, int slot, int cc, const uint8_t *lc)
{
    int flco = lc[0] & 0x3F;
    bool ids = flco == 0 || flco == 3;
    uint32_t dst = (uint32_t)lc[3] << 16 | (uint32_t)lc[4] << 8 | lc[5];
    uint32_t src = (uint32_t)lc[6] << 16 | (uint32_t)lc[7] << 8 | lc[8];
    call_t *c = &d->call[slot];
    if (!strcmp(burst, "terminator")) {
        bool was = c->active && c->known;
        if (was) {
            c->last = d->now;
            finish_call(d, slot);
            return true;
        }
        return !is_dup(d, hash_bytes(9, slot, lc, 9));
    }
    if (ids && c->active && c->known && c->src == src && c->dst == dst && c->flco == flco) {
        c->last = d->now;
        return false; /* repetición de la cabecera / LC ya conocido */
    }
    double t0 = (c->active && !c->known) ? c->t0 : d->now; /* conserva el inicio de una llamada ya vista por voz */
    if (c->active)
        finish_call(d, slot);
    memset(c, 0, sizeof *c);
    c->active = true;
    c->known = ids;
    c->cc = cc;
    c->flco = flco;
    c->fid = lc[1];
    c->svc = lc[2];
    c->group = flco == 0;
    c->src = src;
    c->dst = dst;
    c->t0 = t0;
    c->last = d->now;
    snprintf(c->via, sizeof c->via, "%s", !strcmp(burst, "voice_header") ? "header" : "embedded");
    return true;
}

/* ------------------------------------------------------------------------- */
static void handle_lc_burst(dmr_t *d, int slot, int cc, int dt, const uint8_t *info196)
{
    uint8_t info[96], blk[12];
    int c = dmr_bptc_decode(info196, info);
    if (c < 0) {
        d->n_lc_bad++;
        return;
    }
    bits_to_bytes(info, 12, blk);
    uint8_t m = dt == DMR_DT_VOICE_LC_HEADER ? 0x96 : 0x99; /* máscara de CRC de la tabla B.21 */
    for (int i = 9; i < 12; i++)
        blk[i] ^= m;
    int r = dmr_rs129_check(blk);
    if (r < 0) {
        d->n_lc_bad++;
        return;
    }
    d->n_lc_ok++;
    const char *burst = dt == DMR_DT_VOICE_LC_HEADER ? "voice_header" : "terminator";
    if (note_lc(d, burst, slot, cc, blk))
        emit_lc(d, burst, slot, cc, blk, c + r);
}

static void handle_csbk(dmr_t *d, int slot, int cc, const uint8_t *info196)
{
    uint8_t info[96], b[12];
    int c = dmr_bptc_decode(info196, info);
    if (c < 0)
        return;
    bits_to_bytes(info, 12, b);
    uint16_t rx = (uint16_t)((b[10] << 8 | b[11]) ^ 0xA5A5);
    if (dmr_crc_ccitt(b, 10) != rx)
        return;
    d->n_csbk++;
    if (is_dup(d, hash_bytes(3, slot, b, 10)))
        return;
    char hex[20], line[160], ts[8];
    for (int i = 0; i < 8; i++)
        snprintf(hex + 2 * i, 3, "%02X", b[2 + i]);
    int csbko = b[0] & 0x3F;
    slot_str(ts, sizeof ts, slot);
    snprintf(line, sizeof line, "DMR %s CC%d CSBK opcode %d FID %d datos %s", ts, cc, csbko, b[1], hex);
    sb_t *s = &d->sb;
    sb_clear(s);
    sb_printf(s, "{\"burst\":\"csbk\",\"slot\":%d,\"cc\":%d,\"lb\":%d,\"pf\":%d,\"csbko\":%d,\"fid\":%d,\"data\":\"%s\","
                 "\"corrected\":%d,\"text\":",
              slot, cc, b[0] >> 7, (b[0] >> 6) & 1, csbko, b[1], hex, c);
    sb_json_str(s, line);
    sb_puts(s, "}");
    send(d);
}

static void handle_data_header(dmr_t *d, int slot, int cc, const uint8_t *info196)
{
    uint8_t info[96], b[12];
    int c = dmr_bptc_decode(info196, info);
    if (c < 0)
        return;
    bits_to_bytes(info, 12, b);
    uint16_t rx = (uint16_t)((b[10] << 8 | b[11]) ^ 0xCCCC);
    if (dmr_crc_ccitt(b, 10) != rx)
        return;
    d->n_dh++;
    if (is_dup(d, hash_bytes(6, slot, b, 10)))
        return;
    int dpf = b[0] & 0xF, gi = b[0] >> 7, sap = b[1] >> 4;
    uint32_t dst = (uint32_t)b[2] << 16 | (uint32_t)b[3] << 8 | b[4];
    uint32_t src = (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    const char *dn = dpf == 0 ? "UDT" : dpf == 1 ? "respuesta" : dpf == 2 ? "datos sin confirmar" : dpf == 3 ? "datos confirmados"
                   : dpf == 13 ? "short data definido" : dpf == 14 ? "short data raw/estado" : dpf == 15 ? "propietario" : "reservado";
    char line[200], ts[8];
    slot_str(ts, sizeof ts, slot);
    snprintf(line, sizeof line, "DMR %s CC%d cabecera de datos (%s) %s %u <- %u", ts, cc, dn, gi ? "grupo" : "privada", dst, src);
    sb_t *s = &d->sb;
    sb_clear(s);
    sb_printf(s, "{\"burst\":\"data_header\",\"slot\":%d,\"cc\":%d,\"dpf\":%d,\"dpf_name\":\"%s\",\"sap\":%d,\"call\":\"%s\","
                 "\"dst\":%u,\"src\":%u,\"corrected\":%d,\"text\":",
              slot, cc, dpf, dn, sap, gi ? "group" : "private", dst, src, c);
    sb_json_str(s, line);
    sb_puts(s, "}");
    send(d);
}

static void process_data_burst(dmr_t *d, int slot, const uint8_t *b)
{
    uint32_t st = 0;
    for (int i = 0; i < 10; i++)
        st = (st << 1) | b[98 + i];
    for (int i = 0; i < 10; i++)
        st = (st << 1) | b[156 + i];
    int cc, dt;
    if (dmr_slottype_decode(st, &cc, &dt) < 0) {
        d->n_bad_slot++;
        return;
    }
    d->slot_cc[slot] = cc;
    d->last_cc = cc;
    d->dt_count[dt]++;
    if (dt != DMR_DT_VOICE_LC_HEADER && dt != DMR_DT_TERMINATOR_LC && dt != DMR_DT_CSBK && dt != DMR_DT_DATA_HEADER)
        return;
    uint8_t info[196];
    memcpy(info, b, 98);
    memcpy(info + 98, b + 166, 98);
    if (dt == DMR_DT_VOICE_LC_HEADER || dt == DMR_DT_TERMINATOR_LC)
        handle_lc_burst(d, slot, cc, dt, info);
    else if (dt == DMR_DT_CSBK)
        handle_csbk(d, slot, cc, info);
    else
        handle_data_header(d, slot, cc, info);
}

/* Trama B..F (sin sincronía): EMB + LC incrustado. */
static void process_embedded(dmr_t *d, int slot, int64_t e)
{
    uint8_t b[264];
    get_bits(d, e - 77, 132, b);
    uint32_t w = 0;
    for (int i = 0; i < 8; i++)
        w = (w << 1) | b[108 + i];
    for (int i = 0; i < 8; i++)
        w = (w << 1) | b[148 + i];
    int cc, pi, lcss;
    if (dmr_emb_decode(w, &cc, &pi, &lcss) < 0) {
        d->n_emb_bad++;
        d->nfrag[slot] = 0;
        return;
    }
    d->n_emb++;
    d->slot_cc[slot] = cc;
    d->last_cc = cc;
    uint32_t f = 0;
    for (int i = 0; i < 32; i++)
        f = (f << 1) | b[116 + i];
    if (lcss == 1) {
        d->frag[slot][0] = f;
        d->nfrag[slot] = 1;
    } else if (lcss == 3 && d->nfrag[slot] >= 1 && d->nfrag[slot] < 3) {
        d->frag[slot][d->nfrag[slot]++] = f;
    } else if (lcss == 2 && d->nfrag[slot] == 3) {
        d->frag[slot][3] = f;
        d->nfrag[slot] = 0;
        uint8_t lc[9];
        int c = dmr_emblc_decode(d->frag[slot], lc);
        if (c >= 0) {
            d->n_lc_ok++;
            if (note_lc(d, "embedded_lc", slot, cc, lc))
                emit_lc(d, "embedded_lc", slot, cc, lc, c);
        }
    } else if (lcss != 0) {
        d->nfrag[slot] = 0;
    }
    /* actividad de voz en la ranura */
    if (d->call[slot].active)
        d->call[slot].last = d->now;
}

static void voice_sync(dmr_t *d, int slot, int64_t e)
{
    call_t *c = &d->call[slot];
    if (c->active)
        c->last = d->now;
    else {
        memset(c, 0, sizeof *c);
        c->active = true;
        c->cc = d->slot_cc[slot] ? d->slot_cc[slot] : d->last_cc;
        c->t0 = c->last = d->now;
        snprintf(c->via, sizeof c->via, "voice");
    }
    d->nfrag[slot] = 0;
    for (int k = 1; k <= 5 && d->nsched < NSCHED; k++)
        d->sched[d->nsched++] = (sched_t){e + 288 * k, slot};
}

static void process_burst(dmr_t *d, int64_t e, int kind)
{
    uint8_t b[264];
    int64_t sb0 = e - 77;
    get_bits(d, sb0, 132, b);
    d->n_bursts++;
    int slot = 0;
    if (kind == DMR_SYNC_BS_VOICE || kind == DMR_SYNC_BS_DATA) {
        uint8_t cb[24], pay[17];
        get_bits(d, sb0 - 12, 12, cb);
        uint32_t tact;
        dmr_cach_split(cb, &tact, pay);
        int at, tc, lcss;
        if (dmr_tact_decode(tact, &at, &tc, &lcss) == 0)
            slot = tc + 1;
    } else if (kind == DMR_SYNC_DM1_VOICE || kind == DMR_SYNC_DM1_DATA)
        slot = 1;
    else if (kind == DMR_SYNC_DM2_VOICE || kind == DMR_SYNC_DM2_DATA)
        slot = 2;
    if (dmr_sync_is_voice(kind))
        voice_sync(d, slot, e);
    else
        process_data_burst(d, slot, b);
}

static void expire(dmr_t *d)
{
    for (int s = 0; s < 3; s++)
        if (d->call[s].active && d->now - d->call[s].last > CALL_TTL)
            finish_call(d, s);
}


/* ------------------------------------------------------------------------- */
/*  Adquisición de reloj dirigida por la sincronía                           */
/* ------------------------------------------------------------------------- */
/* El lazo de Gardner tarda mucho en enganchar desde una fase mala (cerca de su
 * punto de equilibrio inestable) y una transmisión nueva trae solo 54 símbolos
 * antes de su sincronía. Mientras no haya habido sincronía reciente se vigila,
 * en las SPS fases posibles, el signo de los últimos 24 símbolos (los patrones
 * de sincronía solo usan los niveles externos ±3, así que basta el signo):
 * un registro de 24 bits por fase se actualiza en O(1) por muestra. Si coincide
 * con algún patrón (≤ 2 diferencias), se espera 9 muestras y se toma el centro de la meseta de máxima coincidencia, se
 * reconstruyen los símbolos con esa fase para confirmar con los 48 bits
 * (≤ SYNC_MAX_ERR) y se reajusta el reloj del lazo y el anillo de símbolos. */
static uint32_t sign_mask(int k)
{
    uint32_t m = 0;
    for (int i = 0; i < 24; i++) /* bit alto del dibit: 1 = desviación negativa */
        m = (m << 1) | (uint32_t)((DMR_SYNC[k] >> (47 - 2 * i)) & 1);
    return m;
}

static int dib_at(const dmr_t *d, int64_t ns_pos)
{
    float v = (d->fh[ns_pos & (FH_SIZE - 1)] - d->ch.ctr) / d->ch.amp;
    return ((v < 0) << 1) | (fabsf(v) > (2.0f / 3.0f));
}

static void acq_accept(dmr_t *d, int64_t cur)
{
    int64_t nb = (d->cand_first + d->cand_last) / 2; /* centro de la meseta de máxima coincidencia */
    uint64_t w = 0;
    for (int j = 23; j >= 0; j--)
        w = (w << 2) | (uint64_t)dib_at(d, nb - (int64_t)SPS * j);
    int k = d->cand_kind;
    if (bits_popcount64(w ^ DMR_SYNC[k]) > SYNC_MAX_ERR + 1)
        return;
    int64_t e = d->nsym - 1;
    for (int j = 0; j < 100; j++) /* reconstruye los 100 símbolos previos con la fase buena */
        d->ring[(e - j) & (RING - 1)] = (uint8_t)dib_at(d, nb - (int64_t)SPS * j);
    d->ch.t = (double)(cur - nb) / SPS;
    d->ch.have_mid = false;
    d->win = w;
    d->pend_end = e;
    d->pend_kind = k;
    d->n_syncs++;
    d->last_sync_sym = d->nsym;
}

static void acq_sample(dmr_t *d)
{
    float f = d->ch.f_prev;
    int64_t cur = d->ns++;
    d->fh[cur & (FH_SIZE - 1)] = f;
    int ph = (int)(cur % SPS);
    d->sreg[ph] = ((d->sreg[ph] << 1) | (f < d->ch.ctr)) & 0xFFFFFFu;
    if (d->pend_end >= 0 || d->nsym - d->last_sync_sym < LOCK_SYMS || cur < 24 * SPS)
        return;
    int best = 99, bk = -1;
    for (int k = 0; k < DMR_SYNC_COUNT; k++) {
        int m = bits_popcount32(d->sreg[ph] ^ sign_mask(k));
        if (m < best) {
            best = m;
            bk = k;
        }
    }
    if (best <= 2 && (!d->cand || best < d->cand_best)) {
        if (!d->cand)
            d->cand_wait = 9;
        d->cand = true;
        d->cand_best = best;
        d->cand_kind = bk;
        d->cand_first = d->cand_last = cur;
    } else if (d->cand && best == d->cand_best && bk == d->cand_kind) {
        d->cand_last = cur;
    }
    if (d->cand && --d->cand_wait <= 0) {
        d->cand = false;
        acq_accept(d, cur);
    }
}

/* ------------------------------------------------------------------------- */
static void sym_in(dmr_t *d, float y)
{
    int dib = ((y < 0) << 1) | (fabsf(y) > (2.0f / 3.0f));
    d->ring[d->nsym & (RING - 1)] = (uint8_t)dib;
    d->nsym++;
    d->win = ((d->win << 2) | (uint64_t)dib) & MASK48;
    int64_t last = d->nsym - 1;
    if (d->pend_end < 0 && d->nsym >= 100) {
        int best = 99, bk = -1;
        for (int k = 0; k < DMR_SYNC_COUNT; k++) {
            int e = bits_popcount64(d->win ^ DMR_SYNC[k]);
            if (e < best) {
                best = e;
                bk = k;
            }
        }
        if (best <= SYNC_MAX_ERR) {
            d->pend_end = last;
            d->pend_kind = bk;
            d->n_syncs++;
            d->last_sync_sym = d->nsym;
        }
    }
    if (d->pend_end >= 0 && last == d->pend_end + 54) {
        int64_t e = d->pend_end;
        d->pend_end = -1;
        process_burst(d, e, d->pend_kind);
    }
    for (int i = 0; i < d->nsched;) {
        if (last == d->sched[i].end + 54) {
            process_embedded(d, d->sched[i].slot, d->sched[i].end);
            d->sched[i] = d->sched[--d->nsched];
        } else
            i++;
    }
}

static void *dmr_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    dmr_t *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    dmr_fec_init();
    d->rate = cfg->rate;
    d->pend_end = -1;
    d->last_cc = -1;
    d->last_sync_sym = -100000;
    int sps = (int)lround(cfg->rate / DMR_BAUD);
    int nt = pulse_ntaps(sps, 8);
    float *h = malloc(sizeof(float) * (size_t)nt);
    if (!h || pulse_rrc(h, 0.2, sps, 8, PULSE_NORM_DC) < 0 || fskch_init(&d->ch, cfg->rate / DMR_BAUD, h, nt, 0.02) < 0) {
        snprintf(err, errlen, "sin memoria");
        free(h);
        free(d);
        return NULL;
    }
    free(h);
    fskfe_init(&d->fe, cfg->rate, 8500.0, DMR_DEV_HZ);
    return d;
}

static void dmr_destroy(void *st)
{
    dmr_t *d = st;
    if (!d)
        return;
    fskch_free(&d->ch);
    sb_free(&d->sb);
    free(d->fm);
    free(d);
}

static void dmr_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    dmr_t *d = st;
    d->emit = emit;
    d->emit_ctx = ctx;
    if (n > d->fm_cap) {
        d->fm_cap = n;
        d->fm = realloc(d->fm, sizeof(float) * (size_t)n);
        if (!d->fm)
            return;
    }
    fskfe_process(&d->fe, iq, n, d->fm);
    for (int k = 0; k < n; k++) {
        d->now = now + (double)k / d->rate;
        float y;
        if (fskch_push(&d->ch, d->fm[k], &y))
            sym_in(d, y);
        acq_sample(d);
    }
    expire(d);
}

static void dmr_state(void *st, sb_t *sb, double now)
{
    dmr_t *d = st;
    d->now = fmax(d->now, now);
    expire(d);
    sb_printf(sb, "{\"bursts\":%lu,\"syncs\":%lu,\"bad_slot\":%lu,\"lc_ok\":%lu,\"lc_bad\":%lu,\"csbk\":%lu,\"data_headers\":%lu,"
                  "\"emb\":%lu,\"cc\":",
              d->n_bursts, d->n_syncs, d->n_bad_slot, d->n_lc_ok, d->n_lc_bad, d->n_csbk, d->n_dh, d->n_emb);
    if (d->last_cc >= 0 && d->n_bursts)
        sb_printf(sb, "%d", d->last_cc);
    else
        sb_puts(sb, "null");
    sb_puts(sb, ",\"calls\":[");
    bool first = true;
    for (int s = 0; s < 3; s++) {
        const call_t *c = &d->call[s];
        if (!c->active)
            continue;
        if (!first)
            sb_puts(sb, ",");
        first = false;
        sb_printf(sb, "{\"slot\":%d,\"cc\":%d,\"via\":\"%s\",\"age\":%.1f,\"dur\":%.1f", s, c->cc, c->via,
                  fmax(0.0, d->now - c->last), fmax(0.0, c->last - c->t0));
        if (c->known)
            sb_printf(sb, ",\"call\":\"%s\",\"dst\":%u,\"src\":%u,\"svc\":%d,\"emergency\":%s", c->group ? "group" : "private",
                      c->dst, c->src, c->svc, (c->svc & 0x80) ? "true" : "false");
        sb_puts(sb, "}");
    }
    sb_puts(sb, "],\"recent\":[");
    int w = 0;
    for (int i = 0; i < d->nrec; i++)
        if (now - d->rec[i].t1 <= RECENT_TTL)
            d->rec[w++] = d->rec[i];
    d->nrec = w;
    for (int i = 0; i < d->nrec; i++) {
        const done_t *r = &d->rec[d->nrec - 1 - i];
        if (i)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"slot\":%d,\"cc\":%d,\"call\":\"%s\",\"dst\":%u,\"src\":%u,\"dur\":%.1f,\"seen\":%.1f}", r->slot, r->cc,
                  r->group ? "group" : "private", r->dst, r->src, fmax(0.0, r->t1 - r->t0), fmax(0.0, now - r->t1));
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_DMR = {
    .id = "dmr",
    .name = "DMR",
    .group = "Radio digital",
    .desc = "DMR (solo metadatos: slot, CC, LC origen/destino, CSBK; sin voz).",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 12500,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {438500000LL, 0LL, 0LL, 0LL},
    .create = dmr_create,
    .destroy = dmr_destroy,
    .process = dmr_process,
    .state_json = dmr_state,
};
