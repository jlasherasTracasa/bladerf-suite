/*
 * dvbt.c — Decodificador DVB-T (ETSI EN 300 744): modo, guarda y señalización TPS.
 *
 * Narrow a 64/7 MHz = 9 142 857,14 Hz (T = 7/64 µs, canal de 8 MHz). Solo lee
 * información de sistema (parámetros del múltiplex); no decodifica el transporte.
 *
 * Funcionamiento por RÁFAGAS DE TIEMPO (para no pasar de ~0,05 s de CPU por
 * segundo de señal): se acumula un segmento de 1,5 Msps (≈ 164 ms: 140+ símbolos,
 * dos tramas TPS de 68 símbolos) y se analiza; después se ignora la entrada
 * durante 0,5 s (sin enganche) o 3 s (con múltiplex ya decodificado).
 *
 * Cadena DSP de cada segmento:
 *  1. Modo e intervalo de guarda por correlación del prefijo cíclico (van de
 *     Beek, Sandell, Borjesson 1997): para N ∈ {2048, 8192} se calcula el
 *     coeficiente de correlación normalizado
 *         ρ(i) = |Σ_{j<N/32} x[i+j]·x*[i+j+N]| / √(E₁·E₂)
 *     con ventana deslizante (N/32 = el CP más corto); se pliega módulo cada
 *     periodo de símbolo candidato Ts = N(1 + 1/g), g ∈ {32,16,8,4}. Solo el Ts
 *     verdadero alinea los picos de todos los símbolos: se elige el de mayor
 *     media del pliegue (>0,3) y contraste (pico/media > 1,5). El centro de la
 *     meseta da el inicio del CP.
 *  2. CFO fraccional: f = −arg(Σ_CP x[n]·x*[n+N])·fs/(2πN) (|f| < Δf/2).
 *  3. FFT de los primeros 16 símbolos (ventana 1/8 de guarda antes del final del
 *     CP, para tolerar eco). CFO ENTERO: los pilotos continuos son constantes de
 *     un símbolo al siguiente, así que para cada desplazamiento s ∈ ±N/16 se
 *     suma Σ_k Y_l[k+s]·Y*_{l−1}[k+s] sobre los pilotos continuos de la norma y se
 *     toma el máximo (no hace falta la PRBS). La pendiente de fase entre grupos
 *     de pilotos da la deriva de reloj ε = −b·N/(2π·Ts); si |ε| > 20 ppm se
 *     remuestrea el segmento (Catmull-Rom) y se refina la sincronía.
 *  4. FFT de todos los símbolos y TPS: para cada par de símbolos la diferencia
 *     de fase común a e inclinación b (error de fase común y de temporización)
 *     se mide con los pilotos continuos; luego cada portadora TPS aporta
 *     Re(Y_l·Y*_{l−1}·e^{−j(a+bκ)}); la suma sobre las 17 (2k) o 68 (8k)
 *     portadoras da el bit DBPSK (>0 → 0, <0 → 1) sin necesidad de canal.
 *  5. Palabra de sincronía de 16 bits (con hasta 4 errores) → trama; BCH(67,53)
 *     corrige hasta 2 bits; indicador de longitud 23/31; campos de parámetros.
 *  6. Ecualización con pilotos dispersos (k mod 12 = 3·(l mod 4), PRBS de
 *     x^11+x^2+1): H interpolado linealmente entre pilotos del propio símbolo;
 *     se mide el MER sobre los pilotos continuos que no son dispersos en ese
 *     símbolo. Solo es diagnóstico (mer_db): TPS no depende de la PRBS.
 *
 * Un múltiplex genera un evento al verse por primera vez o al cambiar sus
 * parámetros (o completarse su cell id). state_json lista los múltiplex vistos
 * (caducan a los 600 s). air_ok=false.
 *
 * RIESGOS (sin señal real ni vectores de la norma para contrastar): tablas de
 * pilotos continuos y TPS (derivadas de la 2k + estructura 1704, recordadas de la
 * norma y coherentes con los totales 45/177 y 17/68), PRBS y orden de bits del
 * cell id, orden de los bits de paridad BCH (se prueba también el inverso y se
 * indica en "bch"). Ver docs/decoders/dvbt.md.
 */
#include "decoder.h"

#include "dvbt_phy.h"
#include "../dsp/fft.h"
#include "../util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DVBT_DEBUG
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif
#define SEG_N 1500000
#define MAX_SYM 145
#define Q1 16                 /* símbolos de la 1.ª pasada */
#define NMAX 8192
#define IDLE_SEARCH_S 0.5
#define IDLE_LOCKED_S 3.0
#define IDLE_CELL_S 0.35     /* hasta tener los dos bytes del cell id (llegan en tramas alternas) */
#define MUX_TTL 600.0
#define MAX_MUX 8

typedef struct {
    bool used;
    int n, guard, constellation, hierarchy, hp, lp;
    int cell_byte[2], have_byte[2];
    int frame, length, bch_alt;
    double cfo_hz, mer_db, level_dbfs, rho, first, last;
    int locks;
} mux_t;

typedef struct {
    double fs;
    int64_t freq_hz;
    cf32 *seg, *work, *pass1, *spec;
    float *rho, *fold, *soft;
    int fill;
    int64_t skip;
    fft_plan_t *plan[2];
    mux_t mux[MAX_MUX];
    double t_now;
    bool locked;
    bool need_cell;      /* múltiplex con cell id aún incompleto: reintento rápido */
    uint64_t scans, cp_found, tps_ok;
    double last_rho;
    dec_emit_fn emit;
    void *ctx;
} dvbt_t;

static void *dvbt_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - DVBT_FS) > 2.0) {
        snprintf(err, errlen, "requiere 9 142 857 Hz (64/7 MHz)");
        return NULL;
    }
    dvbt_t *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->fs = cfg->rate;
    w->freq_hz = cfg->freq_hz;
    w->seg = malloc(sizeof(cf32) * (SEG_N + 4096));
    w->work = malloc(sizeof(cf32) * (SEG_N + 4096));
    w->rho = malloc(sizeof(float) * (SEG_N + 4096));
    w->fold = malloc(sizeof(float) * (NMAX + NMAX / 4 + 8));
    w->pass1 = malloc(sizeof(cf32) * (size_t)Q1 * NMAX);
    w->spec = malloc(sizeof(cf32) * (size_t)MAX_SYM * NMAX);
    w->soft = malloc(sizeof(float) * (MAX_SYM + 8));
    w->plan[0] = fft_plan_create(2048);
    w->plan[1] = fft_plan_create(8192);
    if (!w->seg || !w->work || !w->rho || !w->fold || !w->pass1 || !w->spec || !w->soft || !w->plan[0] ||
        !w->plan[1]) {
        snprintf(err, errlen, "sin memoria");
        free(w->seg); free(w->work); free(w->rho); free(w->fold); free(w->pass1); free(w->spec); free(w->soft);
        if (w->plan[0]) fft_plan_destroy(w->plan[0]);
        if (w->plan[1]) fft_plan_destroy(w->plan[1]);
        free(w);
        return NULL;
    }
    return w;
}

static void dvbt_destroy(void *st)
{
    dvbt_t *w = st;
    if (!w)
        return;
    free(w->seg); free(w->work); free(w->rho); free(w->fold); free(w->pass1); free(w->spec); free(w->soft);
    fft_plan_destroy(w->plan[0]);
    fft_plan_destroy(w->plan[1]);
    free(w);
}

/* ======================================================================== */
/*  1. Modo y guarda por correlación del CP                                 */
/* ======================================================================== */
/* ρ(i) para i = 0…nr−1 con nr = n − N − Lw + 1. */
static int compute_rho(const cf32 *x, int n, int N, int Lw, float *rho)
{
    int nr = n - N - Lw + 1;
    double sr = 0, si = 0, e1 = 0, e2 = 0;
    for (int j = 0; j < Lw; j++) {
        const cf32 a = x[j], b = x[j + N];
        sr += (double)a.re * b.re + (double)a.im * b.im;
        si += (double)a.im * b.re - (double)a.re * b.im;
        e1 += (double)a.re * a.re + (double)a.im * a.im;
        e2 += (double)b.re * b.re + (double)b.im * b.im;
    }
    for (int i = 0; i < nr; i++) {
        rho[i] = (float)sqrt((sr * sr + si * si) / (e1 * e2 + 1e-30));
        if (i + 1 < nr) {
            const cf32 a0 = x[i], b0 = x[i + N], a1 = x[i + Lw], b1 = x[i + Lw + N];
            sr += ((double)a1.re * b1.re + (double)a1.im * b1.im) - ((double)a0.re * b0.re + (double)a0.im * b0.im);
            si += ((double)a1.im * b1.re - (double)a1.re * b1.im) - ((double)a0.im * b0.re - (double)a0.re * b0.im);
            e1 += ((double)a1.re * a1.re + (double)a1.im * a1.im) - ((double)a0.re * a0.re + (double)a0.im * a0.im);
            e2 += ((double)b1.re * b1.re + (double)b1.im * b1.im) - ((double)b0.re * b0.re + (double)b0.im * b0.im);
        }
    }
    return nr;
}

typedef struct {
    int N, L, g, cp_start, M;
    float score, contrast;
} det_t;

/* Pliega ρ módulo Ts. Devuelve media del pico, contraste y centro de la meseta. */
static bool fold_rho(dvbt_t *w, const float *rho, int nr, int Ts, int Lw, int L, det_t *d)
{
    int M = nr / Ts;
    if (M < 20)
        return false;
    float *F = w->fold;
    memset(F, 0, sizeof(float) * (size_t)Ts);
    for (int m = 0; m < M; m++) {
        const float *r = rho + (size_t)m * Ts;
        for (int t = 0; t < Ts; t++)
            F[t] += r[t];
    }
    float pk = 0, sum = 0;
    int ia = 0;
    for (int t = 0; t < Ts; t++) {
        sum += F[t];
        if (F[t] > pk) {
            pk = F[t];
            ia = t;
        }
    }
    int lo = ia, hi = ia;
    while (lo > ia - Ts / 2 && F[((lo - 1) % Ts + Ts) % Ts] >= 0.9f * pk)
        lo--;
    while (hi < ia + Ts / 2 && F[(hi + 1) % Ts] >= 0.9f * pk)
        hi++;
    int center = (((lo + hi) / 2) % Ts + Ts) % Ts;
    d->cp_start = (((center - (L - Lw) / 2) % Ts) + Ts) % Ts;
    d->M = M;
    d->score = pk / (float)M;
    d->contrast = pk / (sum / (float)Ts + 1e-12f);
    return true;
}

static bool detect(dvbt_t *w, const cf32 *x, int n, det_t *best)
{
    best->score = 0;
    for (int ni = 0; ni < 2; ni++) {
        int N = ni ? 8192 : 2048, Lw = N / 32;
        int nr = compute_rho(x, n, N, Lw, w->rho);
        for (int g = 0; g < 4; g++) {
            int L = N / dvbt_guard_div(g), Ts = N + L;
            det_t d = {.N = N, .L = L, .g = g};
            if (!fold_rho(w, w->rho, nr, Ts, Lw, L, &d))
                continue;
            if (d.score > best->score && d.contrast > 1.5f)
                *best = d;
        }
    }
    w->last_rho = best->score;
    return best->score > 0.30f;
}

/* ======================================================================== */
/*  Utilidades                                                              */
/* ======================================================================== */
static void rotate(const cf32 *x, cf32 *y, int n, double f, double fs)
{
    double wphi = -2.0 * M_PI * f / fs;
    cf32 st = {(float)cos(wphi), (float)sin(wphi)}, ph = {1, 0};
    for (int i = 0; i < n; i++) {
        y[i] = (cf32){x[i].re * ph.re - x[i].im * ph.im, x[i].re * ph.im + x[i].im * ph.re};
        float pr = ph.re * st.re - ph.im * st.im, pi = ph.re * st.im + ph.im * st.re;
        ph = (cf32){pr, pi};
        if ((i & 1023) == 1023) {
            float m = 1.0f / sqrtf(ph.re * ph.re + ph.im * ph.im);
            ph.re *= m;
            ph.im *= m;
        }
    }
}

static int resample_cr(const cf32 *in, int n, double ratio, cf32 *out)
{
    int m = (int)((double)(n - 4) * ratio);
    if (m > n + 2048)
        m = n + 2048; /* capacidad de los búferes: SEG_N + 4096 */
    for (int k = 0; k < m; k++) {
        double pos = (double)k / ratio + 1.0;
        int i = (int)pos;
        float f = (float)(pos - i);
        const cf32 *p0 = &in[i - 1], *p1 = &in[i], *p2 = &in[i + 1], *p3 = &in[i + 2];
        float w0 = -0.5f * f * (1 - f) * (1 - f), w1 = 1 - 2.5f * f * f + 1.5f * f * f * f,
              w2 = 0.5f * f * (1 + 4 * f - 3 * f * f), w3 = -0.5f * f * f * (1 - f);
        out[k].re = w0 * p0->re + w1 * p1->re + w2 * p2->re + w3 * p3->re;
        out[k].im = w0 * p0->im + w1 * p1->im + w2 * p2->im + w3 * p3->im;
    }
    return m;
}

/* γ = Σ_símbolos Σ_{CP interior} x[n]·x*[n+N]; devuelve el nº de símbolos usados. */
static int cp_gamma(const cf32 *x, int n, const det_t *d, int cp_start, double *gr, double *gi)
{
    int Ts = d->N + d->L, cnt = 0;
    double sr = 0, si = 0;
    for (int m = 0;; m++) {
        int c = cp_start + m * Ts;
        if (c + d->N + d->L > n)
            break;
        for (int i = d->L / 8; i < d->L - d->L / 8; i++) {
            const cf32 a = x[c + i], b = x[c + i + d->N];
            sr += (double)a.re * b.re + (double)a.im * b.im;
            si += (double)a.im * b.re - (double)a.re * b.im;
        }
        cnt++;
    }
    *gr = sr;
    *gi = si;
    return cnt;
}

/* Fase común a y pendiente b (rad por portadora) de Y_l·Y*_{l-1} sobre los pilotos continuos. */
static void pair_phase(const cf32 *cur, const cf32 *prev, const int *bins, const int *kc, int ncp, double *a_out,
                       double *b_out, double *mag_out)
{
    cf32 S[8];
    double kap[8];
    int G = 8;
    for (int g = 0; g < G; g++) {
        int i0 = g * ncp / G, i1 = (g + 1) * ncp / G;
        float re = 0, im = 0;
        double ks = 0;
        for (int i = i0; i < i1; i++) {
            const cf32 y = cur[bins[i]], p = prev[bins[i]];
            re += y.re * p.re + y.im * p.im;
            im += y.im * p.re - y.re * p.im;
            ks += kc[i];
        }
        S[g] = (cf32){re, im};
        kap[g] = ks / (double)(i1 - i0);
    }
    double num = 0, den = 0;
    for (int g = 0; g + 1 < G; g++) {
        double re = (double)S[g + 1].re * S[g].re + (double)S[g + 1].im * S[g].im;
        double im = (double)S[g + 1].im * S[g].re - (double)S[g + 1].re * S[g].im;
        double wgt = sqrt(re * re + im * im);
        num += wgt * atan2(im, re) / (kap[g + 1] - kap[g]);
        den += wgt;
    }
    double b = den > 0 ? num / den : 0, tr = 0, ti = 0, mag = 0;
    for (int g = 0; g < G; g++) {
        double ph = -b * kap[g], c = cos(ph), s = sin(ph);
        tr += S[g].re * c - S[g].im * s;
        ti += S[g].re * s + S[g].im * c;
        mag += sqrt((double)S[g].re * S[g].re + (double)S[g].im * S[g].im);
    }
    *a_out = atan2(ti, tr);
    *b_out = b;
    *mag_out = mag;
}

/* ======================================================================== */
/*  Múltiplex: tabla, eventos y estado                                      */
/* ======================================================================== */
static const char *hier_name(int h)
{
    static const char *n[] = {"none", "alpha1", "alpha2", "alpha4"};
    return n[h & 3];
}

static void mux_fields(sb_t *sb, const mux_t *m)
{
    sb_printf(sb, "\"mode\":\"%s\",\"guard\":\"%s\",\"constellation\":\"%s\",\"hierarchy\":\"%s\",\"interleaver\":\"%s\"",
              m->n == 2048 ? "2k" : "8k", dvbt_guard_name(m->guard), dvbt_const_name(m->constellation),
              hier_name(m->hierarchy), (m->hierarchy & 4) ? "in-depth" : "native");
    sb_printf(sb, ",\"hp_rate\":\"%s\",\"lp_rate\":\"%s\",\"frame\":%d,\"length_indicator\":%d", dvbt_rate_name(m->hp),
              dvbt_rate_name(m->lp), m->frame, m->length);
    if (m->length == 31 && m->have_byte[0] && m->have_byte[1])
        sb_printf(sb, ",\"cell_id\":%d,\"cell_id_hex\":\"0x%04X\"", m->cell_byte[0] << 8 | m->cell_byte[1],
                  m->cell_byte[0] << 8 | m->cell_byte[1]);
    else if (m->length == 31)
        sb_puts(sb, ",\"cell_id\":null");
    sb_printf(sb, ",\"bch\":\"%s\",\"cfo_hz\":%.0f,\"mer_db\":%.1f,\"level_dbfs\":%.1f", m->bch_alt ? "alt-order" : "std",
              m->cfo_hz, m->mer_db, m->level_dbfs);
}

static void emit_mux(dvbt_t *w, const mux_t *m, bool is_new)
{
    char text[200], cell[24] = "";
    if (m->length == 31 && m->have_byte[0] && m->have_byte[1])
        snprintf(cell, sizeof cell, " cell 0x%04X", m->cell_byte[0] << 8 | m->cell_byte[1]);
    snprintf(text, sizeof text, "DVB-T %s guarda %s %s HP %s LP %s%s @%.3f MHz", m->n == 2048 ? "2k" : "8k",
             dvbt_guard_name(m->guard), dvbt_const_name(m->constellation), dvbt_rate_name(m->hp),
             dvbt_rate_name(m->lp), cell, (double)w->freq_hz / 1e6);
    sb_t sb = {0};
    sb_puts(&sb, "{\"text\":");
    sb_json_str(&sb, text);
    sb_printf(&sb, ",\"event\":\"%s\",\"freq_hz\":%lld,", is_new ? "new" : "changed", (long long)w->freq_hz);
    mux_fields(&sb, m);
    sb_puts(&sb, "}");
    w->emit(w->ctx, sb.data, sb.len);
    sb_free(&sb);
}

static void register_mux(dvbt_t *w, const dvbt_tps_t *t, const det_t *d, const uint8_t *cell_have, const int *cell_val,
                         double cfo, double mer, double level)
{
    mux_t *m = NULL, *slot = NULL;
    for (int i = 0; i < MAX_MUX; i++) {
        mux_t *c = &w->mux[i];
        if (c->used && c->n == d->N && c->guard == t->guard && c->constellation == t->constellation &&
            c->hierarchy == t->hierarchy && c->hp == t->hp && c->lp == t->lp && c->length == t->length) {
            m = c;
            break;
        }
        if (!slot || (slot->used && (!c->used || c->last < slot->last)))
            slot = c;
    }
    bool is_new = !m, changed = false;
    if (!m) {
        m = slot;
        memset(m, 0, sizeof *m);
        m->used = true;
        m->n = d->N;
        m->guard = t->guard;
        m->constellation = t->constellation;
        m->hierarchy = t->hierarchy;
        m->hp = t->hp;
        m->lp = t->lp;
        m->length = t->length;
        m->first = w->t_now;
    }
    bool had = m->have_byte[0] && m->have_byte[1];
    int old_id = m->cell_byte[0] << 8 | m->cell_byte[1];
    for (int p = 0; p < 2; p++)
        if (cell_have[p]) {
            m->cell_byte[p] = cell_val[p];
            m->have_byte[p] = 1;
        }
    bool complete = m->have_byte[0] && m->have_byte[1];
    /* el cell id acaba de completarse o ha cambiado */
    changed = !is_new && complete && (!had || old_id != (m->cell_byte[0] << 8 | m->cell_byte[1]));
    m->frame = t->frame;
    m->bch_alt = t->bch_alt;
    m->cfo_hz = cfo;
    m->mer_db = mer;
    m->level_dbfs = level;
    m->last = w->t_now;
    m->locks++;
    w->need_cell = m->length == 31 && !complete;
    if (is_new || changed)
        emit_mux(w, m, is_new);
}

/* ======================================================================== */
/*  Análisis de un segmento                                                 */
/* ======================================================================== */
static void analyze(dvbt_t *w)
{
    w->scans++;
    w->locked = false;
    w->need_cell = false;
    int n = SEG_N;
    const cf32 *x = w->seg;
    double pw = 0;
    for (int i = 0; i < n; i += 7)
        pw += (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
    double level = 10.0 * log10(pw / ((n + 6) / 7) + 1e-20);
    det_t d;
    bool det = detect(w, x, n, &d);
    DBG("detect=%d N=%d L=%d score=%.3f contrast=%.2f cp_start=%d\n", det, d.N, d.L, d.score, d.contrast, d.cp_start);
    if (!det)
        return;
    w->cp_found++;
    const int N = d.N, L = d.L, Ts = N + L, K = dvbt_ncar(N), back = L / 8;
    const double df = w->fs / N;
    fft_plan_t *plan = w->plan[N == 8192];
    int cp[200], tpsk[80];
    int ncp = dvbt_cp_list(N, cp), ntps = dvbt_tps_list(N, tpsk);
    int kcp[200], kt[80], bcp[200], bt[80];
    for (int i = 0; i < ncp; i++)
        kcp[i] = cp[i] - (K - 1) / 2;
    for (int i = 0; i < ntps; i++)
        kt[i] = tpsk[i] - (K - 1) / 2;

    /* ---- 2. CFO fraccional ---- */
    double gr, gi;
    cp_gamma(x, n, &d, d.cp_start, &gr, &gi);
    double f_frac = -atan2(gi, gr) * w->fs / (2.0 * M_PI * N);
    cf32 *y = w->work;
    rotate(x, y, n, f_frac, w->fs);

    /* ---- 3. 1.ª pasada: CFO entero y deriva de reloj ---- */
    for (int m = 0; m < Q1; m++) {
        int ws = d.cp_start + m * Ts + L - back;
        if (ws + N > n)
            return;
        cf32 *Y = w->pass1 + (size_t)m * NMAX;
        memcpy(Y, y + ws, sizeof(cf32) * (size_t)N);
        fft_exec(plan, Y);
    }
    int S = N / 16, best_s = 0;
    double best = 0, tot = 0;
    for (int s = -S; s <= S; s++) {
        double acc = 0;
        for (int i = 0; i < ncp; i++)
            bcp[i] = ((kcp[i] + s) % N + N) % N;
        for (int m = 1; m < Q1; m++) {
            const cf32 *Y = w->pass1 + (size_t)m * NMAX, *P = w->pass1 + (size_t)(m - 1) * NMAX;
            float re = 0, im = 0;
            for (int i = 0; i < ncp; i++) {
                re += Y[bcp[i]].re * P[bcp[i]].re + Y[bcp[i]].im * P[bcp[i]].im;
                im += Y[bcp[i]].im * P[bcp[i]].re - Y[bcp[i]].re * P[bcp[i]].im;
            }
            acc += sqrt((double)re * re + (double)im * im);
        }
        tot += acc;
        if (acc > best) {
            best = acc;
            best_s = s;
        }
    }
    DBG("f_frac=%.1f best_s=%d best/mean=%.2f\n", f_frac, best_s, best / (tot / (2 * S + 1)));
    if (best < 3.0 * tot / (2 * S + 1))
        return; /* ninguna posición destaca: no son pilotos continuos */
    for (int i = 0; i < ncp; i++)
        bcp[i] = ((kcp[i] + best_s) % N + N) % N;
    for (int i = 0; i < ntps; i++)
        bt[i] = ((kt[i] + best_s) % N + N) % N;
    double bsum = 0, bw = 0;
    for (int m = 1; m < Q1; m++) {
        double a, b, mg;
        pair_phase(w->pass1 + (size_t)m * NMAX, w->pass1 + (size_t)(m - 1) * NMAX, bcp, kcp, ncp, &a, &b, &mg);
        bsum += b * mg;
        bw += mg;
    }
    double b_mean = bw > 0 ? bsum / bw : 0;
    double eps = -b_mean * N / (2.0 * M_PI * Ts); /* deriva relativa de reloj (+ = más muestras/s) */
    DBG("eps=%.1f ppm\n", eps * 1e6);
    int n2 = n, cp_start = d.cp_start;
    double f_total = f_frac + best_s * df;
    if (fabs(eps) > 20e-6 && fabs(eps) < 600e-6) {
        /* Corrige la deriva: toma muestras a paso (1+ε) y vuelve a sincronizar el CP. */
        n2 = resample_cr(y, n, 1.0 / (1.0 + eps), w->seg);
        int Lw = N / 32, nr = compute_rho(w->seg, n2, N, Lw, w->rho);
        det_t d2 = d;
        if (!fold_rho(w, w->rho, nr, Ts, Lw, L, &d2) || d2.score < 0.5f * d.score)
            return;
        cp_start = d2.cp_start;
        DBG("resample: n2=%d cp_start=%d score=%.3f (antes %.3f)\n", n2, cp_start, d2.score, d.score);
        double gr2, gi2;
        cp_gamma(w->seg, n2, &d, cp_start, &gr2, &gi2);
        double f2 = -atan2(gi2, gr2) * w->fs / (2.0 * M_PI * N);
        rotate(w->seg, y, n2, f2, w->fs);
        f_total += f2;
    }

    /* ---- 4. Todos los símbolos: FFT, pares de símbolos y bits TPS ---- */
    int nsym = (n2 - cp_start - Ts) / Ts + 1;
    if (nsym > MAX_SYM)
        nsym = MAX_SYM;
    if (nsym < 70)
        return;
    for (int m = 0; m < nsym; m++) {
        int ws = cp_start + m * Ts + L - back;
        if (ws + N > n2) {
            nsym = m;
            break;
        }
        cf32 *Y = w->spec + (size_t)m * NMAX;
        memcpy(Y, y + ws, sizeof(cf32) * (size_t)N);
        fft_exec(plan, Y);
    }
    int nd = nsym - 1;
    for (int m = 1; m < nsym; m++) {
        const cf32 *cur = w->spec + (size_t)m * NMAX, *prev = w->spec + (size_t)(m - 1) * NMAX;
        double a, b, mg;
        pair_phase(cur, prev, bcp, kcp, ncp, &a, &b, &mg);
        double t = 0;
        for (int j = 0; j < ntps; j++) {
            const cf32 p = cur[bt[j]], q = prev[bt[j]];
            double re = (double)p.re * q.re + (double)p.im * q.im, im = (double)p.im * q.re - (double)p.re * q.im;
            double ph = -(a + b * kt[j]);
            t += re * cos(ph) - im * sin(ph);
        }
        w->soft[m - 1] = (float)t;
    }

    /* ---- 5. Sincronía + BCH ---- */
    uint8_t bits[MAX_SYM];
    for (int i = 0; i < nd; i++)
        bits[i] = w->soft[i] < 0;
    dvbt_tps_t tps;
    memset(&tps, 0, sizeof tps);
    bool found = false;
    int frame_start = 0, cell_val[2] = {0, 0};
    uint8_t cell_have[2] = {0, 0};
    for (int maxd = 0; maxd <= 4 && !found; maxd++) {
        for (int p = 0; p + 67 <= nd && !found; p++) {
            int de = 0, do_ = 0;
            for (int i = 0; i < 16; i++) {
                de += bits[p + i] != DVBT_SYNC_EVEN[i];
                do_ += bits[p + i] != DVBT_SYNC_ODD[i];
            }
            if (MIN(de, do_) != maxd)
                continue;
            uint8_t s[68];
            s[0] = 0;
            memcpy(s + 1, bits + p, 67);
            int alt;
            if (dvbt_tps_check(s, &alt) < 0)
                continue;
            if (memcmp(s + 1, DVBT_SYNC_EVEN, 16) && memcmp(s + 1, DVBT_SYNC_ODD, 16))
                continue;
            dvbt_tps_parse(s, &tps);
            if (tps.length != 23 && tps.length != 31)
                continue;
            tps.bch_alt = alt;
            found = true;
            frame_start = p;
            if (tps.length == 31) {
                cell_val[tps.frame & 1] = tps.cell_byte;
                cell_have[tps.frame & 1] = 1;
                /* 2.ª trama en el mismo segmento: da el otro byte del cell id */
                int p2 = p + 68;
                if (p2 + 67 <= nd) {
                    uint8_t s2[68];
                    s2[0] = 0;
                    memcpy(s2 + 1, bits + p2, 67);
                    int alt2;
                    dvbt_tps_t t2;
                    if (dvbt_tps_check(s2, &alt2) >= 0) {
                        dvbt_tps_parse(s2, &t2);
                        if (t2.length == 31 && ((t2.frame ^ tps.frame) & 1)) {
                            cell_val[t2.frame & 1] = t2.cell_byte;
                            cell_have[t2.frame & 1] = 1;
                        }
                    }
                }
            }
        }
    }
    DBG("nsym=%d found=%d frame_start=%d\n", nsym, found, frame_start);
    if (!found)
        return;
    w->tps_ok++;
    w->locked = true;

    /* ---- 6. Ecualización con pilotos dispersos → MER sobre pilotos continuos ---- */
    float mers[MAX_SYM];
    int nmer = 0;
    int kp[1400];
    for (int m = 0; m < nsym; m += 2) {
        double ps = 0, pe = 0;
        int l = (((m - frame_start) % 68) + 68) % 68;
        const cf32 *Y = w->spec + (size_t)m * NMAX;
        int np = 0, k0 = 3 * (l % 4);
        for (int k = k0; k < K; k += 12)
            kp[np++] = k;
        cf32 H[1400];
        for (int j = 0; j < np; j++) {
            cf32 v = Y[((kp[j] - (K - 1) / 2 + best_s) % N + N) % N];
            float sgn = dvbt_prbs(kp[j]) ? -1.0f : 1.0f;
            H[j] = (cf32){v.re * sgn * 0.75f, v.im * sgn * 0.75f}; /* ÷ (4/3) */
        }
        /* La ventana FFT adelantada y el eco inclinan la fase de H con la frecuencia: se estima la pendiente
         * ψ (rad/portadora) entre pilotos vecinos y se interpola H sin ella (la interpolación lineal de un
         * fasor que gira ~1 rad entre pilotos daría un error grande). */
        double sr = 0, si = 0;
        for (int j = 0; j + 1 < np; j++) {
            sr += (double)H[j + 1].re * H[j].re + (double)H[j + 1].im * H[j].im;
            si += (double)H[j + 1].im * H[j].re - (double)H[j + 1].re * H[j].im;
        }
        float psi = (float)(atan2(si, sr) / 12.0);
        for (int j = 0; j < np; j++) {
            float c = cosf(-psi * (float)kp[j]), s = sinf(-psi * (float)kp[j]);
            H[j] = (cf32){H[j].re * c - H[j].im * s, H[j].re * s + H[j].im * c};
        }
        for (int i = 0; i < ncp; i++) {
            int k = cp[i];
            if (dvbt_is_scattered(k, l))
                continue;
            int j = (k - k0) / 12;
            if (k < k0)
                j = 0;
            cf32 h;
            if (j + 1 >= np) {
                h = H[np - 1];
            } else {
                float f = k < k0 ? 0.0f : (float)(k - kp[j]) / 12.0f;
                h = (cf32){H[j].re * (1 - f) + H[j + 1].re * f, H[j].im * (1 - f) + H[j + 1].im * f};
            }
            {
                float c = cosf(psi * (float)k), s = sinf(psi * (float)k);
                h = (cf32){h.re * c - h.im * s, h.re * s + h.im * c};
            }
            float h2 = h.re * h.re + h.im * h.im + 1e-20f;
            cf32 v = Y[bcp[i]];
            cf32 z = {(v.re * h.re + v.im * h.im) / h2, (v.im * h.re - v.re * h.im) / h2};
            float ideal = (dvbt_prbs(k) ? -1.0f : 1.0f) * (4.0f / 3.0f);
            ps += (double)ideal * ideal;
            pe += (double)(z.re - ideal) * (z.re - ideal) + (double)z.im * z.im;
        }
        if (ps > 0)
            { mers[nmer++] = (float)(10.0 * log10(ps / (pe + 1e-20))); DBG("m=%d l=%d mer=%.1f\n", m, l, mers[nmer - 1]); }
    }
    /* mediana: robusta frente a símbolos corrompidos (cortes, interferencia impulsiva) */
    for (int i = 1; i < nmer; i++)
        for (int j = i; j > 0 && mers[j] < mers[j - 1]; j--) {
            float t = mers[j];
            mers[j] = mers[j - 1];
            mers[j - 1] = t;
        }
    double mer = nmer ? mers[nmer / 2] : 0.0;
    register_mux(w, &tps, &d, cell_have, cell_val, f_total, mer, level);
}

static void dvbt_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    dvbt_t *w = st;
    w->emit = emit;
    w->ctx = ctx;
    while (n > 0) {
        w->t_now = now;
        if (w->skip > 0) {
            int k = (int)MIN((int64_t)n, w->skip);
            w->skip -= k;
            iq += k;
            n -= k;
            now += k / w->fs;
            continue;
        }
        int k = MIN(n, SEG_N - w->fill);
        memcpy(w->seg + w->fill, iq, sizeof(cf32) * (size_t)k);
        w->fill += k;
        iq += k;
        n -= k;
        now += k / w->fs;
        if (w->fill == SEG_N) {
            w->t_now = now;
            analyze(w);
            w->fill = 0;
            w->skip = (int64_t)((w->need_cell ? IDLE_CELL_S : w->locked ? IDLE_LOCKED_S : IDLE_SEARCH_S) * w->fs);
        }
    }
    w->t_now = now;
}

static void dvbt_state_json(void *st, sb_t *sb, double now)
{
    dvbt_t *w = st;
    (void)now;
    sb_puts(sb, "{\"muxes\":[");
    bool first = true;
    for (int i = 0; i < MAX_MUX; i++) {
        mux_t *m = &w->mux[i];
        if (m->used && w->t_now - m->last > MUX_TTL)
            m->used = false;
        if (!m->used)
            continue;
        sb_printf(sb, "%s{\"freq_hz\":%lld,", first ? "" : ",", (long long)w->freq_hz);
        first = false;
        mux_fields(sb, m);
        sb_printf(sb, ",\"locks\":%d,\"age\":%.0f}", m->locks, w->t_now - m->last);
    }
    sb_printf(sb, "],\"scans\":%llu,\"cp_found\":%llu,\"tps_ok\":%llu,\"cp_rho\":%.2f,\"locked\":%s}",
              (unsigned long long)w->scans, (unsigned long long)w->cp_found, (unsigned long long)w->tps_ok,
              w->last_rho, w->locked ? "true" : "false");
}

const decoder_desc_t DEC_DVBT = {
    .id = "dvbt",
    .name = "DVB-T",
    .group = "Televisión",
    .desc = "TDT DVB-T 2k/8k: parámetros TPS.",
    .kind = DEC_NARROW,
    .rate = 64e6 / 7.0,
    .bw_hz = 7610000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {474000000LL, 0LL, 0LL, 0LL},
    .create = dvbt_create,
    .destroy = dvbt_destroy,
    .process = dvbt_process,
    .state_json = dvbt_state_json,
};
