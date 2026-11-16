/*
 * recorder.c — Grabador SigMF con búfer circular y hilo escritor. Ver recorder.h.
 */
#include "recorder.h"

#include "strbuf.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RING_BYTES (256u << 20) /* 256 MiB: ~0,8 s a 61 Msps, ~27 s a 2,4 Msps */

struct recorder {
    char dir[512];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool thread_running;
    bool active;
    bool stopping;
    /* búfer circular */
    uint8_t *ring;
    size_t head, tail, fill; /* en bytes; head = escritura, tail = lectura */
    /* fichero actual */
    FILE *fp;
    char base[160];
    uint32_t rate;
    int64_t freq;
    char hw[128];
    char datetime[40];
    uint64_t max_bytes;
    unsigned long long written, dropped;
};

recorder_t *recorder_create(const char *dir)
{
    recorder_t *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    snprintf(r->dir, sizeof r->dir, "%s", dir);
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    mkdir(dir, 0755);
    return r;
}

/* Escribe el .sigmf-meta (JSON) que describe la grabación. */
static void write_meta(recorder_t *r)
{
    char path[800];
    snprintf(path, sizeof path, "%s/%s.sigmf-meta", r->dir, r->base);
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_E("No se pudo escribir %s: %s", path, strerror(errno));
        return;
    }
    sb_t s = {0};
    sb_puts(&s, "{\n  \"global\": {\n    \"core:datatype\": \"ci16_le\",\n");
    sb_printf(&s, "    \"core:sample_rate\": %u,\n", r->rate);
    sb_puts(&s, "    \"core:version\": \"1.0.0\",\n    \"core:hw\": ");
    sb_json_str(&s, r->hw);
    sb_puts(&s, ",\n    \"core:recorder\": \"" APP_NAME " " APP_VERSION "\",\n");
    sb_puts(&s, "    \"core:description\": \"IQ crudo del bladeRF en formato Q11 (fondo de escala = 2048)\",\n");
    sb_printf(&s, "    \"bladerf_lab:full_scale\": 2048,\n    \"bladerf_lab:dropped_samples\": %llu\n  },\n",
              r->dropped);
    sb_printf(&s,
              "  \"captures\": [\n    {\n      \"core:sample_start\": 0,\n      \"core:frequency\": %lld,\n"
              "      \"core:datetime\": \"%s\"\n    }\n  ],\n  \"annotations\": []\n}\n",
              (long long)r->freq, r->datetime);
    fwrite(s.data, 1, s.len, f);
    fclose(f);
    sb_free(&s);
}

static void *writer_main(void *arg)
{
    recorder_t *r = arg;
    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (r->fill == 0 && !r->stopping)
            pthread_cond_wait(&r->cond, &r->lock);
        if (r->fill == 0 && r->stopping)
            break;
        /* Tramo contiguo desde tail */
        size_t chunk = MIN(r->fill, RING_BYTES - r->tail);
        chunk = MIN(chunk, (size_t)(4u << 20));
        const uint8_t *p = r->ring + r->tail;
        pthread_mutex_unlock(&r->lock);
        size_t w = fwrite(p, 1, chunk, r->fp); /* el disco, sin el cerrojo */
        pthread_mutex_lock(&r->lock);
        if (w != chunk) {
            LOG_E("Error escribiendo la grabación: %s", strerror(errno));
            r->stopping = true;
        }
        r->tail = (r->tail + chunk) % RING_BYTES;
        r->fill -= chunk;
        r->written += w;
        if (r->max_bytes && r->written >= r->max_bytes) {
            LOG_I("Grabación: alcanzado el tamaño máximo");
            r->stopping = true;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

int recorder_start(recorder_t *r, uint32_t rate, int64_t freq, const char *hw, uint64_t max_bytes, char *err,
                   size_t errlen)
{
    recorder_stop(r);
    if (!r->ring) {
        r->ring = malloc(RING_BYTES);
        if (!r->ring) {
            snprintf(err, errlen, "sin memoria para el búfer de grabación");
            return -1;
        }
    }
    time_t t = time(NULL);
    struct tm tm, utc;
    localtime_r(&t, &tm);
    gmtime_r(&t, &utc);
    snprintf(r->base, sizeof r->base, "%04d%02d%02d_%02d%02d%02d_%.3fMHz", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, freq / 1e6);
    snprintf(r->datetime, sizeof r->datetime, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
             utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    char path[800];
    snprintf(path, sizeof path, "%s/%s.sigmf-data", r->dir, r->base);
    r->fp = fopen(path, "wb");
    if (!r->fp) {
        snprintf(err, errlen, "no se puede crear %s: %s", path, strerror(errno));
        return -1;
    }
    r->rate = rate;
    r->freq = freq;
    snprintf(r->hw, sizeof r->hw, "%s", hw);
    r->max_bytes = max_bytes;
    r->head = r->tail = r->fill = 0;
    r->written = r->dropped = 0;
    r->stopping = false;
    write_meta(r); /* se reescribe al terminar con las estadísticas finales */
    pthread_mutex_lock(&r->lock);
    r->active = true;
    pthread_mutex_unlock(&r->lock);
    pthread_create(&r->thread, NULL, writer_main, r);
    r->thread_running = true;
    LOG_I("Grabando IQ en %s", path);
    return 0;
}

void recorder_stop(recorder_t *r)
{
    if (!r->thread_running)
        return;
    pthread_mutex_lock(&r->lock);
    r->active = false;
    r->stopping = true;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->thread, NULL);
    r->thread_running = false;
    fclose(r->fp);
    r->fp = NULL;
    write_meta(r);
    LOG_I("Grabación terminada: %s (%.1f MB, %llu muestras perdidas)", r->base, r->written / 1e6, r->dropped);
}

void recorder_push(recorder_t *r, const int16_t *iq, unsigned n)
{
    size_t bytes = (size_t)n * 4;
    pthread_mutex_lock(&r->lock);
    if (!r->active || r->stopping) {
        pthread_mutex_unlock(&r->lock);
        return;
    }
    if (RING_BYTES - r->fill < bytes) {
        r->dropped += n; /* disco demasiado lento: se pierde este bloque */
    } else {
        const uint8_t *src = (const uint8_t *)iq;
        size_t first = MIN(bytes, RING_BYTES - r->head);
        memcpy(r->ring + r->head, src, first);
        memcpy(r->ring, src + first, bytes - first);
        r->head = (r->head + bytes) % RING_BYTES;
        r->fill += bytes;
        pthread_cond_signal(&r->cond);
    }
    pthread_mutex_unlock(&r->lock);
}

void recorder_status(recorder_t *r, recorder_status_t *st)
{
    pthread_mutex_lock(&r->lock);
    /* Si el escritor se detuvo solo (tamaño máximo o error), se informa
     * como inactivo; el hilo se recoge en el siguiente start/stop. */
    st->active = r->active && !r->stopping;
    snprintf(st->name, sizeof st->name, "%s", r->base);
    st->bytes = r->written;
    st->seconds = r->rate ? r->written / 4.0 / r->rate : 0;
    st->dropped = r->dropped;
    pthread_mutex_unlock(&r->lock);
}

int recorder_delete(recorder_t *r, const char *name)
{
    if (!name[0] || strchr(name, '/') || name[0] == '.')
        return -1;
    char path[800];
    const char *ext = strrchr(name, '.');
    int rc = -1;
    if (ext && (strcmp(ext, ".sigmf-data") == 0 || strcmp(ext, ".sigmf-meta") == 0)) {
        int base = (int)(ext - name);
        snprintf(path, sizeof path, "%s/%.*s.sigmf-data", r->dir, base, name);
        if (unlink(path) == 0)
            rc = 0;
        snprintf(path, sizeof path, "%s/%.*s.sigmf-meta", r->dir, base, name);
        if (unlink(path) == 0)
            rc = 0;
    } else {
        snprintf(path, sizeof path, "%s/%s", r->dir, name);
        rc = unlink(path);
    }
    return rc;
}

void recorder_destroy(recorder_t *r)
{
    if (!r)
        return;
    recorder_stop(r);
    free(r->ring);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r);
}
