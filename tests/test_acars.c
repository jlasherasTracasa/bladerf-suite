/*
 * test_acars.c — ACARS: modulador → canal (AWGN 10 dB, CFO ±5 kHz = ¼ del canal,
 * ±100 ppm de reloj) → decodificador a 48 kHz; solo ruido; coste de CPU.
 *
 * Vectores reales: no se dispone de una captura publicada con licencia clara ni
 * de ejemplos con BCS en la norma ARINC 618 (de pago), así que no hay vectores
 * del aire; el BCS se prueba con la propiedad del CRC (residuo 0 al añadir el
 * valor) y con los valores del catálogo de CRC (CRC-16/KERMIT "123456789" = 0x2189).
 */
#include "test_util.h"

#include "decoders/acars_blk.h"
#include "tx/mod.h"

#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_ACARS;
extern const modulator_desc_t MOD_ACARS;

static void run_case(const char *name, const iqbuf_t *src, double cfo, double ppm, double snr, uint32_t seed,
                     int expect_min, const char *must)
{
    int n = src->n;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    chan_drift(src->s, n, ppm, x);
    chan_cfo(x, n, src->rate, cfo);
    chan_awgn(x, n, snr, &seed);
    evs_t ev = {0};
    run_narrow(&DEC_ACARS, x, n, 131725000, NULL, &ev);
    printf("  %-28s eventos=%d\n", name, evs_count(&ev));
    CHECK(evs_count(&ev) >= expect_min);
    if (must)
        CHECK(ev_has(&ev, must));
    evs_free(&ev);
    free(x);
}

int main(void)
{
    /* --- BCS --- */
    static const uint8_t chk[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(acars_bcs(chk, 9), 0x2189);
    uint8_t buf[16] = {0x32, 0x2E, 0x41, 0x15, 0x48, 0x31, 0x33, 0x03};
    uint16_t bcs = acars_bcs(buf, 8);
    buf[8] = (uint8_t)(bcs & 0xFF);
    buf[9] = (uint8_t)(bcs >> 8);
    CHECK_EQ(acars_bcs(buf, 10), 0);
    CHECK_EQ(acars_add_parity('+'), 0xAB);
    CHECK_EQ(acars_add_parity('*'), 0x2A);

    /* --- Modulador → canal → decodificador --- */
    const char *par = "{\"reg\":\"EC-MXY\",\"flight\":\"IB3456\",\"label\":\"H1\",\"msgno\":\"M12A\","
                      "\"block\":\"3\",\"text\":\"#DFB/WX LEMD LEBL 151200Z 27008KT\",\"repeat\":2}";
    iqbuf_t iq = {0};
    char err[200] = "";
    CHECK(MOD_ACARS.modulate(par, &iq, err, sizeof err) == 0);
    CHECK_NEAR(iq.rate, 48000, 0.5);
    CHECK(!MOD_ACARS.air_ok);

    printf("ACARS: canal limpio y con degradaciones\n");
    run_case("limpio", &iq, 0, 0, 40, 1, 2, "\"reg\":\"EC-MXY\"");
    run_case("10 dB", &iq, 0, 0, 10, 2, 2, NULL);
    run_case("10 dB, +5 kHz, +100 ppm", &iq, 5000, 100, 10, 3, 2, NULL);
    run_case("10 dB, -5 kHz, -100 ppm", &iq, -5000, -100, 10, 4, 2, NULL);
    run_case("10 dB, +5 kHz, -100 ppm", &iq, 5000, -100, 10, 5, 2, NULL);
    run_case("10 dB, -5 kHz, +100 ppm", &iq, -5000, 100, 10, 6, 2, NULL);

    /* Campos */
    {
        cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
        memcpy(x, iq.s, sizeof(cf32) * (size_t)iq.n);
        uint32_t seed = 77;
        chan_cfo(x, iq.n, iq.rate, 3000);
        chan_awgn(x, iq.n, 10, &seed);
        evs_t ev = {0};
        run_narrow(&DEC_ACARS, x, iq.n, 131725000, NULL, &ev);
        CHECK(ev_has(&ev, "\"reg\":\"EC-MXY\""));
        CHECK(ev_has(&ev, "\"flight\":\"IB3456\""));
        CHECK(ev_has(&ev, "\"label\":\"H1\""));
        CHECK(ev_has(&ev, "\"msgno\":\"M12A\""));
        CHECK(ev_has(&ev, "\"block\":\"3\""));
        CHECK(ev_has(&ev, "\"mode\":\"2\""));
        CHECK(ev_has(&ev, "\"dir\":\"down\""));
        CHECK(ev_has(&ev, "\"tak\":\"NAK\""));
        CHECK(ev_has(&ev, "\"body\":\"#DFB/WX LEMD LEBL 151200Z 27008KT\""));
        CHECK(ev_has(&ev, "\"text\":\""));
        evs_free(&ev);
        free(x);
    }

    /* Enlace ascendente, etiqueta Q0 (_ + DEL) y otro canal */
    {
        iqbuf_t u = {0};
        CHECK(MOD_ACARS.modulate("{\"up\":true,\"reg\":\"N123AB\",\"label\":\"_d\",\"text\":\"HELLO\",\"repeat\":1}", &u,
                                 err, sizeof err) == 0);
        uint32_t seed = 9;
        chan_awgn(u.s, u.n, 15, &seed);
        evs_t ev = {0};
        run_narrow(&DEC_ACARS, u.s, u.n, 131525000, NULL, &ev);
        CHECK(ev_has(&ev, "\"reg\":\"N123AB\""));
        CHECK(ev_has(&ev, "\"dir\":\"up\""));
        CHECK(ev_has(&ev, "\"label\":\"_d\""));
        evs_free(&ev);
        iqbuf_free(&u);
    }

    /* Estado */
    {
        void *st = DEC_ACARS.create(&(dec_cfg_t){.rate = 48000, .freq_hz = 131725000}, err, sizeof err);
        CHECK(st != NULL);
        evs_t ev = {0};
        for (int off = 0; off < iq.n; off += 4096)
            DEC_ACARS.process(st, iq.s + off, MIN(4096, iq.n - off), 1000.0, tu_collect, &ev);
        sb_t sb = {0};
        DEC_ACARS.state_json(st, &sb, 1010.0);
        CHECK(strstr(sb.data, "\"reg\":\"EC-MXY\"") != NULL);
        sb_free(&sb);
        DEC_ACARS.state_json(st, &sb, 1000.0 + 5000.0); /* caduca */
        CHECK(strstr(sb.data, "\"reg\":\"EC-MXY\"") == NULL);
        sb_free(&sb);
        evs_free(&ev);
        DEC_ACARS.destroy(st);
    }

    /* Solo ruido: 0 eventos (portadora AM con ruido y ruido solo) */
    {
        int n = 48000 * 20;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++)
            x[i] = (cf32){(float)(0.1 * tu_gauss(&seed)), (float)(0.1 * tu_gauss(&seed))};
        evs_t ev = {0};
        run_narrow(&DEC_ACARS, x, n, 131725000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        for (int i = 0; i < n; i++) /* portadora sin modular + ruido */
            x[i] = (cf32){(float)(0.5 + 0.05 * tu_gauss(&seed)), (float)(0.05 * tu_gauss(&seed))};
        run_narrow(&DEC_ACARS, x, n, 131725000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);

        /* Coste: CPU por segundo de señal */
        clock_t t0 = clock();
        evs_t e2 = {0};
        run_narrow(&DEC_ACARS, x, n, 131725000, NULL, &e2);
        double cost = (double)(clock() - t0) / CLOCKS_PER_SEC / (n / 48000.0);
        printf("  coste: %.4f s de CPU por s de señal\n", cost);
        CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
        evs_free(&e2);
        free(x);
    }

    iqbuf_free(&iq);

    /* Grabación real (V1): ACARS de Nueva Zelanda (ZK-OKM, NZ0006), audio AM de la Signal
     * Identification Wiki ("ACARS sound", sigidwiki.com) remodulado en AM (m = 0,7), 2,0-3,2 s,
     * IQ cs8 a 48 kHz. acarsdec 4.3 decodifica los mismos bloques (matrícula, etiqueta, nº). */
    {
        int n = 0;
        cf32 *x = vr_load_cs8("tests/data/acars_sigid_am.cs8", &n);
        if (!x) {
            printf("  (real ACARS: fichero ausente, omitido)\n");
        } else {
            evs_t ev = {0};
            run_narrow(&DEC_ACARS, x, n, 131525000, NULL, &ev);
            printf("  ACARS real: %d eventos\n", evs_count(&ev));
            CHECK(ev_has(&ev, "\"reg\":\"ZK-OKM\",\"dir\":\"down\",\"tak\":\"S\",\"label\":\"_d\",\"block\":\"0\","
                              "\"msgno\":\"S20A\",\"flight\":\"NZ0006\""));
            CHECK(ev_has(&ev, "\"reg\":\"ZK-SJC\",\"dir\":\"up\",\"tak\":\"5\",\"label\":\"_d\",\"block\":\"V\""));
            CHECK(ev_has(&ev, "\"parity_ok\":true"));
            evs_free(&ev);
            free(x);
        }
    }
    return TEST_SUMMARY();
}
