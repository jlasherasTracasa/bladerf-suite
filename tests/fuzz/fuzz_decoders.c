/*
 * fuzz_decoders.c — Cada decodificador de DECODERS[] con entradas hostiles.
 *
 * Para cada decodificador (narrow y wide) se prepara, al arrancar, una señal
 * válida con su modulador (tx_render a la tasa que espera el decodificador; la
 * wide en Q11 crudo como la entrega el hardware). Después, en cada iteración y
 * por turnos entre todos:
 *   - ruido gaussiano puro, ruido con amplitud enorme (saturación), ceros, DC y
 *     un tono, de longitud aleatoria;
 *   - la señal válida corrompida: inversión de fase de tramos (equivale a bits
 *     volteados en PSK/FSK), ráfagas de ruido, ceros, picos ×1000, desvío de
 *     frecuencia, duplicado o borrado de trozos (deslizamiento de reloj),
 *     truncado a mitad de trama y mezcla con ruido antes y después;
 *   - troceado en bloques de tamaño 0, 1, impar y de decenas de miles;
 *   - create() con opciones JSON hostiles (basura, tipos equivocados, enormes) y,
 *     en los wide, con tasas incompatibles (0, 1 Hz, 61,44 MHz…): debe devolver
 *     NULL con texto en err o funcionar, nunca fallar;
 *   - set_location con latitudes absurdas y state_json a cada bloque.
 * Se comprueba que cada evento emitido sea un objeto JSON válido con "text" (contrato
 * de decoder.h §4.1) y que state_json devuelva JSON válido.
 *
 * Uso: fuzz_decoders [--seconds N] [--seed S] [--only id]
 * Casos de regresión (tests/fuzz/casos/decoders/): primera línea = id del
 * decodificador, a continuación int16 little-endian intercalados I,Q (Q11); el
 * arnés los entrega como un solo bloque (narrow: normalizados a ±1; wide: crudos).
 */
#include "decoders/decoder.h"
#include "fuzz_util.h"
#include "tx/mod.h"
#include "tx/tx.h"
#include "util/json.h"

#include <assert.h>
#include <math.h>

typedef struct {
    const decoder_desc_t *d;
    cf32 *good;       /* señal válida (narrow) */
    int16_t *good_q;  /* señal válida (wide, Q11) */
    int ngood;
    double rate;
    int64_t freq, center;
    unsigned long runs, events, creates_failed;
    double secs;
} slot_t;

static unsigned long g_bad_events;
static jtok_t *g_tok;
#define NTOK 262144

typedef struct {
    unsigned long n;
    const char *id;
} sink_t;

static void check_json(const char *id, const char *what, const char *js, size_t len)
{
    int n = json_parse(js, len, g_tok, NTOK);
    int tx = n > 0 && g_tok[0].type == J_OBJ ? json_obj_get(js, g_tok, 0, "text") : -1;
    if (n < 1 || g_tok[0].type != J_OBJ || (!strcmp(what, "evento") && (tx < 0 || g_tok[tx].type != J_STR))) {
        fprintf(stderr, "[fuzz:decoders] %s: %s no válido (len %zu): %.200s\n", id, what, len, js);
        g_bad_events++;
        abort();
    }
}

static void on_emit(void *ctx, const char *json, size_t len)
{
    sink_t *s = ctx;
    s->n++;
    check_json(s->id, "evento", json, len);
}

/* ---- señales válidas -------------------------------------------------------- */
static void prepare(slot_t *s)
{
    const decoder_desc_t *d = s->d;
    s->rate = d->kind == DEC_WIDE ? (d->wide_rate ? d->wide_rate : d->wide_min_rate) : d->rate;
    s->freq = d->freqs[0] ? d->freqs[0] : 100000000;
    s->center = d->kind == DEC_WIDE ? s->freq : s->freq - (int64_t)(s->rate / 4);
    const modulator_desc_t *m = modulator_find(d->id);
    if (!m)
        return;
    iqbuf_t iq = {0};
    tx_meta_t mt;
    char err[300] = "";
    s->center = s->freq; /* la señal se renderiza centrada en su frecuencia (ya a 0 Hz) */
    if (d->kind == DEC_WIDE)
        s->center = s->freq;
    if (tx_render(m, NULL, s->freq, s->center, s->rate, &iq, &mt, err, sizeof err) < 0 || iq.n <= 0) {
        fprintf(stderr, "[fuzz:decoders] %s: sin señal válida (%s); solo ruido y señal desplazada\n", d->id, err);
        iqbuf_free(&iq);
        return;
    }
    int n = iq.n > 3000000 ? 3000000 : iq.n;
    s->ngood = n;
    if (d->kind == DEC_WIDE) {
        s->good_q = malloc(sizeof(int16_t) * 2 * (size_t)n);
        for (int i = 0; i < n; i++) {
            s->good_q[2 * i] = (int16_t)lrintf(fmaxf(-2048, fminf(2047, iq.s[i].re * 2048.f)));
            s->good_q[2 * i + 1] = (int16_t)lrintf(fmaxf(-2048, fminf(2047, iq.s[i].im * 2048.f)));
        }
    } else {
        /* el decodificador narrow recibe la señal ya a 0 Hz (como tras el DDC) */
        s->good = malloc(sizeof(cf32) * (size_t)n);
        for (int i = 0; i < n; i++)
            s->good[i] = iq.s[i];
    }
    iqbuf_free(&iq);
}

/* ---- corrupción -------------------------------------------------------------- */
static double gauss(fz_rng_t *r)
{
    double u = fz_unit(r) + 1e-12, v = fz_unit(r);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* Genera hasta cap muestras complejas en out. Devuelve cuántas. */
static int make_signal(fz_rng_t *r, const slot_t *s, cf32 *out, int cap)
{
    int n;
    bool have = s->good || s->good_q;
    uint32_t mode = have ? fz_below(r, 10) : fz_below(r, 3);
    if (mode >= 3) { /* señal válida + corrupción */
        n = s->ngood;
        int skip = fz_below(r, 2) ? 0 : (int)fz_below(r, (uint32_t)n);
        n -= skip;
        if (n > cap)
            n = cap;
        for (int i = 0; i < n; i++) {
            if (s->good)
                out[i] = s->good[skip + i];
            else
                out[i] = (cf32){s->good_q[2 * (size_t)(skip + i)] / 2048.f, s->good_q[2 * (size_t)(skip + i) + 1] / 2048.f};
        }
        int ops = 1 + (int)fz_below(r, 4);
        for (int k = 0; k < ops && n > 8; k++) {
            int a = (int)fz_below(r, (uint32_t)n), l = 1 + (int)fz_below(r, (uint32_t)(n - a) / (fz_below(r, 3) ? 16 : 1) + 1);
            if (l > n - a)
                l = n - a;
            switch (fz_below(r, 9)) {
            case 0: /* inversión de fase */
                for (int i = a; i < a + l; i++)
                    out[i] = (cf32){-out[i].re, -out[i].im};
                break;
            case 1: { /* ráfaga de ruido */
                double sg = fz_unit(r) * 2;
                for (int i = a; i < a + l; i++) {
                    out[i].re += (float)(sg * gauss(r));
                    out[i].im += (float)(sg * gauss(r));
                }
                break;
            }
            case 2: /* ceros */
                memset(out + a, 0, sizeof(cf32) * (size_t)l);
                break;
            case 3: /* pico ×1000 */
                for (int i = a; i < a + l; i++)
                    out[i] = (cf32){out[i].re * 1000.f, out[i].im * 1000.f};
                break;
            case 4: { /* desvío de frecuencia */
                double w = (fz_unit(r) - 0.5) * 2 * M_PI * 0.3;
                for (int i = a; i < a + l; i++) {
                    float c = (float)cos(w * (i - a)), sn = (float)sin(w * (i - a));
                    out[i] = (cf32){out[i].re * c - out[i].im * sn, out[i].re * sn + out[i].im * c};
                }
                break;
            }
            case 5: /* borrar trozo (adelanto de reloj) */
                memmove(out + a, out + a + l, sizeof(cf32) * (size_t)(n - a - l));
                n -= l;
                break;
            case 6: /* duplicar trozo (retraso de reloj) */
                if (n + l <= cap) {
                    memmove(out + a + l, out + a, sizeof(cf32) * (size_t)(n - a));
                    n += l;
                }
                break;
            case 7: /* truncar a mitad de trama */
                n = a + 1;
                break;
            default: /* saturación */
                for (int i = a; i < a + l; i++)
                    out[i] = (cf32){out[i].re > 0 ? 1.f : -1.f, out[i].im > 0 ? 1.f : -1.f};
            }
        }
        return n;
    }
    /* ruido y señales degeneradas */
    uint32_t big = fz_below(r, 4);
    n = (int)(big == 0 ? fz_below(r, 4) : fz_below(r, 2) ? fz_below(r, 4000) : fz_below(r, 400000));
    if (n > cap)
        n = cap;
    uint32_t kind = fz_below(r, 5);
    double sg = fz_below(r, 4) ? 0.1 + fz_unit(r) : 100.0;
    for (int i = 0; i < n; i++) {
        switch (kind) {
        case 0:
        case 1: out[i] = (cf32){(float)(sg * gauss(r)), (float)(sg * gauss(r))}; break;
        case 2: out[i] = (cf32){0, 0}; break;
        case 3: out[i] = (cf32){0.7f, -0.3f}; break; /* DC puro */
        default: out[i] = (cf32){(float)cos(0.3 * i), (float)sin(0.3 * i)}; /* tono */
        }
    }
    return n;
}

static void to_q11(const cf32 *x, int n, int16_t *q)
{
    for (int i = 0; i < n; i++) {
        q[2 * i] = (int16_t)lrintf(fmaxf(-2048, fminf(2047, x[i].re * 2048.f)));
        q[2 * i + 1] = (int16_t)lrintf(fmaxf(-2048, fminf(2047, x[i].im * 2048.f)));
    }
}

/* ---- ejecución de un decodificador ----------------------------------------- */
static const char *OPTS[] = {NULL, "{}", "", "{", "null", "[]", "{\"a\":1}", "{\"sf\":99,\"bw\":-1}",
                             "{\"sf\":7,\"bw\":125000}", "{\"rate\":0,\"baud\":0}", "{\"key\":\"\\u\"}",
                             "{\"callsign\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}",
                             "{\"x\":{\"y\":{\"z\":[[[[1]]]]}}}", "{\"sf\":1e999}", "{\"bits\":-5,\"fft\":4294967296}"};

static void run_one(slot_t *s, fz_rng_t *r, cf32 *buf, int16_t *qbuf, int n, bool hostile_cfg, const char *opts_over)
{
    const decoder_desc_t *d = s->d;
    char err[300] = "";
    double rate = s->rate;
    int64_t freq = s->freq, center = s->center;
    if (hostile_cfg) {
        /* Dentro del contrato del host (decoder.h §4.1): un narrow recibe SIEMPRE su tasa
         * exacta (la fija el DDC; con otra tasa, p. ej. 1 Hz, los diseños de filtro
         * degeneran y el coste explota: no es una entrada que pueda darse). Un wide recibe
         * su tasa exacta o, si wide_rate == 0, cualquiera ≥ wide_min_rate hasta 122,88 MHz.
         * La frecuencia y el centro sí pueden ser cualesquiera del rango del hardware. */
        if (d->kind == DEC_WIDE && !d->wide_rate) {
            double lo = d->wide_min_rate ? d->wide_min_rate : 1e6;
            rate = fz_below(r, 2) ? lo : lo * (1 + fz_unit(r) * (122.88e6 / lo - 1));
            if (rate > 122.88e6)
                rate = 122.88e6;
            if (rate * 2 > 122.88e6 || rate > 30e6)
                rate = lo; /* el coste del arnés crece con la tasa */
        }
        static const int64_t F[] = {0, 1, 6000000000LL, 100000000, 47000000, 1090000000, 2437000000LL};
        freq = F[fz_below(r, 7)];
        center = d->kind == DEC_WIDE ? freq : F[fz_below(r, 7)];
    }
    const char *opts = opts_over ? opts_over : (fz_below(r, 4) ? NULL : OPTS[fz_below(r, sizeof OPTS / sizeof *OPTS)]);
    dec_cfg_t cfg = {.rate = rate, .freq_hz = freq, .center_hz = center, .loc_valid = fz_below(r, 2), .lat = 40.4,
                     .lon = -3.7, .opts = opts};
    snprintf(fz_note, sizeof fz_note, "decoder=%s rate=%.0f freq=%lld center=%lld opts=%s n=%d hostil=%d", d->id, rate,
             (long long)freq, (long long)center, opts ? opts : "(null)", n, hostile_cfg);
    void *st = d->create(&cfg, err, sizeof err);
    if (!st) {
        s->creates_failed++;
        return;
    }
    sink_t sk = {0, d->id};
    int off = 0;
    double now = 1.7e9;
    int blocks = 0;
    while (off < n || (blocks == 0 && n == 0)) {
        uint32_t k = fz_below(r, 8);
        int c = k == 0 ? 0 : k == 1 ? 1 : k == 2 ? 3 + 2 * (int)fz_below(r, 500) : k < 5 ? (int)fz_below(r, 4096) : (int)fz_below(r, 70000);
        if (c > n - off)
            c = n - off;
        if (d->kind == DEC_WIDE) {
            to_q11(buf + off, c, qbuf);
            if (d->process_raw)
                d->process_raw(st, qbuf, c, now, on_emit, &sk);
        } else if (d->process) {
            d->process(st, buf + off, c, now, on_emit, &sk);
        }
        off += c;
        now += c / (rate > 0 ? rate : 1);
        blocks++;
        if (d->state_json && fz_below(r, 3) == 0) {
            sb_t sb = {0};
            d->state_json(st, &sb, now);
            if (sb.data)
                check_json(d->id, "state_json", sb.data, sb.len);
            sb_free(&sb);
        }
        if (d->set_location && fz_below(r, 10) == 0) {
            static const double L[] = {0, 90, -90, 91, 1e300, -1e300, 180, 400};
            d->set_location(st, fz_below(r, 2), L[fz_below(r, 8)], L[fz_below(r, 8)]);
        }
        if (c == 0 && n == 0)
            break;
    }
    if (d->state_json) {
        sb_t sb = {0};
        d->state_json(st, &sb, now + 1000);
        if (sb.data)
            check_json(d->id, "state_json", sb.data, sb.len);
        sb_free(&sb);
    }
    d->destroy(st);
    s->events += sk.n;
}

static void regression_case(const uint8_t *b, size_t l, void *ctx)
{
    slot_t *slots = ctx;
    const uint8_t *nl = memchr(b, '\n', l);
    if (!nl)
        return;
    char id[32];
    size_t il = (size_t)(nl - b);
    if (il >= sizeof id)
        return;
    memcpy(id, b, il);
    id[il] = 0;
    const decoder_desc_t *d = decoder_find(id);
    if (!d) {
        fprintf(stderr, "caso de regresión con decodificador desconocido '%s'\n", id);
        abort();
    }
    slot_t *s = NULL;
    for (int i = 0; slots[i].d; i++)
        if (slots[i].d == d)
            s = &slots[i];
    if (!s)
        return; /* --only: este caso es de otro decodificador */
    size_t nb = l - il - 1;
    int n = (int)(nb / 4);
    cf32 *x = malloc(sizeof(cf32) * (size_t)(n + 1));
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)(n + 1));
    const int16_t *src = (const int16_t *)(nl + 1);
    for (int i = 0; i < n; i++) {
        int16_t a, c;
        memcpy(&a, (const uint8_t *)src + 4 * i, 2);
        memcpy(&c, (const uint8_t *)src + 4 * i + 2, 2);
        x[i] = (cf32){a / 2048.f, c / 2048.f};
    }
    fz_rng_t r = {12345};
    /* un solo bloque, con y sin cfg hostil */
    run_one(s, &r, x, q, n, false, NULL);
    run_one(s, &r, x, q, n, false, "{}");
    free(x);
    free(q);
}

int main(int argc, char **argv)
{
    fz_t z;
    fz_init(&z, "decoders", argc, argv, 60);
    const char *only = NULL;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--only"))
            only = argv[i + 1];
    g_tok = malloc(sizeof(jtok_t) * NTOK);
    int nd = 0;
    while (DECODERS[nd])
        nd++;
    slot_t *slots = calloc((size_t)nd + 1, sizeof *slots);
    int ns = 0;
    for (int i = 0; i < nd; i++) {
        if (only && strcmp(only, DECODERS[i]->id))
            continue;
        slots[ns].d = DECODERS[i];
        double t0 = fz_now();
        prepare(&slots[ns]);
        printf("[fuzz:decoders] %-10s %s tasa %.0f, señal válida %d muestras (%.1f s de preparación)\n", DECODERS[i]->id,
               DECODERS[i]->kind == DEC_WIDE ? "wide  " : "narrow", slots[ns].rate, slots[ns].ngood, fz_now() - t0);
        ns++;
    }
    fz_run_cases("decoders", regression_case, slots);

    const int CAP = 4000000;
    cf32 *buf = malloc(sizeof(cf32) * (size_t)CAP);
    int16_t *qbuf = malloc(sizeof(int16_t) * 2 * (size_t)CAP);
    unsigned long it = 0;
    /* El tiempo se reparte a partes iguales entre decodificadores: se cuenta el gastado en cada uno */
    while (fz_alive(&z)) {
        slot_t *s = &slots[0];
        for (int i = 1; i < ns; i++)
            if (slots[i].secs < s->secs)
                s = &slots[i];
        double t0 = fz_now();
        int cap = s->d->kind == DEC_WIDE ? 600000 : CAP;
        int n = make_signal(&z.rng, s, buf, cap);
        fz_current(buf, sizeof(cf32) * (size_t)n); /* en un fallo queda volcado el cf32 que lo provocó */
        run_one(s, &z.rng, buf, qbuf, n, fz_below(&z.rng, 8) == 0, NULL);
        s->runs++;
        s->secs += fz_now() - t0;
        it++;
    }
    printf("[fuzz:decoders] %lu iteraciones\n", it);
    for (int i = 0; i < ns; i++)
        printf("  %-10s ejecuciones %6lu  eventos %6lu  create fallido %5lu  tiempo %.1f s\n", slots[i].d->id, slots[i].runs,
               slots[i].events, slots[i].creates_failed, slots[i].secs);
    for (int i = 0; i < ns; i++) {
        free(slots[i].good);
        free(slots[i].good_q);
    }
    free(buf);
    free(qbuf);
    free(slots);
    free(g_tok);
    printf("[fuzz:decoders] sin fallos\n");
    return 0;
}
