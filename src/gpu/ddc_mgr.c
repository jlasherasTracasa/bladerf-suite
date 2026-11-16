/*
 * ddc_mgr.c — Ver ddc_mgr.h.
 *
 * Cerrojos: m->cm protege el banco de canales y la tabla de callbacks (se
 * mantiene durante ddc_gpu_process, así remove() espera a que no queden
 * entregas en vuelo hacia ese ctx); m->qm protege el anillo de bloques.
 * Orden: cm → (cerrojos del llamador dentro del callback). Nadie toma cm
 * mientras sostiene un cerrojo que el callback pueda necesitar.
 */
#include "ddc_mgr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSLOT 4

typedef struct {
    int16_t *iq;
    size_t cap;
    unsigned n;
    double fs, now;
    int64_t center;
} slot_t;

typedef struct {
    bool used;
    ddc_mgr_out_fn fn;
    void *ctx;
} entry_t;

struct ddc_mgr {
    ddc_gpu_t *g;
    int max_block;
    pthread_t th;
    bool started, stop;
    pthread_mutex_t cm, qm;
    pthread_cond_t cv;
    slot_t slot[NSLOT];
    int rd, wr, count;
    entry_t ent[64];
    /* contexto del bloque en curso para el callback */
    double cur_now, cur_fs;
    int64_t cur_center;
    unsigned long blocks, drops;
    double thread_s;
};

static void on_out(void *ctx, int chan, const cf32 *y, int n, double t_first)
{
    ddc_mgr_t *m = ctx;
    if (chan < 0 || chan >= 64 || !m->ent[chan].used)
        return;
    m->ent[chan].fn(m->ent[chan].ctx, y, n, m->cur_now + t_first, m->cur_fs, m->cur_center);
}

static double thread_cpu_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void *mgr_main(void *arg)
{
    ddc_mgr_t *m = arg;
    pthread_setname_np(pthread_self(), "ddc-gpu");
    for (;;) {
        pthread_mutex_lock(&m->qm);
        while (!m->stop && m->count == 0)
            pthread_cond_wait(&m->cv, &m->qm);
        if (m->stop) {
            pthread_mutex_unlock(&m->qm);
            break;
        }
        slot_t *s = &m->slot[m->rd];
        pthread_mutex_unlock(&m->qm);

        pthread_mutex_lock(&m->cm);
        m->cur_now = s->now;
        m->cur_fs = s->fs;
        m->cur_center = s->center;
        ddc_gpu_process(m->g, s->iq, (int)s->n, s->fs, s->center, on_out, m);
        pthread_mutex_unlock(&m->cm);

        pthread_mutex_lock(&m->qm);
        m->rd = (m->rd + 1) % NSLOT;
        m->count--;
        m->blocks++;
        m->thread_s = thread_cpu_s();
        pthread_mutex_unlock(&m->qm);
    }
    return NULL;
}

ddc_mgr_t *ddc_mgr_create(int max_block, char *err, size_t errlen)
{
    ddc_gpu_t *g = ddc_gpu_create(max_block, err, errlen);
    if (!g)
        return NULL;
    ddc_mgr_t *m = calloc(1, sizeof *m);
    if (!m) {
        ddc_gpu_destroy(g);
        return NULL;
    }
    m->g = g;
    m->max_block = max_block;
    pthread_mutex_init(&m->cm, NULL);
    pthread_mutex_init(&m->qm, NULL);
    pthread_cond_init(&m->cv, NULL);
    if (pthread_create(&m->th, NULL, mgr_main, m) != 0) {
        ddc_mgr_destroy(m);
        return NULL;
    }
    m->started = true;
    return m;
}

void ddc_mgr_destroy(ddc_mgr_t *m)
{
    if (!m)
        return;
    if (m->started) {
        pthread_mutex_lock(&m->qm);
        m->stop = true;
        pthread_cond_signal(&m->cv);
        pthread_mutex_unlock(&m->qm);
        pthread_join(m->th, NULL);
    }
    for (int i = 0; i < NSLOT; i++)
        free(m->slot[i].iq);
    ddc_gpu_destroy(m->g);
    pthread_mutex_destroy(&m->cm);
    pthread_mutex_destroy(&m->qm);
    pthread_cond_destroy(&m->cv);
    free(m);
}

const char *ddc_mgr_device_name(ddc_mgr_t *m) { return ddc_gpu_device_name(m->g); }

int ddc_mgr_add(ddc_mgr_t *m, double fs, int64_t center, int64_t freq, double fs_out, double bw_hz,
                ddc_mgr_out_fn fn, void *ctx, double *fs_mid, char *err, size_t errlen)
{
    ddc_plan_t p;
    if (ddc_plan_design(&p, fs, fs_out, bw_hz) < 0) {
        snprintf(err, errlen, "diezmado insuficiente para la GPU");
        return -1;
    }
    pthread_mutex_lock(&m->cm);
    int id = ddc_gpu_add(m->g, &p, center, freq);
    if (id >= 0 && id < 64) {
        m->ent[id].used = true;
        m->ent[id].fn = fn;
        m->ent[id].ctx = ctx;
        if (fs_mid)
            *fs_mid = p.fs_mid;
    } else {
        if (id >= 0)
            ddc_gpu_remove(m->g, id);
        id = -1;
        snprintf(err, errlen, "no se pudo crear el canal en la GPU");
    }
    pthread_mutex_unlock(&m->cm);
    ddc_plan_free(&p);
    return id;
}

void ddc_mgr_remove(ddc_mgr_t *m, int h)
{
    if (h < 0 || h >= 64)
        return;
    pthread_mutex_lock(&m->cm);
    m->ent[h].used = false;
    ddc_gpu_remove(m->g, h);
    pthread_mutex_unlock(&m->cm);
}

bool ddc_mgr_feed(ddc_mgr_t *m, const int16_t *iq, unsigned n, double fs, int64_t center, double now)
{
    if (!iq || n == 0 || n > (unsigned)m->max_block)
        return false;
    pthread_mutex_lock(&m->qm);
    if (m->count >= NSLOT) {
        m->drops++;
        pthread_mutex_unlock(&m->qm);
        return false;
    }
    slot_t *s = &m->slot[m->wr];
    pthread_mutex_unlock(&m->qm);
    /* Solo el hilo DSP escribe en 'wr' y esa ranura no está en cola todavía */
    if (s->cap < n) {
        free(s->iq);
        s->iq = malloc(sizeof(int16_t) * 2 * (size_t)m->max_block);
        s->cap = s->iq ? (size_t)m->max_block : 0;
        if (!s->iq)
            return false;
    }
    memcpy(s->iq, iq, sizeof(int16_t) * 2 * (size_t)n);
    s->n = n;
    s->fs = fs;
    s->now = now;
    s->center = center;
    pthread_mutex_lock(&m->qm);
    m->wr = (m->wr + 1) % NSLOT;
    m->count++;
    pthread_cond_signal(&m->cv);
    pthread_mutex_unlock(&m->qm);
    return true;
}

void ddc_mgr_stats(ddc_mgr_t *m, ddc_mgr_stats_t *st)
{
    pthread_mutex_lock(&m->qm);
    st->blocks = m->blocks;
    st->drops = m->drops;
    st->thread_s = m->thread_s;
    pthread_mutex_unlock(&m->qm);
    pthread_mutex_lock(&m->cm);
    st->gpu_s = ddc_gpu_busy_s(m->g);
    int c = 0;
    for (int i = 0; i < 64; i++)
        c += m->ent[i].used;
    st->channels = c;
    pthread_mutex_unlock(&m->cm);
}
