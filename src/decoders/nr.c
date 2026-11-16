/*
 * nr.c — Búsqueda de SSB de 5G NR (PSS/SSS → PCI) en la rejilla GSCN. Ver docs/decoders/nr.md.
 *
 * Teoría (3GPP TS 38.211 §7.4.2, TS 38.104 §5.4.3, TS 38.213 §4.1):
 *  - El bloque SS/PBCH (SSB) ocupa 4 símbolos OFDM × 240 subportadoras. La PSS
 *    (símbolo 0) es una m-secuencia BPSK de 127 elementos con 3 versiones (N_ID2), la
 *    SSS (símbolo 2) un código Gold de 127 con 336 versiones (N_ID1): PCI = 3·N_ID1 +
 *    N_ID2 ∈ 0..1007. Ambas ocupan 127 subportadoras = 3,81 MHz (SCS 30 kHz) o
 *    1,90 MHz (SCS 15 kHz) alrededor de SS_REF, la frecuencia del GSCN.
 *  - Los SSB solo están en la rejilla de sincronía global (GSCN, múltiplos de
 *    1,44 MHz sobre 3 GHz; 1,2 MHz + {50,150,250 kHz} bajo 3 GHz), así que en una
 *    captura de 15,36–61,44 Msps solo hay que probar ≈ 7–40 posiciones.
 *  - Cadena por cada posición candidata f_g (nr_gscn_candidates):
 *      1. Se mezcla a banda base y se diezma a 7,68 Msps (÷ fs/7,68 M, FIR de ventana
 *         de Kaiser: paso ±2,4 MHz) → el canal "SSB" de 3,84 MHz de semianchura.
 *      2. nr_corr_run(): correlación normalizada por FFT con 6 plantillas (PSS de
 *         N_ID2 = 0,1,2 × SCS 30 y 15 kHz). El estadístico es R = pico/mediana de la
 *         NCC; en ruido la mediana es 0,69× la media y el máximo de 2·10^5 muestras
 *         llega a ≈ 17× la mediana, por eso el umbral es R ≥ 45.
 *      3. CFO grueso: barrido ±15 kHz de la correlación con la plantilla rotada.
 *      4. SSS: FFT de 256/512 puntos de los símbolos 0 y 2 (separados 2·(N+CP)),
 *         equalización con el canal medido en la PSS y correlación real con las 336
 *         hipótesis (valida la celda: umbral 0,45, el ruido da ≈ 0,27 como máximo).
 *      5. CFO fino: la fase de Σ H_sss·conj(H_pss) entre ambos símbolos (separados
 *         2(N+CP)/fs) da la fracción restante; el grueso resuelve la ambigüedad.
 *      6. Potencia: la media de |Y_sss|²/N² menos el ruido (percentil bajo de la
 *         energía por bloques) = RSRP aproximada (dBFS por RE) y SNR.
 *  - El coste está dominado por nr_corr_run (≈ 7 FFT de 8192 por 7,7 k muestras y
 *    posición): es la función que C1 llevará a GPU (nr_corr.h).
 *
 * Funcionamiento: se analiza una ventana de 25 ms (cabe una ráfaga completa de 5 ms con
 * periodicidad de 20 ms) cada 'period' s (opción {"period":s}; por defecto max(2, 0,4·nº de posiciones GSCN) s).
 * Tasas admitidas: múltiplos enteros de 7,68 Msps (15,36 … 61,44).
 *
 * Limitaciones: sin PBCH (polar), sin índice de SSB ni SFN; SSB de periodicidad > 20 ms
 * pueden perderse en una ventana; solo SCS 15/30 kHz (FR1).
 */
#include "decoder.h"

#include "nr_corr.h"
#include "nr_phy.h"
#include "../dsp/fft.h"
#include "../dsp/filter.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS_D 7680000.0
#define WIN_S 0.025
#define OSN 8192
#define MAXCELLS 32
#define MAXCAND 64
#define R_THR 45.0
#define SSS_THR 0.45f
#define CELL_TTL 120.0

typedef struct {
    int pci, scs_khz;
    int64_t freq_hz;
    double cfo_hz, rsrp_dbfs, snr_db, score, r;
    double first_seen, last_seen;
    int hits;
} nr_cell_t;

typedef struct nr {
    double fs, period, eff_period;
    int dec, nwin, nc, nout;
    int64_t center;
    cf32 *win, *xc, *mix;
    float *cout, *tmp;
    int fill;
    int64_t skip;
    nr_corr_t *corr;
    cf32 tmpl[6][512];
    fft_plan_t *plan[2];     /* 256 y 512 */
    int8_t pss_d[3][127];
    int8_t (*sss)[336][127]; /* [3][336][127] */
    nr_cell_t cells[MAXCELLS];
    int ncells;
    unsigned long n_windows, n_events;
    double last_now;
    sb_t evsb;
} nr_t;

static inline float cabs2(cf32 a) { return a.re * a.re + a.im * a.im; }
static inline cf32 cmulc(cf32 a, cf32 b) { return (cf32){a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im}; }

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Mezcla f_g a 0 Hz y diezma a 7,68 Msps toda la ventana. Devuelve nº de muestras. */
static int channelize(nr_t *st, double df_hz, cf32 *out)
{
    firdec_t fd;
    if (firdec_init(&fd, st->dec, st->fs, 2.4e6, 255) < 0)
        return 0;
    double w = -2.0 * M_PI * df_hz / st->fs;
    double cr = 1, ci = 0, dr = cos(w), di = sin(w);
    int total = 0;
    for (int off = 0; off < st->nwin; off += 4096) {
        int n = MIN(4096, st->nwin - off);
        const cf32 *x = st->win + off;
        for (int i = 0; i < n; i++) {
            st->mix[i].re = (float)(x[i].re * cr - x[i].im * ci);
            st->mix[i].im = (float)(x[i].re * ci + x[i].im * cr);
            double nr_ = cr * dr - ci * di, ni = cr * di + ci * dr;
            cr = nr_;
            ci = ni;
        }
        double m = 1.0 / sqrt(cr * cr + ci * ci);
        cr *= m;
        ci *= m;
        total += firdec_process(&fd, st->mix, n, out + total);
    }
    firdec_free(&fd);
    return total;
}

static double noise_power(const cf32 *x, int n)
{
    int nb = n / 256;
    if (nb < 8)
        return 1e-12;
    float *e = malloc(sizeof(float) * (size_t)nb);
    if (!e)
        return 1e-12;
    for (int b = 0; b < nb; b++) {
        double s = 0;
        for (int i = 0; i < 256; i++)
            s += cabs2(x[b * 256 + i]);
        e[b] = (float)(s / 256.0);
    }
    qsort(e, (size_t)nb, sizeof(float), cmpf);
    double v = e[nb / 5] / 0.95;     /* percentil 20 de una chi² de 512 g.l. ≈ 0,95·media */
    free(e);
    return v > 1e-14 ? v : 1e-14;
}

/* ---------------------------------------------------------------------- */
static void cell_json(const nr_cell_t *c, sb_t *sb, bool with_text, double now)
{
    sb_puts(sb, "{");
    if (with_text) {
        char t[200];
        snprintf(t, sizeof t, "5G NR PCI %d (N_ID1 %d, N_ID2 %d) en %.2f MHz, SCS %d kHz, CFO %+.2f kHz, RSRP %.1f dBFS, SNR %.1f dB",
                 c->pci, c->pci / 3, c->pci % 3, (double)c->freq_hz / 1e6, c->scs_khz, c->cfo_hz / 1e3, c->rsrp_dbfs,
                 c->snr_db);
        sb_puts(sb, "\"text\":");
        sb_json_str(sb, t);
        sb_puts(sb, ",");
    }
    sb_printf(sb, "\"pci\":%d,\"n_id1\":%d,\"n_id2\":%d,\"freq_hz\":%lld,\"scs_khz\":%d,", c->pci, c->pci / 3,
              c->pci % 3, (long long)c->freq_hz, c->scs_khz);
    sb_puts(sb, "\"cfo_hz\":");
    sb_json_num(sb, c->cfo_hz, 0);
    sb_puts(sb, ",\"rsrp_dbfs\":");
    sb_json_num(sb, c->rsrp_dbfs, 1);
    sb_puts(sb, ",\"snr_db\":");
    sb_json_num(sb, c->snr_db, 1);
    sb_puts(sb, ",\"sss_corr\":");
    sb_json_num(sb, c->score, 2);
    sb_printf(sb, ",\"hits\":%d", c->hits);
    if (!with_text) {
        sb_puts(sb, ",\"age\":");
        sb_json_num(sb, now - c->last_seen, 1);
    }
    sb_puts(sb, "}");
}

static void cell_update(nr_t *st, const nr_cell_t *cand, double now, dec_emit_fn emit, void *ctx)
{
    nr_cell_t *c = NULL;
    for (int i = 0; i < st->ncells; i++)
        if (st->cells[i].pci == cand->pci && st->cells[i].freq_hz == cand->freq_hz)
            c = &st->cells[i];
    bool changed = false;
    int hits = 0;
    double first = now;
    if (!c) {
        if (st->ncells == MAXCELLS) {
            int o = 0;
            for (int i = 1; i < st->ncells; i++)
                if (st->cells[i].last_seen < st->cells[o].last_seen)
                    o = i;
            c = &st->cells[o];
        } else {
            c = &st->cells[st->ncells++];
        }
        changed = true;
    } else {
        hits = c->hits;
        first = c->first_seen;
        changed = c->scs_khz != cand->scs_khz;
    }
    *c = *cand;
    c->hits = hits + 1;
    c->first_seen = first;
    c->last_seen = now;
    if (changed) {
        st->evsb.len = 0;
        cell_json(c, &st->evsb, true, now);
        st->n_events++;
        emit(ctx, st->evsb.data, st->evsb.len);
    }
}

/* ---------------------------------------------------------------------- */
/* Verifica un pico de PSS: SSS, CFO, potencia. */
static bool verify_ssb(nr_t *st, int64_t f_g, int ti, int np, double Rv, nr_cell_t *out)
{
    int scs = ti < 3 ? 30 : 15, n2 = ti % 3;
    int N = (int)lround(FS_D / (scs * 1000.0));
    int cp = (int)lround(N * 9.0 / 128.0);
    int bias = cp / 2;
    int t_sss = 2 * (N + cp);
    if (np - bias < 0 || np - bias + t_sss + N > st->nc)
        return false;
    const cf32 *x = st->xc;
    const cf32 *tp = st->tmpl[ti];
    /* CFO grueso: máximo de |Σ x·conj(p)·e^{−j2πfk/fs}|² */
    double sc[13], bs = -1, bf = 0;
    int bi = 0;
    for (int fi = 0; fi < 13; fi++) {
        double f = (fi - 6) * 2500.0, w = -2.0 * M_PI * f / FS_D, re = 0, im = 0;
        for (int k = 0; k < N; k++) {
            double a = w * k;
            double c = cos(a), s = sin(a);
            double pr = x[np + k].re * tp[k].re + x[np + k].im * tp[k].im;
            double pi_ = x[np + k].im * tp[k].re - x[np + k].re * tp[k].im;
            re += pr * c - pi_ * s;
            im += pr * s + pi_ * c;
        }
        sc[fi] = re * re + im * im;
        if (sc[fi] > bs) {
            bs = sc[fi];
            bf = f;
            bi = fi;
        }
    }
    if (bi > 0 && bi < 12) {
        double dd = sc[bi - 1] - 2 * sc[bi] + sc[bi + 1];
        if (dd < 0)
            bf += 2500.0 * 0.5 * (sc[bi - 1] - sc[bi + 1]) / dd;
    }
    /* segmento derotado: símbolo 0 (PSS) y símbolo 2 (SSS) */
    int plan = scs == 30 ? 0 : 1;
    cf32 Yp[512], Ys[512];
    double w = -2.0 * M_PI * bf / FS_D;
    for (int i = 0; i < N; i++) {
        double a = w * i;
        cf32 r = {(float)cos(a), (float)sin(a)};
        cf32 v = x[np - bias + i];
        Yp[i] = (cf32){v.re * r.re - v.im * r.im, v.re * r.im + v.im * r.re};
        a = w * (i + t_sss);
        r = (cf32){(float)cos(a), (float)sin(a)};
        v = x[np - bias + t_sss + i];
        Ys[i] = (cf32){v.re * r.re - v.im * r.im, v.re * r.im + v.im * r.re};
    }
    fft_exec(st->plan[plan], Yp);
    fft_exec(st->plan[plan], Ys);
    cf32 Hp[127], Z[127];
    double nrm = 0;
    for (int n = 0; n < 127; n++) {
        int f = n - 64, b = f >= 0 ? f : N + f;
        Hp[n] = (cf32){Yp[b].re * st->pss_d[n2][n], Yp[b].im * st->pss_d[n2][n]};
        Z[n] = cmulc(Ys[b], Hp[n]);
        nrm += cabs2(Z[n]);
    }
    nrm = sqrt(nrm * 127.0) + 1e-20;
    float best = -2;
    int bn1 = 0;
    for (int n1 = 0; n1 < 336; n1++) {
        const int8_t *d = st->sss[n2][n1];
        float c = 0;
        for (int n = 0; n < 127; n++)
            c += Z[n].re * (float)d[n];
        c = (float)(c / nrm);
        if (c > best) {
            best = c;
            bn1 = n1;
        }
    }
    if (best < SSS_THR)
        return false;
    /* CFO fino por la fase entre los dos símbolos */
    cf32 P = {0, 0};
    double ps = 0;
    for (int n = 0; n < 127; n++) {
        int f = n - 64, b = f >= 0 ? f : N + f;
        float d = (float)st->sss[n2][bn1][n];
        cf32 hs = {Ys[b].re * d, Ys[b].im * d};
        cf32 t = cmulc(hs, Hp[n]);
        P.re += t.re;
        P.im += t.im;
        ps += cabs2(Ys[b]);
    }
    double dt = (double)t_sss / FS_D;
    double f_res = atan2(P.im, P.re) / (2.0 * M_PI * dt);
    /* potencia */
    double nz = noise_power(st->xc, st->nc);
    double pw = ps / 127.0 / ((double)N * N), nbin = nz / N;
    double sig = fmax(pw - nbin, 1e-3 * nbin);
    memset(out, 0, sizeof *out);
    out->pci = 3 * bn1 + n2;
    out->scs_khz = scs;
    out->freq_hz = f_g;
    out->cfo_hz = bf + f_res;
    out->rsrp_dbfs = 10.0 * log10(sig + 1e-30);
    out->snr_db = 10.0 * log10(sig / nbin);
    out->score = best;
    out->r = Rv;
    return true;
}

static void analyze(nr_t *st, double now, dec_emit_fn emit, void *ctx)
{
    st->n_windows++;
    int64_t cand[MAXCAND];
    double half = st->fs / 2 - 2.6e6;
    int ncand = nr_gscn_candidates((double)st->center - half, (double)st->center + half, cand, MAXCAND);
    /* ≈ 57 ms de CPU por posición y ventana: el periodo automático mantiene ≈ 15 % de un núcleo */
    st->eff_period = st->period >= 0 ? st->period : fmax(2.0, 0.4 * ncand);
    for (int ci = 0; ci < ncand; ci++) {
        st->nc = channelize(st, (double)(cand[ci] - st->center), st->xc);
        int nout = nr_corr_run(st->corr, st->xc, st->nc, st->cout);
        if (nout < 1000)
            continue;
        int nsub = nout / 5;
        float *tmp = st->tmp;
        double bestR = 0;
        int bt = 0, bidx = 0;
        for (int t = 0; t < 6; t++) {
            const float *o = st->cout + (size_t)t * nout;
            float pk = 0;
            int pi_ = 0;
            for (int i = 0; i < nout; i++)
                if (o[i] > pk) {
                    pk = o[i];
                    pi_ = i;
                }
            for (int i = 0; i < nsub; i++)
                tmp[i] = o[i * 5];
            qsort(tmp, (size_t)nsub, sizeof(float), cmpf);
            double med = tmp[nsub / 2] + 1e-12;
            double R = pk / med;
            if (R > bestR) {
                bestR = R;
                bt = t;
                bidx = pi_;
            }
        }
        if (bestR < R_THR)
            continue;
        nr_cell_t c;
        if (verify_ssb(st, cand[ci], bt, bidx, bestR, &c))
            cell_update(st, &c, now, emit, ctx);
    }
}

/* ---------------------------------------------------------------------- */
static void nr_destroy(void *p)
{
    nr_t *st = p;
    if (!st)
        return;
    free(st->win);
    free(st->xc);
    free(st->mix);
    free(st->tmp);
    free(st->cout);
    free(st->sss);
    nr_corr_destroy(st->corr);
    fft_plan_destroy(st->plan[0]);
    fft_plan_destroy(st->plan[1]);
    sb_free(&st->evsb);
    free(st);
}

static void *nr_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    double ratio = cfg->rate / FS_D;
    int dec = (int)lround(ratio);
    if (dec < 2 || dec > 8 || fabs(ratio - dec) > 1e-4) {
        snprintf(err, errlen, "NR requiere una tasa múltiplo de 7,68 Msps entre 15,36 y 61,44 Msps");
        return NULL;
    }
    nr_t *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    st->fs = cfg->rate;
    st->dec = dec;
    st->center = cfg->center_hz;
    st->period = -1.0;      /* automático: proporcional al nº de posiciones GSCN */
    if (cfg->opts && *cfg->opts) {
        jtok_t t[16];
        int nt = json_parse(cfg->opts, strlen(cfg->opts), t, 16);
        double v;
        int i;
        if (nt > 0 && t[0].type == J_OBJ && (i = json_obj_get(cfg->opts, t, 0, "period")) >= 0 &&
            json_num(cfg->opts, t, i, &v))
            st->period = CLAMP(v, 0.0, 600.0);
    }
    st->nwin = dec * (int)(WIN_S * FS_D);
    int ncmax = st->nwin / dec + 64;
    st->win = malloc(sizeof(cf32) * (size_t)st->nwin);
    st->xc = malloc(sizeof(cf32) * (size_t)ncmax);
    st->mix = malloc(sizeof(cf32) * 4096);
    st->tmp = malloc(sizeof(float) * (size_t)(ncmax / 5 + 8));
    st->cout = malloc(sizeof(float) * 6 * (size_t)ncmax);
    st->sss = malloc(sizeof(int8_t) * 3 * 336 * 127);
    st->plan[0] = fft_plan_create(256);
    st->plan[1] = fft_plan_create(512);
    if (!st->win || !st->xc || !st->mix || !st->tmp || !st->cout || !st->sss || !st->plan[0] || !st->plan[1]) {
        nr_destroy(st);
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    for (int n2 = 0; n2 < 3; n2++) {
        nr_pss_seq(n2, st->pss_d[n2]);
        for (int n1 = 0; n1 < 336; n1++)
            nr_sss_seq(n1, n2, st->sss[n2][n1]);
    }
    /* plantillas temporales: 0..2 = SCS 30 kHz, 3..5 = SCS 15 kHz */
    const cf32 *tp[6];
    int tl[6];
    for (int t = 0; t < 6; t++) {
        int scs = t < 3 ? 30 : 15, N = (int)lround(FS_D / (scs * 1000.0)), n2 = t % 3;
        double e = 0;
        for (int k = 0; k < N; k++) {
            double re = 0, im = 0;
            for (int n = 0; n < 127; n++) {
                int f = n - 64;
                double a = 2.0 * M_PI * f * k / N;
                re += st->pss_d[n2][n] * cos(a);
                im += st->pss_d[n2][n] * sin(a);
            }
            st->tmpl[t][k] = (cf32){(float)re, (float)im};
            e += re * re + im * im;
        }
        float s = (float)(1.0 / sqrt(e));
        for (int k = 0; k < N; k++) {
            st->tmpl[t][k].re *= s;
            st->tmpl[t][k].im *= s;
        }
        tp[t] = st->tmpl[t];
        tl[t] = N;
    }
    st->corr = nr_corr_create(OSN, 6, tp, tl);
    if (!st->corr) {
        nr_destroy(st);
        snprintf(err, errlen, "no se pudo crear el correlador");
        return NULL;
    }
    return st;
}

static void nr_process_raw(void *p, const int16_t *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    nr_t *st = p;
    st->last_now = now;
    int i = 0;
    const float sc = 1.0f / 2048.0f;
    while (i < n) {
        if (st->skip > 0) {
            int64_t k = MIN((int64_t)(n - i), st->skip);
            st->skip -= k;
            i += (int)k;
            continue;
        }
        int k = MIN(n - i, st->nwin - st->fill);
        for (int j = 0; j < k; j++) {
            st->win[st->fill + j].re = iq[2 * (i + j)] * sc;
            st->win[st->fill + j].im = iq[2 * (i + j) + 1] * sc;
        }
        st->fill += k;
        i += k;
        if (st->fill == st->nwin) {
            analyze(st, now + (double)i / st->fs, emit, ctx);
            st->fill = 0;
            st->skip = (int64_t)(st->eff_period * st->fs);
        }
    }
}

static void nr_state(void *p, sb_t *sb, double now)
{
    nr_t *st = p;
    sb_printf(sb, "{\"windows\":%lu,\"cells\":[", st->n_windows);
    bool first = true;
    for (int i = 0; i < st->ncells; i++) {
        if (now - st->cells[i].last_seen > CELL_TTL)
            continue;
        if (!first)
            sb_puts(sb, ",");
        first = false;
        cell_json(&st->cells[i], sb, false, now);
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_NR = {
    .id = "nr",
    .name = "5G NR",
    .group = "Celular",
    .desc = "Búsqueda de SSB de 5G NR (PSS/SSS → PCI) en la rejilla GSCN.",
    .kind = DEC_WIDE,
    .rate = 0,
    .bw_hz = 0,
    .wide_rate = 0,
    .wide_min_rate = 15360000,
    .freqs = {0LL, 0LL, 0LL, 0LL},
    .create = nr_create,
    .destroy = nr_destroy,
    .process_raw = nr_process_raw,
    .state_json = nr_state,
};
