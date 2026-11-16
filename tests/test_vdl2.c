/*
 * test_vdl2.c — VDL Modo 2: primitivas (cabecera, FEC RS entrelazado, HDLC),
 * modulador → canal (AWGN 10 dB, CFO ±¼ de 25 kHz = ±6,25 kHz, ±100 ppm) →
 * decodificador a 105 kHz, solo ruido y coste de CPU.
 *
 * Vectores reales: no hay ninguna captura IQ de VDL2 con licencia que permita
 * incluirla y los ejemplos de la norma (ICAO Doc 9776, ARINC 631) son de pago;
 * por tanto la capa física y el tramado se prueban solo de forma autoconsistente
 * (modulador y decodificador comparten las mismas constantes, ver vdl2_frame.h).
 * Lo que sí es independiente: el preset RS (verificado en test_fec) y el CRC-16/X-25.
 */
#include "test_util.h"

#include "decoders/vdl2_avlc.h"
#include "decoders/vdl2_frame.h"
#include "tx/mod.h"

#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_VDL2;
extern const modulator_desc_t MOD_VDL2;

static int g_frames;
static void count_cb(void *c, const uint8_t *f, int n)
{
    (void)c;
    (void)f;
    (void)n;
    g_frames++;
}

static void test_primitives(void)
{
    printf("VDL2: cabecera, FEC y HDLC\n");
    uint8_t h[25];
    uint32_t len;
    for (uint32_t l = 100; l < 131071; l += 7919) {
        vdl2_hdr_encode(l, h);
        CHECK_EQ(vdl2_hdr_decode(h, &len), 0);
        CHECK_EQ(len, l);
        for (int i = 0; i < 25; i++) { /* un error en cualquier posición se corrige */
            uint8_t g[25];
            memcpy(g, h, 25);
            g[i] ^= 1;
            CHECK_EQ(vdl2_hdr_decode(g, &len), 1);
            CHECK_EQ(len, l);
        }
    }
    /* aleatorizador involutivo */
    uint8_t a[1000], b[1000];
    for (int i = 0; i < 1000; i++)
        a[i] = b[i] = (uint8_t)((i * 7) & 1);
    vdl2_scramble(b, 1000);
    CHECK(memcmp(a, b, 1000) != 0);
    vdl2_scramble(b, 1000);
    CHECK(memcmp(a, b, 1000) == 0);

    /* FEC: varios tamaños (bloque corto, justo 249, 2 bloques, 3 bloques con resto) */
    int sizes[] = {2, 3, 30, 31, 67, 68, 100, 249, 250, 498, 504, 600, 1300};
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        int D = sizes[s];
        uint8_t *d = malloc((size_t)D), *dd = malloc((size_t)D + 255);
        for (int i = 0; i < D; i++)
            d[i] = (uint8_t)(i * 37 + 11);
        int E = vdl2_encoded_bytes(D);
        uint8_t *e = malloc((size_t)E);
        vdl2_fec_encode(d, D, e);
        CHECK_EQ(vdl2_fec_decode(e, D, dd), 0);
        CHECK(memcmp(d, dd, (size_t)D) == 0);
        /* 2 bytes erróneos consecutivos en el flujo (el entrelazado los reparte) */
        if (D > 30) {
            e[E / 2] ^= 0xFF;
            e[E / 2 + 1] ^= 0x5A;
            int c = vdl2_fec_decode(e, D, dd);
            CHECK(c >= 1);
            CHECK(memcmp(d, dd, (size_t)D) == 0);
        }
        free(d);
        free(dd);
        free(e);
    }
    /* ráfaga de 6 bytes con 3 bloques: el entrelazado la reparte (≤2 por bloque) */
    {
        int D = 600, E = vdl2_encoded_bytes(D);
        uint8_t d[600], dd[900], e[700];
        for (int i = 0; i < D; i++)
            d[i] = (uint8_t)(i ^ 0x33);
        vdl2_fec_encode(d, D, e);
        for (int i = 100; i < 106; i++)
            e[i] ^= 0xA5;
        CHECK(vdl2_fec_decode(e, D, dd) >= 6);
        CHECK(memcmp(d, dd, (size_t)D) == 0);
        (void)E;
    }

    /* HDLC con datos que fuerzan relleno de ceros */
    uint8_t f[40], bits[800];
    for (int i = 0; i < 20; i++)
        f[i] = (i % 3 == 0) ? 0xFF : (uint8_t)(0x7E + i);
    int n = vdl2_add_fcs(f, 20);
    int nb = vdl2_hdlc_encode(f, n, bits, sizeof bits);
    CHECK(nb > 8 * n);
    g_frames = 0;
    CHECK_EQ(vdl2_hdlc_decode(bits, nb, count_cb, NULL), 1);
    CHECK_EQ(g_frames, 1);
    bits[40] ^= 1; /* FCS incorrecto */
    g_frames = 0;
    CHECK_EQ(vdl2_hdlc_decode(bits, nb, count_cb, NULL), 0);

    /* Direcciones AVLC */
    uint8_t fr[12];
    avlc_addr_put(fr, 0x4840D6, 1, 0);
    avlc_addr_put(fr + 4, 0x2B4F10, 4, 1);
    fr[8] = 0x03;
    avlc_t av;
    CHECK_EQ(avlc_parse(fr, 9, &av), 0);
    CHECK_EQ(av.dst.icao, 0x4840D6);
    CHECK_EQ(av.src.icao, 0x2B4F10);
    CHECK_EQ(av.src.type, 4);
    CHECK_EQ(av.src.flag, 1);
    CHECK(strcmp(av.ftype, "UI") == 0);
}

static void run_case(const char *name, const iqbuf_t *src, double cfo, double ppm, double snr, uint32_t seed,
                     int expect, const char *must)
{
    int n = src->n;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    chan_drift(src->s, n, ppm, x);
    chan_cfo(x, n, src->rate, cfo);
    chan_awgn(x, n, snr, &seed);
    evs_t ev = {0};
    run_narrow(&DEC_VDL2, x, n, 136975000, NULL, &ev);
    printf("  %-32s eventos=%d\n", name, evs_count(&ev));
    CHECK_EQ(evs_count(&ev), expect);
    if (must)
        CHECK(ev_has(&ev, must));
    evs_free(&ev);
    free(x);
}

int main(void)
{
    test_primitives();

    char err[200] = "";
    iqbuf_t iq = {0};
    CHECK(MOD_VDL2.modulate(NULL, &iq, err, sizeof err) == 0);
    CHECK_NEAR(iq.rate, 105000, 0.5);
    CHECK(!MOD_VDL2.air_ok);
    printf("VDL2: modulador → canal → decodificador (2 ráfagas por prueba)\n");
    run_case("limpio", &iq, 0, 0, 40, 1, 2, "\"src\":\"4840D6\"");
    run_case("10 dB", &iq, 0, 0, 10, 2, 2, NULL);
    run_case("10 dB, +6,25 kHz, +100 ppm", &iq, 6250, 100, 10, 3, 2, NULL);
    run_case("10 dB, -6,25 kHz, -100 ppm", &iq, -6250, -100, 10, 4, 2, NULL);
    run_case("10 dB, +6,25 kHz, -100 ppm", &iq, 6250, -100, 10, 5, 2, NULL);
    run_case("10 dB, -6,25 kHz, +100 ppm", &iq, -6250, 100, 10, 6, 2, NULL);
    run_case("10 dB, +1,3 kHz", &iq, 1300, 0, 10, 7, 2, NULL);

    /* Campos */
    {
        cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
        memcpy(x, iq.s, sizeof(cf32) * (size_t)iq.n);
        uint32_t seed = 99;
        chan_cfo(x, iq.n, iq.rate, 2500);
        chan_awgn(x, iq.n, 12, &seed);
        evs_t ev = {0};
        run_narrow(&DEC_VDL2, x, iq.n, 136975000, NULL, &ev);
        CHECK(ev_has(&ev, "\"src\":\"4840D6\""));
        CHECK(ev_has(&ev, "\"src_type\":\"aircraft\""));
        CHECK(ev_has(&ev, "\"dst\":\"2B4F10\""));
        CHECK(ev_has(&ev, "\"dst_type\":\"ground\""));
        CHECK(ev_has(&ev, "\"ftype\":\"I\""));
        CHECK(ev_has(&ev, "\"reg\":\"EC-MXY\""));
        CHECK(ev_has(&ev, "\"flight\":\"IB3456\""));
        CHECK(ev_has(&ev, "\"label\":\"H1\""));
        CHECK(ev_has(&ev, "\"body\":\"#DFB/WX LEMD LEBL 151200Z 27008KT CAVOK\""));
        CHECK(ev_has(&ev, "\"text\":\""));
        evs_free(&ev);
        free(x);
    }

    /* Otros tipos de trama y una trama larga (varios bloques RS) */
    {
        const char *cases[][2] = {
            {"{\"ftype\":\"UI\",\"acars\":false,\"repeat\":1}", "\"ftype\":\"UI\""},
            {"{\"ftype\":\"RR\",\"nr\":5,\"poll\":true,\"repeat\":1}", "\"ftype\":\"RR\""},
            {"{\"ftype\":\"XID\",\"info\":\"82F1\",\"repeat\":1}", "\"info\":\"82F1\""},
            {"{\"ftype\":\"I\",\"ns\":3,\"nr\":2,\"src\":\"ABCDEF\",\"dst\":\"123456\",\"repeat\":1}",
             "\"ns\":3,\"nr\":2"},
        };
        for (unsigned k = 0; k < 4; k++) {
            iqbuf_t m = {0};
            CHECK(MOD_VDL2.modulate(cases[k][0], &m, err, sizeof err) == 0);
            uint32_t seed = 31 + k;
            chan_cfo(m.s, m.n, m.rate, -3000);
            chan_awgn(m.s, m.n, 11, &seed);
            evs_t ev = {0};
            run_narrow(&DEC_VDL2, m.s, m.n, 136975000, NULL, &ev);
            CHECK_EQ(evs_count(&ev), 1);
            CHECK(ev_has(&ev, cases[k][1]));
            evs_free(&ev);
            iqbuf_free(&m);
        }
        /* trama de 350 bytes de información: 2 bloques RS entrelazados, con 100 ppm */
        char big[1200] = "{\"ftype\":\"I\",\"repeat\":1,\"info\":\"";
        size_t o = strlen(big);
        for (int i = 0; i < 350; i++)
            o += (size_t)snprintf(big + o, sizeof big - o, "%02X", (i * 13) & 0xFF);
        snprintf(big + o, sizeof big - o, "\"}");
        for (int ppm = -100; ppm <= 100; ppm += 200) {
            iqbuf_t m = {0};
            CHECK(MOD_VDL2.modulate(big, &m, err, sizeof err) == 0);
            uint32_t seed = (uint32_t)(55 + ppm);
            chan_drift(m.s, m.n, ppm, m.s);
            chan_cfo(m.s, m.n, m.rate, 5000.0 * (ppm > 0 ? 1 : -1));
            chan_awgn(m.s, m.n, 10, &seed);
            evs_t ev = {0};
            run_narrow(&DEC_VDL2, m.s, m.n, 136975000, NULL, &ev);
            CHECK_EQ(evs_count(&ev), 1);
            evs_free(&ev);
            iqbuf_free(&m);
        }
    }

    /* Estado */
    {
        void *st = DEC_VDL2.create(&(dec_cfg_t){.rate = 105000, .freq_hz = 136975000}, err, sizeof err);
        CHECK(st != NULL);
        evs_t ev = {0};
        for (int off = 0; off < iq.n; off += 4096)
            DEC_VDL2.process(st, iq.s + off, MIN(4096, iq.n - off), 1000.0, tu_collect, &ev);
        sb_t sb = {0};
        DEC_VDL2.state_json(st, &sb, 1010.0);
        CHECK(strstr(sb.data, "\"icao\":\"4840D6\"") != NULL);
        CHECK(strstr(sb.data, "\"gs\":[{\"icao\":\"2B4F10\"") != NULL);
        sb_free(&sb);
        DEC_VDL2.state_json(st, &sb, 1000.0 + 5000.0);
        CHECK(strstr(sb.data, "4840D6") == NULL);
        sb_free(&sb);
        evs_free(&ev);
        DEC_VDL2.destroy(st);
    }

    /* Solo ruido: 0 eventos; también con una portadora CW intensa */
    {
        int n = 105000 * 30;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++)
            x[i] = (cf32){(float)(0.1 * tu_gauss(&seed)), (float)(0.1 * tu_gauss(&seed))};
        evs_t ev = {0};
        clock_t t0 = clock();
        run_narrow(&DEC_VDL2, x, n, 136975000, NULL, &ev);
        double cost = (double)(clock() - t0) / CLOCKS_PER_SEC / (n / 105000.0);
        CHECK_EQ(evs_count(&ev), 0);
        printf("  coste en ruido: %.4f s de CPU por s de señal\n", cost);
        CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
        evs_free(&ev);
        for (int i = 0; i < n; i++) { /* tono continuo (no es una ráfaga VDL2) */
            double ph = 2 * M_PI * 3000.0 * i / 105000.0;
            x[i].re += (float)(0.5 * cos(ph));
            x[i].im += (float)(0.5 * sin(ph));
        }
        run_narrow(&DEC_VDL2, x, n, 136975000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        free(x);
    }

    /* Coste con tráfico: una ráfaga cada ~0,2 s durante 20 s */
    {
        int per = iq.n / 2, reps = 100;
        int n = per * reps;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        for (int r = 0; r < reps; r++)
            memcpy(x + (size_t)r * per, iq.s, sizeof(cf32) * (size_t)per);
        uint32_t seed = 8;
        chan_awgn(x, n, 12, &seed);
        evs_t ev = {0};
        clock_t t0 = clock();
        run_narrow(&DEC_VDL2, x, n, 136975000, NULL, &ev);
        double secs = n / 105000.0, cost = (double)(clock() - t0) / CLOCKS_PER_SEC / secs;
        printf("  tráfico denso: %d ráfagas decodificadas de %d, coste %.4f s de CPU por s de señal (%.1f s)\n",
               evs_count(&ev), reps, cost, secs);
        CHECK(evs_count(&ev) >= reps * 9 / 10);
        CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
        evs_free(&ev);
        free(x);
    }

    iqbuf_free(&iq);
    return TEST_SUMMARY();
}
