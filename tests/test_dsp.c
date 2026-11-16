/*
 * test_dsp.c — Pruebas de los bloques DSP de A1: FFT, firdec, cresamp y DDC.
 */
#include "test_util.h"

#include "dsp/ddc.h"
#include "dsp/fft.h"
#include "dsp/filter.h"

/* Potencia (amplitud al cuadrado) de la componente a 'hz' por correlación. */
static double tone_power(const cf32 *x, int n, double fs, double hz)
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        double a = -2.0 * M_PI * hz * i / fs, c = cos(a), s = sin(a);
        re += x[i].re * c - x[i].im * s;
        im += x[i].re * s + x[i].im * c;
    }
    re /= n;
    im /= n;
    return re * re + im * im;
}

static cf32 *make_tone(int n, double fs, double hz, double amp)
{
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    for (int i = 0; i < n; i++) {
        double a = 2.0 * M_PI * hz * i / fs;
        x[i].re = (float)(amp * cos(a));
        x[i].im = (float)(amp * sin(a));
    }
    return x;
}

static void test_fft(void)
{
    printf("FFT de un tono\n");
    const int N = 1024, bin = 100;
    fft_plan_t *p = fft_plan_create(N);
    cf32 *x = make_tone(N, N, bin, 1.0); /* exactamente 100 ciclos */
    fft_exec(p, x);
    int best = 0;
    double bm = 0;
    for (int k = 0; k < N; k++) {
        double m = hypot(x[k].re, x[k].im);
        if (m > bm) {
            bm = m;
            best = k;
        }
    }
    CHECK_EQ(best, bin);
    CHECK_NEAR(bm, N, 0.5);
    double leak = 0;
    for (int k = 0; k < N; k++)
        if (k != bin)
            leak = fmax(leak, hypot(x[k].re, x[k].im));
    CHECK(leak < 1e-2);
    free(x);
    fft_plan_destroy(p);
}

static void test_firdec(void)
{
    printf("firdec: banda de paso y de rechazo\n");
    const double fs = 1e6;
    const int n = 200000;
    firdec_t f;
    CHECK(firdec_init(&f, 8, fs, 20e3, 801) == 0);
    cf32 *x = make_tone(n, fs, 5e3, 1.0), *y = malloc(sizeof(cf32) * (n / 8 + 4));
    int m = firdec_process(&f, x, n, y);
    CHECK_EQ(m, n / 8);
    double g = tone_power(y + 1000, m - 1000, fs / 8, 5e3);
    CHECK_NEAR(g, 1.0, 0.02);
    firdec_free(&f);
    free(x);
    /* 140 kHz aliasearía a 15 kHz tras ÷8: debe quedar ≥ 60 dB abajo */
    CHECK(firdec_init(&f, 8, fs, 20e3, 801) == 0);
    x = make_tone(n, fs, 140e3, 1.0);
    m = firdec_process(&f, x, n, y);
    double a = tone_power(y + 1000, m - 1000, fs / 8, 15e3);
    CHECK(10 * log10(a + 1e-30) < -60);
    firdec_free(&f);
    free(x);
    free(y);
}

static void test_cresamp(void)
{
    printf("cresamp: tasa exacta\n");
    struct { double in, out, tone; } cases[] = {{1e6, 48000, 3000}, {52083.0, 48000, 2000}, {48000, 250000, 5000},
                                                 {2.4e6, 250000, 40000}};
    for (size_t c = 0; c < ARRAY_LEN(cases); c++) {
        const double fi = cases[c].in, fo = cases[c].out;
        const int n = (int)(fi * 2.0); /* 2 s de señal */
        cresamp_t r;
        CHECK(cresamp_init(&r, fi, fo, 0.35 * fmin(fi, fo), 0.1 * fmin(fi, fo)) == 0);
        cf32 *x = make_tone(n, fi, cases[c].tone, 1.0);
        int cap = (int)(n * fo / fi) + 16;
        cf32 *y = malloc(sizeof(cf32) * (size_t)cap);
        int m = 0;
        for (int off = 0; off < n; off += 4000) /* por bloques: el estado debe conservarse */
            m += cresamp_process(&r, x + off, MIN(4000, n - off), y + m);
        /* nº de salidas = n·fo/fi con error < 1 muestra: la tasa es exacta */
        CHECK_NEAR(m, n * fo / fi, 1.5);
        /* la frecuencia del tono se conserva: se mide con la fase entre muestras */
        int s = m / 4, e = m - 10;
        double ph = 0;
        for (int i = s; i < e; i++) {
            double d = atan2((double)y[i + 1].im * y[i].re - (double)y[i + 1].re * y[i].im,
                             (double)y[i + 1].re * y[i].re + (double)y[i + 1].im * y[i].im);
            ph += d;
        }
        double f_est = ph / (e - s) * fo / (2 * M_PI);
        CHECK_NEAR(f_est, cases[c].tone, 0.5);
        double amp = sqrt(tone_power(y + s, e - s, fo, cases[c].tone));
        CHECK_NEAR(amp, 1.0, 0.02);
        cresamp_free(&r);
        free(x);
        free(y);
    }
}

static void test_ddc(void)
{
    printf("DDC: lleva el tono a 0 Hz y atenúa lo de fuera\n");
    const double fs = 2.4e6, fo = 48000, bw = 16000;
    const int64_t center = 100000000, freq = 100300000;
    const int n = 2400000;
    ddc_t *d = ddc_create(fs, center, freq, fo, bw);
    CHECK(d != NULL);
    if (!d)
        return;
    /* tono útil a +2 kHz del canal y un interferente fuerte a +30 kHz (fuera de ±8 kHz) */
    cf32 *x = make_tone(n, fs, 300e3 + 2e3, 0.5), *z = make_tone(n, fs, 300e3 + 30e3, 0.5);
    for (int i = 0; i < n; i++) {
        x[i].re += z[i].re;
        x[i].im += z[i].im;
    }
    cf32 *y = malloc(sizeof(cf32) * (ddc_max_out(d, n) + 16));
    int m = 0;
    for (int off = 0; off < n; off += 20000)
        m += ddc_process(d, x + off, MIN(20000, n - off), y + m, (int)ddc_max_out(d, MIN(20000, n - off)));
    CHECK_NEAR(m, n * fo / fs, 3.0); /* tasa de salida exacta */
    int s = 4000, len = m - s;
    double useful = tone_power(y + s, len, fo, 2e3);
    double inter = tone_power(y + s, len, fo, 30e3 - fo); /* +30 kHz aliasea a −18 kHz a 48 ksps */
    CHECK_NEAR(sqrt(useful), 0.5, 0.02);
    double rej = 10 * log10((inter + 1e-30) / useful);
    printf("  interferente a +30 kHz: %.1f dB respecto al tono útil\n", rej);
    CHECK(rej < -50);

    /* ddc_retune sin clics: se sintoniza justo sobre el tono → continua constante */
    ddc_retune(d, center, freq + 2000);
    int m2 = 0;
    cf32 *y2 = malloc(sizeof(cf32) * (ddc_max_out(d, 100000) + 16));
    m2 = ddc_process(d, x, 100000, y2, (int)ddc_max_out(d, 100000));
    double mag_min = 1e9, mag_max = 0;
    for (int i = m2 / 2; i < m2; i++) { /* tras el transitorio del filtro */
        double mg = hypot(y2[i].re, y2[i].im);
        mag_min = fmin(mag_min, mg);
        mag_max = fmax(mag_max, mg);
    }
    CHECK(mag_min > 0.4);
    CHECK(mag_max < 0.65);
    ddc_destroy(d);
    free(x);
    free(z);
    free(y);
    free(y2);

    /* sin remuestreo: fs/fo entero (250 ksps → 50 ksps) y descomposición de ÷5 */
    d = ddc_create(250000, 0, 0, 50000, 20000);
    CHECK(d != NULL);
    ddc_destroy(d);
    /* parámetros imposibles */
    CHECK(ddc_create(48000, 0, 0, 250000, 10000) == NULL);
}

int main(void)
{
    test_fft();
    test_firdec();
    test_cresamp();
    test_ddc();
    return TEST_SUMMARY();
}
