/*
 * test_dsc.c — DSC VHF (ITU-R M.493): símbolos, ECC, mensajes, modulador →
 * canal → decodificador, ruido y coste.
 *
 * Vectores reales publicados: NO los hay con texto accesible (el PDF de
 * ITU-R M.493 no pudo leerse en este entorno). Lo comprobado de fuentes
 * externas es lo que confirma el decodificador libre CuteSDR (fskdemod.cpp):
 * especificadores 112/114/116/120/102/123, símbolo de fase DX = 125, RX = 111…104,
 * EOS 117/122/127 y ECC = XOR desde el especificador de formato. El resto
 * (orden de los 3 bits de comprobación, polaridad de tonos, categorías,
 * naturaleza del socorro, formato de posición) son de memoria de la norma y
 * se prueban solo por coherencia interna emisor-receptor.
 */
#include "test_util.h"

#include "decoders/dsc.h"
#include "dsp/filter.h"
#include "tx/mod.h"

#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_DSC;

static void test_symbols(void)
{
    printf("DSC: símbolos de 10 bits, ECC y mensajes\n");
    uint8_t b[10];
    for (int v = 0; v < 128; v++) {
        dsc_sym_encode(v, b);
        CHECK_EQ(dsc_sym_decode(b), v);
        for (int e = 0; e < 10; e++) { /* un bit erróneo en cualquier posición no puede pasar inadvertido… casi */
            uint8_t c[10];
            memcpy(c, b, 10);
            c[e] ^= 1;
            /* un error en la información cambia el recuento de ceros: siempre se detecta */
            if (e < 7)
                CHECK_EQ(dsc_sym_decode(c), -1);
        }
    }
    dsc_sym_encode(125, b); /* 1111101: un cero → comprobación 1 */
    CHECK(b[0] == 1 && b[1] == 0 && b[2] == 1 && b[6] == 1);
    int s[8] = {112, 100, 5, 127};
    CHECK_EQ(dsc_ecc(s, 3), 112 ^ 100 ^ 5);

    /* llamada de socorro construida a mano: MMSI 224123456, naturaleza 107,
     * 43°21'N 008°24'E (cuadrante 0), 12:34 UTC */
    int m[] = {112, 22, 41, 23, 45, 60, 107, 4, 32, 10, 8, 24, 12, 34, 126, 127, 0};
    m[16] = dsc_ecc(m, 16);
    dsc_msg_t d;
    CHECK_EQ(dsc_parse_message(m, 17, &d), 0);
    CHECK(!strcmp(d.from, "224123456"));
    CHECK_EQ(d.nature, 107);
    CHECK(d.pos_valid && d.time_valid);
    CHECK_NEAR(d.lat, 43 + 21.0 / 60, 1e-6);
    CHECK_NEAR(d.lon, 8 + 24.0 / 60, 1e-6);
    CHECK_EQ(d.hour, 12);
    CHECK_EQ(d.minute, 34);
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

static void channel(cf32 *x, int n, double snr_db, double cfo, double ppm, uint32_t *seed)
{
    if (ppm != 0)
        drift(x, n, ppm);
    if (cfo != 0)
        chan_cfo(x, n, 48000.0, cfo);
    chan_awgn(x, n, snr_db, seed);
    cfir_t f;
    cfir_init_bandpass(&f, 48000.0, -8000, 8000, 3000.0, 301);
    cfir_process(&f, x, n, x);
    cfir_free(&f);
}

static void test_sim(void)
{
    printf("DSC: modulador → canal → decodificador\n");
    const modulator_desc_t *md = modulator_find("dsc");
    CHECK(md && !md->air_ok);
    iqbuf_t iq = {0};
    char err[200] = "";
    CHECK_EQ(md->modulate(NULL, &iq, err, sizeof err), 0);
    printf("  señal de %.2f s (5 llamadas)\n", iq.n / iq.rate);

    /* limpio */
    {
        evs_t ev = {0};
        run_narrow(&DEC_DSC, iq.s, iq.n, 156525000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 5);
        CHECK(ev_has(&ev, "\"fs\":112"));
        CHECK(ev_has(&ev, "\"format\":\"socorro\""));
        CHECK(ev_has(&ev, "\"mmsi\":\"224123456\""));
        CHECK(ev_has(&ev, "\"nature\":107"));
        CHECK(ev_has(&ev, "\"utc\":\"12:34\""));
        CHECK(ev_has(&ev, "\"lat\":43.35"));
        CHECK(ev_has(&ev, "\"lon\":-8.4"));
        CHECK(ev_has(&ev, "\"format\":\"todos los barcos\""));
        CHECK(ev_has(&ev, "\"format\":\"individual\"") && ev_has(&ev, "\"to\":\"366123456\""));
        CHECK(ev_has(&ev, "\"format\":\"grupo\""));
        CHECK(ev_has(&ev, "\"fs\":102") && ev_has(&ev, "\"to\":\"0422080320\""));
        evs_free(&ev);
    }

    const double cfos[] = {-4000, 0, 4000}, ppms[] = {-100, 0, 100};
    int ok = 0, tot = 0;
    for (int ci = 0; ci < 3; ci++)
        for (int pi = 0; pi < 3; pi++) {
            cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
            memcpy(x, iq.s, sizeof(cf32) * (size_t)iq.n);
            uint32_t seed = 4242u + (uint32_t)(ci * 3 + pi);
            channel(x, iq.n, 10.0, cfos[ci], ppms[pi], &seed);
            evs_t ev = {0};
            run_narrow(&DEC_DSC, x, iq.n, 156525000, NULL, &ev);
            printf("  10 dB, CFO %+5.0f Hz, %+4.0f ppm → %d/5\n", cfos[ci], ppms[pi], evs_count(&ev));
            ok += evs_count(&ev);
            tot += 5;
            CHECK(evs_count(&ev) >= 4);
            evs_free(&ev);
            free(x);
        }
    printf("  éxito: %d/%d\n", ok, tot);

    /* polaridad de tonos invertida (conjugar IQ invierte el sentido de la FM) */
    {
        cf32 *x = malloc(sizeof(cf32) * (size_t)iq.n);
        for (int i = 0; i < iq.n; i++)
            x[i] = (cf32){iq.s[i].re, -iq.s[i].im};
        evs_t ev2 = {0};
        run_narrow(&DEC_DSC, x, iq.n, 156525000, NULL, &ev2);
        printf("  polaridad invertida → %d/5\n", evs_count(&ev2));
        CHECK_EQ(evs_count(&ev2), 5);
        evs_free(&ev2);
        free(x);
    }

    /* estado */
    dec_cfg_t cfg = {.rate = 48000, .freq_hz = 156525000, .center_hz = 156525000};
    void *st = DEC_DSC.create(&cfg, err, sizeof err);
    evs_t ev = {0};
    DEC_DSC.process(st, iq.s, iq.n, 100.0, tu_collect, &ev);
    sb_t sb = {0};
    DEC_DSC.state_json(st, &sb, 110.0);
    CHECK(strstr(sb.data, "\"calls\":[") && strstr(sb.data, "\"mmsi\":\"224123456\"") && strstr(sb.data, "\"lat\":43.35"));
    sb_free(&sb);
    DEC_DSC.state_json(st, &sb, 100.0 + 4000.0);
    CHECK(strstr(sb.data, "\"count\":0"));
    sb_free(&sb);
    DEC_DSC.destroy(st);
    evs_free(&ev);
    iqbuf_free(&iq);

    /* parámetros inválidos */
    iq.n = 0;
    CHECK(md->modulate("{\"mmsi\":\"12\"}", &iq, err, sizeof err) < 0);
    CHECK(md->modulate("{\"type\":\"nada\"}", &iq, err, sizeof err) < 0);
    iqbuf_free(&iq);
}

static void test_noise(void)
{
    printf("DSC: solo ruido y coste\n");
    int n = 48000 * 20;
    cf32 *x = calloc((size_t)n, sizeof(cf32));
    uint32_t s = 77;
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
    run_narrow(&DEC_DSC, x, n, 156525000, NULL, &ev);
    double cost = (double)(clock() - t0) / CLOCKS_PER_SEC / (n / 48000.0);
    CHECK_EQ(evs_count(&ev), 0);
    printf("  coste: %.4f s de CPU por s de señal\n", cost);
    CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
    evs_free(&ev);
    free(x);
}

int main(void)
{
    test_symbols();
    test_sim();
    test_noise();

    /* Vectores de la norma: ITU-R M.493-15, Anexo 1, Tabla A1-1 (código de 10 bits, B = 0, Y = 1,
     * bit 1 el primero en transmitirse; fuente ITU, https://www.itu.int/rec/R-REC-M.493, uso
     * como dato de prueba). Fijó el orden de los bits de comprobación (más significativo primero). */
    {
        static const struct { int sym; const char *yb; } t[] = {
            {0, "BBBBBBBYYY"},   {1, "YBBBBBBYYB"},   {2, "BYBBBBBYYB"},   {43, "YYBYBYBBYY"},
            {57, "YBBYYYBBYY"},  {99, "YYBBBYYBYY"},  {100, "BBYBBYYYBB"}, {104, "BBBYBYYYBB"},
            {111, "YYYYBYYBBY"}, {112, "BBBBYYYYBB"}, {117, "YBYBYYYBYB"}, {122, "BYBYYYYBYB"},
            {125, "YBYYYYYBBY"}, {126, "BYYYYYYBBY"}, {127, "YYYYYYYBBB"},
        };
        for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
            uint8_t bits[10], enc[10];
            for (int j = 0; j < 10; j++)
                bits[j] = t[k].yb[j] == 'Y';
            CHECK_EQ(dsc_sym_decode(bits), t[k].sym);
            dsc_sym_encode(t[k].sym, enc);
            CHECK(memcmp(bits, enc, 10) == 0);
        }
    }
    /* Acuse de socorro (Tabla A1-4.2): 116, categoría 112, MMSI propio, UN telemando 110, MMSI en
     * peligro, naturaleza, posición, hora, comunicación posterior, EOS 122. */
    {
        int m[] = {116, 112, 22, 41, 23, 45, 60, 110, 22, 41, 23, 45, 60, 107, 4, 32, 10, 8, 24, 12, 34, 126, 122};
        int n = (int)(sizeof m / sizeof m[0]);
        dsc_msg_t d;
        CHECK_EQ(dsc_parse_message(m, n + 1, &d), 0);
        CHECK_EQ(d.tc1, 110);
        CHECK_EQ(d.tc2, -1);
        CHECK(strcmp(d.dist_mmsi, "224123456") == 0);
        CHECK_EQ(d.nature, 107);
        CHECK(d.pos_valid && fabs(d.lat - (43 + 21.0 / 60)) < 1e-6);
    }
    return TEST_SUMMARY();
}
