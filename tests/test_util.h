/*
 * test_util.h — Utilidades comunes de las pruebas (AGENTS.md §7).
 *
 * Aserciones:   CHECK(cond), CHECK_EQ(a, b), CHECK_NEAR(a, b, tol), TEST_SUMMARY()
 * Canal simulado (sobre vectores cf32):
 *   chan_awgn(x, n, snr_db, &seed)     ruido blanco gaussiano; la SNR se mide contra la
 *                                      potencia de las muestras "encendidas" (≥ 25 % de la
 *                                      potencia máxima), así vale también para ráfagas.
 *   chan_cfo(x, n, fs, hz)             desvío de frecuencia: x·e^{j2π·hz·n/fs}
 *   chan_resample(in, n, ratio, out)   error de reloj/cambio de tasa por interpolación cúbica
 *                                      (Catmull-Rom); ratio = fs_nueva/fs_vieja.
 *   chan_drift(in, n, ppm, out)        igual con ratio = 1 + ppm·1e-6 y n salidas.
 * Alimentación de decodificadores (recogen los eventos en un evs_t):
 *   run_narrow(d, iq, n, freq, opts, &evs)       iq a d->rate, en bloques de 4096.
 *   run_wide(d, iq16, n, rate, center, opts, &evs) iq Q11 a 'rate', en bloques de 65536.
 *   ev_has(&evs, "\"mmsi\":224"), evs_count(&evs), evs_free(&evs)
 * Conversión: cf32_to_q11(in, n, escala_fs, out) (1,0 → escala_fs·2048).
 */
#ifndef BLADERF_LAB_TEST_UTIL_H
#define BLADERF_LAB_TEST_UTIL_H

#include "common.h"
#include "decoders/decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Aserciones --------------------------------------------------------- */
static int tu_checks, tu_fails;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        tu_checks++;                                                                     \
        if (!(cond)) {                                                                   \
            tu_fails++;                                                                  \
            printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        long long tu_a = (long long)(a), tu_b = (long long)(b);                          \
        tu_checks++;                                                                     \
        if (tu_a != tu_b) {                                                              \
            tu_fails++;                                                                  \
            printf("  FALLO %s:%d: %s == %s  (%lld != %lld)\n", __FILE__, __LINE__, #a,  \
                   #b, tu_a, tu_b);                                                      \
        }                                                                                \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                            \
    do {                                                                                 \
        double tu_a = (double)(a), tu_b = (double)(b), tu_t = (double)(tol);             \
        tu_checks++;                                                                     \
        if (!(fabs(tu_a - tu_b) <= tu_t)) {                                              \
            tu_fails++;                                                                  \
            printf("  FALLO %s:%d: %s ≈ %s  (%g vs %g, tol %g)\n", __FILE__, __LINE__,   \
                   #a, #b, tu_a, tu_b, tu_t);                                            \
        }                                                                                \
    } while (0)

#define TEST_SUMMARY()                                                                   \
    (printf("%s: %d comprobaciones, %d fallos\n", __FILE__, tu_checks, tu_fails),        \
     tu_fails ? 1 : 0)

/* ---- Números aleatorios ------------------------------------------------- */
static inline uint32_t tu_rand(uint32_t *s)
{
    *s ^= *s << 13; /* xorshift32 */
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* Gaussiana N(0,1) por Box-Muller. */
static inline double tu_gauss(uint32_t *s)
{
    double u1 = (tu_rand(s) + 1.0) / 4294967297.0, u2 = (tu_rand(s) + 1.0) / 4294967297.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---- Canal simulado ----------------------------------------------------- */
static inline void chan_awgn(cf32 *x, int n, double snr_db, uint32_t *seed)
{
    double pmax = 0;
    for (int i = 0; i < n; i++)
        pmax = fmax(pmax, (double)x[i].re * x[i].re + (double)x[i].im * x[i].im);
    double sum = 0;
    long cnt = 0;
    for (int i = 0; i < n; i++) {
        double p = (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
        if (p >= 0.25 * pmax) {
            sum += p;
            cnt++;
        }
    }
    double psig = cnt ? sum / cnt : 1.0;
    double sigma = sqrt(psig / pow(10.0, snr_db / 10.0) / 2.0); /* por componente */
    for (int i = 0; i < n; i++) {
        x[i].re += (float)(sigma * tu_gauss(seed));
        x[i].im += (float)(sigma * tu_gauss(seed));
    }
}

static inline void chan_cfo(cf32 *x, int n, double fs, double hz)
{
    for (int i = 0; i < n; i++) {
        double a = 2.0 * M_PI * hz * (double)i / fs;
        float c = (float)cos(a), s = (float)sin(a);
        float re = x[i].re * c - x[i].im * s, im = x[i].re * s + x[i].im * c;
        x[i].re = re;
        x[i].im = im;
    }
}

/* out debe tener sitio para n·ratio + 4 muestras. Devuelve las escritas. */
static inline int chan_resample(const cf32 *in, int n, double ratio, cf32 *out)
{
    int m = (int)((double)(n - 4) * ratio);
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

/* Misma longitud de salida que de entrada (rellena con ceros el final). */
static inline void chan_drift(const cf32 *in, int n, double ppm, cf32 *out)
{
    /* chan_resample escribe (n-4)·ratio muestras: con ppm > 0 y n grande supera n+8,
     * así que el tamaño debe depender de ratio (antes: desbordamiento de montón). */
    double ratio = 1.0 + ppm * 1e-6;
    cf32 *tmp = malloc(sizeof(cf32) * ((size_t)((double)n * (ratio > 1.0 ? ratio : 1.0)) + 16));
    int m = chan_resample(in, n, ratio, tmp);
    for (int i = 0; i < n; i++)
        out[i] = i < m ? tmp[i] : (cf32){0, 0};
    free(tmp);
}

static inline void cf32_to_q11(const cf32 *in, int n, double scale, int16_t *out)
{
    for (int i = 0; i < n; i++) {
        double r = in[i].re * scale * 2048.0, q = in[i].im * scale * 2048.0;
        out[2 * i] = (int16_t)lrint(fmax(-2048, fmin(2047, r)));
        out[2 * i + 1] = (int16_t)lrint(fmax(-2048, fmin(2047, q)));
    }
}

/* ---- Recogida de eventos ------------------------------------------------ */
typedef struct {
    char *data; /* un evento JSON por línea */
    size_t len, cap;
    int count;
} evs_t;

static inline void tu_collect(void *ctx, const char *json, size_t len)
{
    evs_t *e = ctx;
    if (e->len + len + 2 > e->cap) {
        e->cap = (e->len + len + 2) * 2;
        e->data = realloc(e->data, e->cap);
    }
    memcpy(e->data + e->len, json, len);
    e->len += len;
    e->data[e->len++] = '\n';
    e->data[e->len] = '\0';
    e->count++;
}

static inline void evs_free(evs_t *e)
{
    free(e->data);
    memset(e, 0, sizeof *e);
}

static inline int evs_count(const evs_t *e) { return e->count; }
static inline bool ev_has(const evs_t *e, const char *needle) { return e->data && strstr(e->data, needle) != NULL; }

static inline void run_narrow(const decoder_desc_t *d, const cf32 *iq, int n, int64_t freq, const char *opts,
                              evs_t *out)
{
    char err[200] = "";
    dec_cfg_t cfg = {.rate = d->rate, .freq_hz = freq, .center_hz = freq, .opts = opts};
    void *st = d->create(&cfg, err, sizeof err);
    if (!st) {
        printf("  (create falló: %s)\n", err);
        return;
    }
    for (int off = 0; off < n; off += 4096)
        d->process(st, iq + off, n - off < 4096 ? n - off : 4096, (double)off / d->rate, tu_collect, out);
    d->destroy(st);
}

static inline void run_wide(const decoder_desc_t *d, const int16_t *iq, int n, double rate, int64_t center,
                            const char *opts, evs_t *out)
{
    char err[200] = "";
    dec_cfg_t cfg = {.rate = rate, .freq_hz = center, .center_hz = center, .opts = opts};
    void *st = d->create(&cfg, err, sizeof err);
    if (!st) {
        printf("  (create falló: %s)\n", err);
        return;
    }
    for (int off = 0; off < n; off += 65536)
        d->process_raw(st, iq + 2 * (size_t)off, n - off < 65536 ? n - off : 65536, (double)off / rate, tu_collect,
                       out);
    d->destroy(st);
}

#endif
