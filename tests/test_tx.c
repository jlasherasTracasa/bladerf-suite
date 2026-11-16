/*
 * test_tx.c — Seguridad de la emisión (AGENTS.md §6), reproducción, calibración.
 *
 * Todo con un "sink" simulado en lugar del bladeRF: nunca se toca el hardware.
 * Los moduladores son propios del test (no dependen de los mod_*.c de los demás).
 */
#include "chan_sim.h"
#include "test_util.h"
#include "tx/tx.h"

#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- Moduladores de prueba: CPFSK a 2400 baudios, tasa 100 ksps ----------- */
static double g_dur = 0.1;       /* s */
static double g_dev = 2400.0;    /* desviación (Hz) */
static int g_mode_tone = 0;      /* 1: tono de 100 kHz a 1 Msps (para medir el IQ) */
static int g_mode_short = 0;     /* 1: 300 muestras de un tono a +400 kHz (a 1 Msps) */
static int g_mode_late = 0;      /* 1: 2,6 s silenciosos con un tono a +400 kHz al final */

static int fake_mod(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    (void)params;
    (void)err;
    (void)errlen;
    if (g_mode_tone || g_mode_short || g_mode_late) {
        out->rate = 1e6;
        int n = g_mode_tone ? 100000 : g_mode_short ? 300 : 2600000;
        double f = g_mode_tone ? 0.1 : 0.4;
        for (int i = 0; i < n; i++) {
            if (g_mode_late && i < n - 20000)
                iqbuf_push(out, (cf32){0, 0});
            else
                iqbuf_push(out, (cf32){(float)(0.5 * cos(2 * M_PI * f * i)), (float)(0.5 * sin(2 * M_PI * f * i))});
        }
        return 0;
    }
    out->rate = 100000;
    int n = (int)(g_dur * out->rate);
    uint32_t sd = 99;
    double ph = 0;
    int sym = 0;
    for (int i = 0; i < n; i++) {
        if (i % 41 == 0) /* 100000/2400 ≈ 41,7 */
            sym = (tu_rand(&sd) & 1) ? 1 : -1;
        ph += 2 * M_PI * sym * g_dev / out->rate;
        iqbuf_push(out, (cf32){(float)(0.8 * cos(ph)), (float)(0.8 * sin(ph))});
    }
    return 0;
}

static const modulator_desc_t MOD_AIR = {.id = "tsta", .name = "Prueba air_ok", .air_ok = true, .modulate = fake_mod};
static const modulator_desc_t MOD_NOAIR = {.id = "tstn", .name = "Prueba sin air", .air_ok = false, .modulate = fake_mod};

/* ---- Sustituto del hardware ------------------------------------------------ */
#define REC_MAX (1 << 21)
typedef struct {
    int nconf, nwrite, nstop;
    int64_t freq;
    uint32_t rate;
    int gain;
    bool loopback;
    int16_t *rec; /* muestras escritas (pares I,Q) */
    volatile int rec_n;
    int delay_ms;
} sink_t;

static int s_conf(void *c, int64_t f, uint32_t r, int g, bool lb)
{
    sink_t *s = c;
    s->nconf++;
    s->freq = f;
    s->rate = r;
    s->gain = g;
    s->loopback = lb;
    s->rec_n = 0;
    return 0;
}
static int s_write(void *c, const int16_t *iq, unsigned n)
{
    sink_t *s = c;
    s->nwrite++;
    if (s->delay_ms)
        usleep((useconds_t)s->delay_ms * 1000);
    if (s->rec_n + (int)n <= REC_MAX)
        memcpy(s->rec + 2 * (size_t)s->rec_n, iq, sizeof(int16_t) * 2 * n);
    __atomic_add_fetch(&s->rec_n, (int)n, __ATOMIC_SEQ_CST);
    return 0;
}
static int s_stop(void *c)
{
    ((sink_t *)c)->nstop++;
    return 0;
}

static sink_t g_sink;
static tx_sink_t g_tsink = {.configure = s_conf, .write = s_write, .stop = s_stop, .ctx = &g_sink};

static char g_dir[128];

static tx_t *make_tx(bool enabled, bool air, const char *callsign, int maxgain)
{
    tx_policy_t p = {.enabled = enabled, .air = air, .max_gain_db = maxgain};
    snprintf(p.callsign, sizeof p.callsign, "%s", callsign);
    tx_t *t = tx_create(NULL, NULL, g_dir, &p);
    tx_set_sink(t, &g_tsink);
    memset(&g_sink.nconf, 0, 3 * sizeof(int));
    return t;
}

/* Envía y, si se acepta, espera a que acabe el hilo. */
static int go(tx_t *t, const modulator_desc_t *m, tx_mode_t mode, int64_t freq, int64_t center, uint32_t rate, int gain,
              int repeat, char *err)
{
    tx_req_t r = {.mode = mode, .freq = freq, .center = center, .rate = rate, .gain_db = gain, .repeat = repeat};
    snprintf(r.proto, sizeof r.proto, "%s", m->id);
    err[0] = 0;
    int rc = tx_submit_with(t, m, &r, err, 300);
    tx_wait(t);
    return rc;
}

/* Meta sintética para tx_check_air: señal de ±6 kHz */
static int chk(const tx_policy_t *p, const modulator_desc_t *m, int64_t f, int64_t c, double lo, double hi, int g,
               double secs, double recent)
{
    tx_meta_t mt = {.center = c, .rate = 1e6, .lo_hz = lo, .hi_hz = hi, .occ_bw_hz = hi - lo, .seconds = secs};
    char e[200];
    return tx_check_air(p, m, f, c, &mt, g, secs, recent, e, sizeof e);
}

/* ---- Modelo del TX con defectos para la calibración ------------------------- */
static const double TX_G = 1.12, TX_PH = 6.0, TX_DCI = 0.02, TX_DCQ = -0.015;
static double TX_DELAY = 37.3;
static void tx_model(const int16_t *iq, int n, cf32 *y)
{
    cf32 *x = malloc(sizeof(cf32) * n);
    for (int i = 0; i < n; i++)
        x[i] = (cf32){iq[2 * i] / 2048.0f, iq[2 * i + 1] / 2048.0f};
    cs_iq_imbalance(x, n, TX_G, TX_PH, TX_DCI, TX_DCQ);
    for (int i = 0; i < CAL_CAPTURE_LEN; i++) { /* retardo fraccionario + ganancia compleja del bucle */
        cf32 s = i >= (int)TX_DELAY + 2 ? cs_interp(x, n, i - TX_DELAY) : (cf32){0, 0};
        y[i] = (cf32){0.8f * (s.re * 0.955f - s.im * 0.2955f), 0.8f * (s.re * 0.2955f + s.im * 0.955f)};
    }
    free(x);
}

static int rx_hook(void *ctx, int64_t freq, uint32_t rate, int16_t *iq, unsigned n, char *err, size_t el)
{
    (void)ctx, (void)freq, (void)rate, (void)err, (void)el;
    int need = CAL_PRE_ZEROS + cal_burst_len() + CAL_POST_ZEROS;
    for (int w = 0; w < 5000 && g_sink.rec_n < need; w++)
        usleep(1000);
    if (g_sink.rec_n < need)
        return -1;
    cf32 *y = malloc(sizeof(cf32) * CAL_CAPTURE_LEN);
    tx_model(g_sink.rec, need, y);
    cs_rng_t r;
    cs_seed(&r, 5);
    cs_awgn(y, (int)n, 45.0, &r);
    for (unsigned i = 0; i < n; i++) {
        iq[2 * i] = (int16_t)lrintf(CLAMP(y[i].re, -0.99f, 0.99f) * 2048.f);
        iq[2 * i + 1] = (int16_t)lrintf(CLAMP(y[i].im, -0.99f, 0.99f) * 2048.f);
    }
    free(y);
    return 0;
}

typedef struct {
    tx_t *t;
    int rc;
} race_t;
static void *race_main(void *a)
{
    race_t *r = a;
    char err[300];
    tx_req_t q = {.mode = TX_LOOPBACK, .freq = 433920000, .repeat = 1};
    snprintf(q.proto, sizeof q.proto, "tsta");
    r->rc = tx_submit_with(r->t, &MOD_AIR, &q, err, sizeof err);
    return NULL;
}

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0, c;
    if (!f)
        return -1;
    while ((c = fgetc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

int main(void)
{
    g_log_level = LOG_LVL_ERROR;
    snprintf(g_dir, sizeof g_dir, "/tmp/tx_test_XXXXXX");
    CHECK(mkdtemp(g_dir) != NULL);
    g_sink.rec = malloc(sizeof(int16_t) * 2 * REC_MAX);
    char err[300];

    /* ---------------- indicativos ---------------- */
    CHECK(tx_callsign_valid("EA1ABC"));
    CHECK(tx_callsign_valid("EA1ABC-9"));
    CHECK(!tx_callsign_valid(""));
    CHECK(!tx_callsign_valid("A1"));
    CHECK(!tx_callsign_valid("ABCDEF"));      /* sin dígito */
    CHECK(!tx_callsign_valid("123456"));      /* sin letra */
    CHECK(!tx_callsign_valid("EA1 ABC"));
    CHECK(!tx_callsign_valid("EA1\"};{"));

    /* ---------------- tx_check_air: bordes de banda ---------------- */
    tx_policy_t P = {.enabled = true, .air = true, .max_gain_db = 0};
    snprintf(P.callsign, sizeof P.callsign, "EA1ABC");
    CHECK_EQ(chk(&P, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, 1, 0), 0);
    CHECK_EQ(chk(&P, &MOD_AIR, 433056000, 433056000, -6000, 6000, 0, 1, 0), 0);  /* borde inferior exacto */
    CHECK(chk(&P, &MOD_AIR, 433055999, 433055999, -6000, 6000, 0, 1, 0) < 0);    /* 1 Hz fuera */
    CHECK_EQ(chk(&P, &MOD_AIR, 434784000, 434784000, -6000, 6000, 0, 1, 0), 0);  /* borde superior exacto */
    CHECK(chk(&P, &MOD_AIR, 434784001, 434784001, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 868300000, 868300000, -350000, 350000, 0, 1, 0) < 0); /* ancho que se sale por ambos lados */
    CHECK(chk(&P, &MOD_AIR, 868597000, 868597000, -6000, 6000, 0, 1, 0) < 0);     /* cruza 868,6 */
    CHECK(chk(&P, &MOD_AIR, 869000000, 869000000, -6000, 6000, 0, 1, 0) < 0);     /* hueco 868,6–869,4 */
    CHECK_EQ(chk(&P, &MOD_AIR, 869500000, 869500000, -6000, 6000, 0, 1, 0), 0);
    CHECK(chk(&P, &MOD_AIR, 869650000, 869650000, -6000, 6000, 0, 1, 0) < 0);
    /* Bandas prohibidas: aviación, marítimo, emergencias, GNSS, celular, broadcast */
    int64_t forb[] = {121500000, 156800000, 162025000, 243000000, 406025000, 1090000000, 1575420000,
                      940000000, 98000000, 2400000000LL, 0, -5, 429900000, 440500000};
    for (size_t i = 0; i < ARRAY_LEN(forb); i++)
        CHECK(chk(&P, &MOD_AIR, forb[i], forb[i], -6000, 6000, 0, 1, 0) < 0);
    /* ham: indicativo */
    CHECK_EQ(chk(&P, &MOD_AIR, 145500000, 145500000, -6000, 6000, 0, 1, 0), 0);
    CHECK(chk(&P, &MOD_AIR, 143999000, 143999000, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 146001000, 146001000, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 440100000, 440100000, -6000, 6000, 0, 1, 0) < 0);
    tx_policy_t Pn = P;
    Pn.callsign[0] = 0;
    CHECK(chk(&Pn, &MOD_AIR, 145500000, 145500000, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&Pn, &MOD_AIR, 435000000, 435000000, -6000, 6000, 0, 1, 0) < 0);
    CHECK_EQ(chk(&Pn, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, 1, 0), 0); /* ISM no lo exige */
    snprintf(Pn.callsign, sizeof Pn.callsign, "X");
    CHECK(chk(&Pn, &MOD_AIR, 145500000, 145500000, -6000, 6000, 0, 1, 0) < 0);  /* indicativo inválido */
    /* air_ok, flags, ganancia, duración, NaN */
    CHECK(chk(&P, &MOD_NOAIR, 433920000, 433920000, -6000, 6000, 0, 1, 0) < 0);
    tx_policy_t Pa = P;
    Pa.air = false;
    CHECK(chk(&Pa, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, 1, 0) < 0);
    Pa = P;
    Pa.enabled = false;
    CHECK(chk(&Pa, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 433920000, 433920000, -6000, 6000, 1, 1, 0) < 0);   /* 1 dB > 0 */
    Pa = P;
    Pa.max_gain_db = 10;
    CHECK_EQ(chk(&Pa, &MOD_AIR, 433920000, 433920000, -6000, 6000, 10, 1, 0), 0);
    CHECK(chk(&Pa, &MOD_AIR, 433920000, 433920000, -6000, 6000, 11, 1, 0) < 0);
    CHECK_EQ(chk(&P, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, 5.0, 0), 0);
    CHECK(chk(&P, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, 5.01, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 433920000, 433920000, -6000, 6000, 0, NAN, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 433920000, 433920000, NAN, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 433920000, 433920000, 6000, -6000, 0, 1, 0) < 0);
    /* ciclo de trabajo 868,0–868,6: 36 s/h */
    CHECK_EQ(chk(&P, &MOD_AIR, 868300000, 868300000, -6000, 6000, 0, 4.0, 32.0), 0);
    CHECK(chk(&P, &MOD_AIR, 868300000, 868300000, -6000, 6000, 0, 4.1, 32.0) < 0);
    CHECK_EQ(chk(&P, &MOD_AIR, 869500000, 869500000, -6000, 6000, 0, 4.0, 1000.0), 0); /* 869,4–869,65 sin límite */
    /* centro del hardware (fuga del LO) en otra banda, o imagen especular fuera */
    CHECK(chk(&P, &MOD_AIR, 433920000, 433000000, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 868300000, 433920000, -6000, 6000, 0, 1, 0) < 0);
    CHECK(chk(&P, &MOD_AIR, 433900000, 433060000, -6000, 6000, 0, 1, 0) < 0);   /* imagen en 432,2 MHz */
    CHECK_EQ(chk(&P, &MOD_AIR, 433900000, 433800000, -6000, 6000, 0, 1, 0), 0); /* imagen en 433,7 MHz: dentro */

    /* ---------------- tx_submit: rutas completas ---------------- */
    tx_t *t = make_tx(true, true, "EA1ABC", 0);
    CHECK_EQ(go(t, &MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 1, err), 0);
    CHECK_EQ(g_sink.nconf, 1);
    CHECK(!g_sink.loopback);
    CHECK_EQ(g_sink.freq, 433920000);
    CHECK_EQ(g_sink.nstop, 1);
    CHECK(g_sink.nwrite > 0);
    memset(&g_sink.nconf, 0, 3 * sizeof(int));
    /* cada rechazo: el hardware NO se toca */
    struct { const modulator_desc_t *m; tx_mode_t mode; int64_t f, c; uint32_t rate; int gain, rep; const char *why; } bad[] = {
        {&MOD_NOAIR, TX_AIR, 433920000, 0, 0, 0, 1, "air_ok=false"},
        {&MOD_AIR, TX_AIR, 121500000, 0, 0, 0, 1, "aviación"},
        {&MOD_AIR, TX_AIR, 406025000, 0, 0, 0, 1, "406 MHz"},
        {&MOD_AIR, TX_AIR, 433920000, 0, 0, 5, 1, "ganancia"},
        {&MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 51, "repeat largo (5,1 s)"},
        {&MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 1000, "repeat 1000"},
        {&MOD_AIR, TX_AIR, 433050500, 0, 0, 0, 1, "ancho se sale por abajo"},
        {&MOD_AIR, TX_AIR, 434789500, 0, 0, 0, 1, "ancho se sale por arriba"},
        {&MOD_AIR, TX_AIR, 433920000, 433000000, 2000000, 0, 1, "centro fuera de banda"},
        {&MOD_AIR, TX_AIR, 433900000, 433060000, 2000000, 0, 1, "imagen especular fuera"},
        {&MOD_AIR, TX_AIR, 433920000, 0, 1000, 0, 1, "tasa absurda"},
        {&MOD_AIR, TX_AIR, 433920000, 0, 4000000000u, 0, 1, "tasa gigante"},
        {&MOD_AIR, TX_AIR, -433920000, 0, 0, 0, 1, "frecuencia negativa"},
        {&MOD_AIR, (tx_mode_t)7, 433920000, 0, 0, 0, 1, "modo desconocido"},
        {&MOD_AIR, TX_AIR, 145500000 + 0, 0, 0, 0, 1, "ham con indicativo OK (se prueba abajo)"},
    };
    for (size_t i = 0; i + 1 < ARRAY_LEN(bad); i++) {
        int rc = go(t, bad[i].m, bad[i].mode, bad[i].f, bad[i].c, bad[i].rate, bad[i].gain, bad[i].rep, err);
        if (rc == 0)
            printf("  aceptado indebidamente: %s\n", bad[i].why);
        CHECK(rc < 0);
    }
    CHECK_EQ(g_sink.nconf, 0);
    CHECK_EQ(g_sink.nwrite, 0);
    /* repeat justo dentro del límite: 0,1 s × 49 = 4,9 s */
    CHECK_EQ(go(t, &MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 49, err), 0);
    CHECK_EQ(g_sink.nconf, 1);
    tx_destroy(t);

    /* política sin permisos */
    t = make_tx(false, false, "", 0);
    CHECK(go(t, &MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 1, err) < 0);
    CHECK(go(t, &MOD_AIR, TX_LOOPBACK, 433920000, 0, 0, 0, 1, err) < 0);
    CHECK_EQ(g_sink.nconf, 0);
    tx_destroy(t);
    t = make_tx(true, false, "", 0); /* --enable-tx sin --enable-tx-air */
    CHECK(go(t, &MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 1, err) < 0);
    CHECK_EQ(g_sink.nconf, 0);
    CHECK_EQ(go(t, &MOD_AIR, TX_LOOPBACK, 433920000, 0, 0, 60, 1, err), 0); /* gain 60 pedido: se ignora */
    CHECK(g_sink.loopback);
    CHECK_EQ(g_sink.gain, -20);
    tx_destroy(t);

    /* ham: sin indicativo no; con indicativo sí */
    t = make_tx(true, true, "", 0);
    CHECK(go(t, &MOD_AIR, TX_AIR, 145500000, 0, 0, 0, 1, err) < 0);
    CHECK_EQ(g_sink.nconf, 0);
    tx_destroy(t);
    t = make_tx(true, true, "EA1ABC", 0);
    CHECK_EQ(go(t, &MOD_AIR, TX_AIR, 145500000, 0, 0, 0, 1, err), 0);
    CHECK_EQ(g_sink.nconf, 1);
    tx_destroy(t);

    /* sin hardware (src NULL y sin sink) */
    {
        tx_policy_t p = {.enabled = true, .air = true};
        tx_t *t2 = tx_create(NULL, NULL, g_dir, &p);
        CHECK(go(t2, &MOD_AIR, TX_LOOPBACK, 433920000, 0, 0, 0, 1, err) < 0);
        CHECK(go(t2, &MOD_AIR, TX_AIR, 433920000, 0, 0, 0, 1, err) < 0);
        tx_destroy(t2);
    }

    /* modo file: siempre permitido, incluso con air_ok=false y sin permisos */
    t = make_tx(false, false, "", 0);
    CHECK_EQ(go(t, &MOD_NOAIR, TX_FILE, 121500000, 0, 0, 0, 2, err), 0);
    CHECK_EQ(g_sink.nconf, 0);
    CHECK(go(t, &MOD_NOAIR, TX_FILE, 121500000, 0, 0, 0, 1, err) == 0);
    /* fichero demasiado grande (repeat × señal larga) */
    g_dur = 5.0; /* a 1 Msps: 5e6 muestras × 1000 */
    CHECK(go(t, &MOD_NOAIR, TX_FILE, 121500000, 0, 0, 0, 1000, err) < 0);
    g_dur = 0.1;
    tx_destroy(t);

    /* ---------------- medida del ancho ocupado ---------------- */
    {
        tx_meta_t mt;
        iqbuf_t iq = {0};
        g_mode_short = 1; /* 300 muestras: antes ancho = 0 */
        CHECK_EQ(tx_render(&MOD_AIR, NULL, 868300000, 868300000, 1e6, &iq, &mt, err, sizeof err), 0);
        CHECK(mt.hi_hz > 380e3);
        iqbuf_free(&iq);
        t = make_tx(true, true, "", 0);
        CHECK(go(t, &MOD_AIR, TX_AIR, 868300000, 0, 1000000, 0, 1, err) < 0); /* 868,3 + 0,4 MHz > 868,6 */
        tx_destroy(t);
        g_mode_short = 0;
        g_mode_late = 1; /* componente ancha solo al final de una señal larga */
        CHECK_EQ(tx_render(&MOD_AIR, NULL, 868300000, 868300000, 1e6, &iq, &mt, err, sizeof err), 0);
        CHECK(mt.hi_hz > 380e3);
        iqbuf_free(&iq);
        t = make_tx(true, true, "", 0);
        CHECK(go(t, &MOD_AIR, TX_AIR, 868300000, 0, 1000000, 0, 1, err) < 0);
        tx_destroy(t);
        g_mode_late = 0;
        CHECK_EQ(tx_render(&MOD_AIR, NULL, 433920000, 433920000, 1e6, &iq, &mt, err, sizeof err), 0);
        CHECK(mt.occ_bw_hz > 3000 && mt.occ_bw_hz < 60000); /* CPFSK ±2,4 kHz */
        iqbuf_free(&iq);
    }

    /* ---------------- ciclo de trabajo 868,0–868,6, tabla llena y persistencia ---------------- */
    {
        t = make_tx(true, true, "", 0);
        g_dur = 0.05; /* 0,05 s por orden: 36 s = 720 órdenes (tabla de 64 → se funde) */
        int ok = 0;
        double secs = 0;
        for (int i = 0; i < 740; i++) {
            if (go(t, &MOD_AIR, TX_AIR, 868300000, 0, 0, 0, 1, err) == 0) {
                ok++;
                secs += 0.05;
            } else
                break;
        }
        CHECK(secs <= 36.0 + 1e-6);
        CHECK(ok >= 700 && ok <= 721);
        int nc = g_sink.nconf;
        CHECK(go(t, &MOD_AIR, TX_AIR, 868300000, 0, 0, 0, 1, err) < 0);
        CHECK_EQ(g_sink.nconf, nc);
        /* 869,5 MHz no está limitada */
        CHECK_EQ(go(t, &MOD_AIR, TX_AIR, 869500000, 0, 0, 0, 1, err), 0);
        tx_destroy(t);
        /* reiniciar el programa NO reinicia el ciclo de trabajo */
        t = make_tx(true, true, "", 0);
        CHECK(go(t, &MOD_AIR, TX_AIR, 868300000, 0, 0, 0, 1, err) < 0);
        CHECK_EQ(g_sink.nconf, 0);
        tx_destroy(t);
        char lp[200];
        snprintf(lp, sizeof lp, "%s/tx_log.jsonl", g_dir);
        CHECK(count_lines(lp) >= ok); /* registro de cada emisión */
        g_dur = 0.1;
    }

    /* ---------------- una sola emisión a la vez (carreras) ---------------- */
    {
        t = make_tx(true, false, "", 0);
        g_sink.delay_ms = 150;
        pthread_t th[4];
        race_t rr[4];
        for (int i = 0; i < 4; i++) {
            rr[i].t = t;
            pthread_create(&th[i], NULL, race_main, &rr[i]);
        }
        int acc = 0;
        for (int i = 0; i < 4; i++) {
            pthread_join(th[i], NULL);
            acc += rr[i].rc == 0;
        }
        CHECK_EQ(acc, 1);
        tx_wait(t);
        g_sink.delay_ms = 0;
        tx_destroy(t);
    }

    /* ---------------- tx_play ---------------- */
    {
        iqbuf_t iq = {0};
        g_mode_tone = 1;
        tx_meta_t mt;
        CHECK_EQ(tx_render(&MOD_NOAIR, NULL, 433920000, 433920000, 1e6, &iq, &mt, err, sizeof err), 0);
        g_mode_tone = 0;
        char pre[200];
        snprintf(pre, sizeof pre, "%s/grab1", g_dir);
        CHECK_EQ(tx_write_sigmf(pre, &iq, 1, 433920000, "prueba", err, sizeof err), 0);
        int n_in = iq.n;
        iqbuf_free(&iq);

        t = make_tx(true, true, "EA1ABC", 0);
        tx_play_req_t pr = {.mode = TX_LOOPBACK, .repeat = 1};
        snprintf(pr.name, sizeof pr.name, "grab1.sigmf-meta");
        CHECK_EQ(tx_play(t, &pr, err, sizeof err), 0);
        tx_wait(t);
        CHECK_EQ(g_sink.nconf, 1);
        CHECK(g_sink.loopback);
        CHECK_EQ(g_sink.gain, -20);
        CHECK_EQ(g_sink.freq, 433920000);
        CHECK_EQ(g_sink.rate, 1000000);
        CHECK(g_sink.rec_n >= n_in);
        memset(&g_sink.nconf, 0, 3 * sizeof(int));
        /* NUNCA por antena, aunque esté todo habilitado */
        pr.mode = TX_AIR;
        CHECK(tx_play(t, &pr, err, sizeof err) < 0);
        pr.mode = (tx_mode_t)9;
        CHECK(tx_play(t, &pr, err, sizeof err) < 0);
        CHECK_EQ(g_sink.nconf, 0);
        /* file: crea una copia */
        pr.mode = TX_FILE;
        CHECK_EQ(tx_play(t, &pr, err, sizeof err), 0);
        /* nombres peligrosos */
        const char *evil[] = {"../grab1", "/etc/passwd", ".grab1", "a/b", "grab1/../grab1", "", "grab1;rm", "..",
                              "/tmp/x.sigmf-meta"};
        pr.mode = TX_LOOPBACK;
        for (size_t i = 0; i < ARRAY_LEN(evil); i++) {
            snprintf(pr.name, sizeof pr.name, "%s", evil[i]);
            CHECK(tx_play(t, &pr, err, sizeof err) < 0);
        }
        /* enlace simbólico: no se sigue */
        char l1[300], l2[300];
        snprintf(l1, sizeof l1, "%s/grab1.sigmf-meta", g_dir);
        snprintf(l2, sizeof l2, "%s/enlace.sigmf-meta", g_dir);
        CHECK_EQ(symlink(l1, l2), 0);
        snprintf(l1, sizeof l1, "%s/grab1.sigmf-data", g_dir);
        snprintf(l2, sizeof l2, "%s/enlace.sigmf-data", g_dir);
        CHECK_EQ(symlink(l1, l2), 0);
        snprintf(pr.name, sizeof pr.name, "enlace");
        CHECK(tx_play(t, &pr, err, sizeof err) < 0);
        snprintf(pr.name, sizeof pr.name, "noexiste");
        CHECK(tx_play(t, &pr, err, sizeof err) < 0);
        CHECK_EQ(g_sink.nconf, 0);
        tx_destroy(t);
        /* loopback sin --enable-tx */
        t = make_tx(false, false, "", 0);
        snprintf(pr.name, sizeof pr.name, "grab1");
        CHECK(tx_play(t, &pr, err, sizeof err) < 0);
        CHECK_EQ(g_sink.nconf, 0);
        tx_destroy(t);
    }

    /* ---------------- calibración ---------------- */
    {
        /* (a) DSP puro: estimar q y c del modelo, con retardos fraccionarios distintos */
        double mr, mi, nr, ni;
        cs_iq_munu(TX_G, TX_PH * M_PI / 180, &mr, &mi, &nr, &ni);
        double qre = (nr * mr + ni * mi) / (mr * mr + mi * mi), qim = (ni * mr - nr * mi) / (mr * mr + mi * mi);
        double delays[] = {37.3, 12.0, 100.6, 5.5};
        for (size_t k = 0; k < ARRAY_LEN(delays); k++) {
            TX_DELAY = delays[k];
            iqbuf_t b;
            CHECK_EQ(cal_burst_make(&b, 1e6, 0.5), 0);
            int need = CAL_PRE_ZEROS + b.n + CAL_POST_ZEROS;
            int16_t *tq = calloc(2 * (size_t)need, sizeof(int16_t));
            for (int i = 0; i < b.n; i++) {
                tq[2 * (CAL_PRE_ZEROS + i)] = (int16_t)lrintf(b.s[i].re * 2047);
                tq[2 * (CAL_PRE_ZEROS + i) + 1] = (int16_t)lrintf(b.s[i].im * 2047);
            }
            cf32 *y = malloc(sizeof(cf32) * CAL_CAPTURE_LEN);
            tx_model(tq, need, y);
            cs_rng_t rg;
            cs_seed(&rg, 11 + k);
            cs_awgn(y, CAL_CAPTURE_LEN, 40.0, &rg);
            cal_t c;
            cal_meas_t m;
            CHECK_EQ(cal_estimate(y, CAL_CAPTURE_LEN, 0.5, &c, &m, err, sizeof err), 0);
            CHECK_NEAR(c.q_re, qre, 0.004);
            CHECK_NEAR(c.q_im, qim, 0.004);
            /* c' = c/μ: |c'| = |dc|/|μ| */
            double dcm = hypot(TX_DCI, TX_DCQ) / hypot(mr, mi);
            CHECK_NEAR(hypot(c.dc_re, c.dc_im), dcm, 0.004);
            CHECK_NEAR(m.delay, delays[k] + CAL_PRE_ZEROS, 0.2);
            CHECK(m.irr_db > 15 && m.irr_db < 30);
            free(tq);
            free(y);
            iqbuf_free(&b);
        }
        TX_DELAY = 37.3;
        /* sin señal → error (no se inventa una calibración) */
        {
            cf32 *z = calloc(CAL_CAPTURE_LEN, sizeof(cf32));
            cal_t c;
            CHECK(cal_estimate(z, CAL_CAPTURE_LEN, 0.5, &c, NULL, err, sizeof err) < 0);
            free(z);
        }
        /* precorrección: el tono de prueba mejora > 25 dB */
        {
            cal_t c = {.q_re = qre, .q_im = qim};
            int n = 100000;
            cf32 *x = malloc(sizeof(cf32) * n), *p = malloc(sizeof(cf32) * n);
            for (int i = 0; i < n; i++)
                x[i] = (cf32){(float)(0.5 * cos(2 * M_PI * 0.1 * i)), (float)(0.5 * sin(2 * M_PI * 0.1 * i))};
            memcpy(p, x, sizeof(cf32) * n);
            cs_iq_imbalance(x, n, TX_G, TX_PH, 0, 0);
            double before = cal_tone_irr_db(x, n, 0.1);
            memcpy(x, p, sizeof(cf32) * n);
            cal_apply(&c, x, n);
            cs_iq_imbalance(x, n, TX_G, TX_PH, 0, 0);
            double after = cal_tone_irr_db(x, n, 0.1);
            CHECK(after - before > 25.0);
            CHECK(after > 55.0);
            free(x);
            free(p);
        }

        /* (b) orden completa con el sink y el receptor simulados */
        t = make_tx(true, false, "", 0);
        CHECK(tx_cal_run(t, 0, 0, err, sizeof err) < 0); /* sin captura del receptor */
        tx_set_rx_capture(t, rx_hook, NULL);
        tx_destroy(t);
        t = make_tx(false, false, "", 0);
        tx_set_rx_capture(t, rx_hook, NULL);
        CHECK(tx_cal_run(t, 0, 0, err, sizeof err) < 0); /* sin --enable-tx */
        CHECK_EQ(g_sink.nconf, 0);
        tx_destroy(t);

        t = make_tx(true, false, "", 0);
        tx_set_rx_capture(t, rx_hook, NULL);
        CHECK_EQ(tx_cal_run(t, 433920000, 1000000, err, sizeof err), 0);
        tx_wait(t);
        CHECK(g_sink.loopback); /* la calibración solo usa loopback, con ganancia mínima */
        CHECK_EQ(g_sink.gain, -20);
        cal_t cc;
        int64_t ccen;
        uint32_t crate;
        CHECK(tx_cal_get(t, &cc, &ccen, &crate));
        CHECK_EQ(ccen, 433920000);
        CHECK_EQ(crate, 1000000);
        CHECK_NEAR(cc.q_re, qre, 0.005);

        /* emisión loopback de un tono: con y sin corrección */
        g_mode_tone = 1;
        CHECK_EQ(go(t, &MOD_AIR, TX_LOOPBACK, 433920000, 433920000, 1000000, 0, 1, err), 0);
        g_mode_tone = 0;
        int n = 100000;
        cf32 *yy = malloc(sizeof(cf32) * n);
        for (int i = 0; i < n; i++)
            yy[i] = (cf32){g_sink.rec[2 * i] / 2048.0f, g_sink.rec[2 * i + 1] / 2048.0f};
        cs_iq_imbalance(yy, n, TX_G, TX_PH, 0, 0);
        double with = cal_tone_irr_db(yy, n, 0.1);
        tx_cal_clear(t);
        g_mode_tone = 1;
        CHECK_EQ(go(t, &MOD_AIR, TX_LOOPBACK, 433920000, 433920000, 1000000, 0, 1, err), 0);
        g_mode_tone = 0;
        for (int i = 0; i < n; i++)
            yy[i] = (cf32){g_sink.rec[2 * i] / 2048.0f, g_sink.rec[2 * i + 1] / 2048.0f};
        cs_iq_imbalance(yy, n, TX_G, TX_PH, 0, 0);
        double without = cal_tone_irr_db(yy, n, 0.1);
        printf("  rechazo de imagen del TX simulado: %.1f dB sin calibrar, %.1f dB calibrado\n", without, with);
        CHECK(with - without > 25.0);
        free(yy);
        tx_destroy(t);
    }

    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (strncmp(g_dir, "/tmp/tx_test_", 13) == 0)
        (void)!system(cmd);
    free(g_sink.rec);
    return TEST_SUMMARY();
}
