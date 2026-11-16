/*
 * fft.c — Implementación de la FFT radix-2. Ver fft.h para la teoría.
 */
#include "fft.h"

#include <stdlib.h>

bool fft_size_valid(int n) { return n >= 64 && n <= 65536 && (n & (n - 1)) == 0; }

fft_plan_t *fft_plan_create(int n)
{
    if (!fft_size_valid(n))
        return NULL;
    fft_plan_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->n = n;
    while ((1 << p->log2n) < n)
        p->log2n++;
    p->tw = malloc(sizeof(cf32) * (size_t)(n / 2));
    p->rev = malloc(sizeof(int) * (size_t)n);
    if (!p->tw || !p->rev) {
        fft_plan_destroy(p);
        return NULL;
    }
    /* Twiddles calculados en doble precisión para no acumular error. */
    for (int k = 0; k < n / 2; k++) {
        double a = -2.0 * M_PI * k / n;
        p->tw[k].re = (float)cos(a);
        p->tw[k].im = (float)sin(a);
    }
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < p->log2n; b++)
            if (i & (1 << b))
                r |= 1 << (p->log2n - 1 - b);
        p->rev[i] = r;
    }
    return p;
}

void fft_plan_destroy(fft_plan_t *p)
{
    if (!p)
        return;
    free(p->tw);
    free(p->rev);
    free(p);
}

void fft_exec(const fft_plan_t *p, cf32 *a)
{
    const int n = p->n;

    /* 1) Reordenación por inversión de bits: cada par se intercambia una vez. */
    for (int i = 0; i < n; i++) {
        int j = p->rev[i];
        if (j > i) {
            cf32 t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }

    /* 2) Etapas de mariposas. "half" es la distancia entre los dos elementos
     *    de cada mariposa; "step" recorre la tabla de twiddles de N puntos
     *    como si fuese la de m puntos (W_m^j = W_N^{j·N/m}). */
    for (int m = 2; m <= n; m <<= 1) {
        const int half = m >> 1;
        const int step = n / m;
        for (int k = 0; k < n; k += m) {
            for (int j = 0; j < half; j++) {
                const cf32 w = p->tw[j * step];
                cf32 *x = &a[k + j];
                cf32 *y = &a[k + j + half];
                const float tr = w.re * y->re - w.im * y->im;
                const float ti = w.re * y->im + w.im * y->re;
                y->re = x->re - tr;
                y->im = x->im - ti;
                x->re += tr;
                x->im += ti;
            }
        }
    }
}

void window_blackman_harris(float *w, int n)
{
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    for (int i = 0; i < n; i++) {
        double x = 2.0 * M_PI * i / (n - 1);
        w[i] = (float)(a0 - a1 * cos(x) + a2 * cos(2 * x) - a3 * cos(3 * x));
    }
}
