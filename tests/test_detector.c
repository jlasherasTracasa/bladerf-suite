/*
 * test_detector.c — Detector de señales: fusión, bordes e histéresis.
 *
 * Se sintetiza una banda de 2,4 Msps con 5 emisoras WFM (portadora modulada en
 * frecuencia por 8 tonos de audio, desviación máx. 75 kHz → ancho ≈ 180 kHz),
 * ruido blanco y, en los 5 % de cada borde, un "aumento" de +12 dB que imita
 * la cola del filtro antialias del AD9361. Se calcula el espectro con la FFT
 * propia (Blackman-Harris, 4096 puntos) y se alimenta al detector como hace el
 * motor (feed por imagen, run cada pocas imágenes).
 * Criterios: un marcador por emisora (±30 kHz), 0 en los bordes, y la lista
 * estable entre imágenes. Si existe la grabación del webtest (build/webtest/fm.sigmf-*,
 * RDS sintético de --gen) se comprueba que su emisora da un único marcador.
 */
#include "test_util.h"

#include "dsp/detector.h"
#include "dsp/fft.h"

#define N 4096
#define FS 2400000.0

static uint32_t rng = 12345;
static double urand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) / 16777216.0;
}
static double grand(void)
{
    return sqrt(-2 * log(urand() + 1e-12)) * cos(2 * M_PI * urand());
}

static const double ST[] = {-800e3, -400e3, 250e3, 600e3, 900e3};
#define NST 5

static void gen_band(cf32 *x, long n0, int n)
{
    static double ph[NST], tf[NST][8], tp[NST][8];
    static bool init;
    if (!init) {
        for (int s = 0; s < NST; s++)
            for (int k = 0; k < 8; k++) {
                tf[s][k] = 300 + 2000 * k + 700 * urand();
                tp[s][k] = 2 * M_PI * urand();
            }
        init = true;
    }
    for (int i = 0; i < n; i++) {
        double t = (double)(n0 + i) / FS;
        cf32 acc = {(float)(grand() * 0.002), (float)(grand() * 0.002)};
        for (int s = 0; s < NST; s++) {
            double m = 0;
            for (int k = 0; k < 8; k++)
                m += sin(2 * M_PI * tf[s][k] * t + tp[s][k]);
            m /= 3.0; /* ≈ ±1 de pico típico */
            m = CLAMP(m, -1.0, 1.0);
            ph[s] += 2 * M_PI * (ST[s] + 75e3 * m) / FS;
            ph[s] = fmod(ph[s], 2 * M_PI);
            acc.re += 0.05f * cosf((float)ph[s]);
            acc.im += 0.05f * sinf((float)ph[s]);
        }
        x[i] = acc;
    }
}

static void spectrum_db(fft_plan_t *p, const float *w, const cf32 *x, float *db, bool edge_boost)
{
    cf32 b[N];
    for (int i = 0; i < N; i++) {
        b[i].re = x[i].re * w[i];
        b[i].im = x[i].im * w[i];
    }
    fft_exec(p, b);
    for (int k = 0; k < N; k++) {
        int bin = (k + N / 2) % N; /* DC al centro */
        float pw = (b[k].re * b[k].re + b[k].im * b[k].im) / ((float)N * N);
        db[bin] = 10.f * log10f(pw + 1e-20f);
        if (edge_boost && abs(bin - N / 2) > (int)(0.46 * N))
            db[bin] += 12.f;
    }
}

static int near_station(double off)
{
    for (int s = 0; s < NST; s++)
        if (fabs(off - ST[s]) < 30e3)
            return s;
    return -1;
}

static void synthetic(void)
{
    fft_plan_t *p = fft_plan_create(N);
    float w[N];
    window_blackman_harris(w, N);
    detector_t *d = detector_create(N);
    cf32 x[N];
    float db[N];
    det_signal_t sg[96];
    long n0 = 0;
    int stable_bad = 0, runs = 0, edge_hits = 0, dup = 0, miss = 0;
    double first[96];
    int nfirst = 0;
    for (int f = 0; f < 320; f++) {
        gen_band(x, n0, N);
        n0 += N;
        spectrum_db(p, w, x, db, true);
        detector_feed(d, db, N, 40.f);
        if (f % 8 != 7 || f < 120)
            continue;
        int n = detector_run(d, FS, 8.f, sg, 96, NULL);
        if (++runs == 1)
            continue; /* la histéresis exige 2 imágenes: la primera aún no muestra nada */
        int seen[NST] = {0};
        if (getenv("DETDBG")) for (int i = 0; i < n; i++) printf("   [%d] off=%.0f bw=%.0f snr=%.1f\n", runs, sg[i].offset_hz, sg[i].bw_hz, sg[i].snr_db);
        for (int i = 0; i < n; i++) {
            if (fabs(sg[i].offset_hz) > FS * 0.45)
                edge_hits++;
            int s = near_station(sg[i].offset_hz);
            if (s >= 0)
                seen[s]++;
            else
                miss++; /* marcador sin emisora */
        }
        for (int s = 0; s < NST; s++)
            if (seen[s] != 1)
                dup++;
        if (runs == 2) {
            nfirst = n;
            for (int i = 0; i < n; i++)
                first[i] = sg[i].offset_hz;
        } else {
            if (n != nfirst)
                stable_bad++;
            else
                for (int i = 0; i < n; i++)
                    if (fabs(sg[i].offset_hz - first[i]) > 15e3)
                        stable_bad++;
        }
    }
    printf("  sintético: %d imágenes, bordes=%d, sin emisora=%d, emisoras ≠1 marcador=%d, inestables=%d\n", runs,
           edge_hits, miss, dup, stable_bad);
    CHECK(runs > 10);
    CHECK_EQ(edge_hits, 0);
    CHECK_EQ(miss, 0);
    CHECK_EQ(dup, 0);
    CHECK_EQ(stable_bad, 0);

    /* Histéresis: tras quitar las emisoras sigue 3 imágenes y desaparece a la 4.ª */
    float noise[N];
    int gone = -1;
    for (int r = 1; r <= 8 && gone < 0; r++) {
        for (int i = 0; i < N; i++)
            noise[i] = -70.f;
        detector_feed(d, noise, N, 1.f);
        if (detector_run(d, FS, 8.f, sg, 96, NULL) == 0)
            gone = r;
    }
    CHECK(gone >= 3 && gone <= 5);
    /* Y no aparece hasta 2 imágenes seguidas */
    gen_band(x, n0, N);
    spectrum_db(p, w, x, db, false);
    detector_feed(d, db, N, 1.f);
    int a1 = detector_run(d, FS, 8.f, sg, 96, NULL);
    detector_feed(d, db, N, 1.f);
    int a2 = detector_run(d, FS, 8.f, sg, 96, NULL);
    CHECK_EQ(a1, 0);
    CHECK(a2 >= 1);
    detector_destroy(d);
    fft_plan_destroy(p);
}

/* Grabación real/sintética del webtest (ci16, una emisora): un marcador exacto */
static void recorded(void)
{
    FILE *f = fopen("build/webtest/fm.sigmf-data", "rb");
    if (!f) {
        printf("  grabación: no disponible (omitida)\n");
        return;
    }
    fft_plan_t *p = fft_plan_create(N);
    float w[N], db[N];
    window_blackman_harris(w, N);
    detector_t *d = detector_create(N);
    int16_t raw[2 * N];
    cf32 x[N];
    det_signal_t sg[96];
    int fr = 0, bad = 0, runs = 0;
    while (fr < 200 && fread(raw, sizeof raw[0], 2 * N, f) == 2 * N) {
        for (int i = 0; i < N; i++) {
            x[i].re = raw[2 * i] / 2048.f;
            x[i].im = raw[2 * i + 1] / 2048.f;
        }
        spectrum_db(p, w, x, db, false);
        detector_feed(d, db, N, 8.f);
        fr++;
        if (fr % 4 == 0 && fr >= 40) {
            int n = detector_run(d, 2.4e6, 8.f, sg, 96, NULL);
            if (++runs == 1)
                continue; /* histéresis: la primera imagen no muestra nada */
            if (getenv("DETDBG")) for (int i = 0; i < n; i++) printf("    [%d] off=%.0f bw=%.0f snr=%.1f\n", fr, sg[i].offset_hz, sg[i].bw_hz, sg[i].snr_db);
            /* La grabación tiene además imágenes espurias reales de la señal sintética
             * (peine de ~34 kHz a −400 kHz, 20 dB): no se exige su ausencia, sí que la
             * emisora de +70 dB sea UN marcador y que no haya ninguno en los bordes. */
            int near = 0;
            for (int i = 0; i < n; i++) {
                if (fabs(sg[i].offset_hz) > 1.08e6)
                    bad++;
                if (sg[i].snr_db > 50)
                    near++;
            }
            if (near != 1)
                bad++;
        }
    }
    fclose(f);
    CHECK(runs > 0);
    CHECK_EQ(bad, 0);
    detector_destroy(d);
    fft_plan_destroy(p);
}

int main(void)
{
    synthetic();
    recorded();
    return TEST_SUMMARY();
}
