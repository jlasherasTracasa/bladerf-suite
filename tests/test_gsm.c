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
 * NCC 2, BCC 7). El test la remuestrea a 1,0833 MS/s y exige FCCH + SCH con CRC correcto, el
 * BSIC y un bloque BCCH con Fire válido (SI2ter con TSC 6: la red no usa TSC = BCC). En 0,25 s
 * solo cabe una multitrama; los otros SI de la misma grabación (10 s, 42 multitramas) se
 * decodificaron con el mismo código y sus 23 octetos están copiados más abajo (REAL_SI*) como
 * vectores de la capa 3: MCC 231 / MNC 02 (Eslovaquia), LAC 1, CI 62301, ARFCN 60.
 * Con esta grabación se descubrió que el orden de los bits del SCH (T1/T2/T3') era distinto del
 * supuesto y que los octetos del BCCH van con el LSB primero (ver docs/protocolos/gsm.md).
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

/* Octetos reales (grabación de sigidwiki, 10 s): decodificados con Fire correcto. */
static const char *REAL_SI[] = {
    "55061900002040000040000000004000000000bd00002b",                              /* SI1 */
    "59061a100000000022a00001c124048000000088bd0000",                              /* SI2 */
    "010603df3f80000000000000080000000000002b2b2b2b",                              /* SI2ter */
    "49061bf35d32f1200001d00314144543bd00008000419b",                              /* SI3 */
    "31061c32f12000014543bd000080004f2b2b2b2b2b2b2b",                              /* SI4 */
};

static void hex23(const char *h, uint8_t o[23])
{
    for (int i = 0; i < 23; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        o[i] = (uint8_t)v;
    }
}

/* Evento con "smp0"/"smp1" absolutos: abs = inicio del bloque + desplazamiento. */
typedef struct {
    evs_t *evs;
    long blk0;
    long first0[8], first1[8];
    char kinds[8][8];
    int n;
} spanctx_t;

static void span_collect(void *ctx, const char *json, size_t len)
{
    spanctx_t *sc = ctx;
    tu_collect(sc->evs, json, len);
    const char *a = strstr(json, "\"smp0\":"), *b = strstr(json, "\"smp1\":"), *k = strstr(json, "\"kind\":\"");
    if (a && b && k && sc->n < 8) {
        sc->first0[sc->n] = sc->blk0 + atol(a + 7);
        sc->first1[sc->n] = sc->blk0 + atol(b + 7);
        sscanf(k + 8, "%7[a-z0-9]", sc->kinds[sc->n]);
        sc->n++;
    }
}

/* Alimenta el decodificador a mano para conocer el inicio de cada bloque. */
static void run_spans(const cf32 *x, int n, evs_t *evs, spanctx_t *sc)
{
    char err[100];
    dec_cfg_t cfg = {.rate = DEC_GSM.rate, .freq_hz = 950000000, .center_hz = 950000000};
    void *st = DEC_GSM.create(&cfg, err, sizeof err);
    sc->evs = evs;
    for (int off = 0; off < n; off += 4096) {
        sc->blk0 = off;
        DEC_GSM.process(st, x + off, n - off < 4096 ? n - off : 4096, (double)off / DEC_GSM.rate, span_collect, sc);
    }
    DEC_GSM.destroy(st);
}

/* Un caso completo: sch + SI1, SI2, SI3, SI4, SI2bis y SI2ter (8 multitramas = 408 tramas). */
static void run_case(const char *name, const char *params, double snr, double cfo, double ppm, int bsic, int tsc, int mcc,
                     int mnc, int lac, int ci)
{
    cf32 *x;
    int n = make(params, snr, cfo, ppm, 31u + (uint32_t)(cfo + 100000), &x);
    evs_t evs = {0};
    spanctx_t sc;
    memset(&sc, 0, sizeof sc);
    run_spans(x, n, &evs, &sc);
    printf("  [%s] eventos=%d\n", name, evs_count(&evs));
    char key[100];
    snprintf(key, sizeof key, "\"bsic\":%d,", bsic);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"tsc\":%d", tsc);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"mcc\":%d,\"mnc\":%d,", mcc, mnc);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"lac\":%d", lac);
    CHECK(ev_has(&evs, key));
    snprintf(key, sizeof key, "\"ci\":%d", ci);
    CHECK(ev_has(&evs, key));
    /* un evento por SCH de celda nueva y uno por cada SI nuevo: exactamente 1 + 6 */
    const char *kinds[] = {"sch", "si1", "si2", "si2bis", "si2ter", "si3", "si4"};
    for (int k = 0; k < 7; k++) {
        snprintf(key, sizeof key, "\"kind\":\"%s\"", kinds[k]);
        CHECK(ev_has(&evs, key));
    }
    CHECK_EQ(evs_count(&evs), 7);
    /* parámetros de SI3/SI4 y listas (valores de gsm_si_build / mod_gsm por omisión) */
    CHECK(ev_has(&evs, "\"hyst_db\":4,\"rxlev_min_dbm\":-104"));
    CHECK(ev_has(&evs, "\"max_retrans\":7"));
    CHECK(ev_has(&evs, "\"arfcns\":[1,5,9,17,33,60,124]"));
    CHECK(ev_has(&evs, "\"neighbours\":[2,11,21,40,80,120]"));
    CHECK(ev_has(&evs, "\"arfcn\":"));           /* ARFCN de la portadora (950 MHz → 75) */
    CHECK(ev_has(&evs, "\"arfcn\":75"));
    if (evs.data && cfo != 0) {
        const char *c = strstr(evs.data, "\"cfo_hz\":");
        if (c)
            CHECK_NEAR(atof(c + 9), cfo, 500.0);
    }
    /* marca de posición: el SCH ocupa una ráfaga (≈ 592 muestras) y un bloque BCCH 3 tramas + 1 ráfaga */
    for (int i = 0; i < sc.n; i++) {
        long len = sc.first1[i] - sc.first0[i];
        if (!strcmp(sc.kinds[i], "sch"))
            CHECK(len > 560 && len < 620);
        else
            CHECK(len > 3 * GSM_FRAME + 560 && len < 3 * GSM_FRAME + 640);
    }
    if (ppm == 0) {
        /* el primer SCH está en la trama 1 (fn 817): muestra ≈ 5000 + posición de símbolo 0 */
        CHECK(sc.n > 0 && labs(sc.first0[0] - 5000) < 24);
        for (int i = 1; i < sc.n; i++) {     /* los BCCH empiezan en tramas con FN mod 51 = 2 */
            long f = (sc.first0[i] + 12) / GSM_FRAME, fn = 816 + f;
            CHECK_EQ(fn % 51, 2);
            CHECK(labs(sc.first0[i] - f * GSM_FRAME) < 24);
        }
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

    /* ---- Mensajes reales: capa 3 de la grabación de sigidwiki ---- */
    {
        uint8_t l3[23], out[23], c[456];
        gsm_si_t si;
        for (int k = 0; k < 5; k++) {
            hex23(REAL_SI[k], l3);
            gsm_bcch_encode(l3, c);          /* ida y vuelta: el LSB primero y Fire de 40 bits */
            float soft[456];
            for (int i = 0; i < 456; i++)
                soft[i] = c[i] ? 1.0f : -1.0f;
            CHECK_EQ(gsm_bcch_decode(soft, out), 0);
            CHECK(memcmp(l3, out, 23) == 0);
            CHECK_EQ(gsm_si_parse(l3, &si), 0);
        }
        hex23(REAL_SI[3], l3);                /* SI3 */
        gsm_si_parse(l3, &si);
        CHECK_EQ(si.type, GSM_SI3);
        CHECK_EQ(si.mcc, 231);
        CHECK_EQ(si.mnc, 2);
        CHECK_EQ(si.mnc_digits, 2);
        CHECK_EQ(si.lac, 1);
        CHECK_EQ(si.ci, 62301);
        CHECK_EQ(si.hyst_db, 4);
        CHECK_EQ(si.rxlev_min_dbm, -107);
        CHECK_EQ(si.ccch_conf, 0);
        CHECK_EQ(si.bs_ag_blks_res, 2);
        CHECK_EQ(si.bs_pa_mfrms, 5);
        CHECK_EQ(si.t3212_decih, 20);
        CHECK_EQ(si.max_retrans, 4);
        CHECK_EQ(si.cell_bar, 0);
        hex23(REAL_SI[4], l3);                /* SI4: mismo LAI */
        gsm_si_parse(l3, &si);
        CHECK(si.lai_ok && si.mcc == 231 && si.mnc == 2 && si.lac == 1 && si.ci == -1);
        hex23(REAL_SI[0], l3);                /* SI1: Cell Channel Description */
        gsm_si_parse(l3, &si);
        CHECK(si.arfcn_ok && si.n_arfcn == 4);
        CHECK(si.arfcn[0] == 39 && si.arfcn[1] == 79 && si.arfcn[2] == 103 && si.arfcn[3] == 110);
        hex23(REAL_SI[1], l3);                /* SI2: 12 vecinas y NCC permitidos 0x88 */
        gsm_si_parse(l3, &si);
        CHECK(si.arfcn_ok && si.n_arfcn == 12 && si.ba_ind == 1 && si.ncc_permitted == 0x88);
        CHECK(si.arfcn[0] == 32 && si.arfcn[11] == 86);
        /* ARFCN 60 = 947 MHz (45.005 §2: DL = 935 + 0,2·n); bordes de banda */
        CHECK_EQ(gsm_arfcn_from_hz(947.0e6), 60);
        CHECK_EQ(gsm_arfcn_from_hz(935.2e6), 1);
        CHECK_EQ(gsm_arfcn_from_hz(925.2e6), 975);
        CHECK_EQ(gsm_arfcn_from_hz(1805.2e6), 512);
        CHECK_EQ(gsm_arfcn_from_hz(1842.6e6), 699);
        CHECK_EQ(gsm_arfcn_from_hz(100.0e6), -1);
    }
    /* Constructor ↔ analizador de SI1/SI2/SI2bis/SI2ter con una lista y de SI3/4 */
    {
        uint8_t l3[23];
        gsm_si_t si;
        int ar[] = {1, 8, 9, 60, 121, 124};
        for (int t = 0; t < 4; t++) {
            int ty[] = {GSM_SI1, GSM_SI2, GSM_SI2BIS, GSM_SI2TER};
            gsm_si_build_arfcn(ty[t], ar, 6, l3);
            CHECK_EQ(gsm_si_parse(l3, &si), 0);
            CHECK_EQ(si.type, ty[t]);
            CHECK(si.arfcn_ok && si.n_arfcn == 6);
            for (int i = 0; i < 6; i++)
                CHECK_EQ(si.arfcn[i], ar[i]);
        }
        gsm_si_build(GSM_SI3, 214, 7, 2, 4660, 22136, l3);
        CHECK_EQ(gsm_si_parse(l3, &si), 0);
        CHECK_EQ(si.hyst_db, GSM_SI_DEF_HYST_DB);
        CHECK_EQ(si.rxlev_min_dbm, GSM_SI_DEF_RXLEV_MIN_DBM);
        CHECK_EQ(si.t3212_decih, GSM_SI_DEF_T3212);
        l3[1] = 0x05;                          /* discriminador de protocolo distinto → no es RR */
        CHECK_EQ(gsm_si_parse(l3, &si), -1);
    }

    /* ---- Señal sintética completa ---- */
    const char *P = "{\"ncc\":3,\"bcc\":5,\"mcc\":214,\"mnc\":7,\"lac\":4660,\"ci\":22136,\"frames\":410}";
    run_case("SNR 10 dB", P, 10, 0, 0, 29, 5, 214, 7, 4660, 22136);
    run_case("CFO +50 kHz", P, 10, 50000, 0, 29, 5, 214, 7, 4660, 22136);
    run_case("CFO -50 kHz, +100 ppm", P, 10, -50000, 100, 29, 5, 214, 7, 4660, 22136);
    run_case("CFO +30 kHz, -100 ppm, TSC 6 con BCC 2 (como la red real), otro operador",
             "{\"ncc\":1,\"bcc\":2,\"tsc\":6,\"mcc\":310,\"mnc\":260,\"mnc_digits\":3,\"lac\":777,\"ci\":4242,\"frames\":410,\"fn\":1326}",
             10, 30000, -100, 10, 6, 310, 260, 777, 4242);

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
            /* BCCH real con Fire válido: SI2ter, TSC 6 (la BCC sería 7) */
            CHECK(ev_has(&ev, "\"kind\":\"si2ter\"") && ev_has(&ev, "\"tsc\":6"));
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
