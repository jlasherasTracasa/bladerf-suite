/*
 * test_nr.c — Búsqueda de SSB de 5G NR con el modulador gemelo.
 *
 * Vectores de la norma: 3GPP TS 38.211 §7.4.2.2 / §7.4.2.3: se comprueban las
 * propiedades de las secuencias (127 elementos ±1, m-secuencia de periodo 127: la
 * autocorrelación periódica es −1 fuera de cero; 3 PSS distintas desplazadas 43) y
 * la rejilla GSCN de TS 38.104 tabla 5.4.3.1-1 (3000 MHz + N·1,44 MHz; N·1,2 MHz + M·50 kHz).
 * No hay grabaciones reales offline: no se verifica contra una señal de una red.
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/nr_phy.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_NR;
extern const modulator_desc_t MOD_NR;

/* Sintetiza un SSB, lo desplaza 'off_hz' (+ cfo), añade ruido y deriva y lo cuantiza a Q11. */
static int16_t *make(const char *params, double rate, double off_hz, double cfo, double ppm, double snr, uint32_t seed,
                     int *n_out)
{
    iqbuf_t b = {0};
    char err[128] = "";
    if (MOD_NR.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        return NULL;
    }
    int n = b.n;
    cf32 *x = b.s;
    if (ppm != 0) {
        cf32 *y = malloc(sizeof(cf32) * (size_t)(n + n / 1000 + 64));
        int m = chan_resample(x, n, 1.0 + ppm * 1e-6, y);
        free(x);
        x = y;
        n = MIN(n, m);
    }
    chan_cfo(x, n, rate, off_hz + cfo);
    chan_awgn(x, n, snr, &seed);
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
    cf32_to_q11(x, n, 0.5, q);
    free(x);
    *n_out = n;
    return q;
}

static double get_num(const char *s, const char *key)
{
    const char *p = s ? strstr(s, key) : NULL;
    return p ? atof(p + strlen(key)) : -1e18;
}

static void run_case(const char *name, const char *params, double rate, int64_t center, int64_t gscn, double cfo, double ppm,
                     int want_pci, int want_scs)
{
    int n;
    int16_t *q = make(params, rate, (double)(gscn - center), cfo, ppm, 10, 77u + (uint32_t)want_pci, &n);
    evs_t evs = {0};
    run_wide(&DEC_NR, q, n, rate, center, "{\"period\":0}", &evs);
    printf("  [%s] eventos=%d\n", name, evs_count(&evs));
    if (evs.data)
        printf("    %.*s\n", (int)(strchr(evs.data, '\n') - evs.data), evs.data);
    char key[64];
    snprintf(key, sizeof key, "\"pci\":%d,", want_pci);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"freq_hz\":%lld,", (long long)gscn);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"scs_khz\":%d,", want_scs);
    CHECK(ev_has(&evs, key));
    double c = get_num(evs.data, "\"cfo_hz\":");
    if (c > -1e17)
        CHECK_NEAR(c, cfo, 600.0);
    evs_free(&evs);
    free(q);
}

int main(void)
{
    /* ---- Secuencias y rejilla ---- */
    int8_t a[127], b[127];
    for (int u = 0; u < 3; u++) {
        nr_pss_seq(u, a);
        int sum = 0;
        for (int i = 0; i < 127; i++)
            sum += a[i];
        CHECK_EQ(sum, -1);          /* m-secuencia: 63 unos y 64 menos unos */
        for (int sh = 1; sh < 127; sh += 31) {
            int c = 0;
            for (int i = 0; i < 127; i++)
                c += a[i] * a[(i + sh) % 127];
            CHECK_EQ(c, -1);        /* autocorrelación periódica ideal */
        }
    }
    nr_pss_seq(0, a);
    nr_pss_seq(1, b);
    for (int i = 0; i < 127; i++)
        CHECK_EQ(b[i], a[(i + 43) % 127]);
    nr_sss_seq(0, 0, a);
    nr_sss_seq(1, 0, b);
    int dot = 0;
    for (int i = 0; i < 127; i++)
        dot += a[i] * b[i];
    CHECK(abs(dot) < 40);
    {
        int64_t g[16];
        int n = nr_gscn_candidates(3495e6, 3504e6, g, 16);
        CHECK_EQ(n, 7);
        CHECK_EQ(g[0], 3495360000LL);   /* 3000 MHz + 344·1,44 MHz */
        CHECK_EQ(g[1] - g[0], 1440000);
        n = nr_gscn_candidates(1.0e6, 3.0e6, g, 16);
        CHECK_EQ(g[0], 1250000LL);
    }

    /* ---- Señales sintéticas ---- */
    const int64_t C = 3500000000LL;
    const int64_t G0 = 3000000000LL + 347LL * 1440000;   /* 3 499 680 000 */
    run_case("SCS 30, 15,36 Msps, SNR 10", "{\"pci\":321,\"scs\":30,\"nssb\":2,\"periods\":3}", 15360000, C, G0, 0, 0, 321, 30);
    run_case("SCS 30, CFO +7,5 kHz", "{\"pci\":1005,\"scs\":30,\"nssb\":1,\"periods\":3}", 15360000, C, G0, 7500, 0, 1005, 30);
    run_case("SCS 30, CFO -7,5 kHz, +100 ppm", "{\"pci\":7,\"scs\":30,\"nssb\":3,\"periods\":3}", 15360000, C, G0, -7500, 100, 7,
             30);
    run_case("SCS 30, CFO +4 kHz, -100 ppm", "{\"pci\":500,\"scs\":30,\"periods\":3}", 15360000, C, G0 + 1440000, 4000, -100, 500, 30);
    run_case("SCS 15, 30,72 Msps", "{\"pci\":812,\"scs\":15,\"rate\":30720000,\"periods\":3}", 30720000, C, G0 + 2 * 1440000, -3000, 0,
             812, 15);
    run_case("SCS 30, 30,72 Msps, GSCN en el borde", "{\"pci\":64,\"scs\":30,\"rate\":30720000,\"periods\":3}", 30720000, C,
             G0 - 6 * 1440000, 2500, 20, 64, 30);

    /* ---- Solo ruido: 0 eventos ---- */
    {
        int n = 1000000;
        int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
        uint32_t seed = 99;
        for (int i = 0; i < 2 * n; i++)
            q[i] = (int16_t)lrint(tu_gauss(&seed) * 200.0);
        evs_t evs = {0};
        run_wide(&DEC_NR, q, n, 15360000, C, "{\"period\":0}", &evs);
        CHECK_EQ(evs_count(&evs), 0);
        evs_free(&evs);
        free(q);
    }
    /* ---- Tasa no admitida ---- */
    {
        dec_cfg_t cfg = {.rate = 20000000, .freq_hz = C, .center_hz = C};
        char err[100] = "";
        void *st = DEC_NR.create(&cfg, err, sizeof err);
        CHECK(st == NULL);
        CHECK(err[0] != 0);
    }
    /* ---- Coste con la configuración por defecto (1 ventana de 25 ms cada max(2, 0,4·nº de GSCN) s) ---- */
    {
        int n;
        int16_t *q = make("{\"pci\":321,\"scs\":30,\"nssb\":2,\"periods\":3}", 15360000, (double)(G0 - C), 2000, 0, 10, 5, &n);
        dec_cfg_t cfg = {.rate = 15360000, .freq_hz = C, .center_hz = C};
        char err[100];
        void *st = DEC_NR.create(&cfg, err, sizeof err);
        evs_t evs = {0};
        clock_t t0 = clock();
        double sig = 0;
        for (int rep = 0; sig < 4.1; rep++) {
            for (int off = 0; off < n; off += 65536)
                DEC_NR.process_raw(st, q + 2 * (size_t)off, MIN(65536, n - off), sig + (double)off / 15360000.0, tu_collect, &evs);
            sig += (double)n / 15360000.0;
        }
        double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("  coste: %.3f s de CPU por s de señal (%.2f s de señal, %d eventos)\n", cpu / sig, sig, evs_count(&evs));
        CHECK(cpu / sig < VR_CPU(0.3));
        CHECK(evs_count(&evs) >= 1);
        DEC_NR.destroy(st);
        evs_free(&evs);
        free(q);
    }
    return TEST_SUMMARY();
}
