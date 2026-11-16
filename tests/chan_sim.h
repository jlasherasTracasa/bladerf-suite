/*
 * chan_sim.h — Emulador de canal reutilizable por los tests (T1).
 *
 * Todo opera sobre vectores cf32 y es determinista dado el generador cs_rng_t.
 * No depende de test_util.h (puede incluirse solo o junto a él; los nombres
 * llevan el prefijo cs_ para no chocar con chan_* de test_util.h).
 *
 *   Desvanecimiento (Jakes, suma de sinusoides de Clarke)
 *     g(t) = (1/√N)·Σ_n exp(j(2π·fd·cos α_n·t + φ_n)),  α_n = 2π(n − ½)/N,
 *     φ_n uniforme. Potencia media 1, autocorrelación ≈ J0(2π·fd·τ) (Jakes 1974;
 *     fases aleatorias: Pop & Beaulieu 2001). Rice: se suma una componente
 *     directa con Doppler fd·cos θ0:  h = √(K/(K+1))·e^{j(...)} + √(1/(K+1))·g.
 *   Multitrayecto: y[n] = Σ_k h_k(n)·x(n − d_k), con retardos fraccionarios por
 *     interpolación cúbica (Catmull-Rom) y cada camino con su propio
 *     desvanecimiento; la potencia total se normaliza a 1.
 *   Ruido de fase: paseo aleatorio (Wiener) de anchura de línea Lorentziana β:
 *     σ² del incremento = 2π·β/fs.
 *   Desequilibrio IQ (modelo del TX en tx/cal.h): yI = xI,
 *     yQ = g·(xQ·cosφ + xI·sinφ); μ = (1 + g·e^{jφ})/2, ν = (1 − g·e^{−jφ})/2.
 *   Deriva de reloj: remuestreo con razón 1 + ppm·1e−6 (+ deriva lineal opcional).
 *   AWGN: SNR medida sobre las muestras ≥ 25 % de la potencia máxima (válido
 *     para ráfagas).
 *   cs_apply(): encadena todo según un cs_cfg_t.
 */
#ifndef BLADERF_LAB_CHAN_SIM_H
#define BLADERF_LAB_CHAN_SIM_H

#include "common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Aleatorios ---------------------------------------------------------- */
typedef struct {
    uint64_t s;
} cs_rng_t;

static inline void cs_seed(cs_rng_t *r, uint64_t seed) { r->s = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL; }

static inline uint64_t cs_u64(cs_rng_t *r) /* xorshift64* */
{
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1DULL;
}

static inline double cs_unif(cs_rng_t *r) { return ((cs_u64(r) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }

static inline double cs_gauss(cs_rng_t *r)
{
    return sqrt(-2.0 * log(cs_unif(r))) * cos(2.0 * M_PI * cs_unif(r));
}

/* ---- Desvanecimiento de Jakes / Rice -------------------------------------- */
#define CS_JAKES_N 32

typedef struct {
    double w[CS_JAKES_N]; /* 2π·fd·cos α_n (rad/s) */
    double ph[CS_JAKES_N];
    double kfac;          /* factor K lineal (0 = Rayleigh) */
    double w_los, ph_los;
} cs_fader_t;

static inline void cs_fader_init(cs_fader_t *f, double fd_hz, double k_db, int rice, cs_rng_t *r)
{
    for (int n = 0; n < CS_JAKES_N; n++) {
        double a = 2.0 * M_PI * (n + 0.5) / CS_JAKES_N;
        f->w[n] = 2.0 * M_PI * fd_hz * cos(a);
        f->ph[n] = 2.0 * M_PI * cs_unif(r);
    }
    f->kfac = rice ? pow(10.0, k_db / 10.0) : 0.0;
    double th0 = 2.0 * M_PI * cs_unif(r);
    f->w_los = 2.0 * M_PI * fd_hz * cos(th0);
    f->ph_los = 2.0 * M_PI * cs_unif(r);
}

/* Ganancia compleja en el instante t (s). E|h|² = 1. */
static inline cf32 cs_fader_at(const cs_fader_t *f, double t)
{
    double re = 0, im = 0;
    for (int n = 0; n < CS_JAKES_N; n++) {
        double a = f->w[n] * t + f->ph[n];
        re += cos(a);
        im += sin(a);
    }
    double s = 1.0 / sqrt((double)CS_JAKES_N);
    re *= s;
    im *= s;
    if (f->kfac > 0) {
        double ks = sqrt(f->kfac / (f->kfac + 1.0)), ds = sqrt(1.0 / (f->kfac + 1.0));
        double a = f->w_los * t + f->ph_los;
        re = ks * cos(a) + ds * re;
        im = ks * sin(a) + ds * im;
    }
    return (cf32){(float)re, (float)im};
}

/* Aplica solo el desvanecimiento plano (un camino). */
static inline void cs_fading(cf32 *x, int n, double fs, double fd_hz, double k_db, int rice, cs_rng_t *r)
{
    cs_fader_t f;
    cs_fader_init(&f, fd_hz, k_db, rice, r);
    for (int i = 0; i < n; i++) {
        cf32 h = cs_fader_at(&f, i / fs);
        float re = x[i].re * h.re - x[i].im * h.im, im = x[i].re * h.im + x[i].im * h.re;
        x[i].re = re;
        x[i].im = im;
    }
}

/* ---- Multitrayecto -------------------------------------------------------- */
#define CS_MAX_TAPS 8
typedef struct {
    int ntaps;
    double delay_s[CS_MAX_TAPS];
    double power_db[CS_MAX_TAPS];
    int rice0; /* el primer camino es Rice (visión directa) con K = k_db */
    double k_db;
    double fd_hz; /* 0 = canal estático (cada camino con una ganancia compleja fija) */
} cs_multipath_t;

static inline cf32 cs_interp(const cf32 *in, int n, double pos)
{
    int i = (int)floor(pos);
    float f = (float)(pos - i);
    if (i < 1 || i + 2 >= n) {
        if (i >= 0 && i < n && f == 0.0f)
            return in[i];
        return (cf32){0, 0};
    }
    const cf32 *p0 = &in[i - 1], *p1 = &in[i], *p2 = &in[i + 1], *p3 = &in[i + 2];
    float w0 = -0.5f * f * (1 - f) * (1 - f), w1 = 1 - 2.5f * f * f + 1.5f * f * f * f,
          w2 = 0.5f * f * (1 + 4 * f - 3 * f * f), w3 = -0.5f * f * f * (1 - f);
    return (cf32){w0 * p0->re + w1 * p1->re + w2 * p2->re + w3 * p3->re,
                  w0 * p0->im + w1 * p1->im + w2 * p2->im + w3 * p3->im};
}

/* out ≠ in; misma longitud. */
static inline void cs_multipath(const cf32 *in, int n, double fs, const cs_multipath_t *mp, cs_rng_t *r, cf32 *out)
{
    cs_fader_t fd[CS_MAX_TAPS];
    double amp[CS_MAX_TAPS], tot = 0;
    cf32 fixed[CS_MAX_TAPS];
    for (int k = 0; k < mp->ntaps; k++) {
        amp[k] = pow(10.0, mp->power_db[k] / 20.0);
        tot += amp[k] * amp[k];
        cs_fader_init(&fd[k], mp->fd_hz, mp->k_db, mp->rice0 && k == 0, r);
        /* canal estático: fase aleatoria de módulo 1 (así la potencia relativa es la pedida) */
        cf32 h0 = cs_fader_at(&fd[k], 0.0);
        double mg = hypot(h0.re, h0.im);
        fixed[k] = mg > 1e-9 ? (cf32){(float)(h0.re / mg), (float)(h0.im / mg)} : (cf32){1, 0};
    }
    double norm = 1.0 / sqrt(tot > 0 ? tot : 1.0);
    for (int i = 0; i < n; i++) {
        double re = 0, im = 0;
        for (int k = 0; k < mp->ntaps; k++) {
            cf32 h = mp->fd_hz > 0 ? cs_fader_at(&fd[k], i / fs) : fixed[k];
            cf32 s = cs_interp(in, n, i - mp->delay_s[k] * fs);
            re += amp[k] * norm * (h.re * s.re - h.im * s.im);
            im += amp[k] * norm * (h.re * s.im + h.im * s.re);
        }
        out[i] = (cf32){(float)re, (float)im};
    }
}

/* ---- Ruido de fase, CFO, desequilibrio, deriva, AWGN ---------------------- */
static inline void cs_phase_noise(cf32 *x, int n, double fs, double linewidth_hz, cs_rng_t *r)
{
    double sig = sqrt(2.0 * M_PI * linewidth_hz / fs), ph = 0;
    for (int i = 0; i < n; i++) {
        ph += sig * cs_gauss(r);
        float c = (float)cos(ph), s = (float)sin(ph);
        float re = x[i].re * c - x[i].im * s, im = x[i].re * s + x[i].im * c;
        x[i].re = re;
        x[i].im = im;
    }
}

static inline void cs_cfo(cf32 *x, int n, double fs, double hz)
{
    for (int i = 0; i < n; i++) {
        double a = 2.0 * M_PI * hz * (double)i / fs;
        float c = (float)cos(a), s = (float)sin(a);
        float re = x[i].re * c - x[i].im * s, im = x[i].re * s + x[i].im * c;
        x[i].re = re;
        x[i].im = im;
    }
}

static inline void cs_iq_munu(double gain, double phase_rad, double *mu_re, double *mu_im, double *nu_re,
                              double *nu_im)
{
    *mu_re = 0.5 * (1.0 + gain * cos(phase_rad));
    *mu_im = 0.5 * gain * sin(phase_rad);
    *nu_re = 0.5 * (1.0 - gain * cos(phase_rad));
    *nu_im = 0.5 * gain * sin(phase_rad); /* −g·e^{−jφ} = −g·cosφ + j·g·sinφ */
}

static inline void cs_iq_imbalance(cf32 *x, int n, double gain, double phase_deg, double dc_i, double dc_q)
{
    double ph = phase_deg * M_PI / 180.0, c = cos(ph), s = sin(ph);
    for (int i = 0; i < n; i++) {
        double xi = x[i].re, xq = x[i].im;
        x[i].re = (float)(xi + dc_i);
        x[i].im = (float)(gain * (xq * c + xi * s) + dc_q);
    }
}

/* Remuestreo por deriva: la muestra k de salida es la entrada en la posición
 * k·(1 + ppm·1e−6) + ½·rate_ppm_s·1e−6·k²/fs. Devuelve las muestras escritas
 * (out admite n). Con ppm > 0 el reloj del receptor va "rápido" y el contenido
 * se estira (la señal parece más lenta). */
static inline int cs_clock_drift(const cf32 *in, int n, double fs, double ppm, double ppm_per_s, cf32 *out)
{
    int m = 0;
    for (int k = 0; k < n; k++) {
        double pos = (double)k * (1.0 + ppm * 1e-6) + 0.5 * ppm_per_s * 1e-6 * (double)k * (double)k / fs;
        if (pos >= n - 3)
            break;
        out[k] = cs_interp(in, n, pos);
        m++;
    }
    for (int k = m; k < n; k++)
        out[k] = (cf32){0, 0};
    return m;
}

/* Potencia media de las muestras "encendidas" (≥ 25 % del máximo). */
static inline double cs_on_power(const cf32 *x, int n)
{
    double pmax = 0, sum = 0;
    long cnt = 0;
    for (int i = 0; i < n; i++)
        pmax = fmax(pmax, (double)x[i].re * x[i].re + (double)x[i].im * x[i].im);
    for (int i = 0; i < n; i++) {
        double p = (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
        if (p >= 0.25 * pmax) {
            sum += p;
            cnt++;
        }
    }
    return cnt ? sum / cnt : 0.0;
}

static inline void cs_awgn(cf32 *x, int n, double snr_db, cs_rng_t *r)
{
    double sigma = sqrt(cs_on_power(x, n) / pow(10.0, snr_db / 10.0) / 2.0);
    for (int i = 0; i < n; i++) {
        x[i].re += (float)(sigma * cs_gauss(r));
        x[i].im += (float)(sigma * cs_gauss(r));
    }
}

/* ---- Cadena completa ------------------------------------------------------- */
typedef struct {
    double iq_gain, iq_phase_deg, dc_i, dc_q; /* iq_gain = 0 → sin desequilibrio */
    int use_multipath;
    cs_multipath_t mp;
    int use_fading; /* desvanecimiento plano (si no hay multitrayecto) */
    double fd_hz, k_db;
    int rice;
    double phase_noise_hz; /* 0 = sin ruido de fase */
    double cfo_hz;
    double ppm, ppm_per_s;
    double snr_db; /* ≥ 200 = sin ruido */
} cs_cfg_t;

/* in → out (ambos de n muestras; out ≠ in). Orden: desequilibrio → canal →
 * ruido de fase → CFO → deriva de reloj → AWGN. Devuelve las muestras válidas. */
static inline int cs_apply(const cf32 *in, int n, double fs, const cs_cfg_t *c, cs_rng_t *r, cf32 *out)
{
    cf32 *a = malloc(sizeof(cf32) * (size_t)n), *b = malloc(sizeof(cf32) * (size_t)n);
    if (!a || !b) {
        free(a);
        free(b);
        return 0;
    }
    memcpy(a, in, sizeof(cf32) * (size_t)n);
    if (c->iq_gain > 0)
        cs_iq_imbalance(a, n, c->iq_gain, c->iq_phase_deg, c->dc_i, c->dc_q);
    if (c->use_multipath) {
        cs_multipath(a, n, fs, &c->mp, r, b);
        memcpy(a, b, sizeof(cf32) * (size_t)n);
    } else if (c->use_fading) {
        cs_fading(a, n, fs, c->fd_hz, c->k_db, c->rice, r);
    }
    if (c->phase_noise_hz > 0)
        cs_phase_noise(a, n, fs, c->phase_noise_hz, r);
    if (c->cfo_hz != 0)
        cs_cfo(a, n, fs, c->cfo_hz);
    int m = n;
    if (c->ppm != 0 || c->ppm_per_s != 0)
        m = cs_clock_drift(a, n, fs, c->ppm, c->ppm_per_s, b);
    else
        memcpy(b, a, sizeof(cf32) * (size_t)n);
    if (c->snr_db < 200)
        cs_awgn(b, m, c->snr_db, r);
    memcpy(out, b, sizeof(cf32) * (size_t)n);
    free(a);
    free(b);
    return m;
}

#endif
