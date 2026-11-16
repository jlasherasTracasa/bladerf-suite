/*
 * test_ais.c — AIS: capa de carga útil con sentencias AIVDM publicadas,
 * modulador → canal → decodificador, ruido y coste de CPU.
 *
 * Vectores publicados:
 *  - gpsd, "AIVDM/AIVDO protocol decoding" (https://gpsd.gitlab.io/gpsd/AIVDM.html):
 *      !AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C   (tipo 1)
 *      !AIVDM,2,1,3,B,55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53,0*3E
 *      !AIVDM,2,2,3,B,1@0000000000000,2*55                (tipo 5, dos partes)
 *    La página no da los campos decodificados; los valores comprobados aquí
 *    (tipo 1: MMSI 477553000, Seattle 47,58283 N -122,34583; tipo 5: MT.MITCHELL,
 *    WDA9674, SEATTLE) salen de nuestra capa y son coherentes entre sí
 *    (checksum NMEA correcto, longitudes 168/424, posición plausible).
 */
#include "test_util.h"

#include "decoders/ais.h"
#include "dsp/filter.h"
#include "tx/mod.h"
#include "util/json.h"

#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_AIS;

/* Canal: deriva de reloj → CFO → AWGN (banda de 48 kHz) → filtro de canal del
 * DDC (±bw/2), que es lo que el decodificador recibiría en el servidor. */
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
    if (snr_db < 100)
        chan_awgn(x, n, snr_db, seed);
    cfir_t f;
    cfir_init_bandpass(&f, 48000.0, -bw / 2, bw / 2, 3000.0, 301);
    cfir_process(&f, x, n, x);
    cfir_free(&f);
}

static double field(const evs_t *e, int type, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"msg_type\":%d,", type);
    const char *l = e->data;
    while (l && (l = strstr(l, pat))) {
        const char *eol = strchr(l, '\n');
        char k[40];
        snprintf(k, sizeof k, "\"%s\":", key);
        const char *p = strstr(l, k);
        if (p && (!eol || p < eol))
            return atof(p + strlen(k));
        l = eol;
    }
    return -9999;
}

static void test_payload(void)
{
    printf("AIS: sentencias AIVDM publicadas\n");
    uint8_t bits[1100];
    const char *s1 = "!AIVDM,1,1,,B,177KQJ5000G?tO`K>RA1wUbN0TKH,0*5C";
    CHECK(ais_nmea_check(s1));
    int n = ais_payload_to_bits(strchr(s1 + 14, ',') ? s1 + 14 : s1, 0, bits, 1100);
    CHECK_EQ(n, 168);
    ais_msg_t m;
    CHECK_EQ(ais_parse(bits, n, &m), 0);
    CHECK_EQ(m.type, 1);
    printf("  tipo 1: MMSI %u lat %.5f lon %.5f sog %.1f cog %.1f hdg %d\n", (unsigned)m.mmsi, m.lat, m.lon,
           m.sog_kn, m.cog_deg, m.hdg);
    CHECK_EQ(m.mmsi, 477553000);
    CHECK(m.pos_valid);
    CHECK_NEAR(m.lat, 47.58283, 1e-4);
    CHECK_NEAR(m.lon, -122.34583, 1e-4);
    /* ida y vuelta: NMEA regenerada == original */
    sb_t sb = {0};
    ais_nmea(bits, n, 'B', 0, &sb);
    CHECK(strcmp(sb.data, s1) == 0);
    if (strcmp(sb.data, s1))
        printf("  regenerada: %s\n", sb.data);
    sb_free(&sb);

    /* tipo 5 en dos sentencias */
    const char *p1 = "55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53";
    const char *p2 = "1@0000000000000";
    CHECK(ais_nmea_check("!AIVDM,2,1,3,B,55P5TL01VIaAL@7WKO@mBplU@<PDhh000000001S;AJ::4A80?4i@E53,0*3E"));
    CHECK(ais_nmea_check("!AIVDM,2,2,3,B,1@0000000000000,2*55"));
    int n1 = ais_payload_to_bits(p1, 0, bits, 1100);
    int n2 = ais_payload_to_bits(p2, 2, bits + n1, 1100 - n1);
    CHECK_EQ(n1 + n2, 424);
    CHECK_EQ(ais_parse(bits, n1 + n2, &m), 0);
    CHECK_EQ(m.type, 5);
    printf("  tipo 5: MMSI %u nombre '%s' indicativo '%s' destino '%s' tipo %d IMO %d dim %d/%d/%d/%d\n",
           (unsigned)m.mmsi, m.name, m.callsign, m.dest, m.shiptype, m.imo, m.bow, m.stern, m.port, m.stbd);
    CHECK_EQ(m.mmsi, 369190000);
    CHECK(strcmp(m.name, "MT.MITCHELL") == 0);
    CHECK(strcmp(m.callsign, "WDA9674") == 0);
    CHECK(strcmp(m.dest, "SEATTLE") == 0);
    /* nuestra partición en sentencias puede diferir de la original, pero cada una
     * debe pasar el checksum y recomponer la misma carga */
    sb_t sb2 = {0};
    ais_nmea(bits, n1 + n2, 'B', 3, &sb2);
    CHECK(strncmp(sb2.data, "!AIVDM,", 7) == 0);
    for (char *l = sb2.data; l && *l;) {
        char *e = strchr(l, '\n');
        if (e)
            *e = 0;
        CHECK(ais_nmea_check(l));
        l = e ? e + 1 : NULL;
    }
    sb_free(&sb2);
}

static int count_type(const evs_t *e, int type)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"msg_type\":%d,", type);
    int c = 0;
    for (const char *p = e->data; p && (p = strstr(p, pat)); p++)
        c++;
    return c;
}

static void test_sim(void)
{
    printf("AIS: modulador → canal → decodificador\n");
    const modulator_desc_t *md = modulator_find("ais");
    CHECK(md != NULL);
    CHECK(!md->air_ok);
    iqbuf_t iq = {0};
    char err[200] = "";
    CHECK_EQ(md->modulate(NULL, &iq, err, sizeof err), 0);
    CHECK_NEAR(iq.rate, 48000.0, 0.1);
    double secs = iq.n / iq.rate;
    printf("  señal de %.2f s, 9 ráfagas (1,5,18,24A,24B,19,21,4,27)\n", secs);

    /* sin ruido */
    {
        cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
        memcpy(x, iq.s, sizeof(cf32) * (size_t)iq.n);
        evs_t ev = {0};
        run_narrow(&DEC_AIS, x, iq.n, 161975000, NULL, &ev);
        printf("  limpio: %d eventos\n", evs_count(&ev));
        CHECK_EQ(evs_count(&ev), 9);
        CHECK(ev_has(&ev, "\"mmsi\":265547250"));
        CHECK(ev_has(&ev, "\"name\":\"EVER DIADEM\""));
        CHECK(ev_has(&ev, "\"dest\":\"NEW YORK\""));
        CHECK(ev_has(&ev, "\"callsign\":\"3FOF8\""));
        CHECK(ev_has(&ev, "\"nmea\":\"!AIVDM,"));
        CHECK_NEAR(field(&ev, 1, "lat"), 57.66035, 1e-5);
        CHECK_NEAR(field(&ev, 1, "lon"), 11.832976, 1e-5);
        CHECK_NEAR(field(&ev, 1, "sog"), 13.9, 0.05);
        CHECK_NEAR(field(&ev, 1, "cog"), 40.4, 0.05);
        CHECK_EQ((int)field(&ev, 1, "hdg"), 41);
        CHECK_NEAR(field(&ev, 18, "lat"), 57.66035, 1e-5);
        CHECK_NEAR(field(&ev, 27, "lat"), 57.66035, 0.002);
        CHECK(count_type(&ev, 24) == 2 && count_type(&ev, 21) == 1 && count_type(&ev, 19) == 1);
        /* cada NMEA del evento debe pasar el checksum */
        for (const char *l = strstr(ev.data, "\"nmea\":\""); l; l = strstr(l + 1, "\"nmea\":\"")) {
            char s1[300];
            const char *a = l + 8;
            size_t k = 0;
            for (; a[k] && a[k] != '"' && k < sizeof s1 - 1; k++)
                s1[k] = a[k];
            s1[k] = 0;
            char *nl = strstr(s1, "\\n");
            if (nl)
                *nl = 0;
            CHECK(ais_nmea_check(s1));
        }
        evs_free(&ev);
        free(x);
    }

    /* AWGN 10 dB, CFO ±1/4 del canal (±6250 Hz), reloj ±100 ppm */
    const double cfos[] = {-6250, 0, 6250}, ppms[] = {-100, 0, 100};
    int tot = 0, ok = 0;
    for (int ci = 0; ci < 3; ci++)
        for (int pi = 0; pi < 3; pi++) {
            cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
            memcpy(x, iq.s, sizeof(cf32) * (size_t)iq.n);
            uint32_t seed = 777u + (uint32_t)(ci * 3 + pi);
            channel(x, iq.n, 10.0, cfos[ci], ppms[pi], &seed, 25000.0);
            evs_t ev = {0};
            run_narrow(&DEC_AIS, x, iq.n, 161975000, NULL, &ev);
            printf("  10 dB, CFO %+6.0f Hz, %+4.0f ppm → %d/9\n", cfos[ci], ppms[pi], evs_count(&ev));
            tot += 9;
            ok += evs_count(&ev);
            CHECK(evs_count(&ev) >= 8);
            if (ci == 1 && pi == 1) {
                CHECK(ev_has(&ev, "\"mmsi\":265547250"));
                CHECK_NEAR(field(&ev, 1, "lat"), 57.66035, 1e-5);
            }
            evs_free(&ev);
            free(x);
        }
    printf("  tasa de éxito con 10 dB: %d/%d\n", ok, tot);
    iqbuf_free(&iq);
}

static void test_noise(void)
{
    printf("AIS: solo ruido\n");
    int n = 48000 * 20;
    cf32 *x = calloc((size_t)n, sizeof(cf32));
    uint32_t s = 4242;
    for (int i = 0; i < n; i++) {
        x[i].re = (float)(0.1 * tu_gauss(&s));
        x[i].im = (float)(0.1 * tu_gauss(&s));
    }
    cfir_t f;
    cfir_init_bandpass(&f, 48000.0, -12500, 12500, 3000, 301);
    cfir_process(&f, x, n, x);
    cfir_free(&f);
    evs_t ev = {0};
    run_narrow(&DEC_AIS, x, n, 161975000, NULL, &ev);
    CHECK_EQ(evs_count(&ev), 0);
    evs_free(&ev);

    /* coste de CPU: segundos de CPU por segundo de señal */
    clock_t t0 = clock();
    run_narrow(&DEC_AIS, x, n, 161975000, NULL, &ev);
    double cost = (double)(clock() - t0) / CLOCKS_PER_SEC / (n / 48000.0);
    printf("  coste: %.4f s de CPU por s de señal\n", cost);
    CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
    evs_free(&ev);
    free(x);
}

static void test_state(void)
{
    printf("AIS: tabla de barcos y caducidad\n");
    iqbuf_t iq = {0};
    char err[200];
    modulator_find("ais")->modulate(NULL, &iq, err, sizeof err);
    dec_cfg_t cfg = {.rate = 48000, .freq_hz = 162025000, .center_hz = 162025000, .loc_valid = true, .lat = 57.7, .lon = 11.9};
    void *st = DEC_AIS.create(&cfg, err, sizeof err);
    evs_t ev = {0};
    DEC_AIS.process(st, iq.s, iq.n, 1000.0, tu_collect, &ev);
    sb_t sb = {0};
    DEC_AIS.state_json(st, &sb, 1010.0);
    CHECK(strstr(sb.data, "\"ships\":["));
    CHECK(strstr(sb.data, "\"mmsi\":265547250") && strstr(sb.data, "\"name\":\"EVER DIADEM\""));
    CHECK(strstr(sb.data, "\"dist_km\":"));
    CHECK(ev_has(&ev, ",B,")); /* canal B por la frecuencia */
    sb_free(&sb);
    DEC_AIS.state_json(st, &sb, 1000.0 + 700.0);
    CHECK(strstr(sb.data, "\"count\":0"));
    sb_free(&sb);
    DEC_AIS.destroy(st);
    evs_free(&ev);
    iqbuf_free(&iq);
}

/* Grabación real (V1): AIS de Long Beach / Los Ángeles (EE. UU.), audio FM de la Signal
 * Identification Wiki ("AIS Sound", sigidwiki.com), 1,0-2,2 s remodulado en FM (±2,4 kHz), IQ
 * cs8 a 48 kHz. Lo que corrigió: los campos de M.1371 van con cada octeto invertido respecto
 * al flujo HDLC (antes salía "tipo 8, MMSI 98xxxxxxx"; una implementación independiente en
 * Python da tipo 1 y MMSI 36xxxxxxx). La trama es de un buque de EE. UU. (MID 366/367). */
static void test_real(void)
{
    printf("AIS: grabación real\n");
    /* NMEA tal como lo emite el decodificador sobre la grabación completa */
    static const char *nmea[2] = {"15N07b?P00GRn56CDU`@0?wJ0HKO", "15MvlaPP00GRm`pCDHE82?wf08QP"};
    static const uint32_t mmsi[2] = {367003560, 366982310};
    for (int i = 0; i < 2; i++) {
        uint8_t bits[200];
        int nb = ais_payload_to_bits(nmea[i], 0, bits, sizeof bits);
        ais_msg_t m;
        CHECK_EQ(nb, 168);
        CHECK(ais_parse(bits, nb, &m) == 0);
        CHECK_EQ(m.type, 1);
        CHECK_EQ(m.mmsi, mmsi[i]);
        /* ida y vuelta del reordenado de octetos */
        uint8_t w[200], b2[200];
        ais_unscramble_bytes(bits, nb, w);
        ais_unscramble_bytes(w, nb, b2);
        CHECK(memcmp(bits, b2, (size_t)nb) == 0);
    }
    int n = 0;
    cf32 *x = vr_load_cs8("tests/data/ais_la_fm48k.cs8", &n);
    if (!x) {
        printf("  (fichero ausente, omitido)\n");
        return;
    }
    evs_t ev = {0};
    run_narrow(&DEC_AIS, x, n, 161975000, NULL, &ev);
    printf("  %d eventos\n", evs_count(&ev));
    CHECK(ev_has(&ev, "\"msg_type\":1,\"repeat\":0,\"mmsi\":367003560"));
    CHECK(ev_has(&ev, "\"lat\":33.767095,\"lon\":-118.217115"));
    evs_free(&ev);
    free(x);
}

int main(void)
{
    test_real();
    test_payload();
    test_sim();
    test_noise();
    test_state();
    return TEST_SUMMARY();
}
