/*
 * test_ism.c — Decodificador ISM (OOK/FSK) y analizador genérico.
 *
 * Vectores REALES (tests/data/ism_*_rtl433.cu8): fragmentos recortados de las
 * grabaciones públicas del repositorio rtl_433_tests (github.com/merbanan/
 * rtl_433_tests, RTL-SDR 250 kS/s cu8 a 433,92 MHz, de sensores comerciales).
 * Se usan solo como datos; los valores esperados son los que publica el propio
 * repositorio en el .json de cada captura (id, canal, temperatura, humedad…).
 * Licencia del repositorio: GPL-2.0 (los .cu8 se incluyen solo como fixtures de
 * prueba, recortados a < 200 KB). Cubren Acurite-592TXR, Nexus-TH, Ambient
 * F007TH, Fine Offset WH1080, LaCrosse TX141B v2 y v3 y Oregon v2.1 (THN132N,
 * THGR122N). El resto de tramas son
 * sintéticas (modulador local): verifican la coherencia modulador↔decodificador.
 *
 * Los generadores de TPMS y de tramas desconocidas viven AQUÍ (no en el
 * modulador, que solo emite meteorología y la trama de prueba).
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/ism_protocols.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_ISM;
extern const modulator_desc_t MOD_ISM;

#define FS 250000.0

static double cpu_s, sig_s;

static cf32 *g_buf;
static int g_n, g_cap;

static void sig_reset(void) { g_n = 0; }

/* Transmite un tren OOK o FSK con silencio delante y detrás. */
static void sig_add(const ism_train_t *t, bool fsk)
{
    double ph = 0.4;
    ism_render_silence(&g_buf, &g_n, &g_cap, 12500);
    ism_render(&g_buf, &g_n, &g_cap, t, fsk, FS, 0.8, 35000.0, &ph);
    ism_render_silence(&g_buf, &g_n, &g_cap, 12500);
}

/* Canal: deriva de reloj, desvío de frecuencia y AWGN; decodifica y mide CPU. */
static void decode_signal(double snr_db, double cfo_hz, double ppm, uint32_t seed, evs_t *evs)
{
    int n = g_n;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    if (ppm != 0) {
        /* chan_drift de test_util.h reserva n+8 muestras y se desborda con ppm>0 y n grande:
         * se hace aquí con búfer holgado. */
        cf32 *tmp = malloc(sizeof(cf32) * (size_t)(n + n / 500 + 16));
        int m = chan_resample(g_buf, n, 1.0 + ppm * 1e-6, tmp);
        for (int i = 0; i < n; i++)
            x[i] = i < m ? tmp[i] : (cf32){0, 0};
        free(tmp);
    } else
        memcpy(x, g_buf, sizeof(cf32) * (size_t)n);
    chan_cfo(x, n, FS, cfo_hz);
    if (snr_db < 200)
        chan_awgn(x, n, snr_db, &seed);
    clock_t c0 = clock();
    run_narrow(&DEC_ISM, x, n, 433920000LL, NULL, evs);
    cpu_s += (double)(clock() - c0) / CLOCKS_PER_SEC;
    sig_s += n / FS;
    free(x);
}

/* ---- Generadores locales (no existen en el modulador) ------------------ */
static void tpms_fsk_train(ism_train_t *t, uint32_t id, double kpa, double temp_c, bool low)
{
    uint8_t by[8], bits[160];
    by[0] = (uint8_t)(id >> 24);
    by[1] = (uint8_t)(id >> 16);
    by[2] = (uint8_t)(id >> 8);
    by[3] = (uint8_t)id;
    by[4] = (uint8_t)lrint(kpa / 2.5);
    by[5] = (uint8_t)lrint(temp_c + 50.0);
    by[6] = low ? 1 : 0;
    uint8_t s = 0;
    for (int i = 0; i < 7; i++)
        s = (uint8_t)(s + by[i]);
    by[7] = s;
    int n = 0;
    for (int i = 0; i < 16; i++)
        bits[n++] = 1;
    n = ism_put_bits(bits, n, 0x2DD4, 16);
    for (int i = 0; i < 8; i++)
        n = ism_put_bits(bits, n, by[i], 8);
    ism_train_clear(t);
    ism_train_manch(t, bits, n, 60, 0);
}

static void tpms_ook_train(ism_train_t *t, uint32_t id, double kpa, double temp_c)
{
    uint8_t by[6], bits[160];
    by[0] = (uint8_t)(id >> 16);
    by[1] = (uint8_t)(id >> 8);
    by[2] = (uint8_t)id;
    by[3] = (uint8_t)lrint(kpa / 2.5);
    by[4] = (uint8_t)lrint(temp_c + 50.0);
    by[5] = ism_crc8(by, 5, 0x07, 0);
    int n = 0;
    for (int i = 0; i < 16; i++)
        bits[n++] = 1;
    n = ism_put_bits(bits, n, 0xA5, 8);
    for (int i = 0; i < 6; i++)
        n = ism_put_bits(bits, n, by[i], 8);
    ism_train_clear(t);
    ism_train_manch(t, bits, n, 120, 0);
}

/* Tramas "desconocidas" para el analizador genérico. */
static void unknown_pwm(ism_train_t *t, const uint8_t *bits, int n, int rows)
{
    ism_train_clear(t);
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < n; i++)
            ism_train_pair(t, bits[i] ? 700 : 300, 400);
        ism_train_pair(t, 300, 6000);
    }
}

static void unknown_ppm(ism_train_t *t, const uint8_t *bits, int n, int rows)
{
    ism_train_clear(t);
    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < n; i++)
            ism_train_pair(t, 400, bits[i] ? 1200 : 600);
        ism_train_pair(t, 400, 7000);
    }
}

static void unknown_fsk_nrz(ism_train_t *t, const uint8_t *bits, int n, double bit_us)
{
    /* Rachas de bits iguales → duraciones alternas, empezando por la marca (bit 1). */
    ism_train_clear(t);
    if (bits[0] == 0)
        t->d[t->n++] = 0;
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && bits[j] == bits[i])
            j++;
        t->d[t->n++] = (uint32_t)((j - i) * bit_us);
        i = j;
    }
}

/* ---- Casos meteorológicos --------------------------------------------- */
typedef struct {
    ism_msg_t m;
    const char *expect[6];
    int repeat;
} wcase_t;

static void run_weather(const wcase_t *c, double cfo, double ppm, uint32_t seed)
{
    ism_train_t *t = malloc(sizeof *t);
    ism_train_clear(t);
    ism_encode(&c->m, c->repeat, t);
    sig_reset();
    sig_add(t, false);
    free(t);
    evs_t evs = {0};
    decode_signal(10.0, cfo, ppm, seed, &evs);
    bool ok = evs_count(&evs) > 0;
    for (int i = 0; c->expect[i]; i++)
        if (!ev_has(&evs, c->expect[i])) {
            ok = false;
            printf("  falta %s en %s (cfo %+.0f ppm %+.0f)\n", c->expect[i], c->m.model, cfo, ppm);
        }
    if (!ok && evs.data)
        printf("  eventos: %s\n", evs.data);
    CHECK(ok);
    evs_free(&evs);
}

int main(void)
{
    /* CRC-8 con vector publicado (catálogo reveng, CRC-8/SMBUS). */
    CHECK_EQ(ism_crc8((const uint8_t *)"123456789", 9, 0x07, 0), 0xF4);

    wcase_t wc[] = {
        {{.model = "Oregon-THGR122N", .id = 0xA7, .channel = 2, .temp_c = 21.4, .humidity = 56, .batt_low = false},
         {"\"model\":\"Oregon-THGR122N\"", "\"temperature_c\":21.4", "\"humidity\":56", "\"channel\":2", "\"id\":167"},
         2},
        {{.model = "Oregon-THN132N", .id = 0x33, .channel = 1, .temp_c = -3.7, .batt_low = true},
         {"\"model\":\"Oregon-THN132N\"", "\"temperature_c\":-3.7", "\"battery_low\":true"},
         2},
        {{.model = "Oregon-THGR810", .id = 0x5C, .channel = 3, .temp_c = 30.2, .humidity = 41},
         {"\"model\":\"Oregon-THGR810\"", "\"temperature_c\":30.2", "\"humidity\":41", "\"channel\":3"},
         2},
        {{.model = "LaCrosse-TX141B", .id = 0xD2, .temp_c = 18.6},
         {"\"model\":\"LaCrosse-TX141B\"", "\"temperature_c\":18.6", "\"id\":210"},
         3},
        {{.model = "Acurite-592TXR", .id = 0x1234, .channel = 2, .temp_c = 23.5, .humidity = 48},
         {"\"model\":\"Acurite-592TXR\"", "\"temperature_c\":23.5", "\"humidity\":48", "\"id\":4660"},
         3},
        {{.model = "Nexus-TH", .id = 0x6B, .channel = 3, .temp_c = 19.8, .humidity = 71},
         {"\"model\":\"Nexus-TH\"", "\"temperature_c\":19.8", "\"humidity\":71", "\"channel\":3"},
         12},
        {{.model = "FineOffset-WH1080", .id = 0xB4, .temp_c = 12.3, .humidity = 80, .wind_avg_ms = 3.4,
          .wind_gust_ms = 6.8, .rain_mm = 4.5, .wind_dir_deg = 90},
         {"\"model\":\"FineOffset-WH1080\"", "\"temperature_c\":12.3", "\"humidity\":80", "\"wind_dir_deg\":90",
          "\"rain_mm\":4.5"},
         2},
        {{.model = "Ambient-F007TH", .id = 0x71, .channel = 4, .temp_c = 22.2, .humidity = 39},
         {"\"model\":\"Ambient-F007TH\"", "\"temperature_c\":22.2", "\"humidity\":39", "\"channel\":4"},
         2},
        {{.model = "BladeRFLab-test", .id = 0x0102}, {"\"model\":\"BladeRFLab-test\"", "\"id\":258"}, 3},
    };
    int ncase = (int)(sizeof wc / sizeof wc[0]);

    /* (1) Cada modelo: CFO ±50 kHz (¼ de 200 kHz) y reloj ±100 ppm, AWGN 10 dB. */
    for (int i = 0; i < ncase; i++) {
        printf("  %s\n", wc[i].m.model);
        run_weather(&wc[i], +50000.0, +100.0, 11u + (uint32_t)i);
        run_weather(&wc[i], -50000.0, -100.0, 77u + (uint32_t)i);
    }

    /* (1b) TPMS (solo lectura; tramas generadas aquí) */
    {
        ism_train_t *t = malloc(sizeof *t);
        for (int v = 0; v < 2; v++) {
            double cfo = v ? -50000.0 : 50000.0, ppm = v ? -100.0 : 100.0;
            tpms_fsk_train(t, 0xA1B2C3D4u, 230.0, 27.0, v);
            sig_reset();
            sig_add(t, true);
            sig_add(t, true);   /* los sensores repiten la trama */
            evs_t e1 = {0};
            decode_signal(10.0, cfo, ppm, 5u + (uint32_t)v, &e1);
            CHECK(ev_has(&e1, "\"model\":\"TPMS-FSK\"") && ev_has(&e1, "\"pressure_kpa\":230.0") &&
                  ev_has(&e1, "\"temperature_c\":27.0") && ev_has(&e1, "\"id\":2712847316"));
            if (e1.data)
                printf("  TPMS-FSK eventos: %s\n", e1.data);
            evs_free(&e1);

            tpms_ook_train(t, 0x00C0FFEEu & 0xFFFFFF, 220.0, 31.0);
            sig_reset();
            sig_add(t, false);
            sig_add(t, false);   /* el sensor repite */
            evs_t e2 = {0};
            decode_signal(10.0, cfo, ppm, 9u + (uint32_t)v, &e2);
            CHECK(ev_has(&e2, "\"model\":\"TPMS-OOK\"") && ev_has(&e2, "\"pressure_kpa\":220.0") &&
                  ev_has(&e2, "\"temperature_c\":31.0"));
            if (!ev_has(&e2, "TPMS-OOK") && e2.data)
                printf("  TPMS-OOK eventos: %s\n", e2.data);
            evs_free(&e2);
        }
        free(t);
    }

    /* (1c) Analizador genérico: tramas de protocolo desconocido */
    {
        ism_train_t *t = malloc(sizeof *t);
        uint8_t bits[40];
        const uint8_t by[5] = {0x5A, 0xC3, 0x0F, 0x99, 0x21};
        for (int i = 0; i < 40; i++)
            bits[i] = (by[i >> 3] >> (7 - (i & 7))) & 1;
        unknown_pwm(t, bits, 40, 5);
        sig_reset();
        sig_add(t, false);
        evs_t e = {0};
        decode_signal(10.0, 30000.0, 100.0, 3, &e);
        CHECK(ev_has(&e, "\"kind\":\"generic\"") && ev_has(&e, "\"modulation\":\"OOK-PWM\"") &&
              ev_has(&e, "\"hex\":\"5AC30F9921\"") && ev_has(&e, "\"bits\":40"));
        if (e.data)
            printf("  genérico PWM: %.200s\n", e.data);
        evs_free(&e);

        unknown_ppm(t, bits, 40, 6);
        sig_reset();
        sig_add(t, false);
        decode_signal(10.0, -30000.0, -100.0, 4, &e);
        CHECK(ev_has(&e, "\"modulation\":\"OOK-PPM\"") && ev_has(&e, "\"hex\":\"5AC30F9921\""));
        if (e.data)
            printf("  genérico PPM: %.200s\n", e.data);
        evs_free(&e);

        /* FSK NRZ con 3 paquetes iguales */
        uint8_t nb[96];
        int n = 0;
        for (int i = 0; i < 16; i++)
            nb[n++] = (uint8_t)(i & 1);
        for (int i = 0; i < 40; i++)
            nb[n++] = bits[i];
        unknown_fsk_nrz(t, nb, n, 104.0);
        sig_reset();
        sig_add(t, true);
        sig_add(t, true);
        sig_add(t, true);
        decode_signal(14.0, 20000.0, 50.0, 6, &e);
        CHECK(ev_has(&e, "\"kind\":\"generic\"") && ev_has(&e, "FSK"));
        if (e.data)
            printf("  genérico FSK: %.240s\n", e.data);
        evs_free(&e);
        free(t);
    }

    /* (2) Solo ruido: 0 eventos (y ningún análisis genérico). */
    {
        int n = (int)(3.0 * FS);
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 12345;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t e = {0};
        clock_t c0 = clock();
        run_narrow(&DEC_ISM, x, n, 433920000LL, NULL, &e);
        cpu_s += (double)(clock() - c0) / CLOCKS_PER_SEC;
        sig_s += n / FS;
        CHECK_EQ(evs_count(&e), 0);
        evs_free(&e);
        free(x);
    }

    /* (2a) Grabaciones reales (rtl_433_tests) */
    {
        static const struct {
            const char *file;
            const char *want[6];
        } rv[] = {
            {"tests/data/ism_acurite592txr_rtl433.cu8",
             {"\"model\":\"Acurite-592TXR\"", "\"id\":12053", "\"temperature_c\":26.7", "\"humidity\":74", NULL}},
            {"tests/data/ism_nexus_rtl433.cu8",
             {"\"model\":\"Nexus-TH\"", "\"id\":181", "\"channel\":2", "\"temperature_c\":19.0", "\"humidity\":71"}},
            {"tests/data/ism_f007th_rtl433.cu8",
             {"\"model\":\"Ambient-F007TH\"", "\"id\":169", "\"temperature_c\":-20.3", "\"humidity\":19", NULL}},
            {"tests/data/ism_wh1080_rtl433.cu8",
             {"\"model\":\"FineOffset-WH1080\"", "\"id\":5", "\"temperature_c\":7.5", "\"humidity\":91", "\"rain_mm\":184.5"}},
            {"tests/data/ism_oregon_thn132n_rtl433.cu8",
             {"\"model\":\"Oregon-THN132N\"", "\"id\":231", "\"temperature_c\":18.0", NULL}},
            {"tests/data/ism_oregon_thgr122n_rtl433.cu8",
             {"\"model\":\"Oregon-THGR122N\"", "\"id\":187", "\"temperature_c\":23.0", "\"humidity\":39"}},
            {"tests/data/ism_tx141bv2_rtl433.cu8",
             {"\"model\":\"LaCrosse-TX141B\"", "\"id\":117", "\"temperature_c\":20.1", NULL}},
            {"tests/data/ism_tx141bv3_rtl433.cu8",
             {"\"model\":\"LaCrosse-TX141B\"", "\"id\":33", "\"temperature_c\":20.3", NULL}},
        };
        for (size_t i = 0; i < sizeof rv / sizeof rv[0]; i++) {
            int nn = 0;
            cf32 *x = vr_load_cu8(rv[i].file, &nn);
            if (!x) {
                printf("  (sin %s: se omite)\n", rv[i].file);
                continue;
            }
            /* El detector aprende el suelo de ruido durante las primeras decenas de
             * ms (τ ≈ 8 ms): el fragmento se pasa dos veces seguidas, igual que hace
             * --replay al repetir el fichero en bucle. */
            cf32 *x2 = malloc(sizeof(cf32) * 2 * (size_t)nn);
            memcpy(x2, x, sizeof(cf32) * (size_t)nn);
            memcpy(x2 + nn, x, sizeof(cf32) * (size_t)nn);
            evs_t e = {0};
            run_narrow(&DEC_ISM, x2, 2 * nn, 433920000LL, NULL, &e);
            free(x2);
            for (int k = 0; k < 6 && rv[i].want[k]; k++) {
                if (!ev_has(&e, rv[i].want[k]))
                    printf("  real %s: falta %s\n", rv[i].file, rv[i].want[k]);
                CHECK(ev_has(&e, rv[i].want[k]));
            }
            evs_free(&e);
            free(x);
        }
    }

    /* (2b) El modulador no genera TPMS ni mandos */
    {
        iqbuf_t out = {0};
        char err[200] = "";
        CHECK(MOD_ISM.air_ok);
        CHECK(MOD_ISM.modulate("{\"model\":\"TPMS-FSK\"}", &out, err, sizeof err) < 0);
        CHECK(MOD_ISM.modulate("{\"model\":\"mando\"}", &out, err, sizeof err) < 0);
        iqbuf_free(&out);
    }

    /* (3) Modulador por defecto → decodificador: todos los modelos de ejemplo */
    {
        iqbuf_t out = {0};
        char err[200] = "";
        CHECK_EQ(MOD_ISM.modulate(NULL, &out, err, sizeof err), 0);
        CHECK_NEAR(out.rate, FS, 1);
        evs_t e = {0};
        uint32_t seed = 99;
        for (int i = 0; i < out.n; i++)
            CHECK(fabsf(out.s[i].re) <= 0.9f + 1e-6f && fabsf(out.s[i].im) <= 0.9f);
        chan_awgn(out.s, out.n, 12.0, &seed);
        clock_t c0 = clock();
        run_narrow(&DEC_ISM, out.s, out.n, 433920000LL, NULL, &e);
        cpu_s += (double)(clock() - c0) / CLOCKS_PER_SEC;
        sig_s += out.n / FS;
        for (int i = 0; ISM_TX_MODELS[i]; i++) {
            char key[80];
            snprintf(key, sizeof key, "\"model\":\"%s\"", ISM_TX_MODELS[i]);
            if (!ev_has(&e, key))
                printf("  modulador por defecto: falta %s\n", ISM_TX_MODELS[i]);
            CHECK(ev_has(&e, key));
        }
        evs_free(&e);
        iqbuf_free(&out);
    }

    /* state_json */
    {
        void *st = DEC_ISM.create(&(dec_cfg_t){.rate = FS}, (char[200]){0}, 200);
        sb_t sb = {0};
        DEC_ISM.state_json(st, &sb, 0);
        CHECK(strstr(sb.data, "\"devices\":[]") != NULL);
        sb_free(&sb);
        DEC_ISM.destroy(st);
    }

    printf("  coste: %.4f s de CPU por s de señal\n", sig_s > 0 ? cpu_s / sig_s : 0.0);
    CHECK(cpu_s / sig_s < VR_CPU(0.3));
    free(g_buf);
    return TEST_SUMMARY();
}
