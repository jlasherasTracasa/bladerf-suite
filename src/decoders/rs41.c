/*
 * rs41.c — Decodificador de la radiosonda Vaisala RS41. Ver docs/protocolos/rs41.md
 * y rs41_frame.h (estructura de la trama).
 *
 * Capa física: GFSK a 4800 Bd, desviación ±2,4 kHz (índice de modulación 1), a
 * 403-406 MHz. El DDC entrega 48 kHz complejos → 10 muestras por símbolo.
 *
 * Cadena:
 *   IQ ── FIR paso bajo (±9 kHz) ── discriminador FM arg(y[n]·conj(y[n−1]))
 *      ── media de 10 muestras (filtro adaptado al NRZ) ── umbral = media móvil
 *         (absorbe el desvío de portadora; el preámbulo 0101… equilibra la media)
 *      ── búsqueda de la cabecera de 64 bits en las 10 fases a la vez
 *         (≤ 10 errores; también invertida) ── se elige la mejor fase
 *      ── captura de 312 octetos con un lazo temprano/tardío que sigue el reloj
 *         (±100 ppm de error en 0,53 s son 0,26 símbolos: hace falta)
 *      ── XOR con la máscara ── bloques con CRC-16 ── (si fallan) Reed-Solomon
 *      ── evento + tabla de sondas.
 *
 * Es un decodificador de difusión pública (telemetría meteorológica). Las
 * variantes cifradas/de otros fabricantes no se tratan.
 */
#include "decoder.h"
#include "rs41_frame.h"
#include "../dsp/filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPS 10
#define LP_TAPS 41
#define LP_HZ 9000.0
#define HDR_MAX_ERR 10
#define RING 256
#define MAX_SONDES 16
#define SONDE_TTL 600.0

typedef struct {
    char serial[9];
    rs41_info_t last;
    double t_last;
    unsigned frames;
    bool has_pos;
} sonde_t;

typedef struct {
    double fs;
    float lp[LP_TAPS];
    cf32 hist[LP_TAPS];      /* línea de retardo circular */
    int hi;
    cf32 prev;
    float fd[SPS];           /* últimas SPS salidas del discriminador */
    int fdi;
    float fsum;
    float sym[RING];         /* media de SPS muestras, indexada por n & (RING−1) */
    double mean;             /* umbral */
    int64_t n;
    uint64_t reg[SPS];       /* registros de decisiones por fase */
    uint64_t hdr;
    /* detección */
    int cand_left;
    int cand_best;
    int64_t cand_n;
    bool cand_inv;
    int64_t hold_until;
    /* captura */
    bool cap;
    bool inv;
    double pos;              /* instante (muestras absolutas) de la próxima decisión */
    int nbits;
    uint8_t rx[RS41_FRAME_LEN];
    unsigned long frames_ok, frames_fail, rs_fixed_frames;
    sonde_t sonde[MAX_SONDES];
    int nsonde;
    double t0;
    int64_t n_block0;
    sb_t sb;
} rs41_t;

static uint64_t header_reg(void)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 8; k++)   /* LSB primero */
            v = (v << 1) | ((RS41_HEADER_TX[i] >> k) & 1);
    return v;
}

static void *rs41_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    rs41_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->fs = cfg->rate > 0 ? cfg->rate : 48000.0;
    fir_design_lowpass(s->lp, LP_TAPS, LP_HZ / s->fs, kaiser_beta(50.0));
    s->hdr = header_reg();
    return s;
}

static void rs41_destroy(void *st)
{
    rs41_t *s = st;
    if (!s)
        return;
    sb_free(&s->sb);
    free(s);
}

/* ---- Tabla de sondas ---------------------------------------------------- */
static void update_sonde(rs41_t *s, const rs41_info_t *in, double now)
{
    sonde_t *d = NULL;
    for (int i = 0; i < s->nsonde; i++)
        if (!strcmp(s->sonde[i].serial, in->serial))
            d = &s->sonde[i];
    if (!d) {
        if (s->nsonde < MAX_SONDES)
            d = &s->sonde[s->nsonde++];
        else {
            d = &s->sonde[0];
            for (int i = 1; i < s->nsonde; i++)
                if (s->sonde[i].t_last < d->t_last)
                    d = &s->sonde[i];
        }
        memset(d, 0, sizeof *d);
        snprintf(d->serial, sizeof d->serial, "%s", in->serial);
    }
    bool keep_pos = d->has_pos && !in->pos_ok;
    rs41_info_t old = d->last;
    d->last = *in;
    if (keep_pos) {
        d->last.lat = old.lat;
        d->last.lon = old.lon;
        d->last.alt_m = old.alt_m;
        d->last.speed_ms = old.speed_ms;
        d->last.heading_deg = old.heading_deg;
        d->last.climb_ms = old.climb_ms;
        d->last.sats = old.sats;
    }
    d->has_pos = d->has_pos || in->pos_ok;
    d->t_last = now;
    d->frames++;
}

static void emit_frame(rs41_t *s, const rs41_info_t *in, int rs_fixed, double now, dec_emit_fn emit, void *ctx)
{
    char txt[200];
    int o = snprintf(txt, sizeof txt, "RS41 %s  trama %d", in->serial, in->frame_no);
    if (in->pos_ok && o < (int)sizeof txt)
        o += snprintf(txt + o, sizeof txt - (size_t)o, "  %.4f° %.4f°  %.0f m  %+.1f m/s", in->lat, in->lon, in->alt_m,
                      in->climb_ms);
    if (in->status_ok && o < (int)sizeof txt)
        snprintf(txt + o, sizeof txt - (size_t)o, "  %.1f V", in->batt_v);
    sb_t *b = &s->sb;
    sb_clear(b);
    sb_puts(b, "{\"serial\":");
    sb_json_str(b, in->serial);
    sb_printf(b, ",\"frame\":%d,\"battery_v\":", in->frame_no);
    sb_json_num(b, in->batt_v, 1);
    if (in->time_ok)
        sb_printf(b, ",\"gps_week\":%d,\"gps_tow_ms\":%u", in->gps_week, in->gps_tow_ms);
    if (in->pos_ok) {
        sb_puts(b, ",\"lat\":");
        sb_json_num(b, in->lat, 5);
        sb_puts(b, ",\"lon\":");
        sb_json_num(b, in->lon, 5);
        sb_puts(b, ",\"alt_m\":");
        sb_json_num(b, in->alt_m, 1);
        sb_puts(b, ",\"speed_ms\":");
        sb_json_num(b, in->speed_ms, 1);
        sb_puts(b, ",\"heading_deg\":");
        sb_json_num(b, in->heading_deg, 0);
        sb_puts(b, ",\"climb_ms\":");
        sb_json_num(b, in->climb_ms, 1);
        sb_printf(b, ",\"sats\":%d", in->sats);
    }
    if (in->ptu_ok) {
        sb_puts(b, ",\"ptu_raw\":[");
        for (int i = 0; i < 6; i++)
            sb_printf(b, "%s%u", i ? "," : "", in->ptu_raw[i]);
        sb_puts(b, "]");
    }
    sb_printf(b, ",\"blocks_ok\":%d,\"rs_fixed\":%d,\"text\":", in->nblocks_ok, rs_fixed);
    sb_json_str(b, txt);
    sb_puts(b, "}");
    emit(ctx, b->data, b->len);
    update_sonde(s, in, now);
}

/* ---- Trama completa recibida ------------------------------------------- */
static void frame_done(rs41_t *s, double now, dec_emit_fn emit, void *ctx)
{
    uint8_t plain[RS41_FRAME_LEN];
    memcpy(plain, s->rx, sizeof plain);
    rs41_scramble(plain, RS41_FRAME_LEN);
    rs41_info_t in;
    int rs_fixed = 0;
    rs41_parse(plain, RS41_FRAME_LEN, &in);
    bool good = in.status_ok && in.nblocks_ok >= 2;
    if (!good && plain[0x38] == 0x0F) {
        uint8_t fixed[RS41_FRAME_LEN];
        memcpy(fixed, plain, sizeof fixed);
        int r = rs41_rs_correct(fixed, RS41_FRAME_LEN);
        if (r > 0) {
            rs41_info_t in2;
            rs41_parse(fixed, RS41_FRAME_LEN, &in2);
            if (in2.status_ok && in2.nblocks_ok >= 2) {
                in = in2;
                rs_fixed = r;
                good = true;
                s->rs_fixed_frames++;
            }
        }
    }
    if (!good) {
        s->frames_fail++;
        return;
    }
    s->frames_ok++;
    emit_frame(s, &in, rs_fixed, now, emit, ctx);
}

static void rs41_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    rs41_t *s = st;
    s->t0 = now;
    s->n_block0 = s->n;
    for (int i = 0; i < n; i++, s->n++) {
        /* FIR paso bajo complejo */
        s->hist[s->hi] = iq[i];
        float yr = 0, yi = 0;
        for (int k = 0; k < LP_TAPS; k++) {
            const cf32 *h = &s->hist[(s->hi - k + LP_TAPS) % LP_TAPS];
            yr += s->lp[k] * h->re;
            yi += s->lp[k] * h->im;
        }
        s->hi = (s->hi + 1) % LP_TAPS;
        /* discriminador y media de SPS muestras */
        float re = yr * s->prev.re + yi * s->prev.im;
        float im = yi * s->prev.re - yr * s->prev.im;
        s->prev = (cf32){yr, yi};
        float f = atan2f(im, re);
        s->fsum += f - s->fd[s->fdi];
        s->fd[s->fdi] = f;
        s->fdi = (s->fdi + 1) % SPS;
        float sym = s->fsum / SPS;
        s->sym[s->n & (RING - 1)] = sym;
        s->mean += (f - s->mean) * (1.0 / 480.0);

        if (!s->cap) {
            /* búsqueda de cabecera en la fase actual */
            int ph = (int)(s->n % SPS);
            s->reg[ph] = (s->reg[ph] << 1) | (sym > s->mean);
            if (s->n < s->hold_until || s->n < 200)
                continue;
            int d = __builtin_popcountll(s->reg[ph] ^ s->hdr);
            int dinv = 64 - d;   /* ~reg ^ hdr */
            bool inv = dinv < d;
            if (inv)
                d = dinv;
            if (d <= HDR_MAX_ERR) {
                if (s->cand_left == 0) {
                    s->cand_left = SPS;
                    s->cand_best = 99;
                }
                if (d < s->cand_best) {
                    s->cand_best = d;
                    s->cand_n = s->n;
                    s->cand_inv = inv;
                }
            }
            if (s->cand_left > 0 && --s->cand_left == 0) {
                /* Se confirma la mejor fase y se arranca la captura. */
                s->cap = true;
                s->inv = s->cand_inv;
                s->pos = (double)s->cand_n + SPS;
                s->nbits = 0;
                memset(s->rx, 0, sizeof s->rx);
                for (int k = 0; k < RS41_HDR_LEN; k++)
                    s->rx[k] = RS41_HEADER_TX[k];
                s->nbits = RS41_HDR_LEN * 8;
            }
            continue;
        }
        /* captura: decide el bit cuando están disponibles las muestras tardías */
        while (s->cap) {
            int64_t c = (int64_t)floor(s->pos + 0.5);
            if (c + 2 > s->n)
                break;
            float v = s->sym[c & (RING - 1)] - (float)s->mean;
            float e = fabsf(s->sym[(c - 2) & (RING - 1)] - (float)s->mean);
            float l = fabsf(s->sym[(c + 2) & (RING - 1)] - (float)s->mean);
            int bit = (v > 0) ^ s->inv;
            int byte = s->nbits >> 3, k = s->nbits & 7;
            if (bit)
                s->rx[byte] |= (uint8_t)(1 << k);
            s->nbits++;
            s->pos += SPS + 0.1 * (l - e) / (l + e + 1e-6);
            if (s->nbits >= RS41_FRAME_LEN * 8) {
                double tnow = s->t0 + (double)(s->n - s->n_block0) / s->fs;
                frame_done(s, tnow, emit, ctx);
                s->cap = false;
                s->cand_left = 0;
                s->hold_until = s->n + (int64_t)(0.05 * s->fs);
                memset(s->reg, 0, sizeof s->reg);
            }
        }
    }
}

static void rs41_state_json(void *st, sb_t *sb, double now)
{
    rs41_t *s = st;
    int w = 0;
    for (int i = 0; i < s->nsonde; i++)
        if (now - s->sonde[i].t_last <= SONDE_TTL)
            s->sonde[w++] = s->sonde[i];
    s->nsonde = w;
    sb_printf(sb, "{\"frames_ok\":%lu,\"frames_bad\":%lu,\"rs_fixed\":%lu,\"sondes\":[", s->frames_ok, s->frames_fail,
              s->rs_fixed_frames);
    for (int i = 0; i < s->nsonde; i++) {
        const sonde_t *d = &s->sonde[i];
        if (i)
            sb_puts(sb, ",");
        sb_puts(sb, "{\"serial\":");
        sb_json_str(sb, d->serial);
        if (d->has_pos) {
            sb_puts(sb, ",\"lat\":");
            sb_json_num(sb, d->last.lat, 5);
            sb_puts(sb, ",\"lon\":");
            sb_json_num(sb, d->last.lon, 5);
            sb_puts(sb, ",\"alt_m\":");
            sb_json_num(sb, d->last.alt_m, 0);
            sb_puts(sb, ",\"speed_ms\":");
            sb_json_num(sb, d->last.speed_ms, 1);
            sb_puts(sb, ",\"climb_ms\":");
            sb_json_num(sb, d->last.climb_ms, 1);
        }
        sb_puts(sb, ",\"battery_v\":");
        sb_json_num(sb, d->last.batt_v, 1);
        sb_printf(sb, ",\"frame\":%d,\"frames\":%u,\"age_s\":", d->last.frame_no, d->frames);
        sb_json_num(sb, now - d->t_last, 0);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_RS41 = {
    .id = "rs41",
    .name = "Radiosonda RS41",
    .group = "Meteorología",
    .desc = "Radiosonda Vaisala RS41 (GFSK 4800): posición, altura, PTU.",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 12000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {403000000LL, 0LL, 0LL, 0LL},
    .create = rs41_create,
    .destroy = rs41_destroy,
    .process = rs41_process,
    .state_json = rs41_state_json,
};
