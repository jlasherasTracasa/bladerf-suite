/*
 * test_pulse.c — Pruebas de src/dsp/pulse.*: RRC, RC, gaussiano, FIR real y
 * complejo (diezmado, bloques) y conformación TX.
 */
#include "dsp/pulse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double dtft_mag(const float *h, int n, double f)   /* f en ciclos/muestra */
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        re += h[i] * cos(2 * M_PI * f * i);
        im -= h[i] * sin(2 * M_PI * f * i);
    }
    return sqrt(re * re + im * im);
}

static void test_shapes(void)
{
    enum { SPS = 8, SPAN = 16, N = SPS * SPAN + 1 };
    float rrc[N], rc[N], g[N];
    CHECK(pulse_rrc(rrc, 0.35, SPS, SPAN, PULSE_NORM_ENERGY) == N);
    CHECK(pulse_rc(rc, 0.35, SPS, SPAN, PULSE_NORM_PEAK) == N);
    double e = 0;
    for (int i = 0; i < N; i++) e += rrc[i] * rrc[i];
    CHECK(fabs(e - 1.0) < 1e-5);
    /* RC: ceros exactos en los múltiplos de T */
    double worst = 0;
    for (int k = 1; k < SPAN / 2; k++)
        worst = fmax(worst, fabs(rc[N / 2 + k * SPS]));
    CHECK(fabs(rc[N / 2] - 1.0f) < 1e-6 && worst < 1e-6);
    /* RRC * RRC = RC (cero ISI): convolución en los instantes de símbolo */
    double pk = 0, isi = 0;
    for (int k = -4; k <= 4; k++) {
        double acc = 0;
        for (int i = 0; i < N; i++) {
            int j = i + k * SPS;
            if (j >= 0 && j < N) acc += (double)rrc[i] * rrc[j];
        }
        if (k == 0) pk = acc; else isi = fmax(isi, fabs(acc));
    }
    printf("RRC*RRC: pico %.4f, ISI maxima %.2e\n", pk, isi);
    CHECK(isi < 5e-3);
    /* casos singulares del RRC (t = 1/(4β)) y β = 0 / 1 */
    float tmp[N];
    CHECK(pulse_rrc(tmp, 0.25, SPS, SPAN, PULSE_NORM_DC) == N);
    for (int i = 0; i < N; i++) CHECK(isfinite(tmp[i]));
    CHECK(pulse_rrc(tmp, 1.0, SPS, SPAN, PULSE_NORM_DC) == N && pulse_rc(tmp, 0.0, SPS, SPAN, PULSE_NORM_DC) == N);
    CHECK(pulse_rrc(tmp, 1.5, SPS, SPAN, PULSE_NORM_DC) == -1);
    /* Gaussiano: ganancia a BT/T = −3 dB respecto a continua */
    for (int bi = 0; bi < 3; bi++) {
        double bt = bi == 0 ? 0.3 : bi == 1 ? 0.5 : 1.0;
        pulse_gaussian(g, bt, SPS, SPAN, PULSE_NORM_DC);
        double r = dtft_mag(g, N, bt / SPS) / dtft_mag(g, N, 0);
        CHECK(fabs(r - 0.7071) < 0.01);
    }
    float gm[N + SPS];
    int ng = pulse_gmsk(gm, 0.3, SPS, SPAN);
    double s = 0;
    for (int i = 0; i < ng; i++) s += gm[i];
    CHECK(ng == N + SPS - 1 && fabs(s - 1.0) < 1e-5);
}

static void test_fir(void)
{
    float h[21];
    for (int i = 0; i < 21; i++) h[i] = (float)sin(0.3 * i + 0.1) / (1.0f + i);
    enum { N = 1000 };
    static float x[N], y_ref[N], y[N];
    static cf32 xc[N], yc[N];
    for (int i = 0; i < N; i++) {
        x[i] = (float)rand() / RAND_MAX - 0.5f;
        xc[i].re = x[i];
        xc[i].im = (float)rand() / RAND_MAX - 0.5f;
    }
    for (int i = 0; i < N; i++) {
        double a = 0;
        for (int k = 0; k < 21; k++)
            if (i - k >= 0) a += (double)h[k] * x[i - k];
        y_ref[i] = (float)a;
    }
    fir_f_t f;
    CHECK(fir_f_init(&f, h, 21, 1) == 0);
    int w = 0, pos = 0;
    while (pos < N) {
        int b = 1 + rand() % 97;
        b = MIN(N - pos, b);
        w += fir_f_process(&f, x + pos, b, y + w);
        pos += b;
    }
    double err = 0;
    for (int i = 0; i < N; i++) err = fmax(err, fabs(y[i] - y_ref[i]));
    CHECK(w == N && err < 1e-5);
    fir_f_free(&f);
    /* diezmado por 5: salidas = y_ref[4], y_ref[9], … */
    CHECK(fir_f_init(&f, h, 21, 5) == 0);
    w = fir_f_process(&f, x, N, y);
    err = 0;
    for (int i = 0; i < w; i++) err = fmax(err, fabs(y[i] - y_ref[5 * i + 4]));
    CHECK(w == N / 5 && err < 1e-5);
    fir_f_reset(&f);
    fir_f_free(&f);
    /* complejo × real */
    fir_cf_t c;
    CHECK(fir_cf_init(&c, h, 21, 4) == 0);
    int wc = 0;
    pos = 0;
    while (pos < N) {
        int b = 1 + rand() % 53;
        b = MIN(N - pos, b);
        wc += fir_cf_process(&c, xc + pos, b, yc + wc);
        pos += b;
    }
    CHECK(wc == N / 4);
    err = 0;
    for (int i = 0; i < wc; i++) {
        int n = 4 * i + 3;
        double ar = 0, ai = 0;
        for (int k = 0; k < 21; k++)
            if (n - k >= 0) { ar += (double)h[k] * xc[n - k].re; ai += (double)h[k] * xc[n - k].im; }
        err = fmax(err, fmax(fabs(yc[i].re - ar), fabs(yc[i].im - ai)));
    }
    CHECK(err < 1e-5);
    fir_cf_free(&c);
    CHECK(fir_f_init(&f, h, 0, 1) == -1);
}

static void test_shape(void)
{
    enum { SPS = 4, SPAN = 8, NS = 20 };
    float h[SPS * SPAN + 1];
    pulse_rrc(h, 0.35, SPS, SPAN, PULSE_NORM_ENERGY);
    float sym[NS], out[NS * SPS + SPS * SPAN];
    for (int i = 0; i < NS; i++) sym[i] = (i * 7 % 3) - 1.0f;
    int n = pulse_shape_f(sym, NS, h, SPS * SPAN + 1, SPS, out);
    CHECK(n == NS * SPS + SPS * SPAN);
    /* referencia: zero-stuffing + FIR */
    float up[NS * SPS + SPS * SPAN] = {0};
    for (int i = 0; i < NS; i++) up[i * SPS] = sym[i];
    double err = 0;
    for (int i = 0; i < n; i++) {
        double a = 0;
        for (int k = 0; k <= SPS * SPAN; k++)
            if (i - k >= 0) a += (double)h[k] * up[i - k];
        err = fmax(err, fabs(a - out[i]));
    }
    CHECK(err < 1e-5);
    cf32 cs[NS], co[NS * SPS + SPS * SPAN];
    for (int i = 0; i < NS; i++) { cs[i].re = sym[i]; cs[i].im = -sym[i]; }
    CHECK(pulse_shape_cf(cs, NS, h, SPS * SPAN + 1, SPS, co) == n);
    CHECK(fabsf(co[10].re - out[10]) < 1e-6f && fabsf(co[10].im + out[10]) < 1e-6f);
}

int main(void)
{
    test_shapes();
    test_fir();
    test_shape();
    printf("test_pulse: %d comprobaciones, %d fallos\n", g_n, g_fail);
    return g_fail != 0;
}
