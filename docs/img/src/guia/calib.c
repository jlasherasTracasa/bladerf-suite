/* calib.c (DOC2b) — calibración del espectro con NUESTRO spectrum.c (CPU):
 * tono complejo de amplitud conocida (dBFS) → pico medido. Mismo método que
 * tests/test_gpu.c (test_spectrum_calibration), con más niveles y un tono
 * entre bins (festoneo de la ventana). Salida CSV: fft,offset_bins,real,medido */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include "dsp/spectrum.h"

static float peak;
static void on_frame(void *ctx, const float *db, int n, int navg)
{
    (void)ctx; (void)navg;
    float pk = -999.f;
    for (int i = 0; i < n; i++) pk = fmaxf(pk, db[i]);
    peak = pk;
}

static uint32_t rs = 12345;
static double gauss(void)
{
    double u1, u2;
    rs = rs * 1664525u + 1013904223u; u1 = (rs >> 8) / 16777216.0 + 1e-9;
    rs = rs * 1664525u + 1013904223u; u2 = (rs >> 8) / 16777216.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static double measure(int n, double lvl, double off_bins)
{
    const double fs = 2.4e6;
    const long total = 3L * 120000;
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)total);
    double a = 2048.0 * pow(10.0, lvl / 20.0);
    double f = (n / 8.0 + off_bins) / n; /* ciclos por muestra */
    for (long i = 0; i < total; i++) {
        double ph = 2.0 * M_PI * f * (double)i;
        q[2 * i] = (int16_t)lrint(fmin(2047.0, a * cos(ph)) + 0.3 * gauss());
        q[2 * i + 1] = (int16_t)lrint(fmin(2047.0, a * sin(ph)) + 0.3 * gauss());
    }
    spectrum_t *sp = spectrum_create(n, fs, 20, false);
    peak = -999.f;
    for (long off = 0; off < total; off += 40000)
        spectrum_feed(sp, q + 2 * off, (int)fmin(40000, (double)(total - off)), on_frame, NULL);
    spectrum_feed(sp, q, 1, on_frame, NULL);
    spectrum_destroy(sp);
    free(q);
    return peak;
}

int main(void)
{
    printf("fft,offset_bins,real,medido\n");
    const int sizes[] = {1024, 4096, 65536};
    for (int e = 0; e < 3; e++)
        for (double off = 0; off <= 0.5; off += 0.5)
            for (int l = 0; l <= 90; l += 5)
                printf("%d,%.1f,%d,%.3f\n", sizes[e], off, -l, measure(sizes[e], -(double)l, off));
    return 0;
}
