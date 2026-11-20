/*
 * test_flex.c — Decodificador FLEX.
 *
 *  1. modulador → canal (AWGN 10 dB, CFO ±3125 Hz, ±100 ppm) → decodificador
 *     en los cuatro modos (1600/2, 3200/2, 3200/4, 6400/4): capcode, fase
 *     (A; A,C; A,B; A–D), tipo y texto (alfa, numérico, tono).
 *  1b. polaridad invertida.  1c. posición marcada (smp0/smp1) contra la verdad.
 *  2. solo ruido → 0 eventos.
 *  3. BCH de la palabra FLEX; tabla de modos y reparto de bits.
 *  4. grabación real 1600/2 (P2000, multimon-ng) y, si existen, las de 6400/4
 *     (sigidwiki, CC BY-SA 3.0) y 3200/2.
 *  5. coste de CPU por segundo de señal.
 * Fuentes públicas de la estructura: ver flex.h y docs/protocolos/flex.md.
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


/* Posición: emisor que suma el desplazamiento del bloque a smp0/smp1 del primer evento. */
typedef struct {
    long base, s0, s1;
    int n;
} pos_t;

static void pos_emit(void *ctx, const char *json, size_t len)
{
    (void)len;
    pos_t *p = ctx;
    const char *a = strstr(json, "\"smp0\":"), *b = strstr(json, "\"smp1\":");
    if (!a || !b)
        return;
    if (p->n++ == 0) {
        p->s0 = p->base + atol(a + 7);
        p->s1 = p->base + atol(b + 7);
    }
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
    for (int p = 1; p < FLEX_MODES[mode].phases; p++) {
        switch (FLEX_MODES[mode].ph[p]) {
        case 1:
            CHECK(ev_has(&ev, "\"capcode\":777777,\"phase\":\"B\""));
            CHECK(ev_has(&ev, "FASE B: SEGUNDO BUSCA"));
            break;
        case 2:
            CHECK(ev_has(&ev, "\"capcode\":31337,\"phase\":\"C\""));
            CHECK(ev_has(&ev, "\"msg\":\"FASE C\""));
            break;
        default:
            CHECK(ev_has(&ev, "\"capcode\":31338,\"phase\":\"D\""));
            CHECK(ev_has(&ev, "\"msg\":\"FASE D\""));
        }
        expect++;
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
    for (int md = 0; md < FLEX_NMODES_TX; md++) {
        run_case(md, 0, 0, 100u + (uint32_t)md);
        run_case(md, 3125, 100, 200u + (uint32_t)md);
        run_case(md, -3125, -100, 300u + (uint32_t)md);
    }

    printf("[1d] direcciones largas y tipos de vector (6400/4 y 1600/2)\n");
    for (int md = 0; md < 4; md += 3) {
        const char *params =
            "{\"mode\":\"%s\",\"cycle\":7,\"frame\":99,\"messages\":["
            "{\"capcode\":7298909,\"phase\":\"A\",\"type\":\"alpha\",\"text\":\"DIRECCION LARGA: PRUEBA 123\"},"
            "{\"capcode\":3644966,\"phase\":\"A\",\"type\":\"numeric\",\"text\":\"9148487751-6\"},"
            "{\"capcode\":6063416,\"phase\":\"A\",\"type\":\"tone\",\"text\":\"888\"},"
            "{\"capcode\":4297068542,\"phase\":\"A\",\"type\":\"alpha\",\"text\":\"MAXIMO\"},"
            "{\"capcode\":1933312,\"phase\":\"A\",\"type\":\"alpha\",\"text\":\"corta maxima\"},"
            "{\"capcode\":77,\"phase\":\"A\",\"type\":\"numeric\",\"text\":\"1 2 3\"}]}";
        char p2[1000], err[200];
        snprintf(p2, sizeof p2, params, FLEX_MODES[md].name);
        iqbuf_t m = {0};
        CHECK_EQ(MOD_FLEX.modulate(p2, &m, err, sizeof err), 0);
        int pad = 12000, n = m.n + 2 * pad;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        memcpy(x + pad, m.s, sizeof(cf32) * (size_t)m.n);
        for (int i = 0; i < pad; i++)
            x[i] = x[n - 1 - i] = (cf32){0.8f, 0};
        uint32_t seed = 1234u + (uint32_t)md;
        chan_awgn(x, n, 14.0, &seed);
        evs_t ev = {0};
        run_narrow(&DEC_FLEX, x, n, 466075000, NULL, &ev);
        CHECK(ev_has(&ev, "\"capcode\":7298909,\"phase\":\"A\",\"cycle\":7,\"frame\":99,\"kind\":\"alpha\","
                          "\"msg\":\"DIRECCION LARGA: PRUEBA 123\""));
        CHECK(ev_has(&ev, "\"capcode\":3644966,\"phase\":\"A\",\"cycle\":7,\"frame\":99,\"kind\":\"numeric\","
                          "\"msg\":\"9148487751-6\""));
        CHECK(ev_has(&ev, "\"capcode\":6063416,\"phase\":\"A\",\"cycle\":7,\"frame\":99,\"kind\":\"tone\",\"msg\":\"888\""));
        CHECK(ev_has(&ev, "\"capcode\":4297068542,\"phase\":\"A\",\"cycle\":7,\"frame\":99,\"kind\":\"alpha\",\"msg\":\"MAXIMO\""));
        CHECK(ev_has(&ev, "\"capcode\":1933312,\"phase\":\"A\",\"cycle\":7,\"frame\":99,\"kind\":\"alpha\",\"msg\":\"corta maxima\""));
        CHECK(ev_has(&ev, "\"capcode\":77,\"phase\":\"A\",\"cycle\":7,\"frame\":99,\"kind\":\"numeric\",\"msg\":\"1 2 3\""));
        CHECK_EQ(evs_count(&ev), 6);
        if (tu_fails)
            printf("%s\n", ev.data ? ev.data : "(sin eventos)");
        evs_free(&ev);
        free(x);
        iqbuf_free(&m);
    }

    printf("[1b] polaridad: señal conjugada (desviación invertida), los cuatro modos\n");
    for (int md = 0; md < FLEX_NMODES_TX; md++) {
        char err[200], params[100];
        snprintf(params, sizeof params, "{\"mode\":\"%s\"}", FLEX_MODES[md].name);
        iqbuf_t m = {0};
        CHECK_EQ(MOD_FLEX.modulate(params, &m, err, sizeof err), 0);
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
        CHECK_EQ(evs_count(&ev), 3 + FLEX_MODES[md].phases - 1);
        evs_free(&ev);
        free(x);
        iqbuf_free(&m);
    }

    printf("[1c] posición (smp0/smp1) en los cuatro modos\n");
    for (int md = 0; md < FLEX_NMODES_TX; md++) {
        /* La trama modulada empieza tras 2400 muestras de portadora (0,05 s) con el primer símbolo
         * de BS1; termina tras Sync 1 + FIW (144 símbolos a 1600 Bd), Sync 2 y 1760 ms de datos.
         * Se alimenta el decodificador a mano para sumar los desplazamientos de cada bloque. */
        char params[100], err[200];
        snprintf(params, sizeof params, "{\"mode\":\"%s\"}", FLEX_MODES[md].name);
        iqbuf_t m = {0};
        CHECK_EQ(MOD_FLEX.modulate(params, &m, err, sizeof err), 0);
        int pad = 24000, n = m.n + 2 * pad;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        memcpy(x + pad, m.s, sizeof(cf32) * (size_t)m.n);
        for (int i = 0; i < pad; i++)
            x[i] = x[n - 1 - i] = (cf32){0.8f, 0};
        uint32_t seed = 900u + (uint32_t)md;
        chan_awgn(x, n, 20.0, &seed);
        pos_t pc = {0};
        dec_cfg_t cfg = {.rate = 48000, .freq_hz = 466075000, .center_hz = 466075000};
        char e2[200];
        void *st = DEC_FLEX.create(&cfg, e2, sizeof e2);
        CHECK(st != NULL);
        for (int i = 0; i < n; i += 4096) {
            pc.base = i;
            DEC_FLEX.process(st, x + i, n - i < 4096 ? n - i : 4096, 0.0, pos_emit, &pc);
        }
        DEC_FLEX.destroy(st);
        const flex_mode_t *fm = &FLEX_MODES[md];
        long t0 = pad + 2400;
        long t1 = t0 + (144L * 30) + (long)(flex_sync2_syms(fm) + flex_data_syms(fm)) * (48000 / fm->baud);
        printf("  %s: %d eventos, smp0 %ld (esperado %ld), smp1 %ld (esperado %ld)\n", fm->name, pc.n, pc.s0, t0, pc.s1, t1);
        CHECK(pc.n > 0);
        CHECK(labs(pc.s0 - t0) <= 90);  /* ±3 símbolos de 1600 Bd */
        CHECK(labs(pc.s1 - t1) <= 120); /* ±4 símbolos */
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

    /* Grabaciones reales de alta velocidad (V1/D3): audio FM de sigidwiki (CC BY-SA 3.0) remodulado
     * en FM (±4,8 kHz), tests/data/real/flex6400.sigmf-data (una trama de 6400 bit/s, sync 0xDEA0,
     * 4FSK a 3200 Bd, direcciones largas) y flex3200.sigmf-data (3200 bit/s, sync 0xB068, 4FSK a
     * 1600 Bd, fases A y B). Para la primera, multimon-ng (FLEX_NEXT) da exactamente los mismos
     * capcodes y textos (ciclo 4, trama 56): "From: nexrad@mailhub.wx.ll.mit.edu Subject: **MACHINE
     * DOWN** nexrad005 ..." (fase C, capcode 3644966), 9148487751-6 (numérico, 6063476), etc.
     * Para la segunda multimon-ng no decodifica nada (envolvente baja): solo se comprueba que salen
     * mensajes con BCH y suma de nibbles válidos y el formato de número de teléfono. */
    {
        int n = 0;
        cf32 *x = vr_load_cs8("tests/data/real/flex6400.sigmf-data", &n);
        if (!x) {
            printf("  (real FLEX 6400: fichero ausente, omitido)\n");
        } else {
            evs_t ev = {0};
            run_narrow(&DEC_FLEX, x, n, 466075000, NULL, &ev);
            printf("  FLEX real 6400/4: %d eventos\n", evs_count(&ev));
            CHECK(ev_has(&ev, "\"capcode\":3644966,\"phase\":\"C\",\"cycle\":4,\"frame\":56,\"kind\":\"alpha\","
                              "\"msg\":\"From: nexrad@mailhub.wx.ll.mit.edu Subject: **MACHINE DOWN** nexrad005"));
            CHECK(ev_has(&ev, "\"capcode\":6063476,\"phase\":\"A\",\"cycle\":4,\"frame\":56,\"kind\":\"numeric\","
                              "\"msg\":\"9148487751-6\""));
            CHECK(ev_has(&ev, "\"capcode\":3644953,\"phase\":\"A\",\"cycle\":4,\"frame\":56,\"kind\":\"alpha\","
                              "\"msg\":\"16  [84]\""));
            CHECK(ev_has(&ev, "\"capcode\":5780262,\"phase\":\"A\""));
            CHECK_EQ(evs_count(&ev), 6);
            evs_free(&ev);
            free(x);
        }
        x = vr_load_cs8("tests/data/real/flex3200.sigmf-data", &n);
        if (!x) {
            printf("  (real FLEX 3200/4: fichero ausente, omitido)\n");
        } else {
            evs_t ev = {0};
            run_narrow(&DEC_FLEX, x, n, 466075000, NULL, &ev);
            printf("  FLEX real 3200/4: %d eventos\n", evs_count(&ev));
            CHECK(ev_has(&ev, "\"mode\":\"3200/4\""));
            CHECK(ev_has(&ev, "\"capcode\":121191,\"phase\":\"A\",\"cycle\":8,\"frame\":117,\"kind\":\"numeric\","
                              "\"msg\":\"488-9798\""));
            CHECK(evs_count(&ev) >= 10);
            evs_free(&ev);
            free(x);
        }
    }
    return TEST_SUMMARY();
}
