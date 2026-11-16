/*
 * test_rs41.c — Decodificador de radiosondas Vaisala RS41.
 *
 * Datos publicados que se usan (bazjo/RS41_Decoding, documentación del formato):
 *   - cabecera transmitida 10 B6 CA 11 22 96 12 F8 → 86 35 F4 40 93 DF 1A 60 tras
 *     el XOR con la máscara (el test comprueba nuestra tabla contra ese par);
 *   - CRC-16/CCITT-FALSE("123456789") = 0x29B1 (catálogo reveng).
 *
 * VECTORES REALES (sección "real" de main):
 *   tests/data/rs41_real_frames.hex: 3 tramas de 320 octetos DESALEATORIZADAS de la
 *   grabación pública 20140717_402MHz.wav de github.com/rs1729/RS (rs41/wav, audio
 *   FM demodulado, 48 kHz, sonda K1930308, ~28 km de altura sobre Croacia). Se
 *   eligieron las que tienen síndrome Reed-Solomon cero con (0x11D, fcr 0, prim 1,
 *   24 raíces; datos entrelazados 0x38+k+2i, paridad 0x08+24k+j consecutiva). Sin
 *   licencia explícita en el repositorio; se usan 3 tramas como datos de prueba.
 *   tests/data/rs41_demod_20140717.u8: 1,9 s del mismo audio (8 bit sin signo) que
 *   el test RE-MODULA en FM (±2,4 kHz) para alimentar el decodificador de IQ.
 * Antes de esta validación se suponía la paridad entrelazada; la real es en bloque.
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/rs41_frame.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_RS41;
extern const modulator_desc_t MOD_RS41;

static double cpu_s, sig_s;

/* chan_drift de test_util.h reserva n+8 muestras y se desborda con ppm>0 y n grande. */
static void drift(const cf32 *in, int n, double ppm, cf32 *out)
{
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)(n + n / 500 + 16));
    int m = chan_resample(in, n, 1.0 + ppm * 1e-6, tmp);
    for (int i = 0; i < n; i++)
        out[i] = i < m ? tmp[i] : (cf32){0, 0};
    free(tmp);
}

static void run_case(const char *params, double snr, double cfo, double ppm, uint32_t seed, evs_t *evs)
{
    iqbuf_t out = {0};
    char err[200] = "";
    CHECK_EQ(MOD_RS41.modulate(params, &out, err, sizeof err), 0);
    CHECK_NEAR(out.rate, 48000.0, 1e-6);
    int n = out.n;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    if (ppm != 0)
        drift(out.s, n, ppm, x);
    else
        memcpy(x, out.s, sizeof(cf32) * (size_t)n);
    chan_cfo(x, n, 48000.0, cfo);
    chan_awgn(x, n, snr, &seed);
    clock_t c0 = clock();
    run_narrow(&DEC_RS41, x, n, 403000000LL, NULL, evs);
    cpu_s += (double)(clock() - c0) / CLOCKS_PER_SEC;
    sig_s += n / 48000.0;
    free(x);
    iqbuf_free(&out);
}

int main(void)
{
    /* Datos publicados */
    uint8_t h[8];
    for (int i = 0; i < 8; i++)
        h[i] = RS41_HEADER_TX[i] ^ RS41_MASK[i];
    const uint8_t hd[8] = {0x86, 0x35, 0xF4, 0x40, 0x93, 0xDF, 0x1A, 0x60};
    CHECK(memcmp(h, hd, 8) == 0);
    CHECK_EQ(rs41_crc16((const uint8_t *)"123456789", 9), 0x29B1);

    /* ECEF ↔ geodésicas */
    double x, y, z, la, lo, al;
    rs41_lla_to_ecef(40.4168, -3.7038, 12500.0, &x, &y, &z);
    rs41_ecef_to_lla(x, y, z, &la, &lo, &al);
    CHECK_NEAR(la, 40.4168, 1e-7);
    CHECK_NEAR(lo, -3.7038, 1e-7);
    CHECK_NEAR(al, 12500.0, 1e-3);
    rs41_lla_to_ecef(0, 0, 0, &x, &y, &z);   /* punto del ecuador: a = 6 378 137 m */
    CHECK_NEAR(x, 6378137.0, 1e-6);

    /* Trama construida → parseada (sin radio) y Reed-Solomon con errores */
    {
        rs41_params_t p = {.frame_no = 77, .batt_v = 2.9, .gps_week = 2400, .gps_tow_ms = 1000, .lat = -33.9,
                           .lon = 151.2, .alt_m = 3000, .vn_ms = 1, .ve_ms = 0, .vu_ms = -4, .sats = 10};
        snprintf(p.serial, sizeof p.serial, "P0123456");
        uint8_t f[RS41_FRAME_LEN], g[RS41_FRAME_LEN];
        rs41_build(&p, f);
        rs41_info_t in;
        CHECK_EQ(rs41_parse(f, RS41_FRAME_LEN, &in), 4);
        CHECK(in.status_ok && in.pos_ok && in.time_ok && in.ptu_ok);
        CHECK(strcmp(in.serial, "P0123456") == 0);
        CHECK_NEAR(in.lat, -33.9, 1e-4);
        CHECK_NEAR(in.climb_ms, -4.0, 0.05);
        p.inject_errors = 10;
        rs41_build(&p, g);
        CHECK(memcmp(f, g, sizeof f) != 0);
        CHECK(rs41_parse(g, RS41_FRAME_LEN, &in) < 4);
        CHECK_EQ(rs41_rs_correct(g, RS41_FRAME_LEN), 10);
        CHECK(memcmp(f, g, sizeof f) == 0);
        p.inject_errors = 14;   /* > t = 12 por palabra en total ~7+7: sigue corrigiendo; 30 no */
        rs41_build(&p, g);
        CHECK(rs41_rs_correct(g, RS41_FRAME_LEN) >= 0);
        p.inject_errors = 40;
        rs41_build(&p, g);
        CHECK(rs41_rs_correct(g, RS41_FRAME_LEN) < 0 || memcmp(f, g, sizeof f) != 0);
    }

    /* (1) modulador → canal (AWGN 10 dB, CFO ±¼ de 12 kHz, ±100 ppm) → decodificador */
    for (int v = 0; v < 2; v++) {
        double cfo = v ? -3000.0 : 3000.0, ppm = v ? -100.0 : 100.0;
        evs_t e = {0};
        run_case("{\"repeat\":3}", 10.0, cfo, ppm, 21u + (uint32_t)v, &e);
        printf("  CFO %+.0f ppm %+.0f: %d eventos\n", cfo, ppm, evs_count(&e));
        CHECK(evs_count(&e) >= 2);
        CHECK(ev_has(&e, "\"serial\":\"N1234567\""));
        CHECK(ev_has(&e, "\"frame\":121"));
        CHECK(ev_has(&e, "\"battery_v\":2.7"));
        CHECK(ev_has(&e, "\"lat\":40.4168") || ev_has(&e, "\"lat\":40.4167") || ev_has(&e, "\"lat\":40.4169") ||
              ev_has(&e, "\"lat\":40.417"));
        CHECK(ev_has(&e, "\"sats\":9"));
        CHECK(ev_has(&e, "\"climb_ms\":5.0"));
        CHECK(ev_has(&e, "\"heading_deg\":284"));
        CHECK(ev_has(&e, "\"gps_week\":2400"));
        if (v == 0 && e.data)
            printf("  %.400s\n", e.data);
        evs_free(&e);
    }

    /* (1b) con errores de octeto inyectados: debe recuperarlos Reed-Solomon */
    {
        evs_t e = {0};
        run_case("{\"errors\":10,\"serial\":\"M2345678\",\"repeat\":2}", 14.0, 2000.0, 50.0, 5, &e);
        CHECK(ev_has(&e, "\"serial\":\"M2345678\""));
        CHECK(ev_has(&e, "\"rs_fixed\":10") || ev_has(&e, "\"rs_fixed\":11") || ev_has(&e, "\"rs_fixed\":12"));
        evs_free(&e);
    }

    /* (2) Solo ruido */
    {
        int n = 48000 * 5;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.2 * tu_gauss(&seed));
            x[i].im = (float)(0.2 * tu_gauss(&seed));
        }
        evs_t e = {0};
        clock_t c0 = clock();
        run_narrow(&DEC_RS41, x, n, 403000000LL, NULL, &e);
        cpu_s += (double)(clock() - c0) / CLOCKS_PER_SEC;
        sig_s += n / 48000.0;
        CHECK_EQ(evs_count(&e), 0);
        evs_free(&e);
        free(x);
    }

    /* (real) tramas y audio reales */
    {
        FILE *fh = fopen("tests/data/rs41_real_frames.hex", "r");
        if (!fh)
            printf("  (sin tests/data/rs41_real_frames.hex: se omite)\n");
        else {
            static char line[800];
            int nf = 0;
            while (fgets(line, sizeof line, fh) && nf < 3) {
                uint8_t f[320], g[320];
                for (int i = 0; i < 320; i++) {
                    unsigned v = 0;
                    sscanf(line + 2 * i, "%2x", &v);
                    f[i] = (uint8_t)v;
                }
                nf++;
                rs41_info_t in;
                memset(&in, 0, sizeof in);
                CHECK_EQ(rs41_parse(f, 320, &in) >= 4, 1);
                CHECK(in.pos_ok && in.status_ok && in.time_ok);
                CHECK(strcmp(in.serial, "K1930308") == 0);
                CHECK(in.alt_m > 20000 && in.alt_m < 40000 && in.lat > 45.0 && in.lat < 46.5 && in.gps_week == 1801);
                /* la trama real ya es una palabra de código: corrección = 0 */
                memcpy(g, f, 320);
                CHECK_EQ(rs41_rs_correct(g, 320), 0);
                /* 12 octetos erróneos repartidos (6 por palabra: t = 12 por palabra) */
                for (int e = 0; e < 12; e++)
                    g[0x39 + 11 * e + (e & 1)] ^= (uint8_t)(0x31 + e);
                for (int e = 0; e < 4; e++)
                    g[0x08 + 5 * e] ^= 0xA5;   /* y en la paridad */
                CHECK_EQ(rs41_rs_correct(g, 320), 16);
                CHECK(memcmp(f, g, 320) == 0);
            }
            fclose(fh);
            CHECK_EQ(nf, 3);
        }
        /* audio real → FM remodulada → decodificador */
        FILE *fa = fopen("tests/data/rs41_demod_20140717.u8", "rb");
        if (fa) {
            static uint8_t au[100000];
            int na = (int)fread(au, 1, sizeof au, fa);
            fclose(fa);
            /* escala: percentil 90 de |a − 128| (el audio demodulado no tiene escala absoluta) */
            int hist[256] = {0};
            for (int i = 0; i < na; i++)
                hist[abs(au[i] - 128)]++;
            int acc = 0, p90 = 1;
            for (int v = 0; v < 256; v++) {
                acc += hist[v];
                if (acc >= na * 9 / 10) {
                    p90 = v ? v : 1;
                    break;
                }
            }
            cf32 *x = malloc(sizeof(cf32) * (size_t)na);
            double ph = 0;
            for (int i = 0; i < na; i++) {
                ph += 2.0 * M_PI * 2400.0 * ((double)(au[i] - 128) / p90) / 48000.0;
                x[i].re = (float)(0.5 * cos(ph));
                x[i].im = (float)(0.5 * sin(ph));
            }
            evs_t e = {0};
            run_narrow(&DEC_RS41, x, na, 403000000LL, NULL, &e);
            if (!ev_has(&e, "\"serial\":\"K1930308\""))
                printf("  audio real: sin trama\n");
            CHECK(ev_has(&e, "\"serial\":\"K1930308\"") && ev_has(&e, "\"gps_week\":1801"));
            evs_free(&e);
            free(x);
        } else
            printf("  (sin tests/data/rs41_demod_20140717.u8: se omite)\n");
    }

    /* state_json: sondas activas con lat/lon (el mapa de la web las dibuja) */
    {
        iqbuf_t out = {0};
        char err[200] = "";
        MOD_RS41.modulate("{\"repeat\":1}", &out, err, sizeof err);
        dec_cfg_t cfg = {.rate = 48000, .freq_hz = 403000000LL, .center_hz = 403000000LL};
        void *st = DEC_RS41.create(&cfg, err, sizeof err);
        evs_t e = {0};
        DEC_RS41.process(st, out.s, out.n, 100.0, tu_collect, &e);
        sb_t sb = {0};
        DEC_RS41.state_json(st, &sb, 105.0);
        CHECK(strstr(sb.data, "\"sondes\":[{\"serial\":\"N1234567\"") != NULL && strstr(sb.data, "\"lat\":40.41"));
        sb_clear(&sb);
        DEC_RS41.state_json(st, &sb, 100.0 + 700.0);   /* caducada */
        CHECK(strstr(sb.data, "\"sondes\":[]") != NULL);
        sb_free(&sb);
        evs_free(&e);
        DEC_RS41.destroy(st);
        iqbuf_free(&out);
    }

    printf("  coste: %.4f s de CPU por s de señal\n", cpu_s / sig_s);
    CHECK(cpu_s / sig_s < VR_CPU(0.3));
    return TEST_SUMMARY();
}
