/*
 * test_dmr.c — Decodificador DMR (solo metadatos).
 *
 *  1. modulador → canal (AWGN 10 dB, CFO ±3125 Hz, ±100 ppm) → decodificador:
 *     cabeceras de voz y terminadores con LC (grupo y privada, emergencia),
 *     CSBK, cabecera de datos, slot y código de color.
 *  2. llegada tarde (sin cabecera): LC incrustado en la supertrama de voz.
 *  3. solo ruido → 0 eventos.
 *  4. vectores publicados en ETSI TS 102 361-1 V2.5.1 anexo B:
 *     - B.3.6 tabla B.18: filas de la matriz generadora de RS(12,9);
 *     - B.3.1..B.3.5: distancias mínimas de los códigos transcritos (Golay
 *       20,8 → 7; QR 16,7 → 6; Hamming 13,9 y 15,11 y 7,4 → 3; 16,11 → 4);
 *     - B.3.8: CRC-CCITT = complemento de CRC-16/XMODEM (check 0x31C3 del
 *       catálogo de Greg Cook, reveng), registro inicial 0;
 *     - B.1.1 / B.4.1: ida y vuelta de BPTC(196,96) y CACH con errores.
 *     NO hay capturas reales con licencia clara: la concordancia de los
 *     bits sobre el aire con equipos reales no está verificada.
 *  5. coste de CPU por segundo de señal.
 */
#include "test_util.h"

#include "decoders/dmr.h"
#include "decoders/dmr_fec.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_DMR;
extern const modulator_desc_t MOD_DMR;

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

static int count_of(const evs_t *e, const char *needle)
{
    int n = 0;
    for (const char *p = e->data; p && (p = strstr(p, needle)); p++)
        n++;
    return n;
}

static void chain(const char *params, double cfo, double ppm, uint32_t seed, double snr, evs_t *ev, double *cpu)
{
    char err[200];
    iqbuf_t m = {0};
    CHECK_EQ(MOD_DMR.modulate(params, &m, err, sizeof err), 0);
    int pad = 12000, n = m.n + 2 * pad;
    cf32 *x = calloc((size_t)n, sizeof(cf32)), *y = calloc((size_t)n, sizeof(cf32));
    memcpy(x + pad, m.s, sizeof(cf32) * (size_t)m.n);
    for (int i = 0; i < pad; i++)
        x[i] = x[n - 1 - i] = (cf32){0.8f, 0};
    drift(x, n, ppm, y);
    chan_cfo(y, n, 48000, cfo);
    chan_awgn(y, n, snr, &seed);
    double t0 = cpu_s();
    run_narrow(&DEC_DMR, y, n, 438500000, NULL, ev);
    *cpu = (cpu_s() - t0) / (n / 48000.0);
    free(x);
    free(y);
    iqbuf_free(&m);
}

static void check_default(const evs_t *ev)
{
    CHECK(ev_has(ev, "\"burst\":\"voice_header\",\"slot\":1,\"cc\":1,\"pf\":0,\"flco\":0,\"fid\":0,\"call\":\"group\","
                     "\"dst\":91,\"src\":2001234,\"svc\":0,\"emergency\":false"));
    CHECK(ev_has(ev, "\"burst\":\"voice_header\",\"slot\":2,\"cc\":1,\"pf\":0,\"flco\":3,\"fid\":0,\"call\":\"private\","
                     "\"dst\":4321,\"src\":1234567,\"svc\":128,\"emergency\":true"));
    CHECK(ev_has(ev, "\"burst\":\"terminator\",\"slot\":1,\"cc\":1,\"pf\":0,\"flco\":0"));
    CHECK(ev_has(ev, "\"burst\":\"terminator\",\"slot\":2,\"cc\":1,\"pf\":0,\"flco\":3"));
    CHECK(ev_has(ev, "\"burst\":\"csbk\",\"slot\":2,\"cc\":1,\"lb\":1,\"pf\":0,\"csbko\":4,\"fid\":0,\"data\":\"2131415161718191\""));
    CHECK(ev_has(ev, "\"burst\":\"data_header\",\"slot\":2,\"cc\":1,\"dpf\":2,\"dpf_name\":\"datos sin confirmar\",\"sap\":4,"
                     "\"call\":\"private\",\"dst\":77713,\"src\":88760"));
    CHECK_EQ(evs_count(ev), 6);
}

int main(void)
{
    printf("[1] modulador -> canal -> decodificador\n");
    struct { double cfo, ppm; const char *tag; } cases[3] = {{0, 0, "limpio"}, {3125, 100, "cfo+"}, {-3125, -100, "cfo-"}};
    for (int i = 0; i < 3; i++) {
        evs_t ev = {0};
        double cpu;
        chain(NULL, cases[i].cfo, cases[i].ppm, 40u + (uint32_t)i, 10.0, &ev, &cpu);
        printf("  %-7s cfo %+5.0f ppm %+4.0f -> %d eventos, CPU %.4f s/s\n", cases[i].tag, cases[i].cfo, cases[i].ppm,
               evs_count(&ev), cpu);
        check_default(&ev);
        if (tu_fails)
            printf("%s\n", ev.data ? ev.data : "(sin eventos)");
        evs_free(&ev);
    }

    printf("[2] llegada tarde: LC incrustado\n");
    {
        evs_t ev = {0};
        double cpu;
        chain("{\"cc\":5,\"csbk\":false,\"data_header\":false,\"calls\":[{\"slot\":1,\"group\":true,\"dst\":262,\"src\":9990001,"
              "\"superframes\":2,\"header\":false},{\"slot\":2,\"group\":false,\"dst\":7,\"src\":8,\"svc\":128,\"header\":false}]}",
              1500, 50, 7, 12.0, &ev, &cpu);
        CHECK(ev_has(&ev, "\"burst\":\"embedded_lc\",\"slot\":1,\"cc\":5,\"pf\":0,\"flco\":0,\"fid\":0,\"call\":\"group\","
                          "\"dst\":262,\"src\":9990001"));
        CHECK(ev_has(&ev, "\"burst\":\"embedded_lc\",\"slot\":2,\"cc\":5,\"pf\":0,\"flco\":3,\"fid\":0,\"call\":\"private\","
                          "\"dst\":7,\"src\":8,\"svc\":128"));
        CHECK_EQ(count_of(&ev, "\"burst\":\"embedded_lc\",\"slot\":1"), 1);
        CHECK(ev_has(&ev, "\"burst\":\"terminator\""));
        if (tu_fails)
            printf("%s\n", ev.data ? ev.data : "(sin eventos)");
        evs_free(&ev);
    }

    printf("[2b] estado JSON con llamada activa\n");
    {
        char err[200];
        iqbuf_t m = {0};
        CHECK_EQ(MOD_DMR.modulate("{\"csbk\":false,\"data_header\":false,\"calls\":[{\"slot\":1,\"group\":true,\"dst\":91,"
                                  "\"src\":2001234,\"superframes\":6}]}",
                                  &m, err, sizeof err),
                 0);
        dec_cfg_t cfg = {.rate = 48000, .freq_hz = 438500000, .center_hz = 438500000};
        void *st = DEC_DMR.create(&cfg, err, sizeof err);
        evs_t ev = {0};
        int cut = m.n * 7 / 10; /* corta a mitad de llamada */
        for (int o = 0; o < cut; o += 4096)
            DEC_DMR.process(st, m.s + o, cut - o < 4096 ? cut - o : 4096, o / 48000.0, tu_collect, &ev);
        sb_t sb = {0};
        DEC_DMR.state_json(st, &sb, cut / 48000.0);
        CHECK(strstr(sb.data, "\"calls\":[{\"slot\":1,\"cc\":1,\"via\":\"header\"") != NULL);
        CHECK(strstr(sb.data, "\"dst\":91,\"src\":2001234") != NULL);
        sb_clear(&sb);
        DEC_DMR.state_json(st, &sb, cut / 48000.0 + 10.0); /* 10 s después: caducada */
        CHECK(strstr(sb.data, "\"calls\":[]") != NULL);
        CHECK(strstr(sb.data, "\"recent\":[{\"slot\":1") != NULL);
        sb_free(&sb);
        evs_free(&ev);
        DEC_DMR.destroy(st);
        iqbuf_free(&m);
    }

    printf("[3] solo ruido\n");
    {
        int n = 48000 * 20;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 3;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t ev = {0};
        run_narrow(&DEC_DMR, x, n, 438500000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        free(x);
    }

    printf("[4] vectores de ETSI TS 102 361-1 anexo B\n");
    {
        /* Tabla B.18: filas de G de RS(12,9); parte de paridad de las filas 1, 3, 4 y 9 */
        static const struct { int row; uint8_t p[3]; } rs[] = {
            {0, {0x1C, 0xBC, 0xFD}}, {2, {0xAD, 0x41, 0x36}}, {3, {0x7D, 0x71, 0x16}}, {8, {0x0E, 0x38, 0x40}}};
        for (unsigned i = 0; i < sizeof rs / sizeof rs[0]; i++) {
            uint8_t d[9] = {0}, p[3];
            d[rs[i].row] = 1;
            dmr_rs129_parity(d, p);
            CHECK(memcmp(p, rs[i].p, 3) == 0);
        }
        /* distancias mínimas */
        /* La norma lo llama (20,8,7): es el Golay (23,12,7) acortado + paridad global,
         * de distancia 8 (todas las filas de la tabla B.11 tienen peso par). */
        CHECK_EQ(dmr_fec_dmin(0), 8);
        CHECK_EQ(dmr_fec_dmin(1), 6);
        CHECK_EQ(dmr_fec_dmin(2), 3);
        CHECK_EQ(dmr_fec_dmin(3), 3);
        CHECK_EQ(dmr_fec_dmin(4), 4);
        CHECK_EQ(dmr_fec_dmin(5), 3);
        /* CRC-CCITT: FH = ~CRC-16/XMODEM; check "123456789" = 0x31C3 */
        CHECK_EQ(dmr_crc_ccitt((const uint8_t *)"123456789", 9), 0x31C3 ^ 0xFFFF);
        /* tipo de ranura con 3 errores; EMB con 2; TACT con 1 */
        uint32_t w = dmr_slottype_encode(9, 3);
        int cc, dt, pi, lcss, at, tc;
        CHECK_EQ(dmr_slottype_decode(w ^ 0x80001 ^ 0x00400, &cc, &dt), 3);
        CHECK(cc == 9 && dt == 3);
        w = dmr_emb_encode(7, 1, 2);
        CHECK_EQ(dmr_emb_decode(w ^ 0x4001, &cc, &pi, &lcss), 2);
        CHECK(cc == 7 && pi == 1 && lcss == 2);
        w = dmr_tact_encode(1, 1, 3);
        CHECK_EQ(dmr_tact_decode(w ^ 0x10, &at, &tc, &lcss), 0);
        CHECK(at == 1 && tc == 1 && lcss == 3);
        /* BPTC ida y vuelta con 3 errores dispersos */
        uint8_t info[96], enc[196], dec[96];
        uint32_t s = 99;
        for (int i = 0; i < 96; i++)
            info[i] = (uint8_t)(tu_rand(&s) & 1);
        dmr_bptc_encode(info, enc);
        enc[5] ^= 1;
        enc[77] ^= 1;
        enc[150] ^= 1;
        int c = dmr_bptc_decode(enc, dec);
        CHECK(c >= 3);
        CHECK(memcmp(info, dec, 96) == 0);
        /* LC incrustado */
        uint8_t lc[9] = {0, 0, 0x80, 0, 0, 91, 0x1E, 0xAA, 0x52}, lc2[9];
        uint32_t fr[4];
        dmr_emblc_encode(lc, fr);
        fr[1] ^= 0x00010400;
        CHECK(dmr_emblc_decode(fr, lc2) >= 0);
        CHECK(memcmp(lc, lc2, 9) == 0);
    }
    return TEST_SUMMARY();
}
