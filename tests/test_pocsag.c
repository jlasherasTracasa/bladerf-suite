/*
 * test_pocsag.c — Decodificador POCSAG.
 *
 *  1. modulador → canal (AWGN 10 dB, CFO ±¼ del canal = ±3125 Hz, ±100 ppm) →
 *     decodificador, para 512, 1200 y 2400 bit/s: RIC, función, tipo, texto.
 *  2. solo ruido → 0 eventos.
 *  3. constantes publicadas en ITU-R M.584 (palabra de sincronía 0x7CD215D8 y
 *     de reposo 0x7A89C197): deben ser palabras BCH(31,21)+paridad válidas.
 *  4. coste de CPU por segundo de señal.
 *
 * No hay vectores de audio/IQ públicos con licencia clara para POCSAG; los de
 * la prueba 1 son sintéticos (el modulador es independiente del decodificador:
 * comparten solo el BCH de fec.c y las constantes de la norma).
 */
#include "test_util.h"

#include "decoders/pocsag.h"
#include "dsp/bits.h"
#include "dsp/fec.h"
#include "tx/mod.h"

#include "vec_phase.h"
#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_POCSAG;
extern const modulator_desc_t MOD_POCSAG;

/* chan_drift de test_util.h reserva n+8 muestras y se desborda con ppm > 0
 * (n·ratio > n+8): versión local con el búfer bien dimensionado. */
static void drift(const cf32 *in, int n, double ppm, cf32 *out)
{
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)(n * 1.01 + 64));
    int m = chan_resample(in, n, 1.0 + ppm * 1e-6, tmp);
    for (int i = 0; i < n; i++)
        out[i] = i < m ? tmp[i] : (cf32){0, 0};
    free(tmp);
}

static double cpu_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void run_case(int baud, double cfo, double ppm, uint32_t seed, const char *tag)
{
    char params[200], err[200];
    snprintf(params, sizeof params, "{\"baud\":%d}", baud);
    iqbuf_t m = {0};
    CHECK_EQ(MOD_POCSAG.modulate(params, &m, err, sizeof err), 0);
    CHECK_EQ((int)m.rate, 48000);
    /* silencio al principio y al final para que el reloj/seguidores arranquen en ruido */
    int pad = 24000, n = m.n + 2 * pad;
    cf32 *x = calloc((size_t)n, sizeof(cf32)), *y = calloc((size_t)n, sizeof(cf32));
    memcpy(x + pad, m.s, sizeof(cf32) * (size_t)m.n);
    for (int i = 0; i < pad; i++) /* portadora sin modular antes y después */
        x[i] = x[n - 1 - i] = (cf32){0.8f, 0};
    drift(x, n, ppm, y);
    chan_cfo(y, n, 48000, cfo);
    chan_awgn(y, n, 10.0, &seed);
    evs_t ev = {0};
    double t0 = cpu_s();
    run_narrow(&DEC_POCSAG, y, n, 466075000, NULL, &ev);
    double dt = cpu_s() - t0;
    printf("  %-9s %4d Bd cfo %+5.0f ppm %+4.0f -> %d eventos, CPU %.4f s/s\n", tag, baud, cfo, ppm, evs_count(&ev),
           dt / (n / 48000.0));
    CHECK(ev_has(&ev, "\"ric\":1234567,\"func\":3,\"kind\":\"alpha\",\"msg\":\"BLADERF LAB: PRUEBA POCSAG 1234567890\""));
    CHECK(ev_has(&ev, "\"ric\":2001002,\"func\":0,\"kind\":\"numeric\",\"msg\":\"0123456789 -[]\""));
    CHECK(ev_has(&ev, "\"ric\":424242,\"func\":1,\"kind\":\"tone\""));
    CHECK(ev_has(&ev, baud == 512 ? "\"baud\":512" : baud == 1200 ? "\"baud\":1200" : "\"baud\":2400"));
    CHECK_EQ(evs_count(&ev), 3);
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
    int bauds[3] = {512, 1200, 2400};
    for (int b = 0; b < 3; b++) {
        run_case(bauds[b], 0, 0, 11u + (uint32_t)b, "limpio");
        run_case(bauds[b], 3125, 100, 21u + (uint32_t)b, "cfo+");
        run_case(bauds[b], -3125, -100, 31u + (uint32_t)b, "cfo-");
    }

    printf("[1b] polaridad invertida y mensaje largo (2 lotes)\n");
    {
        char err[200];
        iqbuf_t m = {0};
        const char *p = "{\"baud\":1200,\"invert\":true,\"messages\":[{\"ric\":1000003,\"func\":3,\"text\":\"UN MENSAJE LARGO "
                        "QUE CRUZA LOTES: ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 abcdefghijklmnopqrstuvwxyz\"}]}";
        CHECK_EQ(MOD_POCSAG.modulate(p, &m, err, sizeof err), 0);
        cf32 *y = calloc((size_t)m.n, sizeof(cf32));
        uint32_t seed = 5;
        chan_cfo(m.s, m.n, 48000, 2000);
        memcpy(y, m.s, sizeof(cf32) * (size_t)m.n);
        chan_awgn(y, m.n, 10.0, &seed);
        evs_t ev = {0};
        run_narrow(&DEC_POCSAG, y, m.n, 466075000, NULL, &ev);
        CHECK(ev_has(&ev, "\"ric\":1000003,\"func\":3,\"kind\":\"alpha\",\"msg\":\"UN MENSAJE LARGO QUE CRUZA LOTES: "
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 abcdefghijklmnopqrstuvwxyz\""));
        evs_free(&ev);
        free(y);
        iqbuf_free(&m);
    }

    printf("[2] solo ruido\n");
    {
        int n = 48000 * 20;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 99;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t ev = {0};
        run_narrow(&DEC_POCSAG, x, n, 466075000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        free(x);
    }

    printf("[3] constantes de ITU-R M.584\n");
    {
        bch_t b;
        CHECK_EQ(bch_init_pocsag(&b), 0);
        uint32_t idle = POCSAG_IDLE, sync = POCSAG_SYNC;
        for (int k = 0; k < 2; k++) {
            uint32_t w = k ? sync : idle;
            uint32_t c31 = bch_encode_u32(&b, w >> 11, 21);
            CHECK_EQ(c31, w >> 1);
            CHECK_EQ(bits_parity32(w), 0); /* paridad par */
        }
    }

    printf("[4] estado JSON\n");
    {
        char err[200];
        dec_cfg_t cfg = {.rate = 48000, .freq_hz = 466075000, .center_hz = 466075000};
        void *st = DEC_POCSAG.create(&cfg, err, sizeof err);
        CHECK(st != NULL);
        sb_t sb = {0};
        DEC_POCSAG.state_json(st, &sb, 100.0);
        CHECK(strstr(sb.data, "\"pages\":[]") != NULL);
        sb_free(&sb);
        DEC_POCSAG.destroy(st);
    }

    /* Grabaciones reales (V1): 3 muestras públicas de POCSAG del repositorio de pruebas de
     * multimon-ng (EliasOenal/multimon-ng, test/samples/POCSAG_sample_-_*_bps.flac; grabaciones
     * de aire, GPL-2.0 el repositorio; aquí solo datos de prueba locales). Audio FM remodulado
     * en FM (±4,5 kHz) y guardado como fase de 8 bits. La referencia (multimon-ng) da las mismas
     * direcciones y el mismo texto. */
    {
        static const struct { const char *f; int baud; const char *ric; const char *func; } v[3] = {
            {"tests/data/pocsag512_mm_phase.u8", 512, "\"ric\":273040,\"func\":3", "512 B SIDE ZZZZZZ"},
            {"tests/data/pocsag1200_mm_phase.u8", 1200, "\"ric\":273040,\"func\":3", "+++TIME=0008300324+++TIME=0008300324"},
            {"tests/data/pocsag2400_mm_phase.u8", 2400, "\"ric\":1022869,\"func\":1", "+++TIME=0008300324+++TIME=0008300324"},
        };
        for (int k = 0; k < 3; k++) {
            int n = 0;
            cf32 *x = vp_load_phase(v[k].f, &n);
            if (!x) {
                printf("  (real %d bps: fichero ausente, omitido)\n", v[k].baud);
                continue;
            }
            evs_t ev = {0};
            run_narrow(&DEC_POCSAG, x, n, 466075000, NULL, &ev);
            printf("  POCSAG real %d bps: %d eventos\n", v[k].baud, evs_count(&ev));
            CHECK(ev_has(&ev, v[k].ric));
            CHECK(ev_has(&ev, v[k].func));
            CHECK(ev_has(&ev, v[k].baud == 512 ? "\"baud\":512" : v[k].baud == 1200 ? "\"baud\":1200" : "\"baud\":2400"));
            evs_free(&ev);
            free(x);
        }
    }
    return TEST_SUMMARY();
}
