/*
 * test_rds.c — Decodificador RDS (src/decoders/rds.c) y su modulador.
 *
 *  1. Constantes del código de bloque (síndromes) contra las que publica la
 *     matriz de paridad H de EN 50067 anexo B tal como la usa el decodificador
 *     abierto redsea (https://github.com/windytan/redsea, src/block_sync.cc).
 *  2. Modulador → canal (CFO ±50 kHz, ±100 ppm de reloj, AWGN) → decodificador.
 *  3. Solo ruido → 0 eventos.
 *  4. Vectores reales publicados: grupos capturados de emisoras reales, tomados
 *     de test/components-hex.cc de redsea (YLE X3M 2016-09-15 PS; JACK 96.9 RT
 *     método A; Radio Krka 2B; BR-KLASSIK y 98.5 KFOX CT; PI 0x3ABC PTYN).
 *     Se pasan por el modulador (que les añade CRC y offsets) y por toda la
 *     cadena de señal.
 *  5. Coste de CPU por segundo de señal.
 */
#include "test_util.h"

#include "decoders/rds.h"
#include "tx/mod.h"

#include "vec_real.h"
#include <time.h>

extern const decoder_desc_t DEC_RDS;
extern const modulator_desc_t MOD_RDS;

typedef struct {
    evs_t evs;
    char state[2048];
    double cpu;
} res_t;

static void run_rds(const cf32 *iq, int n, const char *opts, res_t *r)
{
    char err[200] = "";
    dec_cfg_t cfg = {.rate = RDS_RATE, .freq_hz = 100300000, .center_hz = 100300000, .opts = opts};
    void *st = DEC_RDS.create(&cfg, err, sizeof err);
    memset(r, 0, sizeof *r);
    if (!st) {
        printf("  create falló: %s\n", err);
        return;
    }
    clock_t c0 = clock();
    for (int off = 0; off < n; off += 4096)
        DEC_RDS.process(st, iq + off, n - off < 4096 ? n - off : 4096, 1000.0 + off / RDS_RATE, tu_collect, &r->evs);
    r->cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    sb_t sb = {0};
    DEC_RDS.state_json(st, &sb, 1000.0 + n / RDS_RATE);
    snprintf(r->state, sizeof r->state, "%s", sb.data);
    sb_free(&sb);
    DEC_RDS.destroy(st);
}

static int gen(const char *params, iqbuf_t *b)
{
    char err[200];
    memset(b, 0, sizeof *b);
    int rc = MOD_RDS.modulate(params, b, err, sizeof err);
    if (rc < 0)
        printf("  modulate falló: %s\n", err);
    return rc;
}

/* Aplica el canal: desvío de reloj, CFO y ruido. snr_db < 0 → sin ruido. */
static cf32 *channel(const iqbuf_t *b, double cfo, double ppm, double snr_db, uint32_t seed)
{
    cf32 *x = malloc(sizeof(cf32) * (size_t)b->n);
    if (ppm != 0) {
        /* chan_drift() de test_util.h reserva n+8 y se desborda con ppm > 0 y n grande */
        cf32 *tmp = malloc(sizeof(cf32) * (size_t)(b->n + b->n / 5000 + 64));
        int m = chan_resample(b->s, b->n, 1.0 + ppm * 1e-6, tmp);
        for (int i = 0; i < b->n; i++)
            x[i] = i < m ? tmp[i] : (cf32){0, 0};
        free(tmp);
    } else
        memcpy(x, b->s, sizeof(cf32) * (size_t)b->n);
    if (cfo != 0)
        chan_cfo(x, b->n, b->rate, cfo);
    if (snr_db > 0)
        chan_awgn(x, b->n, snr_db, &seed);
    return x;
}

static void check_synthetic(const char *name, double cfo, double ppm, double snr, int secs)
{
    iqbuf_t b;
    char prm[64];
    snprintf(prm, sizeof prm, "{\"seconds\":%d}", secs);
    if (gen(prm, &b) < 0)
        return;
    cf32 *x = channel(&b, cfo, ppm, snr, 12345);
    res_t r;
    run_rds(x, b.n, NULL, &r);
    printf("  %-34s eventos=%d cpu=%.3f s/s\n    %s\n", name, r.evs.count, r.cpu / (b.n / RDS_RATE), r.state);
    CHECK(strstr(r.state, "\"pi\":\"C201\""));
    CHECK(strstr(r.state, "\"ps\":\"BLADERF\""));
    CHECK(strstr(r.state, "\"rt\":\"BladeRF Lab RDS: hola mundo!\""));
    CHECK(strstr(r.state, "\"pty\":10,\"pty_name\":\"Pop music\""));
    CHECK(strstr(r.state, "\"tp\":true,\"ta\":false,\"ms\":\"music\""));
    CHECK(strstr(r.state, "\"af\":[100300000,98500000,104700000]"));
    CHECK(strstr(r.state, "\"ct\":\"2026-11-15 12:34 +01:00\""));
    CHECK(ev_has(&r.evs, "\"kind\":\"ps\""));
    CHECK(ev_has(&r.evs, "\"kind\":\"rt\""));
    CHECK(ev_has(&r.evs, "\"kind\":\"pi\""));
    evs_free(&r.evs);
    free(x);
    free(b.s);
}

/* Vector real: grupos (hex de 16 dígitos) → señal → decodificador. */
static void check_real(const char *groups, const char *want_state, const char *want_event, double cfo, double ppm,
                       double snr)
{
    char params[1024];
    snprintf(params, sizeof params, "{\"seconds\":5,\"groups\":[%s]}", groups);
    iqbuf_t b;
    if (gen(params, &b) < 0)
        return;
    cf32 *x = channel(&b, cfo, ppm, snr, 777);
    res_t r;
    run_rds(x, b.n, NULL, &r);
    printf("  real: %s\n", r.state);
    if (want_state)
        CHECK(strstr(r.state, want_state));
    if (want_event)
        CHECK(ev_has(&r.evs, want_event));
    evs_free(&r.evs);
    free(x);
    free(b.s);
}

int main(void)
{
    /* ---- 1. Constantes del código de bloque ---- */
    /* Matriz H de EN 50067 anexo B (copiada de redsea): síndrome = XOR de las filas
     * correspondientes a los bits a 1 (primer bit transmitido = bit 25). */
    static const uint16_t H[26] = {0x200, 0x100, 0x080, 0x040, 0x020, 0x010, 0x008, 0x004, 0x002,
                                   0x001, 0x2DC, 0x16E, 0x0B7, 0x287, 0x39F, 0x313, 0x355, 0x376,
                                   0x1BB, 0x201, 0x3DC, 0x1EE, 0x0F7, 0x2A7, 0x38F, 0x31B};
    static const uint16_t SYN_H[5] = {0x3D8, 0x3D4, 0x25C, 0x3CC, 0x258};
    uint32_t rs = 1;
    for (int o = 0; o < 5; o++)
        for (int t = 0; t < 20; t++) {
            uint16_t data = (uint16_t)tu_rand(&rs);
            uint32_t w = rds_block_encode(data, o);
            uint32_t s = 0;
            for (int k = 0; k < 26; k++)
                if ((w >> k) & 1)
                    s ^= H[25 - k];
            CHECK_EQ(s, SYN_H[o]);
            CHECK_EQ(rds_block_offset(w), o);
            CHECK_EQ(rds_rem(w, 26), rds_offset_word(o));
            CHECK_EQ(rds_block_offset(w ^ 1), -1); /* 1 bit de error: ya no es un offset */
        }
    /* MJD: ejemplo de EN 300 468 anexo C: 0xC079 = 49273 → 1993-10-13 */
    int y, m, d;
    rds_mjd_to_date(49273, &y, &m, &d);
    CHECK(y == 1993 && m == 10 && d == 13);
    CHECK_EQ(rds_date_to_mjd(1993, 10, 13), 49273);
    rds_mjd_to_date(57847, &y, &m, &d);
    CHECK(y == 2017 && m == 4 && d == 4);
    CHECK(strcmp(rds_pty_name(10), "Pop music") == 0 && strcmp(rds_pty_name(31), "Alarm") == 0);

    /* ---- 2. Modulador → canal → decodificador ---- */
    printf("Síntesis (SNR en 250 kHz):\n");
    check_synthetic("limpio", 0, 0, -1, 6);
    check_synthetic("SNR 30 dB", 0, 0, 30, 6);
    check_synthetic("CFO +50 kHz, +100 ppm, 30 dB", 50000, 100, 30, 6);
    check_synthetic("CFO -50 kHz, -100 ppm, 30 dB", -50000, -100, 30, 6);
    check_synthetic("SNR 20 dB", 0, 0, 20, 20);
    check_synthetic("CFO +50 kHz, +100 ppm, 20 dB", 50000, 100, 20, 20);
    check_synthetic("CFO -50 kHz, -100 ppm, 20 dB", -50000, -100, 20, 20);

    /* 10 dB en 250 kHz está por debajo del umbral de la FM: lo que se decodifique
     * debe ser correcto (se informa, no se exige que decodifique). */
    {
        iqbuf_t b;
        gen("{\"seconds\":30}", &b);
        cf32 *x = channel(&b, 0, 0, 10, 99);
        res_t r;
        run_rds(x, b.n, NULL, &r);
        printf("  SNR 10 dB (30 s, umbral FM): eventos=%d\n    %s\n", r.evs.count, r.state);
        CHECK(!ev_has(&r.evs, "\"kind\":\"ps\"") || ev_has(&r.evs, "\"ps\":\"BLADERF\""));
        evs_free(&r.evs);
        free(x);
        free(b.s);
    }

    /* Estéreo con L−R junto al borde de la banda de RDS (tono de 15 kHz → 53 kHz) */
    {
        iqbuf_t b;
        gen("{\"seconds\":6,\"audio_hz\":15000}", &b);
        cf32 *x = channel(&b, 0, 0, 30, 9);
        res_t r;
        run_rds(x, b.n, NULL, &r);
        CHECK(strstr(r.state, "\"ps\":\"BLADERF\""));
        evs_free(&r.evs);
        free(x);
        free(b.s);
    }

    /* Corrección de ráfagas activada: el resultado no debe empeorar */
    {
        iqbuf_t b;
        gen("{\"seconds\":6}", &b);
        cf32 *x = channel(&b, 0, 0, 20, 5);
        res_t r;
        run_rds(x, b.n, "{\"fec\":2}", &r);
        CHECK(strstr(r.state, "\"ps\":\"BLADERF\""));
        CHECK(strstr(r.state, "\"rt\":\"BladeRF Lab RDS: hola mundo!\""));
        evs_free(&r.evs);
        free(x);
        free(b.s);
    }

    /* Resincronización rápida: se concatena otra emisora (PI distinto) */
    {
        iqbuf_t a, c;
        gen("{\"seconds\":3}", &a);
        gen("{\"seconds\":3,\"pi\":\"D314\",\"ps\":\"OTRA FM \",\"rt\":\"Segunda emisora\"}", &c);
        int n = a.n + c.n;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        memcpy(x, a.s, sizeof(cf32) * (size_t)a.n);
        memcpy(x + a.n, c.s, sizeof(cf32) * (size_t)c.n);
        res_t r;
        run_rds(x, n, NULL, &r);
        printf("  cambio de emisora: %s\n", r.state);
        CHECK(strstr(r.state, "\"pi\":\"D314\""));
        CHECK(strstr(r.state, "\"ps\":\"OTRA FM\""));
        CHECK(strstr(r.state, "\"rt\":\"Segunda emisora\""));
        CHECK(ev_has(&r.evs, "\"pi\":\"C201\""));
        evs_free(&r.evs);
        free(x);
        free(a.s);
        free(c.s);
    }

    /* ---- 3. Solo ruido ---- */
    {
        int n = (int)(RDS_RATE * 20);
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.3 * tu_gauss(&seed));
            x[i].im = (float)(0.3 * tu_gauss(&seed));
        }
        res_t r;
        run_rds(x, n, NULL, &r);
        printf("Solo ruido (20 s): eventos=%d\n", r.evs.count);
        CHECK_EQ(r.evs.count, 0);
        CHECK(strstr(r.state, "\"pi\":\"\""));
        evs_free(&r.evs);
        /* El estado caduca: 40 s después del último grupo todo vuelve a vacío */
        iqbuf_t b;
        gen("{\"seconds\":4}", &b);
        char err[100];
        dec_cfg_t cfg = {.rate = RDS_RATE, .freq_hz = 100300000, .center_hz = 100300000};
        void *st = DEC_RDS.create(&cfg, err, sizeof err);
        evs_t e = {0};
        DEC_RDS.process(st, b.s, b.n, 1000.0, tu_collect, &e);
        sb_t sb = {0};
        DEC_RDS.state_json(st, &sb, 1004.0);
        CHECK(strstr(sb.data, "\"ps\":\"BLADERF\""));
        sb_clear(&sb);
        DEC_RDS.state_json(st, &sb, 1100.0);
        CHECK(strstr(sb.data, "\"ps\":\"\"") && strstr(sb.data, "\"pi\":\"\""));
        sb_free(&sb);
        DEC_RDS.destroy(st);
        evs_free(&e);
        free(b.s);
        free(x);
    }

    /* ---- 4. Vectores reales (redsea, test/components-hex.cc) ---- */
    printf("Vectores reales:\n");
    /* YLE X3M (fi) 2016-09-15: grupos 0A con PS "YLE X3M ", PTY 9 (Varied), TA */
    check_real("\"62040130966B594C\",\"6204013193CD4520\",\"62040132E4725833\",\"62040137966B4D20\"",
               "\"pi\":\"6204\",\"ps\":\"YLE X3M\",\"rt\":\"\",\"pty\":9,\"pty_name\":\"Varied\",\"tp\":false,\"ta\":true",
               "\"ps\":\"YLE X3M\"", 0, 0, -1);
    /* JACK 96.9 (ca) 2019-05-05: RadioText terminado con 0x0D */
    check_real("\"C95424F04A41434B\",\"C95424F12039362E\",\"C95424F2390D0000\"", "\"rt\":\"JACK 96.9\"",
               "\"rt\":\"JACK 96.9\"", 20000, 50, 35);
    /* Radio Krka (si): grupo 2B, 16 segmentos de 2 caracteres */
    check_real("\"9423280000005052\",\"942328010000494A\",\"9423280200004554\",\"9423280300004E4F\","
               "\"9423280400002050\",\"9423280500004F53\",\"9423280600004C55\",\"9423280700005341\","
               "\"9423280800004E4A\",\"9423280900004520\",\"9423280A00005241\",\"9423280B00004449\","
               "\"9423280C00004120\",\"9423280D00004B52\",\"9423280E00004B41\",\"9423280F00002020\"",
               "\"rt\":\"PRIJETNO POSLUSANJE RADIA KRKA\"", NULL, 0, 0, -1);
    /* BR-KLASSIK 2017-04-04: CT 23:43 +02:00 (también con CFO) */
    check_real("\"D31441C1C3EF5AC4\",\"D31441C1C3EF5AC4\"", "\"ct\":\"2017-04-04 23:43 +02:00\"", NULL, -30000, -80, -1);
    /* 98.5 KFOX 2020-08-19: desfase negativo −07:00 */
    check_real("\"456940DDCD923BAE\"", "\"ct\":\"2020-08-19 20:46 -07:00\"", NULL, 0, 0, -1);
    /* PI 3ABC (Polonia) 2019-05-04: PTYN "CRI.CN" y PTY 26 */
    check_real("\"3ABCA7504352492E\",\"3ABCA751434E0D0D\"", "\"pty\":26,\"pty_name\":\"National music\"",
               "\"kind\":\"ptyn\",\"pi\":\"3ABC\",\"pty\":26,\"pty_name\":\"National music\",\"ptyn\":\"CRI.CN\"", 0, 0, -1);

    /* ---- 5. Coste ---- */
    {
        iqbuf_t b;
        gen("{\"seconds\":10}", &b);
        cf32 *x = channel(&b, 10000, 50, 25, 31);
        res_t r;
        run_rds(x, b.n, NULL, &r);
        double cost = r.cpu / (b.n / RDS_RATE);
        printf("Coste: %.4f s de CPU por s de señal\n", cost);
        CHECK(cost < VR_CPU(0.3)); /* x8 bajo sanitizer */
        evs_free(&r.evs);
        free(x);
        free(b.s);
    }
    return TEST_SUMMARY();
}
