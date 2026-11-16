/*
 * sweep.c — Barrido rápido con quick-tune. La teoría está en sweep.h.
 */
#include "sweep.h"

#include "../dsp/fft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QAHEAD 8              /* saltos con el retune ya encolado por delante de la lectura */
#define USEFUL 0.70           /* fracción de fs que se aprovecha de cada salto */
#define EDGE_LIMIT 0.42       /* más allá de ±42 % de fs el filtro del RFIC ya no vale */
#define MAX_BINS (1 << 22)
#define PARTIAL_PERIOD 0.25   /* s entre tramas parciales */

typedef struct {
    int64_t center;
    int off;     /* primer bin de la trama que cubre este salto */
    int count;   /* bins útiles */
    int k0;      /* primer bin útil dentro de la FFT (con fftshift) */
    int qt;      /* perfil de sintonía rápida */
} hop_t;

struct sweep {
    sweep_cfg_t cfg;
    sweep_io_t io;
    uint32_t rate;
    bool quick;
    int n;               /* FFT */
    int K;               /* bins útiles por salto */
    double bin_hz;
    int nh;
    hop_t *hop;
    int nbins;
    int64_t start_hz;    /* frecuencia del bin 0 */
    unsigned settle, dwell;

    fft_plan_t *plan;
    float *win;
    double norm_db;
    cf32 *work;
    float *db;
    int16_t *buf;
    unsigned cap;        /* capacidad de buf en muestras */

    /* estado de adquisición */
    bool started, synced, pos_valid, disc;
    uint64_t pos;        /* marca de tiempo de la próxima muestra a leer */
    uint64_t t0;         /* instante del retune del salto 0 de esta sincronía */
    uint64_t g, g_sched; /* contador de saltos (continuo) y primer salto sin encolar */
    unsigned bad;        /* re-sincronizaciones seguidas */
    uint64_t sweeps, restarts;
    double last_emit;
};

/* ======================================================================= */
/*  Planificación                                                          */
/* ======================================================================= */
static const uint32_t RATES[] = {10000000, 20000000, 30720000, 40000000, 56000000, 61440000};

/* Rellena la geometría del barrido para la tasa dada. Devuelve nº de saltos. */
static int build_hops(sweep_t *w, uint32_t rate)
{
    const sweep_cfg_t *c = &w->cfg;
    w->rate = rate;
    w->bin_hz = (double)rate / w->n;
    w->K = (int)(USEFUL * w->n) & ~1;
    double step = w->K * w->bin_hz;
    double span = (double)(c->stop_hz - c->start_hz);
    int nh = (int)ceil(span / step);
    if (nh < 1)
        nh = 1;
    free(w->hop);
    w->hop = calloc((size_t)nh, sizeof(hop_t));
    if (!w->hop)
        return -1;
    int off = 0, kept = 0;
    const int ilimit = w->n / 2 + (int)(EDGE_LIMIT * w->n);
    for (int h = 0; h < nh; h++) {
        double ideal = (double)c->start_hz + ((double)h * w->K + w->K / 2.0) * w->bin_hz;
        int64_t center = llround(ideal);
        int m = 0;
        if (center > c->fmax_hz) /* desplazamiento entero de bins: sigue en la rejilla */
            m = (int)ceil((double)(center - c->fmax_hz) / w->bin_hz);
        if (m > 0)
            center = llround(ideal - m * w->bin_hz);
        hop_t *hp = &w->hop[kept];
        hp->center = center;
        hp->k0 = w->n / 2 - w->K / 2 + m;
        hp->count = MIN(w->K, ilimit - hp->k0);
        if (hp->count <= 0)
            continue; /* este tramo queda fuera de lo sintonizable */
        hp->off = off;
        off += hp->count;
        kept++;
    }
    /* La trama cubre [inicio, fin]: lo que sobre del último salto se recorta. */
    const int maxbins = (int)ceil(span / w->bin_hz);
    while (kept > 1 && w->hop[kept - 1].off >= maxbins)
        kept--;
    if (off > maxbins) {
        off = maxbins;
        hop_t *last = &w->hop[kept - 1];
        last->count = off - last->off;
    }
    w->nh = kept;
    w->nbins = off;
    w->start_hz = c->start_hz;
    return kept;
}

static int plan(sweep_t *w, char *err, size_t errlen)
{
    sweep_cfg_t *c = &w->cfg;
    if (c->start_hz < c->fmin_hz)
        c->start_hz = c->fmin_hz;
    if (c->stop_hz > c->fmax_hz)
        c->stop_hz = c->fmax_hz;
    if (c->stop_hz <= c->start_hz) {
        snprintf(err, errlen, "barrido: rango vacío (el equipo sintoniza %lld–%lld Hz)", (long long)c->fmin_hz,
                 (long long)c->fmax_hz);
        return -1;
    }
    w->quick = c->allow_quick && w->io.qt_free && w->io.qt_free(w->io.ctx) > 4;
    const int budget = w->quick ? w->io.qt_free(w->io.ctx) - 2 : 0;
    const uint32_t rmax = RATES[ARRAY_LEN(RATES) - 1];
    int nh;
    if (c->rate) {
        nh = build_hops(w, c->rate);
    } else if (w->quick) {
        /* Modo rápido: la tasa más baja con la que caben los perfiles de sintonía. */
        nh = 0;
        for (size_t i = 0; i < ARRAY_LEN(RATES) && (nh = build_hops(w, RATES[i])) > budget; i++)
            ;
    } else {
        /* Modo lento: cada salto cuesta milisegundos, interesa la tasa mayor. */
        nh = build_hops(w, rmax);
    }
    if (nh < 0) {
        snprintf(err, errlen, "barrido: sin memoria");
        return -1;
    }
    if (w->quick && nh > budget) { /* ni a 61,44 Msps caben los perfiles */
        w->quick = false;
        if (!c->rate && build_hops(w, rmax) < 0) {
            snprintf(err, errlen, "barrido: sin memoria");
            return -1;
        }
    }
    if (w->nbins > MAX_BINS) {
        snprintf(err, errlen, "barrido: %d bins superan el máximo (%d); reduce 'fft' o el rango", w->nbins, MAX_BINS);
        return -1;
    }
    return 0;
}

/* ======================================================================= */
/*  Creación / destrucción                                                 */
/* ======================================================================= */
void sweep_destroy(sweep_t *w)
{
    if (!w)
        return;
    if (w->started) {
        if (w->io.retune_cancel)
            w->io.retune_cancel(w->io.ctx);
        w->io.stop(w->io.ctx);
    }
    fft_plan_destroy(w->plan);
    free(w->win);
    free(w->work);
    free(w->db);
    free(w->buf);
    free(w->hop);
    free(w);
}

sweep_t *sweep_create(const sweep_cfg_t *cfg, const sweep_io_t *io, char *err, size_t errlen)
{
    sweep_t *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->cfg = *cfg;
    w->io = *io;
    w->n = cfg->fft;
    if (!fft_size_valid(w->n) || w->n < 256 || w->n > 8192) {
        snprintf(err, errlen, "barrido: 'fft' debe ser potencia de 2 entre 256 y 8192");
        free(w);
        return NULL;
    }
    if (plan(w, err, errlen) < 0)
        goto fail;

    double settle_us = cfg->settle_us > 0 ? cfg->settle_us : 250.0;
    w->settle = (unsigned)ceil(settle_us * 1e-6 * w->rate);
    w->dwell = w->settle + (unsigned)w->n;
    w->cap = MAX(w->dwell, 16384u);
    w->plan = fft_plan_create(w->n);
    w->win = malloc(sizeof(float) * (size_t)w->n);
    w->work = malloc(sizeof(cf32) * (size_t)w->n);
    w->db = malloc(sizeof(float) * (size_t)w->nbins);
    w->buf = malloc(sizeof(int16_t) * 2 * w->cap);
    if (!w->plan || !w->win || !w->work || !w->db || !w->buf) {
        snprintf(err, errlen, "barrido: sin memoria");
        goto fail;
    }
    window_blackman_harris(w->win, w->n);
    double wsum = 0;
    for (int i = 0; i < w->n; i++)
        wsum += w->win[i];
    w->norm_db = -20.0 * log10(wsum * 2048.0); /* tono de fondo de escala = 0 dBFS */
    for (int i = 0; i < w->nbins; i++)
        w->db[i] = SWEEP_DB_MIN;

    if (io->configure(io->ctx, w->rate) < 0) {
        snprintf(err, errlen, "barrido: %s", io->error ? io->error(io->ctx) : "no se pudo configurar la tasa");
        goto fail;
    }
    if (w->quick) {
        /* Un perfil por salto (se reutilizan los ya creados para la misma frecuencia). */
        for (int h = 0; h < w->nh; h++) {
            w->hop[h].qt = io->qt_prepare(io->ctx, w->hop[h].center);
            if (w->hop[h].qt < 0) {
                w->quick = false; /* sin perfiles: modo lento, sigue siendo válido */
                break;
            }
        }
    }
    if (io->start(io->ctx) < 0) {
        snprintf(err, errlen, "barrido: %s", io->error ? io->error(io->ctx) : "no se pudo arrancar el flujo");
        goto fail;
    }
    w->started = true;
    w->last_emit = now_monotonic();
    return w;
fail:
    sweep_destroy(w);
    return NULL;
}

void sweep_get_info(const sweep_t *w, sweep_info_t *o)
{
    o->hops = w->nh;
    o->bins = w->nbins;
    o->rate = w->rate;
    o->fft = w->n;
    o->quick = w->quick;
    o->start_hz = w->start_hz;
    o->stop_hz = w->start_hz + llround(w->nbins * w->bin_hz);
    o->sweeps = w->sweeps;
    o->restarts = w->restarts;
}

/* ======================================================================= */
/*  Adquisición                                                            */
/* ======================================================================= */
/* Lee n muestras llevando la cuenta de la posición y de las anomalías. */
static int rd(sweep_t *w, unsigned n, int16_t *dst)
{
    uint64_t ts = 0;
    unsigned ovr = 0;
    int r = w->io.read(w->io.ctx, dst, n, &ts, &ovr);
    if (r < 0)
        return r;
    if (w->pos_valid && ts != w->pos)
        w->disc = true; /* hueco en el flujo */
    if (ovr)
        w->disc = true;
    w->pos = ts + n;
    w->pos_valid = true;
    return 0;
}

/* Lee y descarta hasta que la posición alcance 'target'. */
static int skip_to(sweep_t *w, uint64_t target)
{
    while (!w->pos_valid || w->pos < target) {
        uint64_t left = w->pos_valid ? target - w->pos : 2048;
        unsigned n = (unsigned)MIN(left, (uint64_t)w->cap);
        int r = rd(w, n, w->buf);
        if (r < 0)
            return r;
    }
    return 0;
}

static int quick_sync(sweep_t *w)
{
    if (w->io.retune_cancel)
        w->io.retune_cancel(w->io.ctx);
    uint64_t c;
    int r = w->io.timestamp(w->io.ctx, &c);
    if (r < 0)
        return r;
    w->t0 = c + (uint64_t)(0.02 * w->rate); /* 20 ms de margen para encolar y empezar a leer */
    w->g = w->g_sched = 0;
    w->pos_valid = false;
    w->disc = false;
    w->synced = true;
    return 0;
}

/* Devuelve 1 si hay datos válidos en buf (a partir de 'settle'), 0 si hay que
 * descartar este salto (re-sincronización), < 0 si error. */
static int acquire_quick(sweep_t *w)
{
    int r;
    if (!w->synced && (r = quick_sync(w)) < 0)
        return r;
    /* Mantener la cola de retunes llena por delante de la lectura. */
    while (w->g_sched < w->g + QAHEAD) {
        r = w->io.retune_at(w->io.ctx, w->t0 + w->g_sched * w->dwell, w->hop[w->g_sched % (uint64_t)w->nh].qt);
        if (r == SWEEP_QUEUE_FULL)
            break;
        if (r < 0)
            return r;
        w->g_sched++;
    }
    if (!w->pos_valid) {
        /* Primera lectura tras sincronizar: ver dónde está el flujo y elegir el
         * primer salto cuyo inicio aún no ha pasado. */
        if ((r = rd(w, 2048, w->buf)) < 0)
            return r;
        w->disc = false;
        if (w->pos > w->t0) {
            w->g = (w->pos - w->t0 + w->dwell - 1) / w->dwell;
            if (w->g_sched < w->g)
                w->g_sched = w->g;
        }
    }
    const uint64_t tg = w->t0 + w->g * w->dwell;
    if (w->pos > tg)
        goto resync; /* el salto ya empezó: vamos tarde */
    if ((r = skip_to(w, tg)) < 0)
        return r;
    w->disc = false; /* lo ocurrido en lo descartado no importa */
    if ((w->g % (uint64_t)w->nh) == 0) {
        /* Una vez por pasada: los retunes de los QAHEAD saltos siguientes se
         * encolan con la lectura a la altura de tg; el FPGA va por delante de la
         * lectura por el colchón USB. Si esa ventaja se come la antelación, el
         * retune caería en el pasado y se ejecutaría "ya" → datos mezclados. */
        uint64_t c;
        if (w->io.timestamp(w->io.ctx, &c) == 0 && c > tg + (QAHEAD - 2) * (uint64_t)w->dwell)
            goto resync;
    }
    if ((r = rd(w, w->dwell, w->buf)) < 0)
        return r;
    if (w->disc)
        goto resync;
    w->bad = 0;
    return 1;
resync:
    w->restarts++;
    w->synced = false;
    if (++w->bad >= 6) { /* el sistema no llega a tiempo: se renuncia al retune programado */
        LOG_W("Barrido: el retune programado no llega a tiempo (%u re-sincronizaciones seguidas); modo lento", w->bad);
        w->quick = false;
        w->io.retune_cancel(w->io.ctx);
    }
    return 0;
}

static int acquire_slow(sweep_t *w)
{
    const hop_t *hp = &w->hop[w->g % (uint64_t)w->nh];
    int r = w->io.set_freq(w->io.ctx, hp->center);
    if (r < 0)
        return r;
    uint64_t c;
    if ((r = w->io.timestamp(w->io.ctx, &c)) < 0)
        return r;
    /* Todo lo anterior al reloj actual del FPGA puede venir de la sintonía vieja
     * (el colchón USB guarda decenas de ms); lo posterior ya es de la nueva. */
    if ((r = skip_to(w, c + w->settle)) < 0)
        return r;
    w->disc = false;
    if ((r = rd(w, (unsigned)w->n, w->buf)) < 0)
        return r;
    return 1;
}

/* FFT del salto y pegado de su tramo útil en la trama. 'x' apunta a n muestras. */
static void process_hop(sweep_t *w, const hop_t *hp, const int16_t *x)
{
    const int n = w->n;
    double mi = 0, mq = 0;
    for (int i = 0; i < n; i++) {
        mi += x[2 * i];
        mq += x[2 * i + 1];
    }
    mi /= n; /* la media es la fuga de continua del receptor: fuera */
    mq /= n;
    for (int i = 0; i < n; i++) {
        w->work[i].re = (float)(x[2 * i] - mi) * w->win[i];
        w->work[i].im = (float)(x[2 * i + 1] - mq) * w->win[i];
    }
    fft_exec(w->plan, w->work);
    for (int i = 0; i < hp->count; i++) {
        int j = hp->k0 + i;              /* índice con fftshift (0 = −fs/2) */
        int k = (j + n / 2) % n;         /* índice de la FFT directa */
        float p = w->work[k].re * w->work[k].re + w->work[k].im * w->work[k].im;
        w->db[hp->off + i] = (float)(power_to_db(p) + w->norm_db);
    }
}

int sweep_step(sweep_t *w, sweep_frame_t *fr, char *err, size_t errlen)
{
    int r = w->quick ? acquire_quick(w) : acquire_slow(w);
    if (r < 0) {
        snprintf(err, errlen, "barrido: %s", w->io.error ? w->io.error(w->io.ctx) : "error de lectura");
        return r;
    }
    if (r == 0)
        return 0; /* re-sincronizando */
    const uint64_t hidx = w->g % (uint64_t)w->nh;
    process_hop(w, &w->hop[hidx], w->quick ? w->buf + 2 * (size_t)w->settle : w->buf);
    w->g++;

    bool complete = hidx == (uint64_t)w->nh - 1;
    double now = now_monotonic();
    if (!complete && now - w->last_emit < PARTIAL_PERIOD)
        return 0;
    w->last_emit = now;
    if (complete)
        w->sweeps++;
    fr->db = w->db;
    fr->n = w->nbins;
    fr->start_hz = w->start_hz;
    fr->bin_hz = w->bin_hz;
    fr->complete = complete;
    fr->progress_pct = (int)((hidx + 1) * 100 / (uint64_t)w->nh);
    return 1;
}

/* ======================================================================= */
/*  Mensaje binario tipo 4                                                 */
/* ======================================================================= */
size_t sweep_msg_size(int n) { return SWEEP_HEADER + (size_t)n; }

size_t sweep_pack(const sweep_frame_t *fr, uint8_t *m)
{
    memset(m, 0, SWEEP_HEADER);
    m[0] = 4;
    m[2] = fr->complete ? 1 : 0;
    m[3] = (uint8_t)CLAMP(fr->progress_pct, 0, 100);
    uint32_t nn = (uint32_t)fr->n;
    double span = fr->n * fr->bin_hz;
    double center = (double)fr->start_hz + span / 2;
    float dbmin = SWEEP_DB_MIN, dbstep = SWEEP_DB_STEP;
    memcpy(m + 4, &nn, 4);
    memcpy(m + 8, &center, 8);
    memcpy(m + 16, &span, 8);
    memcpy(m + 24, &dbmin, 4);
    memcpy(m + 28, &dbstep, 4);
    for (int i = 0; i < fr->n; i++) {
        float v = (fr->db[i] - SWEEP_DB_MIN) / SWEEP_DB_STEP;
        m[SWEEP_HEADER + i] = (uint8_t)CLAMP(v + 0.5f, 0.f, 255.f); /* redondeo al nivel más cercano */
    }
    return SWEEP_HEADER + (size_t)fr->n;
}

/* ======================================================================= */
/*  Envoltorio sobre source_t                                              */
/* ======================================================================= */
static int io_configure(void *c, uint32_t rate)
{
    source_t *s = c;
    source_stop(s);
    source_set_low_latency(s, true);
    uint32_t act = 0;
    int r = source_set_rate(s, rate, &act);
    if (r < 0)
        return r;
    return source_set_bandwidth(s, (uint32_t)(rate * 0.85), NULL);
}
static int io_start(void *c) { return source_start(c); }
static void io_stop(void *c)
{
    source_t *s = c;
    source_stop(s);
    source_set_low_latency(s, false);
}
static int io_set_freq(void *c, int64_t hz) { return source_set_frequency(c, hz); }
static int io_qt_prepare(void *c, int64_t hz) { return source_qt_prepare(c, hz); }
static int io_qt_free(void *c) { return source_qt_free(c); }
static int io_retune_at(void *c, uint64_t ts, int id) { return source_retune_at(c, ts, id); }
static int io_retune_cancel(void *c) { return source_retune_cancel(c); }
static int io_timestamp(void *c, uint64_t *ts) { return source_timestamp(c, ts); }
static int io_read(void *c, int16_t *iq, unsigned n, uint64_t *ts, unsigned *ovr)
{
    return source_read_ts(c, iq, n, ts, ovr);
}
static const char *io_error(void *c) { return source_error(c); }

sweep_io_t sweep_io_source(source_t *s)
{
    sweep_io_t io = {s,           io_configure,    io_start,     io_stop,         io_set_freq, io_qt_prepare,
                     io_qt_free,  io_retune_at,    io_retune_cancel, io_timestamp, io_read,     io_error};
    return io;
}
