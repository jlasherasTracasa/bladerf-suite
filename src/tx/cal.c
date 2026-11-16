/*
 * cal.c — Calibración IQ / LO del transmisor. Teoría y fórmulas en cal.h.
 */
#include "cal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 2048
#define PN_LEN 2048
#define TONE_LEN 4096
#define TAIL_LEN 4096
#define MARGIN 512 /* se descartan los bordes de cada tramo (transitorios del filtro del RFIC) */
#define TONE_DIV 16 /* f_tono = fs/16 */
#define BURST_LEN (GUARD + PN_LEN + 2 * TONE_LEN + TAIL_LEN)

int cal_burst_len(void) { return BURST_LEN; }

void cal_identity(cal_t *c) { memset(c, 0, sizeof *c); }

bool cal_is_identity(const cal_t *c) { return c->q_re == 0 && c->q_im == 0 && c->dc_re == 0 && c->dc_im == 0; }

/* Preámbulo: chips QPSK de un LFSR de 11 bits (x^11 + x^9 + 1, período 2047).
 * Su autocorrelación tiene un pico único de N y lóbulos de ~√N: da el retardo. */
static void make_pn(cf32 *pn, double amp)
{
    unsigned s = 0x5A5;
    float a = (float)(amp / sqrt(2.0));
    for (int i = 0; i < PN_LEN; i++) {
        unsigned bi = s & 1;
        s = (s >> 1) ^ (-(s & 1) & 0x500); /* realimentación x^11 + x^9 + 1 (Galois) */
        unsigned bq = s & 1;
        s = (s >> 1) ^ (-(s & 1) & 0x500);
        pn[i].re = bi ? a : -a;
        pn[i].im = bq ? a : -a;
    }
}

int cal_burst_make(iqbuf_t *b, double rate, double amp)
{
    memset(b, 0, sizeof *b);
    b->s = calloc(BURST_LEN, sizeof(cf32));
    if (!b->s)
        return -1;
    b->n = b->cap = BURST_LEN;
    b->rate = rate;
    make_pn(b->s + GUARD, amp);
    int s1 = GUARD + PN_LEN, s2 = s1 + TONE_LEN;
    for (int m = 0; m < TONE_LEN; m++) {
        double ph = 2.0 * M_PI * (m % TONE_DIV) / TONE_DIV;
        b->s[s1 + m] = (cf32){(float)(amp * cos(ph)), (float)(amp * sin(ph))};
        b->s[s2 + m] = (cf32){(float)(amp * cos(ph)), (float)(-amp * sin(ph))};
    }
    return 0;
}

/* Media de r[k]·e^{−j·sgn·ω·(k − k0)} para k en [a, b): proyección sobre ±fs/16.
 * k0 es el inicio del tono, así la fase de referencia coincide con la del
 * generador (ciclos enteros: (b − a) múltiplo de 16). */
typedef struct {
    double re, im;
} dc_t;

static dc_t project(const cf32 *r, int a, int b, int k0, int sgn)
{
    double sr = 0, si = 0;
    for (int k = a; k < b; k++) {
        double ph = -sgn * 2.0 * M_PI * (((k - k0) % TONE_DIV + TONE_DIV) % TONE_DIV) / TONE_DIV;
        double c = cos(ph), s = sin(ph);
        sr += r[k].re * c - r[k].im * s;
        si += r[k].re * s + r[k].im * c;
    }
    int n = b - a;
    return (dc_t){sr / n, si / n};
}

static dc_t cmul(dc_t a, dc_t b) { return (dc_t){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
static dc_t cdiv(dc_t a, dc_t b)
{
    double d = b.re * b.re + b.im * b.im;
    return (dc_t){(a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d};
}
static dc_t cexpj(double th) { return (dc_t){cos(th), sin(th)}; }
static double cabs2(dc_t a) { return a.re * a.re + a.im * a.im; }

int cal_estimate(const cf32 *rx, int n, double amp, cal_t *out, cal_meas_t *m, char *err, size_t errlen)
{
    if (n < BURST_LEN + 16 || amp <= 0) {
        snprintf(err, errlen, "captura demasiado corta (%d muestras)", n);
        return -1;
    }
    /* 1. Retardo entero d: máximo de |Σ conj(pn[k])·r[d + GUARD + k]| */
    cf32 pn[PN_LEN];
    make_pn(pn, 1.0);
    int nlag = n - BURST_LEN + 1;
    double *mag = malloc(sizeof(double) * (size_t)nlag);
    if (!mag) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    double best = -1, sum = 0;
    int dbest = 0;
    for (int d = 0; d < nlag; d++) {
        const cf32 *r = rx + d + GUARD;
        float sr = 0, si = 0;
        for (int k = 0; k < PN_LEN; k++) {
            sr += pn[k].re * r[k].re + pn[k].im * r[k].im;
            si += pn[k].re * r[k].im - pn[k].im * r[k].re;
        }
        double p = (double)sr * sr + (double)si * si;
        mag[d] = p;
        sum += p;
        if (p > best) {
            best = p;
            dbest = d;
        }
    }
    double mean = sum / nlag;
    free(mag);
    if (best < 40.0 * mean || best <= 0) {
        snprintf(err, errlen, "no se detecta el preámbulo de calibración (¿bucle sin señal?)");
        return -1;
    }
    int d = dbest;

    /* 2. Proyecciones sobre los dos tonos */
    int s1 = d + GUARD + PN_LEN, s2 = s1 + TONE_LEN, st = s2 + TONE_LEN;
    int a1 = s1 + MARGIN, b1 = s1 + TONE_LEN - MARGIN;
    int a2 = s2 + MARGIN, b2 = s2 + TONE_LEN - MARGIN;
    dc_t Pp = project(rx, a1, b1, s1, +1), ip = project(rx, a1, b1, s1, -1);
    dc_t Pm = project(rx, a2, b2, s2, -1), im = project(rx, a2, b2, s2, +1);
    if (cabs2(Pp) < 1e-12 || cabs2(Pm) < 1e-12) {
        snprintf(err, errlen, "señal del bucle demasiado débil");
        return -1;
    }
    /* θ = ½·arg(P−/P+) (en (−π/2, π/2]); H y q combinan los dos tonos */
    dc_t ratio = cdiv(Pm, Pp);
    double th = 0.5 * atan2(ratio.im, ratio.re);
    dc_t e_p = cexpj(th), e_m = cexpj(-th);
    dc_t t1 = cmul(Pp, e_p), t2 = cmul(Pm, e_m);
    dc_t H = {0.5 * (t1.re + t2.re) / amp, 0.5 * (t1.im + t2.im) / amp};
    dc_t u1 = cmul(ip, e_m), u2 = cmul(im, e_p);
    dc_t qn = {0.5 * (u1.re + u2.re), 0.5 * (u1.im + u2.im)};
    dc_t q = cdiv(qn, (dc_t){H.re * amp, H.im * amp});
    /* 3. Fuga de LO: media del silencio final (el TX está encendido y transmite 0) */
    int at = st + MARGIN, bt = MIN(st + TAIL_LEN, n) - 0;
    if (bt - at < 256) {
        snprintf(err, errlen, "captura sin silencio final para medir la fuga de LO");
        return -1;
    }
    double sr = 0, si = 0;
    for (int k = at; k < bt; k++) {
        sr += rx[k].re;
        si += rx[k].im;
    }
    dc_t c = cdiv((dc_t){sr / (bt - at), si / (bt - at)}, H);

    out->q_re = q.re;
    out->q_im = q.im;
    out->dc_re = c.re;
    out->dc_im = c.im;
    if (m) {
        m->q_abs = sqrt(cabs2(q));
        m->irr_db = -20.0 * log10(m->q_abs + 1e-12);
        m->lo_dbc = 20.0 * log10(sqrt(cabs2(c)) / amp + 1e-12);
        m->delay = d + th / (2.0 * M_PI / TONE_DIV);
        m->gain_db = 20.0 * log10(sqrt(cabs2(H)) + 1e-12);
    }
    return 0;
}

void cal_apply(const cal_t *c, cf32 *x, int n)
{
    double qr = c->q_re, qi = c->q_im;
    double den = 1.0 - (qr * qr + qi * qi);
    if (den < 0.25) /* |q| > 0,87: el modelo no es creíble, no se toca la señal */
        return;
    double peak = 0;
    for (int i = 0; i < n; i++) {
        double wr = x[i].re - c->dc_re, wi = x[i].im - c->dc_im;
        /* x = (w − q·w*)/(1 − |q|²);  q·w* = (qr·wr + qi·wi) + j(qi·wr − qr·wi) */
        double xr = (wr - (qr * wr + qi * wi)) / den;
        double xi = (wi - (qi * wr - qr * wi)) / den;
        x[i].re = (float)xr;
        x[i].im = (float)xi;
        double a = xr * xr + xi * xi;
        if (a > peak)
            peak = a;
    }
    peak = sqrt(peak);
    if (peak > 0.95) {
        float k = (float)(0.95 / peak);
        for (int i = 0; i < n; i++) {
            x[i].re *= k;
            x[i].im *= k;
        }
    }
}

double cal_tone_irr_db(const cf32 *x, int n, double f_norm)
{
    double pr = 0, pi_ = 0, mr = 0, mi = 0;
    for (int k = 0; k < n; k++) {
        double ph = 2.0 * M_PI * f_norm * (double)k;
        double c = cos(ph), s = sin(ph);
        pr += x[k].re * c + x[k].im * s; /* x·e^{−jφ} */
        pi_ += x[k].im * c - x[k].re * s;
        mr += x[k].re * c - x[k].im * s; /* x·e^{+jφ} */
        mi += x[k].im * c + x[k].re * s;
    }
    return 10.0 * log10((pr * pr + pi_ * pi_ + 1e-30) / (mr * mr + mi * mi + 1e-30));
}
