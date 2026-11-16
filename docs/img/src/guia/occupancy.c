/* occupancy.c (DOC2b) — medida del ancho ocupado de TX.md sobre una señal
 * generada por NUESTRO modulador. Replica measure_occupancy() de src/tx/tx.c
 * (es static): Welch, FFT 1024, Blackman-Harris, 50 % de solape, bordes con el
 * 0,01 % de la potencia fuera por lado (OCC_TAIL = 1e-4).
 * Salida: <dir>/occ_<id>.csv  (cabecera con rate, lo, hi; luego bin,freq_hz,potencia_rel)
 * Uso: occupancy <id> <dir> */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "dsp/fft.h"
#include "tx/mod.h"

#define OCC_N 1024
#define OCC_TAIL 1e-4

int main(int argc, char **argv)
{
    if (argc < 3) return 1;
    const modulator_desc_t *m = modulator_find(argv[1]);
    if (!m) { fprintf(stderr, "sin modulador %s\n", argv[1]); return 1; }
    iqbuf_t b = {0};
    char err[200] = "";
    if (m->modulate(NULL, &b, err, sizeof err) < 0) { fprintf(stderr, "%s: %s\n", argv[1], err); return 1; }
    fft_plan_t *plan = fft_plan_create(OCC_N);
    float *w = malloc(sizeof(float) * OCC_N);
    cf32 *x = malloc(sizeof(cf32) * OCC_N);
    double *p = calloc(OCC_N, sizeof(double));
    window_blackman_harris(w, OCC_N);
    int step = OCC_N / 2;
    if (b.n > 4000 * step) step = b.n / 4000;
    int nseg = b.n <= OCC_N ? 1 : (b.n - OCC_N) / step + 1;
    for (int sg = 0; sg < nseg; sg++) {
        int s = b.n <= OCC_N ? 0 : (sg * step < b.n - OCC_N ? sg * step : b.n - OCC_N);
        for (int i = 0; i < OCC_N; i++) {
            int in = s + i < b.n;
            x[i].re = in ? b.s[s + i].re * w[i] : 0.f;
            x[i].im = in ? b.s[s + i].im * w[i] : 0.f;
        }
        fft_exec(plan, x);
        for (int k = 0; k < OCC_N; k++) p[k] += (double)x[k].re * x[k].re + (double)x[k].im * x[k].im;
    }
    double tot = 0;
    for (int k = 0; k < OCC_N; k++) tot += p[k];
    double acc = 0, bin = b.rate / OCC_N;
    int klo = 0, khi = OCC_N - 1;
    for (int j = 0; j < OCC_N; j++) {
        acc += p[(j + OCC_N / 2) % OCC_N];
        if (acc >= OCC_TAIL * tot) { klo = j; break; }
    }
    acc = 0;
    for (int j = OCC_N - 1; j >= 0; j--) {
        acc += p[(j + OCC_N / 2) % OCC_N];
        if (acc >= OCC_TAIL * tot) { khi = j; break; }
    }
    double lo = (klo - OCC_N / 2) * bin, hi = (khi + 1 - OCC_N / 2) * bin;
    char path[512];
    snprintf(path, sizeof path, "%s/occ_%s.csv", argv[2], argv[1]);
    FILE *o = fopen(path, "w");
    fprintf(o, "# id=%s rate=%.1f samples=%d lo=%.1f hi=%.1f occ_bw=%.1f\n", argv[1], b.rate, b.n, lo, hi, hi - lo);
    for (int j = 0; j < OCC_N; j++)
        fprintf(o, "%d,%.2f,%.6e\n", j, (j - OCC_N / 2) * bin, p[(j + OCC_N / 2) % OCC_N] / tot);
    fclose(o);
    printf("%s rate=%.0f n=%d ancho ocupado %.0f Hz [%.0f, %.0f]\n", argv[1], b.rate, b.n, hi - lo, lo, hi);
    return 0;
}
