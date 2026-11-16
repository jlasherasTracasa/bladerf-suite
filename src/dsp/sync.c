/*
 * sync.c — Implementación de los bloques de sincronización. Ver sync.h.
 */
#include "sync.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/*  AGC                                                                    */
/* ======================================================================= */
void agc_init(agc_t *a, float target, float attack, float decay)
{
    a->gain = 1.0f;
    a->target = target;
    a->attack = attack;
    a->decay = decay;
    a->max_gain = 1e5f;
}

/* Realimentación sobre la magnitud de salida: g ← g·(1 + μ·(objetivo − |y|)).
 * El factor se acota para que una ráfaga fuerte no haga negativa la ganancia. */
static inline void agc_adapt(agc_t *a, float mag)
{
    float e = a->target - mag;
    float f = 1.0f + (e < 0 ? a->attack : a->decay) * e;
    a->gain *= f < 0.5f ? 0.5f : f;
    if (a->gain > a->max_gain)
        a->gain = a->max_gain;
}

void agc_process(agc_t *a, const cf32 *in, int n, cf32 *out)
{
    for (int i = 0; i < n; i++) {
        cf32 y = {in[i].re * a->gain, in[i].im * a->gain};
        out[i] = y;
        agc_adapt(a, sqrtf(y.re * y.re + y.im * y.im));
    }
}

void agc_process_f(agc_t *a, const float *in, int n, float *out)
{
    for (int i = 0; i < n; i++) {
        float y = in[i] * a->gain;
        out[i] = y;
        agc_adapt(a, fabsf(y));
    }
}

/* ======================================================================= */
/*  Discriminador FM                                                       */
/* ======================================================================= */
void fmdisc_init(fmdisc_t *d, double fs, double dev_hz)
{
    d->prev.re = 1.0f;
    d->prev.im = 0.0f;
    d->gain = (float)(fs / (2.0 * M_PI * dev_hz));
}

/* Δφ = arg(x[n]·conj(x[n−1])): evita desenrollar la fase. */
void fmdisc_process(fmdisc_t *d, const cf32 *in, int n, float *out)
{
    cf32 p = d->prev;
    for (int i = 0; i < n; i++) {
        float re = in[i].re * p.re + in[i].im * p.im;
        float im = in[i].im * p.re - in[i].re * p.im;
        out[i] = atan2f(im, re) * d->gain;
        p = in[i];
    }
    d->prev = p;
}

/* ======================================================================= */
/*  PLL                                                                    */
/* ======================================================================= */
void pll_init(pll_t *p, double bw, double zeta)
{
    double theta = bw / (zeta + 1.0 / (4.0 * zeta));
    double d = 1.0 + 2.0 * zeta * theta + theta * theta;
    p->alpha = (float)(4.0 * zeta * theta / d);
    p->beta = (float)(4.0 * theta * theta / d);
    p->phase = p->freq = 0.0f;
    p->freq_min = -(float)M_PI;
    p->freq_max = (float)M_PI;
}

void pll_set_limits(pll_t *p, double lo, double hi)
{
    p->freq_min = (float)lo;
    p->freq_max = (float)hi;
}

void pll_update(pll_t *p, float err)
{
    p->freq += p->beta * err;
    p->freq = CLAMP(p->freq, p->freq_min, p->freq_max);
    p->phase += p->freq + p->alpha * err;
    if (p->phase > (float)M_PI)
        p->phase -= 2.0f * (float)M_PI;
    else if (p->phase < -(float)M_PI)
        p->phase += 2.0f * (float)M_PI;
}

cf32 pll_derotate(const pll_t *p, cf32 x)
{
    float c = cosf(p->phase), s = sinf(p->phase);
    cf32 y = {x.re * c + x.im * s, x.im * c - x.re * s};
    return y;
}

void pll_track_carrier(pll_t *p, const cf32 *in, int n, cf32 *out)
{
    for (int i = 0; i < n; i++) {
        cf32 y = pll_derotate(p, in[i]);
        out[i] = y;
        pll_update(p, atan2f(y.im, y.re));
    }
}

/* ======================================================================= */
/*  Costas                                                                 */
/* ======================================================================= */
int costas_init(costas_t *c, int order, double bw, double zeta)
{
    if (order != 2 && order != 4 && order != 8)
        return -1;
    c->order = order;
    pll_init(&c->pll, bw, zeta);
    return 0;
}

static inline float costas_err(int order, cf32 y)
{
    float mag = sqrtf(y.re * y.re + y.im * y.im) + 1e-12f;
    float e;
    switch (order) {
    case 2:
        e = (y.re > 0 ? y.im : -y.im) / mag;
        break;
    case 4:
        e = ((y.re > 0 ? y.im : -y.im) - (y.im > 0 ? y.re : -y.re)) / mag;
        break;
    default: {   /* arg(y^8)/8 */
        float re = y.re / mag, im = y.im / mag, r2, i2;
        for (int k = 0; k < 3; k++) {          /* tres cuadrados: y^8 */
            r2 = re * re - im * im;
            i2 = 2 * re * im;
            re = r2;
            im = i2;
        }
        e = atan2f(im, re) / 8.0f;
        break;
    }
    }
    return CLAMP(e, -1.0f, 1.0f);
}

void costas_process(costas_t *c, const cf32 *in, int n, cf32 *out)
{
    for (int i = 0; i < n; i++) {
        cf32 y = pll_derotate(&c->pll, in[i]);
        out[i] = y;
        pll_update(&c->pll, costas_err(c->order, y));
    }
}

/* ======================================================================= */
/*  FLL                                                                    */
/* ======================================================================= */
struct fll {
    int mode;            /* 0 = bandedge, 1 = disc */
    int power;
    float phase, freq, gain;
    int ntaps, pos;
    cf32 *tu, *tl, *hist;
    cf32 prev;
};

fll_t *fll_create_disc(int power, double loop_bw)
{
    if (power < 1 || power > 8)
        return NULL;
    fll_t *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->mode = 1;
    f->power = power;
    f->gain = (float)loop_bw;
    f->prev.re = 1.0f;
    return f;
}

fll_t *fll_create_bandedge(double sps, double rolloff, int ntaps, double loop_bw)
{
    if (sps < 1.5 || rolloff <= 0 || rolloff > 1)
        return NULL;
    if (ntaps <= 0)
        ntaps = (int)(8.0 * sps / rolloff) | 1;
    ntaps = CLAMP(ntaps, 9, 1023) | 1;
    fll_t *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->ntaps = ntaps;
    f->gain = (float)loop_bw;
    f->tu = malloc((size_t)ntaps * sizeof(cf32));
    f->tl = malloc((size_t)ntaps * sizeof(cf32));
    f->hist = calloc((size_t)ntaps * 2, sizeof(cf32));
    if (!f->tu || !f->tl || !f->hist) {
        fll_destroy(f);
        return NULL;
    }
    /* Prototipo paso bajo (sinc·Hamming) de corte rolloff/(2·sps), desplazado
     * a ±(1+rolloff)/(2·sps). */
    double fc = rolloff / (2.0 * sps), fe = (1.0 + rolloff) / (2.0 * sps), m = (ntaps - 1) / 2.0, sum = 0;
    double *h = malloc((size_t)ntaps * sizeof(double));
    if (!h) {
        fll_destroy(f);
        return NULL;
    }
    for (int i = 0; i < ntaps; i++) {
        double t = i - m;
        double s = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        h[i] = s * (0.54 - 0.46 * cos(2 * M_PI * i / (ntaps - 1)));
        sum += h[i];
    }
    for (int i = 0; i < ntaps; i++) {
        double a = 2 * M_PI * fe * (i - m), w = h[i] / sum;
        f->tu[i].re = (float)(w * cos(a));
        /* y = Σ t[k]·x[k] (sin conjugar) responde a e^{−jθk}: se cambia el signo */
        f->tu[i].im = (float)(-w * sin(a));
        f->tl[i].re = (float)(w * cos(a));
        f->tl[i].im = (float)(w * sin(a));
    }
    free(h);
    f->gain = (float)(loop_bw * 0.5);
    return f;
}

void fll_destroy(fll_t *f)
{
    if (!f)
        return;
    free(f->tu);
    free(f->tl);
    free(f->hist);
    free(f);
}

void fll_reset(fll_t *f)
{
    f->phase = f->freq = 0.0f;
    f->pos = 0;
    f->prev.re = 1.0f;
    f->prev.im = 0.0f;
    if (f->hist)
        memset(f->hist, 0, (size_t)f->ntaps * 2 * sizeof(cf32));
}

double fll_freq_hz(const fll_t *f, double fs) { return f->freq * fs / (2.0 * M_PI); }

static inline cf32 cpow_n(cf32 y, int p)
{
    cf32 r = y;
    for (int k = 1; k < p; k++) {
        float re = r.re * y.re - r.im * y.im, im = r.re * y.im + r.im * y.re;
        r.re = re;
        r.im = im;
    }
    return r;
}

void fll_process(fll_t *f, const cf32 *in, int n, cf32 *out)
{
    const int N = f->ntaps;
    for (int i = 0; i < n; i++) {
        float c = cosf(f->phase), s = sinf(f->phase);
        cf32 y = {in[i].re * c + in[i].im * s, in[i].im * c - in[i].re * s};
        float err;
        if (f->mode == 1) {
            cf32 a = cpow_n(y, f->power), b = cpow_n(f->prev, f->power);
            float re = a.re * b.re + a.im * b.im, im = a.im * b.re - a.re * b.im;
            err = atan2f(im, re) / (float)f->power;
            f->prev = y;
        } else {
            f->hist[f->pos] = f->hist[f->pos + N] = y;
            f->pos = f->pos + 1 == N ? 0 : f->pos + 1;
            const cf32 *h = f->hist + f->pos;
            float ur = 0, ui = 0, lr = 0, li = 0;
            for (int k = 0; k < N; k++) {
                ur += f->tu[k].re * h[k].re - f->tu[k].im * h[k].im;
                ui += f->tu[k].re * h[k].im + f->tu[k].im * h[k].re;
                lr += f->tl[k].re * h[k].re - f->tl[k].im * h[k].im;
                li += f->tl[k].re * h[k].im + f->tl[k].im * h[k].re;
            }
            float pu = ur * ur + ui * ui, pl = lr * lr + li * li;
            err = (pu - pl) / (pu + pl + 1e-20f);
        }
        out[i] = y;
        f->freq += f->gain * err;
        f->freq = CLAMP(f->freq, -(float)M_PI, (float)M_PI);
        f->phase += f->freq;
        if (f->phase > (float)M_PI)
            f->phase -= 2.0f * (float)M_PI;
        else if (f->phase < -(float)M_PI)
            f->phase += 2.0f * (float)M_PI;
    }
}

double cfo_estimate_mpower(const cf32 *x, int n, int power, double fs)
{
    double re = 0, im = 0;
    cf32 prev = cpow_n(x[0], power);
    for (int i = 1; i < n; i++) {
        cf32 a = cpow_n(x[i], power);
        re += (double)a.re * prev.re + (double)a.im * prev.im;
        im += (double)a.im * prev.re - (double)a.re * prev.im;
        prev = a;
    }
    return atan2(im, re) / (2.0 * M_PI * power) * fs;
}

/* ======================================================================= */
/*  Farrow cúbico                                                          */
/* ======================================================================= */
float farrow3_f(const float *x, float mu)
{
    float v3 = -x[0] * (1.0f / 6) + x[1] * 0.5f - x[2] * 0.5f + x[3] * (1.0f / 6);
    float v2 = x[0] * 0.5f - x[1] + x[2] * 0.5f;
    float v1 = -x[0] * (1.0f / 3) - x[1] * 0.5f + x[2] - x[3] * (1.0f / 6);
    return ((v3 * mu + v2) * mu + v1) * mu + x[1];
}

cf32 farrow3_c(const cf32 *x, float mu)
{
    float re[4] = {x[0].re, x[1].re, x[2].re, x[3].re}, im[4] = {x[0].im, x[1].im, x[2].im, x[3].im};
    cf32 y = {farrow3_f(re, mu), farrow3_f(im, mu)};
    return y;
}

/* ======================================================================= */
/*  Recuperación de reloj                                                  */
/* ======================================================================= */
/* Ganancias del detector (error por periodo de símbolo de desfase) con pulsos
 * Nyquist y amplitud unidad; valores empíricos que normalizan la ganancia del
 * lazo para que loop_bw signifique aproximadamente Bn·T por símbolo. */
#define KP_GARDNER 2.0
#define KP_MM 1.0

struct symsync {
    ted_t ted;
    slicer_t slicer;
    double sps, omega, omega_min, omega_max, alpha, beta, t_next;
    int64_t total;
    cf32 *ring;
    int mask;
    cf32 prev_y, prev_d;
    bool have_prev;
    float pam_a;
};

symsync_t *symsync_create(ted_t ted, slicer_t slicer, double sps, double tol, double bw, double zeta)
{
    if (sps < 2.0 || sps > 32.0 || tol < 0 || tol > 0.5)
        return NULL;
    symsync_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    int cap = 64;
    while (cap < 2 * (int)ceil(sps * (1 + tol)) + 8)
        cap *= 2;
    s->ring = calloc((size_t)cap, sizeof(cf32));
    if (!s->ring) {
        free(s);
        return NULL;
    }
    s->mask = cap - 1;
    s->ted = ted;
    s->slicer = slicer;
    s->sps = sps;
    s->omega_min = sps * (1 - tol);
    s->omega_max = sps * (1 + tol);
    s->pam_a = 1.0f / 3.0f;
    double theta = bw / (zeta + 1.0 / (4.0 * zeta)), d = 1 + 2 * zeta * theta + theta * theta;
    double kp = (ted == TED_GARDNER ? KP_GARDNER : KP_MM) / sps;   /* por muestra de desfase */
    s->alpha = 4 * zeta * theta / d / kp;
    s->beta = 4 * theta * theta / d / kp;
    symsync_reset(s);
    return s;
}

void symsync_destroy(symsync_t *s)
{
    if (s) {
        free(s->ring);
        free(s);
    }
}

void symsync_reset(symsync_t *s)
{
    memset(s->ring, 0, (size_t)(s->mask + 1) * sizeof(cf32));
    s->total = 0;
    s->omega = s->sps;
    s->t_next = 2.0 * s->sps;
    s->have_prev = false;
    memset(&s->prev_y, 0, sizeof s->prev_y);
    memset(&s->prev_d, 0, sizeof s->prev_d);
}

void symsync_set_pam4_level(symsync_t *s, float a) { s->pam_a = a; }
double symsync_period(const symsync_t *s) { return s->omega; }
int symsync_max_out(const symsync_t *s, int n) { return (int)(n / s->omega_min) + 2; }

static inline cf32 ss_interp(const symsync_t *s, double t)
{
    int64_t i0 = (int64_t)floor(t);
    float mu = (float)(t - (double)i0);
    cf32 x[4];
    for (int k = 0; k < 4; k++)
        x[k] = s->ring[(i0 - 1 + k) & s->mask];
    return farrow3_c(x, mu);
}

static inline float slice_pam4(float v, float a)
{
    float av = fabsf(v), l = av < 2 * a ? a : 3 * a;
    return v < 0 ? -l : l;
}

static inline cf32 ss_slice(const symsync_t *s, cf32 y)
{
    cf32 d;
    switch (s->slicer) {
    case SLICE_BPSK:
        d.re = y.re >= 0 ? 1.0f : -1.0f;
        d.im = 0.0f;
        break;
    case SLICE_QPSK:
        d.re = y.re >= 0 ? 0.7071f : -0.7071f;
        d.im = y.im >= 0 ? 0.7071f : -0.7071f;
        break;
    default:
        d.re = slice_pam4(y.re, s->pam_a) * (1.0f / (3.0f * s->pam_a));
        d.im = 0.0f;
        break;
    }
    return d;
}

static inline void ss_push(symsync_t *s, cf32 x)
{
    s->ring[s->total & s->mask] = x;
    s->total++;
}

/* Genera, si hay muestras suficientes, el siguiente símbolo. */
static inline bool ss_step(symsync_t *s, cf32 *out)
{
    if (s->t_next >= (double)s->total - 2.0)
        return false;
    double T = s->t_next;
    cf32 y = ss_interp(s, T);
    float e = 0.0f;
    if (s->ted == TED_GARDNER) {
        if (s->have_prev) {
            cf32 ym = ss_interp(s, T - s->omega * 0.5);
            float dr = y.re - s->prev_y.re, di = y.im - s->prev_y.im;
            e = ym.re * dr + ym.im * di;
        }
    } else {
        cf32 d = ss_slice(s, y);
        if (s->have_prev)
            e = d.re * s->prev_y.re + d.im * s->prev_y.im - (s->prev_d.re * y.re + s->prev_d.im * y.im);
        s->prev_d = d;
    }
    e = CLAMP(e, -2.0f, 2.0f);
    s->prev_y = y;
    s->have_prev = true;
    s->omega = CLAMP(s->omega - s->beta * e, s->omega_min, s->omega_max);
    s->t_next = T + s->omega - s->alpha * e;
    *out = y;
    return true;
}

int symsync_process(symsync_t *s, const cf32 *in, int n, cf32 *out, int out_cap)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        ss_push(s, in[i]);
        cf32 y;
        while (ss_step(s, &y))
            if (w < out_cap)
                out[w++] = y;
    }
    return w;
}

int symsync_process_f(symsync_t *s, const float *in, int n, float *out, int out_cap)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        cf32 x = {in[i], 0.0f}, y;
        ss_push(s, x);
        while (ss_step(s, &y))
            if (w < out_cap)
                out[w++] = y.re;
    }
    return w;
}

/* ======================================================================= */
/*  Correlador                                                             */
/* ======================================================================= */
struct corr {
    int len, pos, window;
    cf32 *rc, *hist;
    double eref, e;
    int64_t count;
    float thr;
    bool have;
    float best;
    int64_t best_pos;
    cf32 best_c;
};

corr_t *corr_create(const cf32 *ref, int len, float thr, int window)
{
    if (len < 2 || len > 65536 || thr <= 0 || thr > 1)
        return NULL;
    corr_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->rc = malloc((size_t)len * sizeof(cf32));
    c->hist = calloc((size_t)len * 2, sizeof(cf32));
    if (!c->rc || !c->hist) {
        corr_destroy(c);
        return NULL;
    }
    c->len = len;
    c->thr = thr;
    c->window = window < 1 ? 1 : window;
    for (int i = 0; i < len; i++) {
        c->rc[i].re = ref[i].re;
        c->rc[i].im = -ref[i].im;
        c->eref += (double)ref[i].re * ref[i].re + (double)ref[i].im * ref[i].im;
    }
    if (c->eref <= 0) {
        corr_destroy(c);
        return NULL;
    }
    return c;
}

corr_t *corr_create_f(const float *ref, int len, float thr, int window)
{
    cf32 *tmp = malloc((size_t)(len > 0 ? len : 1) * sizeof(cf32));
    if (!tmp)
        return NULL;
    for (int i = 0; i < len; i++) {
        tmp[i].re = ref[i];
        tmp[i].im = 0.0f;
    }
    corr_t *c = corr_create(tmp, len, thr, window);
    free(tmp);
    return c;
}

void corr_destroy(corr_t *c)
{
    if (c) {
        free(c->rc);
        free(c->hist);
        free(c);
    }
}

void corr_reset(corr_t *c)
{
    memset(c->hist, 0, (size_t)c->len * 2 * sizeof(cf32));
    c->pos = 0;
    c->e = 0;
    c->count = 0;
    c->have = false;
}

static inline int corr_push(corr_t *c, cf32 x, corr_peak_t *pk)
{
    const int L = c->len;
    cf32 old = c->hist[c->pos];
    c->e += (double)x.re * x.re + (double)x.im * x.im - ((double)old.re * old.re + (double)old.im * old.im);
    if (c->e < 0)
        c->e = 0;
    c->hist[c->pos] = c->hist[c->pos + L] = x;
    c->pos = c->pos + 1 == L ? 0 : c->pos + 1;
    c->count++;
    int emitted = 0;
    if (c->count >= L && c->e > 1e-20) {
        const cf32 *h = c->hist + c->pos;
        float re = 0, im = 0;
        for (int i = 0; i < L; i++) {
            re += c->rc[i].re * h[i].re - c->rc[i].im * h[i].im;
            im += c->rc[i].re * h[i].im + c->rc[i].im * h[i].re;
        }
        double norm = c->e * c->eref;
        float m = (float)(((double)re * re + (double)im * im) / norm);
        if (m >= c->thr && (!c->have || m > c->best)) {
            c->have = true;
            c->best = m;
            c->best_pos = c->count - 1;
            float inv = (float)(1.0 / sqrt(norm));
            c->best_c.re = re * inv;
            c->best_c.im = im * inv;
        }
    }
    if (c->have && (c->count - 1) - c->best_pos >= c->window) {
        pk->pos = c->best_pos;
        pk->metric = c->best;
        pk->corr = c->best_c;
        c->have = false;
        emitted = 1;
    }
    return emitted;
}

int corr_process(corr_t *c, const cf32 *in, int n, corr_peak_t *peaks, int max_peaks)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        corr_peak_t pk;
        if (corr_push(c, in[i], &pk) && w < max_peaks)
            peaks[w++] = pk;
    }
    return w;
}

int corr_process_f(corr_t *c, const float *in, int n, corr_peak_t *peaks, int max_peaks)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        cf32 x = {in[i], 0.0f};
        corr_peak_t pk;
        if (corr_push(c, x, &pk) && w < max_peaks)
            peaks[w++] = pk;
    }
    return w;
}
