/*
 * test_flex.c — Decodificador FLEX.
 *
 *  1. modulador → canal (AWGN 10 dB, CFO ±3125 Hz, ±100 ppm) → decodificador
 *     en los cuatro modos (1600/2, 3200/2, 3200/4, 6400/4): capcode, fase,
 *     tipo y texto.
 *  2. solo ruido → 0 eventos.
 *  3. vectores reales publicados: NO HAY. La especificación FLEX no es pública
 *     y no he localizado capturas con licencia clara; por eso este test valida
 *     la autoconsistencia modulador/decodificador y el BCH, no la concordancia
 *     con transmisores reales (ver docs/decoders/flex.md).
 *  4. coste de CPU por segundo de señal.
 */
#include "test_util.h"

#include "decoders/flex.h"
#include "tx/mod.h"

#include "vec_phase.h"
#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_FLEX;
extern const modulator_desc_t MOD_FLEX;

/* chan_drift de test_util.h se desborda con ppm > 0: versión con búfer correcto. */
static void drift(const cf32 *in, int n, double ppm, cf32 *out)
{
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)(n * 1.01 + 64));
    int m = chan_resample(in, n, 1.0 + ppm * 1e-6, tmp);
    for (int i = 0; i < n; i++)
        out[i] = i < m ? tmp[i] : (cf32){0, 0};
    free(tmp);
}

static double cpu_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void run_case(int mode, double cfo, double ppm, uint32_t seed)
{
    char params[100], err[200];
    snprintf(params, sizeof params, "{\"mode\":\"%s\"}", FLEX_MODES[mode].name);
    iqbuf_t m = {0};
    CHECK_EQ(MOD_FLEX.modulate(params, &m, err, sizeof err), 0);
    int pad = 24000, n = m.n + 2 * pad;
    cf32 *x = calloc((size_t)n, sizeof(cf32)), *y = calloc((size_t)n, sizeof(cf32));
    memcpy(x + pad, m.s, sizeof(cf32) * (size_t)m.n);
    for (int i = 0; i < pad; i++)
        x[i] = x[n - 1 - i] = (cf32){0.8f, 0};
    drift(x, n, ppm, y);
    chan_cfo(y, n, 48000, cfo);
    chan_awgn(y, n, 10.0, &seed);
    evs_t ev = {0};
    double t0 = cpu_s();
    run_narrow(&DEC_FLEX, y, n, 466075000, NULL, &ev);
    double dt = cpu_s() - t0;
    printf("  modo %-6s cfo %+5.0f ppm %+4.0f -> %d eventos, CPU %.4f s/s\n", FLEX_MODES[mode].name, cfo, ppm,
           evs_count(&ev), dt / (n / 48000.0));
    CHECK(ev_has(&ev, "\"capcode\":123456,\"phase\":\"A\",\"cycle\":3,\"frame\":42,\"kind\":\"alpha\","
                      "\"msg\":\"BLADERF LAB: PRUEBA FLEX 0123456789\""));
    CHECK(ev_has(&ev, "\"capcode\":1600000,\"phase\":\"A\",\"cycle\":3,\"frame\":42,\"kind\":\"numeric\","
                      "\"msg\":\"0123456789 -[]\""));
    CHECK(ev_has(&ev, "\"capcode\":4242,\"phase\":\"A\",\"cycle\":3,\"frame\":42,\"kind\":\"tone\""));
    int expect = 3;
    if (FLEX_MODES[mode].phases > 1) {
        CHECK(ev_has(&ev, "\"capcode\":777777,\"phase\":\"B\""));
        CHECK(ev_has(&ev, "FASE B: SEGUNDO BUSCA"));
        expect = 4;
    }
    if (FLEX_MODES[mode].phases > 2) {
        CHECK(ev_has(&ev, "\"capcode\":31337,\"phase\":\"C\""));
        CHECK(ev_has(&ev, "\"capcode\":31338,\"phase\":\"D\""));
        expect = 6;
    }
    CHECK_EQ(evs_count(&ev), expect);
    if (tu_fails)
        printf("%s\n", ev.data ? ev.data : "(sin eventos)");
    evs_free(&ev);
    free(x);
    free(y);
    iqbuf_free(&m);
}

int main(void)
{
    printf("[1] modulador -> canal -> decodificador\n");
    for (int md = 0; md < 4; md++) {
        run_case(md, 0, 0, 100u + (uint32_t)md);
        run_case(md, 3125, 100, 200u + (uint32_t)md);
        run_case(md, -3125, -100, 300u + (uint32_t)md);
    }

    printf("[1b] polaridad: señal conjugada (desviación invertida)\n");
    {
        char err[200];
        iqbuf_t m = {0};
        CHECK_EQ(MOD_FLEX.modulate("{\"mode\":\"3200/4\"}", &m, err, sizeof err), 0);
        for (int i = 0; i < m.n; i++)
            m.s[i].im = -m.s[i].im;
        int pad = 12000, n = m.n + 2 * pad;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        memcpy(x + pad, m.s, sizeof(cf32) * (size_t)m.n);
        for (int i = 0; i < pad; i++)
            x[i] = x[n - 1 - i] = (cf32){0.8f, 0};
        uint32_t seed = 4;
        chan_awgn(x, n, 12.0, &seed);
        evs_t ev = {0};
        run_narrow(&DEC_FLEX, x, n, 466075000, NULL, &ev);
        CHECK(ev_has(&ev, "\"capcode\":123456"));
        evs_free(&ev);
        free(x);
        iqbuf_free(&m);
    }

    printf("[2] solo ruido\n");
    {
        int n = 48000 * 20;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 77;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t ev = {0};
        run_narrow(&DEC_FLEX, x, n, 466075000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        free(x);
    }

    printf("[3] palabra FLEX: ida y vuelta con 2 errores\n");
    {
        bch_t b;
        bch_init_pocsag(&b);
        uint8_t bits[32];
        uint32_t d = 0x12345 & 0x1FFFFF, out = 0;
        flex_word_bits(&b, d, bits);
        bits[3] ^= 1;
        bits[27] ^= 1;
        CHECK_EQ(flex_word_fix(&b, bits, &out), 2);
        CHECK_EQ(out, d);
    }

    /* Grabación real (V1): P2000 (Países Bajos), FLEX 1600/2 FSK, "proef alarm" del repositorio de
     * pruebas de multimon-ng (test/samples/FLEX_1600_2fsk_P2000_proef_alarm.flac; GPL-2.0 el
     * repositorio, datos de prueba locales). Un fotograma (t = 39,0-41,2 s) remodulado en FM
     * (±4,8 kHz), fase de 8 bits. multimon-ng (FLEX_NEXT) da para él: capcode 1123201, fase A,
     * ciclo 0, trama 38, "B2 Eindhoven Rit: 105012". Este fotograma fijó la estructura de Sync 1
     * ([A][0xA6C6] en vez de [A][~A]) y la cabecera del texto alfanumérico. */
    {
        int n = 0;
        cf32 *x = vp_load_phase("tests/data/flex_p2000_phase.u8", &n);
        if (!x) {
            printf("  (real FLEX: fichero ausente, omitido)\n");
        } else {
            evs_t ev = {0};
            run_narrow(&DEC_FLEX, x, n, 466075000, NULL, &ev);
            printf("  FLEX real P2000: %d eventos\n", evs_count(&ev));
            CHECK(ev_has(&ev, "\"capcode\":1123201,\"phase\":\"A\",\"cycle\":0,\"frame\":38,\"kind\":\"alpha\","
                              "\"msg\":\"B2 Eindhoven Rit: 105012\""));
            evs_free(&ev);
            free(x);
        }
    }
    return TEST_SUMMARY();
}
