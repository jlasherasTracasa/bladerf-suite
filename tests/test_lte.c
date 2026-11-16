/*
 * test_lte.c — Decodificador LTE (búsqueda de celda + MIB) con el modulador gemelo.
 *
 * VECTOR REAL: tests/data/lte_sigid_vf14_1p92M.cs8 = 50 ms (8 bit con signo) de la grabación pública
 * "LTE_VF_1.4_downlink.zip" de sigidwiki.com (celda LTE de 1,4 MHz, IQ de 8 bit a 1,25 MS/s remuestreada
 * a 1,92 MS/s): PCI 65 (N_ID1 21, N_ID2 2), FDD, CP normal, 1 puerto, MIB de 6 RB; el CRC del PBCH solo
 * pasa si codificación, aleatorizado y adaptación de velocidad son los de TS 36.212 §5.3.1. Una segunda
 * grabación (LTE_OR_10: PCI 44, 50 RB, 2 puertos) se comprobó fuera del test (demasiado grande).
 * Además se comprueban propiedades definidas en 3GPP TS 36.211 (PSS: |d| = 1 y raíces 25/29/34 de la
 * tabla 6.11.1.1; SSS ±1, N_ID1 y subtrama 0/5 dan secuencias distintas y casi ortogonales) y la
 * codificación del PBCH de TS 36.212 §5.3.1 por ida y vuelta con errores de canal. La prueba
 * extremo a extremo es modulador → canal → decodificador (ver docs/decoders/lte.md).
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/lte_phy.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_LTE;
extern const modulator_desc_t MOD_LTE;

/* Genera, recorta 'crop' muestras al principio y aplica el canal. */
static int make_signal(const char *params, int crop, double snr_db, double cfo_hz, double ppm, uint32_t seed,
                       cf32 **out)
{
    iqbuf_t b = {0};
    char err[128] = "";
    if (MOD_LTE.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        return 0;
    }
    int n = b.n - crop;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    memcpy(x, b.s + crop, sizeof(cf32) * (size_t)n);
    iqbuf_free(&b);
    if (ppm != 0) {
        /* chan_drift desborda con ppm > 0 y n grande (n·ppm > 8): se usa chan_resample directo */
        cf32 *y = malloc(sizeof(cf32) * (size_t)(n + n / 1000 + 64));
        int m = chan_resample(x, n, 1.0 + ppm * 1e-6, y);
        free(x);
        x = y;
        n = MIN(n, m);
    }
    if (cfo_hz != 0)
        chan_cfo(x, n, 1920000.0, cfo_hz);
    chan_awgn(x, n, snr_db, &seed);
    *out = x;
    return n;
}

static void case_run(const char *name, const char *params, int crop, double snr, double cfo, double ppm, int want_pci,
                     int want_ports, int want_bw, int want_mib)
{
    cf32 *x;
    int n = make_signal(params, crop, snr, cfo, ppm, 1234u + (uint32_t)crop, &x);
    evs_t evs = {0};
    run_narrow(&DEC_LTE, x, n, 800000000, "{\"period\":0}", &evs);
    printf("  [%s] eventos=%d\n", name, evs_count(&evs));
    if (evs.data)
        printf("    %.*s\n", (int)(strchr(evs.data, '\n') - evs.data), evs.data);
    char key[64];
    snprintf(key, sizeof key, "\"pci\":%d,", want_pci);
    CHECK(ev_has(&evs, key));
    if (want_ports) {
        snprintf(key, sizeof key, "\"ports\":%d", want_ports);
        CHECK(ev_has(&evs, key));
    }
    if (want_mib) {
        CHECK(ev_has(&evs, "\"mib\":true"));
        snprintf(key, sizeof key, "\"bw_rb\":%d", want_bw);
        CHECK(ev_has(&evs, key));
        CHECK(ev_has(&evs, "\"phich_resource\":2"));
    }
    if (evs.data && cfo != 0) {
        const char *c = strstr(evs.data, "\"cfo_hz\":");
        if (c)
            CHECK_NEAR(atof(c + 9), cfo, 400.0);
    }
    evs_free(&evs);
    free(x);
}

int main(void)
{
    /* ---- Vectores de la norma / propiedades ---- */
    int8_t s0[62];
    /* N_ID1 = 0 → (m0,m1) = (0,1), N_ID2 = 0: d(0) = s̃(0)·c̃(0) = +1 */
    lte_sss_seq(0, 0, 0, s0);
    CHECK_EQ(s0[0], 1);      /* s̃(0) = 1−2x(0) = +1 y c̃(0) = +1 */
    for (int u = 0; u < 3; u++) {
        cf32 d[62];
        lte_pss_seq(u, d);
        double mx = 0, mn = 9;
        for (int n = 0; n < 62; n++) {
            double m = hypot(d[n].re, d[n].im);
            mx = fmax(mx, m);
            mn = fmin(mn, m);
        }
        CHECK_NEAR(mx, 1.0, 1e-5);
        CHECK_NEAR(mn, 1.0, 1e-5);
    }
    CHECK_EQ(lte_pss_root(0), 25);
    CHECK_EQ(lte_pss_root(1), 29);
    CHECK_EQ(lte_pss_root(2), 34);
    /* Las SSS de N_ID1 distintos y de subtramas 0/5 son distintas pero con correlación baja */
    {
        int8_t a[62], b[62];
        lte_sss_seq(29, 1, 0, a);
        lte_sss_seq(30, 1, 0, b);   /* (29,30) frente a (0,2): secuencias distintas */
        int dot = 0;
        for (int i = 0; i < 62; i++)
            dot += a[i] * b[i];
        CHECK(abs(dot) < 40);
        lte_sss_seq(29, 1, 0, b);
        dot = 0;
        for (int i = 0; i < 62; i++)
            dot += a[i] * b[i];
        CHECK_EQ(dot, 62);
        lte_sss_seq(29, 1, 1, b);
        dot = 0;
        for (int i = 0; i < 62; i++)
            dot += a[i] * b[i];
        CHECK(abs(dot) < 40);
    }
    /* PBCH: ida y vuelta de la codificación, con 5 bits erróneos en la ráfaga */
    for (int ext = 0; ext < 2; ext++)
        for (int ports = 1; ports <= 4; ports += (ports == 1 ? 1 : 2)) {
            lte_mib_t m = {.bw_rb = 25, .phich_ext = 1, .phich_res = 1, .sfn8 = 0xA5}, r;
            uint8_t bits[1920];
            int E = lte_pbch_encode(&m, ports, 77, ext, bits);
            int eseg = E / 4;
            float soft[480];
            for (int seg = 0; seg < 4; seg += 3) {
                for (int i = 0; i < eseg; i++)
                    soft[i] = bits[seg * eseg + i] ? -1.0f : 1.0f;
                for (int k = 0; k < 5; k++)
                    soft[(k * 97) % eseg] *= -1;
                int pr = lte_pbch_decode(soft, seg, 77, ext, &r);
                CHECK_EQ(pr, ports);
                CHECK_EQ(r.bw_rb, 25);
                CHECK_EQ(r.sfn8, 0xA5);
                CHECK_EQ(r.phich_res, 1);
            }
        }

    /* ---- Señales sintéticas ---- */
    const char *p1 = "{\"pci\":301,\"ports\":1,\"sfn\":100,\"frames\":12,\"bw_rb\":50,\"phich_res\":2}";
    case_run("FDD normal 1p, SNR 10", p1, 5001, 10, 0, 0, 301, 1, 50, 1);
    case_run("FDD normal 1p, CFO +7,5 kHz", p1, 7003, 10, 7500, 0, 301, 1, 50, 1);
    case_run("FDD normal 1p, CFO -7,5 kHz, +100 ppm", p1, 2222, 10, -7500, 100, 301, 1, 50, 1);
    case_run("FDD normal 1p, CFO +3 kHz, -100 ppm", p1, 9999, 10, 3000, -100, 301, 1, 50, 1);
    case_run("FDD extendido 1p",
             "{\"pci\":57,\"cp\":\"extended\",\"ports\":1,\"sfn\":8,\"frames\":12,\"bw_rb\":50,\"phich_res\":2}", 4321,
             10, 2000, 0, 57, 1, 50, 1);
    case_run("TDD normal 2p",
             "{\"pci\":503,\"duplex\":\"tdd\",\"ports\":2,\"sfn\":40,\"frames\":12,\"bw_rb\":50,\"phich_res\":2}", 8888, 10,
             -5000, 50, 503, 2, 50, 1);
    case_run("FDD normal 2p",
             "{\"pci\":2,\"ports\":2,\"sfn\":4,\"frames\":12,\"bw_rb\":50,\"phich_res\":2}", 1500, 10, 4000, 0, 2, 2, 50, 1);
    /* detección de duplexado y CP en el texto */
    {
        cf32 *x;
        int n = make_signal("{\"pci\":503,\"duplex\":\"tdd\",\"cp\":\"extended\",\"frames\":12,\"bw_rb\":50}", 3000, 10,
                            1000, 0, 99, &x);
        evs_t evs = {0};
        run_narrow(&DEC_LTE, x, n, 800000000, "{\"period\":0}", &evs);
        CHECK(ev_has(&evs, "\"duplex\":\"TDD\""));
        CHECK(ev_has(&evs, "\"cp\":\"extended\""));
        CHECK(ev_has(&evs, "\"pci\":503,"));
        evs_free(&evs);
        free(x);
    }
    /* ---- Solo ruido: 0 eventos ---- */
    {
        int n = 300000;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t evs = {0};
        run_narrow(&DEC_LTE, x, n, 800000000, "{\"period\":0}", &evs);
        CHECK_EQ(evs_count(&evs), 0);
        evs_free(&evs);
        free(x);
    }
    /* ---- Grabación real (sigidwiki, celda de 1,4 MHz) ---- */
    {
        int nn = 0;
        cf32 *x = vr_load_cs8("tests/data/lte_sigid_vf14_1p92M.cs8", &nn);
        if (!x)
            printf("  (sin tests/data/lte_sigid_vf14_1p92M.cs8: se omite)\n");
        else {
            evs_t evs = {0};
            run_narrow(&DEC_LTE, x, nn, 800000000, "{\"period\":0}", &evs);
            if (!ev_has(&evs, "\"pci\":65"))
                printf("  real: sin celda (%s)\n", evs.data ? evs.data : "-");
            CHECK(ev_has(&evs, "\"pci\":65") && ev_has(&evs, "\"n_id2\":2") && ev_has(&evs, "\"duplex\":\"FDD\""));
            CHECK(ev_has(&evs, "\"mib\":true") && ev_has(&evs, "\"bw_rb\":6") && ev_has(&evs, "\"ports\":1"));
            evs_free(&evs);
            free(x);
        }
    }

    /* ---- Coste: s de CPU por s de señal con la configuración por defecto (1 ventana por s) ---- */
    {
        cf32 *x;
        int n = make_signal("{\"pci\":301,\"ports\":2,\"frames\":110,\"bw_rb\":50}", 0, 10, 2000, 0, 5, &x);
        evs_t evs = {0};
        clock_t t0 = clock();
        run_narrow(&DEC_LTE, x, n, 800000000, NULL, &evs);
        double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC, sig = n / 1920000.0;
        printf("  coste: %.3f s de CPU por s de señal (%.2f s de señal, %d eventos)\n", cpu / sig, sig, evs_count(&evs));
        CHECK(cpu / sig < VR_CPU(0.3));
        CHECK(evs_count(&evs) >= 1);
        evs_free(&evs);
        free(x);
    }
    return TEST_SUMMARY();
}
