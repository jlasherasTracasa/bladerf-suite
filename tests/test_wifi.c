/*
 * test_wifi.c — Decodificador WiFi (balizas 802.11b/g/a), AGENTS.md §7.
 *
 * Vectores y comprobaciones independientes del modulador (IEEE 802.11-2016):
 *   - secuencia del aleatorizador x^7+x^4+1 con estado 1111111 (§17.3.5.5): los
 *     48 primeros bits 0000111011110010110010010000001000100110001011 10…;
 *   - polaridad de pilotos p0..p15 (§17.3.5.10) = 1,1,1,1,-1,-1,-1,1,-1,-1,-1,-1,1,1,-1,1;
 *   - CRC-32 de "123456789" = 0xCBF43926 (FCS de 802.11 es el CRC-32 de IEEE 802.3).
 * Pruebas de sistema: modulador → canal (AWGN con SNR de potencia MEDIA, CFO,
 * deriva de reloj ±100 ppm y, en OFDM, 2 ecos) → decodificador.
 * VECTOR REAL: tests/data/wifi_sigid_beacon_20M.cs8 = 3,8 ms (8 bit con signo, 20 MS/s) de la
 * grabación pública "Wi-Fi.zip" de sigidwiki.com (IQ 8 bit a 18 MS/s, remuestreada a 20 MS/s):
 * una baliza 802.11b DSSS 1 Mb/s de "UPC8443073.2,4G" (BSSID 34:2C:C4:18:95:3B, canal 1,
 * WPA2+WPA). Valida preámbulo/SFD, CRC-16 del PLCP, DBPSK/Barker, aleatorizador y FCS. En
 * esa grabación no hay tramas OFDM: OFDM 6 Mb/s sigue sin validarse con aire real.
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/wifi_phy.h"
#include "dsp/bits.h"
#include "dsp/crc.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_WIFI;
extern const modulator_desc_t MOD_WIFI;

/* AWGN con SNR respecto a la potencia media de las muestras no nulas. */
static void awgn_avg(cf32 *x, int n, double snr_db, uint32_t *seed)
{
    double sum = 0;
    long cnt = 0;
    for (int i = 0; i < n; i++) {
        double p = (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
        if (p > 1e-10) {
            sum += p;
            cnt++;
        }
    }
    double sigma = sqrt(sum / (double)cnt / pow(10.0, snr_db / 10.0) / 2.0);
    for (int i = 0; i < n; i++) {
        x[i].re += (float)(sigma * tu_gauss(seed));
        x[i].im += (float)(sigma * tu_gauss(seed));
    }
}

static void multipath(cf32 *x, int n)
{
    /* eco 1: 3 muestras (150 ns) −6 dB con fase 1,0 rad; eco 2: 7 muestras (350 ns) −10 dB con fase −2,1 rad */
    cf32 *c = malloc(sizeof(cf32) * (size_t)n);
    memcpy(c, x, sizeof(cf32) * (size_t)n);
    const float a1 = 0.5f, p1 = 1.0f, a2 = 0.316f, p2 = -2.1f;
    for (int i = 0; i < n; i++) {
        if (i >= 3) {
            x[i].re += a1 * (c[i - 3].re * cosf(p1) - c[i - 3].im * sinf(p1));
            x[i].im += a1 * (c[i - 3].re * sinf(p1) + c[i - 3].im * cosf(p1));
        }
        if (i >= 7) {
            x[i].re += a2 * (c[i - 7].re * cosf(p2) - c[i - 7].im * sinf(p2));
            x[i].im += a2 * (c[i - 7].re * sinf(p2) + c[i - 7].im * cosf(p2));
        }
    }
    free(c);
}

/* chan_drift de test_util.h reserva solo n+8 muestras y desborda con ppm>0 y n grande
 * (la salida de chan_resample mide ~n·(1+ppm·1e-6)): versión con búfer suficiente. */
static void drift(const cf32 *in, int n, double ppm, cf32 *out)
{
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)(n * 1.001 + 64));
    int m = chan_resample(in, n, 1.0 + ppm * 1e-6, tmp);
    for (int i = 0; i < n; i++)
        out[i] = i < m ? tmp[i] : (cf32){0, 0};
    free(tmp);
}

/* Genera, degrada y decodifica. Devuelve los eventos en evs. */
static int run_case(const char *params, double snr, double cfo, double ppm, bool mp, evs_t *evs, double *cpu_s_per_s)
{
    iqbuf_t b = {0};
    char err[200] = "";
    if (MOD_WIFI.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        return -1;
    }
    int n = b.n;
    cf32 *x = calloc((size_t)n, sizeof(cf32));
    if (ppm != 0)
        drift(b.s, n, ppm, x);
    else
        memcpy(x, b.s, sizeof(cf32) * (size_t)n);
    if (mp)
        multipath(x, n);
    if (cfo != 0)
        chan_cfo(x, n, WIFI_FS, cfo);
    uint32_t seed = 12345u + (uint32_t)(snr * 10);
    awgn_avg(x, n, snr, &seed);
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
    cf32_to_q11(x, n, 1.0, q);
    clock_t t0 = clock();
    run_wide(&DEC_WIFI, q, n, WIFI_FS, 2437000000LL, NULL, evs);
    if (cpu_s_per_s)
        *cpu_s_per_s = (double)(clock() - t0) / CLOCKS_PER_SEC / ((double)n / WIFI_FS);
    free(q);
    free(x);
    iqbuf_free(&b);
    return n;
}

int main(void)
{
    /* ---- Vectores de la norma ---- */
    lfsr_t l;
    lfsr_init_wifi(&l, 0x7F);
    const char *ref = "00001110111100101100100100000010001001100010111010110110"; /* §17.3.5.5, 56 primeros */
    char got[64];
    for (int i = 0; i < 56; i++)
        got[i] = (char)('0' + lfsr_prbs_bit(&l));
    got[56] = 0;
    CHECK(strcmp(got, ref) == 0);
    static const int pol[16] = {1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, -1, 1, 1, -1, 1};
    for (int i = 0; i < 16; i++)
        CHECK_EQ(wifi_pilot_polarity(i), pol[i]);
    CHECK_EQ(wifi_pilot_polarity(127), 1);
    crc_t c;
    crc_init(&c, crc_preset("CRC-32"));
    CHECK_EQ(crc_compute(&c, (const uint8_t *)"123456789", 9), 0xCBF43926u);
    /* LTF y STF: periodicidad 64 y 16 en el tiempo */
    CHECK_EQ(wifi_data_carrier(0), -26);
    CHECK_EQ(wifi_data_carrier(47), 26);
    CHECK_EQ(wifi_data_carrier(5), -20); /* se salta el piloto −21 */
    int jk[288];
    wifi_interleave_table(wifi_rate_by_mbps(6), jk);
    CHECK_EQ(jk[0], 0);
    CHECK_EQ(jk[1], 3);   /* k=1 → i = 3·1 */
    CHECK_EQ(jk[16], 1);  /* k=16 → i = 0 + 1 */
    CHECK_EQ(jk[47], 47);

    /* ---- 1. Dos redes (OFDM 6M + DSSS 1M), 12 dB, CFO +80 kHz, +100 ppm ---- */
    evs_t ev = {0};
    /* ---- Grabación real (sigidwiki) ---- */
    {
        int nn = 0;
        cf32 *x = vr_load_cs8("tests/data/wifi_sigid_beacon_20M.cs8", &nn);
        if (!x)
            printf("  (sin tests/data/wifi_sigid_beacon_20M.cs8: se omite)\n");
        else {
            int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)nn);
            for (int i = 0; i < nn; i++) {
                q[2 * i] = (int16_t)(x[i].re * 1800.0f);
                q[2 * i + 1] = (int16_t)(x[i].im * 1800.0f);
            }
            evs_t er = {0};
            run_wide(&DEC_WIFI, q, nn, 20e6, 2437000000LL, NULL, &er);
            if (!ev_has(&er, "34:2C:C4:18:95:3B"))
                printf("  real: sin baliza (%s)\n", er.data ? er.data : "-");
            CHECK(ev_has(&er, "\"ssid\":\"UPC8443073.2,4G\"") && ev_has(&er, "\"bssid\":\"34:2C:C4:18:95:3B\"") &&
                  ev_has(&er, "\"channel\":1") && ev_has(&er, "\"phy\":\"DSSS 1M\""));
            evs_free(&er);
            free(q);
            free(x);
        }
    }

    double cpu = 0;
    run_case(NULL, 12, 80e3, 100, false, &ev, &cpu);
    printf("  [mixto] eventos=%d  CPU=%.3f s/s\n%s", evs_count(&ev), cpu, ev.data ? ev.data : "");
    CHECK(ev_has(&ev, "\"ssid\":\"BladeRF-Lab\""));
    CHECK(ev_has(&ev, "\"ssid\":\"LegacyAP\""));
    CHECK(ev_has(&ev, "\"bssid\":\"02:00:5E:10:00:01\""));
    CHECK(ev_has(&ev, "\"channel\":6"));
    CHECK(ev_has(&ev, "\"security\":\"WPA2\""));
    CHECK(ev_has(&ev, "\"security\":\"open\""));
    CHECK(ev_has(&ev, "\"phy\":\"OFDM 6M\"") && ev_has(&ev, "\"phy\":\"DSSS 1M\""));
    CHECK_EQ(evs_count(&ev), 2); /* un evento por red, no por trama (repeat=2) */
    evs_free(&ev);

    /* ---- 2. OFDM con 2 ecos, 10 dB, CFO −120 kHz y −100 ppm; luego +100 ppm y +150 kHz ---- */
    const char *og = "{\"std\":\"g\",\"ssid\":\"Casa Ñandú\",\"channel\":11,\"security\":\"wpa3\",\"repeat\":3}";
    run_case(og, 10, -120e3, -100, true, &ev, NULL);
    CHECK(ev_has(&ev, "Casa Ñandú") && ev_has(&ev, "\"security\":\"WPA3\"") && ev_has(&ev, "\"channel\":11"));
    CHECK_EQ(evs_count(&ev), 1);
    evs_free(&ev);
    run_case(og, 11, 150e3, 100, true, &ev, NULL);
    CHECK(ev_has(&ev, "\"security\":\"WPA3\""));
    evs_free(&ev);

    /* ---- 3. DSSS 1 Mb/s: 12 dB, CFO −100 kHz, −100 ppm ---- */
    const char *ob = "{\"std\":\"b\",\"ssid\":\"Cafe-Retro\",\"security\":\"wep\",\"channel\":1,\"repeat\":2}";
    run_case(ob, 12, -100e3, -100, false, &ev, NULL);
    CHECK(ev_has(&ev, "\"ssid\":\"Cafe-Retro\"") && ev_has(&ev, "\"security\":\"WEP\"") && ev_has(&ev, "\"std\":\"b\""));
    evs_free(&ev);
    run_case(ob, 15, 60e3, 100, false, &ev, NULL);
    CHECK(ev_has(&ev, "\"ssid\":\"Cafe-Retro\""));
    evs_free(&ev);

    /* ---- 4. Otras tasas OFDM y seguridad WPA/open/HT ---- */
    static const int rates[3] = {9, 12, 18};
    for (int i = 0; i < 3; i++) {
        char p[200];
        snprintf(p, sizeof p, "{\"std\":\"g\",\"ssid\":\"R%d\",\"rate\":%d,\"repeat\":1,\"security\":\"wpa\",\"ht\":true}",
                 rates[i], rates[i]);
        run_case(p, 15, 40e3, 60, true, &ev, NULL);
        char needle[40];
        snprintf(needle, sizeof needle, "\"phy\":\"OFDM %dM\"", rates[i]);
        CHECK(ev_has(&ev, needle));
        CHECK(ev_has(&ev, "\"security\":\"WPA\"") && ev_has(&ev, "\"std\":\"n\""));
        evs_free(&ev);
    }

    /* ---- 5. Ética: respuesta de sondeo (la MAC del cliente no sale) y datos (se descartan) ---- */
    run_case("{\"std\":\"g\",\"ssid\":\"Sondeo\",\"frame\":\"probe_resp\",\"repeat\":1}", 14, 0, 0, false, &ev, NULL);
    CHECK(ev_has(&ev, "\"kind\":\"probe_resp\"") && ev_has(&ev, "\"ssid\":\"Sondeo\""));
    CHECK(!ev_has(&ev, "AA:BB:CC") && !ev_has(&ev, "aa:bb:cc") && !ev_has(&ev, "DD:EE"));
    evs_free(&ev);
    run_case("{\"std\":\"gb\",\"ssid\":\"Datos\",\"frame\":\"data\",\"repeat\":2}", 14, 0, 0, false, &ev, NULL);
    CHECK_EQ(evs_count(&ev), 0);
    evs_free(&ev);

    /* ---- 6. Solo ruido (gaussiano a −30 dBFS RMS): 0 eventos ---- */
    {
        int n = 4000000;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 777;
        for (int i = 0; i < n; i++) {
            x[i].re = 0.03f * (float)tu_gauss(&seed);
            x[i].im = 0.03f * (float)tu_gauss(&seed);
        }
        int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
        cf32_to_q11(x, n, 1.0, q);
        clock_t t0 = clock();
        run_wide(&DEC_WIFI, q, n, WIFI_FS, 2437000000LL, NULL, &ev);
        printf("  [ruido] eventos=%d  CPU=%.3f s/s\n", evs_count(&ev),
               (double)(clock() - t0) / CLOCKS_PER_SEC / ((double)n / WIFI_FS));
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        free(x);
        free(q);
    }

    /* ---- 7. Coste ---- */
    {
        double c2 = 0, c3 = 0;
        /* realista: ~25 % de ocupación del aire con balizas OFDM y DSSS (≈ 280 balizas/s) */
        run_case("{\"std\":\"gb\",\"repeat\":40,\"gap_us\":3000}", 20, 20e3, 20, false, &ev, &c2);
        evs_free(&ev);
        /* saturado: 100 % de ocupación (peor caso, no realista); solo se informa */
        run_case("{\"std\":\"gb\",\"repeat\":40,\"gap_us\":100}", 20, 20e3, 20, false, &ev, &c3);
        evs_free(&ev);
        printf("  [coste] ocupación ~25 %%: %.3f s CPU por s de señal (límite 0,3); saturado 100 %%: %.3f\n", c2, c3);
        CHECK(c2 < VR_CPU(0.3)); /* bajo sanitizer el umbral se relaja ×8 */
    }
    return TEST_SUMMARY();
}
