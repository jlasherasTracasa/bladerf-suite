/*
 * test_gsm.c — Decodificador GSM (FCCH → SCH → BCCH/SI3) con el modulador gemelo.
 *
 * Vectores y propiedades de las normas: 3GPP TS 45.002 §5.2.3 (8 secuencias de
 * entrenamiento de 26 bits y la ampliada del SCH de 64 bits: longitudes, y la propiedad de
 * buena autocorrelación de las TSC), TS 45.003 §4.1 (Fire de 40 bits, 456 bits tras el
 * convolucional) y §4.7 (SCH: 78 bits), TS 45.002 §4.3.3 (FN = 51·((T3−T2) mod 26) + T3 +
 * 1326·T1).
 * VECTOR REAL: tests/data/gsm_sigid_bcch_300k.cu8 = 0,25 s (8 bit sin signo, 300 kS/s) de la
 * grabación pública "GSM_downlink_BCCH.zip" de sigidwiki.com (BCCH de una celda con BSIC 23 =
 * NCC 2, BCC 7). El test la remuestrea a 1,0833 MS/s y exige FCCH + SCH con CRC correcto y el
 * BSIC. Con esta grabación se descubrió que el orden de los bits del SCH (T1/T2/T3') era
 * distinto del supuesto (ver docs/decoders/gsm.md). El BCCH/SI3 real NO decodifica todavía.
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/gsm_phy.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_GSM;
extern const modulator_desc_t MOD_GSM;

static int make(const char *params, double snr, double cfo, double ppm, uint32_t seed, cf32 **out)
{
    iqbuf_t b = {0};
    char err[128] = "";
    if (MOD_GSM.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        return 0;
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
    if (cfo != 0)
        chan_cfo(x, n, 1083333.3333, cfo);
    chan_awgn(x, n, snr, &seed);
    *out = x;
    return n;
}

static void run_case(const char *name, const char *params, double snr, double cfo, double ppm, int bsic, int mcc, int mnc,
                     int lac, int ci)
{
    cf32 *x;
    int n = make(params, snr, cfo, ppm, 31u + (uint32_t)(cfo + 100000), &x);
    evs_t evs = {0};
    run_narrow(&DEC_GSM, x, n, 950000000, NULL, &evs);
    printf("  [%s] eventos=%d\n", name, evs_count(&evs));
    for (const char *p = evs.data; p && *p;) {
        const char *e = strchr(p, '\n');
        printf("    %.*s\n", (int)(e - p), p);
        p = e + 1;
    }
    char key[80];
    snprintf(key, sizeof key, "\"bsic\":%d,", bsic);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"mcc\":%d,\"mnc\":%d,", mcc, mnc);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"lac\":%d", lac);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"ci\":%d", ci);
    CHECK(ev_has(&evs, key));
    CHECK(evs_count(&evs) >= 2 && evs_count(&evs) <= 3);   /* un evento por celda nueva / por SI nueva */
    if (evs.data && cfo != 0) {
        const char *c = strstr(evs.data, "\"cfo_hz\":");
        if (c)
            CHECK_NEAR(atof(c + 9), cfo, 500.0);
    }
    evs_free(&evs);
    free(x);
}

int main(void)
{
    /* ---- Constantes y propiedades de la norma ---- */
    /* Propiedad de diseño de las TSC (45.002 §5.2.3): la correlación de los 16 bits centrales
     * (posiciones 5..20) con la secuencia completa vale 16 en el retardo 0 y EXACTAMENTE 0 en
     * los retardos ±1..±5. Es un contraste fuerte de que las tablas están bien transcritas. */
    for (int t = 0; t < 8; t++)
        for (int lag = -5; lag <= 5; lag++) {
            int c = 0;
            for (int i = 5; i < 21; i++)
                c += (1 - 2 * GSM_TSC[t][i]) * (1 - 2 * GSM_TSC[t][i + lag]);
            CHECK_EQ(c, lag == 0 ? 16 : 0);
        }
    {   /* la ampliada del SCH: autocorrelación aperiódica baja */
        int worst = 0;
        for (int sh = 1; sh < 64; sh++) {
            int c = 0;
            for (int i = 0; i + sh < 64; i++)
                c += (1 - 2 * GSM_SCH_TRAIN[i]) * (1 - 2 * GSM_SCH_TRAIN[i + sh]);
            worst = MAX(worst, abs(c));
        }
        printf("  autocorrelación lateral máxima de la ESQ: %d (de 64)\n", worst);
        CHECK(worst < 24);
    }
    /* SCH: ida y vuelta, FN de ejemplo y errores corregibles */
    {
        uint32_t fns[] = {1, 11, 1326 * 3 + 52, 51 * 53247 + 1, 816 + 103};
        for (unsigned k = 0; k < sizeof fns / sizeof fns[0]; k++) {
            uint32_t fn = fns[k];
            while (fn % 51 != 1)
                fn++;
            uint8_t bits[78];
            float soft[78];
            gsm_sch_encode(0x1D, fn, bits);
            for (int i = 0; i < 78; i++)
                soft[i] = bits[i] ? 1.0f : -1.0f;
            for (int e = 0; e < 4; e++)
                soft[e * 19] *= -1;
            int bsic;
            uint32_t fo;
            CHECK_EQ(gsm_sch_decode(soft, &bsic, &fo), 0);
            CHECK_EQ(bsic, 0x1D);
            CHECK_EQ(fo, fn);
        }
        /* FN de 45.002 §4.3.3: T1 = 0, T2 = 1, T3 = 1 → FN = 51·((1−1) mod 26) + 1 + 0 = 1 */
        uint8_t bits[78];
        float soft[78];
        gsm_sch_encode(0, 1, bits);
        for (int i = 0; i < 78; i++)
            soft[i] = bits[i] ? 1.0f : -1.0f;
        int bsic;
        uint32_t fo;
        CHECK_EQ(gsm_sch_decode(soft, &bsic, &fo), 0);
        CHECK_EQ(fo, 1);
    }
    /* BCCH: Fire + convolucional + entrelazado, SI3 con LAI de ejemplo (MCC 214 MNC 07) */
    {
        uint8_t l3[23], out[23], c[456];
        gsm_si_build(0x1B, 214, 7, 2, 4660, 22136, l3);
        gsm_bcch_encode(l3, c);
        float soft[456];
        for (int i = 0; i < 456; i++)
            soft[i] = c[i] ? 2.0f : -2.0f;
        for (int e = 0; e < 12; e++)
            soft[e * 37] *= -1;
        CHECK_EQ(gsm_bcch_decode(soft, out), 0);
        CHECK(memcmp(l3, out, 23) == 0);
        gsm_si_t si;
        CHECK_EQ(gsm_si_parse(out, &si), 0);
        CHECK_EQ(si.mcc, 214);
        CHECK_EQ(si.mnc, 7);
        CHECK_EQ(si.mnc_digits, 2);
        CHECK_EQ(si.lac, 4660);
        CHECK_EQ(si.ci, 22136);
        /* 3 dígitos de MNC (p. ej. 310 / 260) */
        gsm_si_build(0x1B, 310, 260, 3, 1, 2, l3);
        CHECK_EQ(gsm_si_parse(l3, &si), 0);
        CHECK_EQ(si.mcc, 310);
        CHECK_EQ(si.mnc, 260);
        CHECK_EQ(si.mnc_digits, 3);
        /* bytes de LAI según 44.018 §10.5.1.3: MCC 214 → 0x12 0xF4 ; MNC 07 → 0x70 */
        gsm_si_build(0x1B, 214, 7, 2, 4660, 22136, l3);
        CHECK_EQ(l3[5], 0x12);
        CHECK_EQ(l3[6], 0xF4);
        CHECK_EQ(l3[7], 0x70);
        /* el entrelazado es una permutación: cada (ráfaga, bit) aparece una vez */
        static uint8_t seen[4][114];
        for (int k = 0; k < 456; k++) {
            int b, j;
            gsm_interleave_pos(k, &b, &j);
            CHECK(j >= 0 && j < 114);
            seen[b][j]++;
        }
        int bad = 0;
        for (int b = 0; b < 4; b++)
            for (int j = 0; j < 114; j++)
                bad += seen[b][j] != 1;
        CHECK_EQ(bad, 0);
    }

    /* ---- Señal sintética completa ---- */
    const char *P = "{\"ncc\":3,\"bcc\":5,\"mcc\":214,\"mnc\":7,\"lac\":4660,\"ci\":22136,\"frames\":161}";
    run_case("SNR 10 dB", P, 10, 0, 0, 29, 214, 7, 4660, 22136);
    run_case("CFO +50 kHz", P, 10, 50000, 0, 29, 214, 7, 4660, 22136);
    run_case("CFO -50 kHz, +100 ppm", P, 10, -50000, 100, 29, 214, 7, 4660, 22136);
    run_case("CFO +30 kHz, -100 ppm, otro operador/LAC",
             "{\"ncc\":1,\"bcc\":2,\"mcc\":310,\"mnc\":260,\"mnc_digits\":3,\"lac\":777,\"ci\":4242,\"frames\":161,\"fn\":1326}",
             10, 30000, -100, 10, 310, 260, 777, 4242);

    /* ---- Solo ruido: 0 eventos ---- */
    {
        int n = 1083333;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 5;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t evs = {0};
        run_narrow(&DEC_GSM, x, n, 950000000, NULL, &evs);
        CHECK_EQ(evs_count(&evs), 0);
        evs_free(&evs);
        free(x);
    }
    /* ---- Grabación real (sigidwiki, BSIC 23) ---- */
    {
        FILE *f = fopen("tests/data/gsm_sigid_bcch_300k.cu8", "rb");
        if (!f)
            printf("  (sin tests/data/gsm_sigid_bcch_300k.cu8: se omite)\n");
        else {
            static uint8_t raw[150000];
            size_t nr = fread(raw, 1, sizeof raw, f);
            fclose(f);
            int n0 = (int)(nr / 2);
            cf32 *x0 = malloc(sizeof(cf32) * (size_t)n0);
            for (int i = 0; i < n0; i++) {
                x0[i].re = ((float)raw[2 * i] - 127.5f) / 127.5f;
                x0[i].im = ((float)raw[2 * i + 1] - 127.5f) / 127.5f;
            }
            double ratio = 1083333.3333 / 300000.0;
            cf32 *x = malloc(sizeof(cf32) * (size_t)((double)n0 * ratio + 64));
            int n = chan_resample(x0, n0, ratio, x);
            evs_t ev = {0};
            run_narrow(&DEC_GSM, x, n, 947000000, NULL, &ev);
            if (!ev_has(&ev, "\"bsic\":23"))
                printf("  real: sin SCH (%s)\n", ev.data ? ev.data : "-");
            CHECK(ev_has(&ev, "\"bsic\":23") && ev_has(&ev, "\"ncc\":2") && ev_has(&ev, "\"bcc\":7") &&
                  ev_has(&ev, "\"kind\":\"sch\""));
            evs_free(&ev);
            free(x);
            free(x0);
        }
    }

    /* ---- Coste ---- */
    {
        cf32 *x;
        int n = make(P, 10, 20000, 0, 3, &x);
        evs_t evs = {0};
        clock_t t0 = clock();
        run_narrow(&DEC_GSM, x, n, 950000000, NULL, &evs);
        double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC, sig = n / 1083333.3333;
        printf("  coste: %.3f s de CPU por s de señal (%.2f s de señal)\n", cpu / sig, sig);
        CHECK(cpu / sig < VR_CPU(0.3));
        evs_free(&evs);
        free(x);
    }
    return TEST_SUMMARY();
}
