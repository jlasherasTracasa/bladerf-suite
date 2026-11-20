/*
 * afsk.c — Ver afsk.h.
 */
#include "afsk.h"

#include <stdlib.h>

struct afsk {
    int n;                     /* muestras por símbolo (ventana) */
    double fs, step, gain0, inertia;
    double w1, w0;             /* fase por muestra de cada tono */
    double ph1, ph0;           /* fase actual */
    double *r1, *r0;           /* productos (re,im) de la ventana, anillo n×2 */
    int pos;
    double s1r, s1i, s0r, s0i; /* sumas deslizantes */
    long count;
    long smp;                  /* muestras consumidas (incluida la actual) */
    double pll;
    int prev;
};

afsk_t *afsk_create(double fs, double baud, double f1, double f0, double gain0, double inertia)
{
    afsk_t *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->n = (int)lrint(fs / baud);
    a->fs = fs;
    a->step = baud / fs;
    a->gain0 = gain0;
    a->inertia = inertia;
    a->w1 = 2.0 * M_PI * f1 / fs;
    a->w0 = 2.0 * M_PI * f0 / fs;
    a->r1 = calloc((size_t)a->n * 2, sizeof(double));
    a->r0 = calloc((size_t)a->n * 2, sizeof(double));
    if (!a->r1 || !a->r0) {
        afsk_destroy(a);
        return NULL;
    }
    return a;
}

void afsk_destroy(afsk_t *a)
{
    if (!a)
        return;
    free(a->r1);
    free(a->r0);
    free(a);
}

long afsk_index(const afsk_t *a) { return a->smp - 1; }

void afsk_process(afsk_t *a, const float *x, int n, afsk_bit_fn fn, void *ctx)
{
    for (int i = 0; i < n; i++) {
        a->smp++;
        double v = x[i];
        double c1 = cos(a->ph1), s1 = -sin(a->ph1), c0 = cos(a->ph0), s0 = -sin(a->ph0);
        double *q1 = &a->r1[2 * a->pos], *q0 = &a->r0[2 * a->pos];
        a->s1r += v * c1 - q1[0];
        a->s1i += v * s1 - q1[1];
        a->s0r += v * c0 - q0[0];
        a->s0i += v * s0 - q0[1];
        q1[0] = v * c1;
        q1[1] = v * s1;
        q0[0] = v * c0;
        q0[1] = v * s0;
        if (++a->pos == a->n)
            a->pos = 0;
        a->ph1 = fmod(a->ph1 + a->w1, 2.0 * M_PI);
        a->ph0 = fmod(a->ph0 + a->w0, 2.0 * M_PI);
        if (++a->count >= (1L << 20)) {   /* recalcula las sumas: evita deriva numérica */
            a->count = 0;
            a->s1r = a->s1i = a->s0r = a->s0i = 0;
            for (int k = 0; k < a->n; k++) {
                a->s1r += a->r1[2 * k];
                a->s1i += a->r1[2 * k + 1];
                a->s0r += a->r0[2 * k];
                a->s0i += a->r0[2 * k + 1];
            }
        }
        double e1 = a->s1r * a->s1r + a->s1i * a->s1i, e0 = a->s0r * a->s0r + a->s0i * a->s0i;
        int d = (e1 - a->gain0 * e0) > 0 ? 1 : 0;
        if (d != a->prev) {
            a->pll *= a->inertia;
            a->prev = d;
        }
        a->pll += a->step;
        if (a->pll >= 0.5) {
            a->pll -= 1.0;
            fn(ctx, d);
        }
    }
}
