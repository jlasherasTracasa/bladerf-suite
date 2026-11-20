/*
 * bench_decoders.c — Presupuesto de CPU por decodificador (AGENTS.md §4.1).
 *
 * Por qué: un decodificador corre en su propio hilo y debe seguir la tasa de
 * muestras que le llega en tiempo real con holgura. Si gasta más del 30 % de un
 * núcleo a su tasa, al añadir varias instancias el host se satura y se
 * descartan bloques. Coste = segundos de CPU del hilo / segundos de señal; el
 * tiempo se mide con CLOCK_THREAD_CPUTIME_ID (solo CPU del hilo, sin esperas).
 *
 * Señal: la de ejemplo del modulador (modulate(NULL)), repetida hasta 10 s y
 * con ruido blanco gaussiano a 10 dB de SNR (chan_awgn). Generar la señal y el
 * ruido queda fuera del cronómetro: solo se mide run_narrow()/run_wide(), que
 * hacen create + process en bloques (4096 / 65536 muestras) + destroy.
 *
 * Tasas: NARROW se alimenta a d->rate; WIDE a d->wide_rate (o a d->wide_min_rate,
 * o a la tasa del modulador si el descriptor no exige ninguna). Si la tasa del
 * modulador no coincide, se remuestrea con la interpolación cúbica de
 * chan_resample (sin filtro antialias: en ese caso la señal es algo peor de lo
 * real y la cifra es orientativa).
 *
 * Uso: bench_decoders [id]     sin id mide todos los decodificadores con modulador.
 * Sale con 1 si alguno supera el 30 % de un núcleo.
 */
#include "common.h"
#include "decoders/decoder.h"
#include "tx/mod.h"
#include "test_util.h"

#include <time.h>

#define BENCH_SECONDS 10.0
#define BENCH_SNR_DB 10.0
#define BUDGET_PCT 30.0

typedef enum { ST_OK, ST_EXCEDE, ST_SIN_MEDIR } st_t;

typedef struct {
    double rate;     /* tasa a la que se alimentó el decodificador */
    double cost_pct; /* % de un núcleo a su tasa */
    int events;
    st_t st;
    char note[256];  /* motivo (sin medir) o aviso (remuestreo) */
} row_t;

static double cpu_thread_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* Señal de ejemplo del modulador llevada a 'target' Hz (target <= 0: tasa nativa).
 * Devuelve un búfer malloc'ed de n muestras; NULL con err si el modulador falla. */
static cf32 *make_tile(const modulator_desc_t *md, double target, int *n_out, double *rate_out,
                       bool *resampled, char *err, size_t errlen)
{
    iqbuf_t b = {0};
    if (md->modulate(NULL, &b, err, errlen) < 0 || b.n <= 0) {
        if (!err[0])
            snprintf(err, errlen, "modulate devolvió una señal vacía");
        iqbuf_free(&b);
        return NULL;
    }
    if (target <= 0 || fabs(b.rate - target) < 0.5) {
        cf32 *s = b.s;
        *n_out = b.n;
        *rate_out = b.rate;
        *resampled = false;
        b.s = NULL;
        iqbuf_free(&b);
        return s;
    }
    /* Remuestreo: chan_resample escribe (n-4)·ratio muestras. */
    double ratio = target / b.rate;
    cf32 *out = malloc(sizeof(cf32) * ((size_t)((double)b.n * ratio) + 16));
    if (!out) {
        iqbuf_free(&b);
        snprintf(err, errlen, "sin memoria para remuestrear");
        return NULL;
    }
    int m = chan_resample(b.s, b.n, ratio, out);
    iqbuf_free(&b);
    *n_out = m;
    *rate_out = target;
    *resampled = true;
    return out;
}

static void measure(const decoder_desc_t *d, row_t *r)
{
    memset(r, 0, sizeof *r);
    r->rate = d->kind == DEC_NARROW ? d->rate : (d->wide_rate ? (double)d->wide_rate : (double)d->wide_min_rate);

    const modulator_desc_t *md = modulator_find(d->id);
    if (!md) {
        r->st = ST_SIN_MEDIR;
        snprintf(r->note, sizeof r->note, "sin modulador en MODULATORS[]");
        return;
    }

    char err[200] = "";
    int tn = 0;
    double trate = 0;
    bool resampled = false;
    cf32 *tile = make_tile(md, r->rate, &tn, &trate, &resampled, err, sizeof err);
    if (!tile) {
        r->st = ST_SIN_MEDIR;
        snprintf(r->note, sizeof r->note, "modulate falló: %s", err);
        return;
    }
    r->rate = trate;
    if (resampled)
        snprintf(r->note, sizeof r->note, "remuestreo del modulador a %.0f Hz (sin antialias)", trate);

    /* Pre-comprobación de create (fuera del cronómetro) para no medir un fallo. */
    int64_t freq = d->freqs[0] ? d->freqs[0] : 100000000;
    {
        dec_cfg_t cfg = {.rate = trate, .freq_hz = freq, .center_hz = freq};
        void *st = d->create(&cfg, err, sizeof err);
        if (!st) {
            free(tile);
            r->st = ST_SIN_MEDIR;
            snprintf(r->note, sizeof r->note, "create falló: %s", err);
            return;
        }
        d->destroy(st);
    }

    /* 10 s de señal: el ejemplo repetido, con ruido a 10 dB. */
    size_t total = (size_t)llround(BENCH_SECONDS * trate);
    cf32 *sig = malloc(sizeof(cf32) * total);
    if (!sig) {
        free(tile);
        r->st = ST_SIN_MEDIR;
        snprintf(r->note, sizeof r->note, "sin memoria para 10 s de señal");
        return;
    }
    for (size_t i = 0, j = 0; i < total; i++) {
        sig[i] = tile[j];
        if (++j == (size_t)tn)
            j = 0;
    }
    free(tile);
    uint32_t seed = 0x2545F491u;
    chan_awgn(sig, (int)total, BENCH_SNR_DB, &seed);

    evs_t ev = {0};
    double secs = (double)total / trate;
    double c0 = 0, c1 = 0;

    if (d->kind == DEC_NARROW) {
        c0 = cpu_thread_s();
        run_narrow(d, sig, (int)total, freq, NULL, &ev);
        c1 = cpu_thread_s();
        free(sig);
    } else {
        /* Conversión a Q11 fuera del cronómetro: el decodificador recibe Q11 crudo. */
        int16_t *q = malloc(sizeof(int16_t) * 2 * total);
        if (!q) {
            free(sig);
            r->st = ST_SIN_MEDIR;
            snprintf(r->note, sizeof r->note, "sin memoria para Q11");
            return;
        }
        cf32_to_q11(sig, (int)total, 1.0, q);
        free(sig);
        c0 = cpu_thread_s();
        run_wide(d, q, (int)total, trate, freq, NULL, &ev);
        c1 = cpu_thread_s();
        free(q);
    }

    r->events = evs_count(&ev);
    evs_free(&ev);
    r->cost_pct = 100.0 * (c1 - c0) / secs;
    r->st = r->cost_pct > BUDGET_PCT ? ST_EXCEDE : ST_OK;
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    (void)tu_checks; /* test_util.h define contadores que este medidor no usa */
    (void)tu_fails;
    if (only && !decoder_find(only)) {
        fprintf(stderr, "decodificador desconocido: %s\n", only);
        return 2;
    }

    printf("# bench_decoders: %.0f s de señal, SNR %.0f dB, presupuesto %.0f %% de un núcleo\n", BENCH_SECONDS,
           BENCH_SNR_DB, BUDGET_PCT);
    printf("%-10s %-7s %12s %9s %9s  %s\n", "id", "tipo", "tasa_Hz", "coste_%", "eventos", "estado");

    int exceed = 0;
    for (int i = 0; DECODERS[i]; i++) {
        const decoder_desc_t *d = DECODERS[i];
        if (only && strcmp(only, d->id) != 0)
            continue;
        fprintf(stderr, "midiendo %s...\n", d->id);
        row_t r;
        measure(d, &r);
        const char *tipo = d->kind == DEC_NARROW ? "narrow" : "wide";
        if (r.st == ST_SIN_MEDIR) {
            printf("%-10s %-7s %12.0f %9s %9s  sin medir: %s\n", d->id, tipo, r.rate, "-", "-", r.note);
            continue;
        }
        if (r.st == ST_EXCEDE)
            exceed++;
        printf("%-10s %-7s %12.0f %9.2f %9d  %s\n", d->id, tipo, r.rate, r.cost_pct, r.events,
               r.st == ST_EXCEDE ? "EXCEDE" : "OK");
        if (r.note[0])
            printf("           nota: %s\n", r.note);
    }

    printf("\nResumen: %d decodificadores exceden el %.0f %% de un núcleo\n", exceed, BUDGET_PCT);
    return exceed ? 1 : 0;
}
