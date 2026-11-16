/*
 * test_aprs.c — APRS/AX.25: paquetes publicados, modulador → canal →
 * decodificador, ruido y coste.
 *
 * Vectores publicados (ejemplos de "APRS Protocol Reference 1.0.1", recogidos en
 * las pruebas del crate aprs-decode, https://docs.rs/crate/aprs-decode/0.1.1/source/tests/integration.rs):
 *   !4903.50N/07201.75W-Test 123            → 49,058333 N, 72,029167 W
 *   @074849h4821.61N\01224.49E^322/103/A=003054
 *   !/ABCD#$%^- sT                         → compresión base 91 (25,970047 / −171,954290)
 *   ...W_220/004g005t077r000p000P000h50b09900   → meteorología
 *   N0CALL>PPPPPP:`(_fn"Oj/Hello world!     → Mic-E, 20 kn, 251°, símbolo "/j"
 *   ::W1AW-9   :Hello world{001, ;HFEST-18H*170403z3443.55N\08635.47Wh146.940MHz…
 */
#include "test_util.h"

#include "decoders/aprs.h"
#include "dsp/filter.h"
#include "tx/mod.h"

#include "vec_phase.h"
#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_APRS;

static void info(const char *dst, const char *s, aprs_pkt_t *p)
{
    memset(p, 0, sizeof *p);
    aprs_parse_info(dst, (const uint8_t *)s, (int)strlen(s), p);
}

static void test_parse(void)
{
    printf("APRS: paquetes publicados\n");
    aprs_pkt_t p;
    info("APRS", "!4903.50N/07201.75W-Test 123", &p);
    CHECK(!strcmp(p.kind, "position") && p.pos_valid);
    CHECK_NEAR(p.lat, 49.058333, 1e-5);
    CHECK_NEAR(p.lon, -72.029167, 1e-5);
    CHECK(p.symtab == '/' && p.symcode == '-');
    CHECK(!strcmp(p.comment, "Test 123"));

    info("APRS", "@074849h4821.61N\\01224.49E^322/103/A=003054", &p);
    CHECK_NEAR(p.lat, 48.360167, 1e-5);
    CHECK_NEAR(p.lon, 12.408167, 1e-5);
    CHECK(p.has_course && p.has_speed);
    CHECK_NEAR(p.course_deg, 322, 0.1);
    CHECK_NEAR(p.speed_kn, 103, 0.1);
    CHECK(p.has_alt);
    CHECK_NEAR(p.alt_m, 3054 * 0.3048, 0.1);

    info("APRS", "!/ABCD#$%^- sT", &p);
    CHECK(p.pos_valid);
    CHECK_NEAR(p.lat, 25.970047, 1e-3);
    CHECK_NEAR(p.lon, -171.954290, 1e-3);
    /* APRS101 cap. 9: "/5L!!<*e7>7P[" → 49,5 N, −72,75 E; 88° y 36,2 kn */
    info("APRS", "=/5L!!<*e7>7P[", &p);
    CHECK_NEAR(p.lat, 49.5, 1e-3);
    CHECK_NEAR(p.lon, -72.75, 1e-3);
    CHECK_NEAR(p.course_deg, 88.0, 0.1);
    CHECK_NEAR(p.speed_kn, 36.2, 0.1);

    info("APRS", "!4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900", &p);
    CHECK(!strcmp(p.kind, "weather") && p.wx);
    CHECK_NEAR(p.wind_dir, 220, 0.1);
    CHECK_NEAR(p.wind_mph, 4, 0.1);
    CHECK_NEAR(p.gust_mph, 5, 0.1);
    CHECK_NEAR(p.temp_f, 77, 0.1);
    CHECK_NEAR(p.hum_pct, 50, 0.1);
    CHECK_NEAR(p.baro_hpa, 990.0, 0.1);

    info("_10071820220/004g005t077r000p000P000h50b09900", "_10071820220/004g005t077r000p000P000h50b09900", &p);
    CHECK(!strcmp(p.kind, "weather"));
    CHECK_NEAR(p.temp_f, 77, 0.1);

    info("PPPPPP", "`(_fn\"Oj/Hello world!", &p);
    CHECK(!strcmp(p.kind, "mic-e") && p.pos_valid);
    CHECK_NEAR(p.speed_kn, 20, 0.1);
    CHECK_NEAR(p.course_deg, 251, 0.1);
    CHECK(p.symcode == 'j' && p.symtab == '/');
    CHECK(!strcmp(p.comment, "Hello world!"));
    CHECK(!strcmp(p.mice_msg, "Off Duty"));

    info("APDR15", ":W1AW-9   :Hello world{001", &p);
    CHECK(!strcmp(p.kind, "message"));
    CHECK(!strcmp(p.to, "W1AW-9") && !strcmp(p.msg, "Hello world") && !strcmp(p.msg_id, "001"));

    info("APZWX", ";HFEST-18H*170403z3443.55N\\08635.47Wh146.940MHz T100 Huntsville Hamfest", &p);
    CHECK(!strcmp(p.kind, "object") && !strcmp(p.obj_name, "HFEST-18H") && p.obj_live);
    CHECK_NEAR(p.lat, 34.725833, 1e-5);

    info("APRS", ">312359zSystem online", &p);
    CHECK(!strcmp(p.kind, "status") && !strcmp(p.status, "System online"));
    info("APRS", ">12.6V 0.2A 22degC", &p);
    CHECK(!strcmp(p.status, "12.6V 0.2A 22degC"));

    /* trama AX.25: construir y volver a leer */
    uint8_t f[300];
    const char *inf = "!4903.50N/07201.75W-Test 123";
    int n = ax25_build_ui("W1AW-9", "APRS", "WIDE1-1,WIDE2-2", (const uint8_t *)inf, (int)strlen(inf), f, sizeof f);
    CHECK(n > 0);
    CHECK_EQ(aprs_parse_frame(f, n, &p), 0);
    CHECK(!strcmp(p.src, "W1AW-9") && !strcmp(p.dst, "APRS") && !strcmp(p.path, "WIDE1-1,WIDE2-2"));
    CHECK(ax25_callsign_ok("EA2XYZ-9") && !ax25_callsign_ok("") && !ax25_callsign_ok("ea2xyz") && !ax25_callsign_ok("ABCDEFG"));
}

/* chan_drift de test_util.h reserva n+8 muestras y se desborda si n·ppm > 12:
 * se hace aquí con un búfer holgado (aviso en el informe). */
static void drift(cf32 *x, int n, double ppm)
{
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)(n + n / 500 + 64));
    int m = chan_resample(x, n, 1.0 + ppm * 1e-6, tmp);
    for (int i = 0; i < n; i++)
        x[i] = i < m ? tmp[i] : (cf32){0, 0};
    free(tmp);
}

static void channel(cf32 *x, int n, double snr_db, double cfo, double ppm, uint32_t *seed, double bw)
{
    if (ppm != 0)
        drift(x, n, ppm);
    if (cfo != 0)
        chan_cfo(x, n, 48000.0, cfo);
    chan_awgn(x, n, snr_db, seed);
    cfir_t f;
    cfir_init_bandpass(&f, 48000.0, -bw / 2, bw / 2, 3000.0, 301);
    cfir_process(&f, x, n, x);
    cfir_free(&f);
}

static void test_sim(void)
{
    printf("APRS: modulador → canal → decodificador\n");
    const modulator_desc_t *md = modulator_find("aprs");
    CHECK(md && md->air_ok);
    iqbuf_t iq = {0};
    char err[200] = "";
    CHECK(md->modulate("{\"callsign\":\"\"}", &iq, err, sizeof err) < 0);
    CHECK(strstr(err, "indicativo") != NULL);
    iq.n = 0;
    CHECK(md->modulate("{\"callsign\":\"mal indicativo\"}", &iq, err, sizeof err) < 0);
    CHECK_EQ(md->modulate("{\"callsign\":\"EA2XYZ-9\"}", &iq, err, sizeof err), 0);
    double secs = iq.n / iq.rate;
    printf("  señal de %.2f s (5 paquetes)\n", secs);
    CHECK(secs < 5.0);

    const double cfos[] = {-4000, 0, 4000}, ppms[] = {-100, 0, 100};
    int ok = 0, tot = 0;
    for (int ci = 0; ci < 3; ci++)
        for (int pi = 0; pi < 3; pi++) {
            cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
            memcpy(x, iq.s, sizeof(cf32) * (size_t)iq.n);
            uint32_t seed = 99u + (uint32_t)(ci * 3 + pi);
            channel(x, iq.n, 10.0, cfos[ci], ppms[pi], &seed, 16000.0);
            evs_t ev = {0};
            run_narrow(&DEC_APRS, x, iq.n, 144800000, NULL, &ev);
            printf("  10 dB, CFO %+5.0f Hz, %+4.0f ppm → %d/5\n", cfos[ci], ppms[pi], evs_count(&ev));
            ok += evs_count(&ev);
            tot += 5;
            CHECK(evs_count(&ev) >= 4);
            if (ci == 1 && pi == 1) {
                CHECK(ev_has(&ev, "\"src\":\"EA2XYZ-9\""));
                CHECK(ev_has(&ev, "\"kind\":\"position\""));
                CHECK(ev_has(&ev, "\"kind\":\"mic-e\""));
                CHECK(ev_has(&ev, "\"kind\":\"status\""));
                CHECK(ev_has(&ev, "\"kind\":\"message\""));
                CHECK(ev_has(&ev, "\"message\":\"Hola desde BladeRF Lab\""));
                CHECK(ev_has(&ev, "\"lat\":49.058"));
                CHECK(ev_has(&ev, "\"lon\":-72.029"));
            }
            evs_free(&ev);
            free(x);
        }
    printf("  éxito: %d/%d\n", ok, tot);
    iqbuf_free(&iq);

    /* objeto y meteorología, y preénfasis ±6 dB sobre el audio */
    const char *types[] = {"{\"callsign\":\"EA2XYZ\",\"type\":\"weather\"}", "{\"callsign\":\"EA2XYZ\",\"type\":\"object\"}"};
    const char *needles[] = {"\"temp_f\":77", "\"object\":\"BLFOBJ\""};
    for (int t = 0; t < 2; t++) {
        iq.n = 0;
        CHECK_EQ(md->modulate(types[t], &iq, err, sizeof err), 0);
        uint32_t seed = 5;
        channel(iq.s, iq.n, 14.0, 1500, 0, &seed, 16000.0);
        evs_t ev = {0};
        run_narrow(&DEC_APRS, iq.s, iq.n, 144800000, NULL, &ev);
        CHECK(ev_has(&ev, needles[t]));
        evs_free(&ev);
        iqbuf_free(&iq);
    }
}

static void test_noise_state(void)
{
    printf("APRS: ruido, estado y coste\n");
    int n = 48000 * 20;
    cf32 *x = calloc((size_t)n, sizeof(cf32));
    uint32_t s = 31337;
    for (int i = 0; i < n; i++) {
        x[i].re = (float)(0.1 * tu_gauss(&s));
        x[i].im = (float)(0.1 * tu_gauss(&s));
    }
    cfir_t f;
    cfir_init_bandpass(&f, 48000.0, -8000, 8000, 3000, 301);
    cfir_process(&f, x, n, x);
    cfir_free(&f);
    evs_t ev = {0};
    clock_t t0 = clock();
    run_narrow(&DEC_APRS, x, n, 144800000, NULL, &ev);
    double cost = (double)(clock() - t0) / CLOCKS_PER_SEC / (n / 48000.0);
    CHECK_EQ(evs_count(&ev), 0);
    printf("  coste: %.4f s de CPU por s de señal\n", cost);
    CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
    evs_free(&ev);
    free(x);

    iqbuf_t iq = {0};
    char err[200];
    modulator_find("aprs")->modulate("{\"callsign\":\"EA2XYZ-9\"}", &iq, err, sizeof err);
    dec_cfg_t cfg = {.rate = 48000, .freq_hz = 144800000, .center_hz = 144800000, .loc_valid = true, .lat = 49.0, .lon = -72.0};
    void *st = DEC_APRS.create(&cfg, err, sizeof err);
    DEC_APRS.process(st, iq.s, iq.n, 5000.0, tu_collect, &ev);
    sb_t sb = {0};
    DEC_APRS.state_json(st, &sb, 5010.0);
    CHECK(strstr(sb.data, "\"call\":\"EA2XYZ-9\"") && strstr(sb.data, "\"lat\":49.058") && strstr(sb.data, "\"dist_km\":"));
    sb_free(&sb);
    DEC_APRS.state_json(st, &sb, 5000.0 + 4000.0);
    CHECK(strstr(sb.data, "\"count\":0"));
    sb_free(&sb);
    DEC_APRS.destroy(st);
    evs_free(&ev);
    iqbuf_free(&iq);
}

/* Grabación real (V1): AFSK 1200 de la Signal Identification Wiki (sigidwiki.com, "AFSK1200
 * Sound", audio de un receptor FM; trama AX.25 UI de un BBS FBB, ZL1AB>FBB). Primeros 4 s
 * remodulados en FM (±3 kHz). multimon-ng (AFSK1200) y Dire Wolf dan la misma trama. */
static void test_real(void)
{
    printf("APRS: grabación real\n");
    int n = 0;
    cf32 *x = vp_load_phase("tests/data/aprs_afsk1200_phase.u8", &n);
    if (!x) {
        printf("  (fichero ausente, omitido)\n");
        return;
    }
    evs_t ev = {0};
    run_narrow(&DEC_APRS, x, n, 144800000, NULL, &ev);
    printf("  %d eventos\n", evs_count(&ev));
    CHECK(ev_has(&ev, "\"src\":\"ZL1AB\",\"dst\":\"FBB\""));
    CHECK(ev_has(&ev, "541973 P    439 ZL1BAV"));
    evs_free(&ev);
    free(x);
}

int main(void)
{
    test_parse();
    test_real();
    test_sim();
    test_noise_state();
    return TEST_SUMMARY();
}
