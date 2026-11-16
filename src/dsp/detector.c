/*
 * detector.c — Detector de señales OS-CFAR por segmentos. Ver detector.h.
 */
#include "detector.h"

#include <stdlib.h>
#include <string.h>

#define NSEG 32

struct detector {
    int n;
    bool primed;
    float *avg_lin;  /* potencia media por bin (lineal) */
    float *avg_db;
    float *floor_db; /* suelo de ruido estimado por bin */
    float *tmp;
};

detector_t *detector_create(int n)
{
    detector_t *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->n = n;
    d->avg_lin = calloc((size_t)n, sizeof(float));
    d->avg_db = calloc((size_t)n, sizeof(float));
    d->floor_db = calloc((size_t)n, sizeof(float));
    d->tmp = calloc((size_t)n, sizeof(float));
    return d;
}

void detector_destroy(detector_t *d)
{
    if (!d)
        return;
    free(d->avg_lin);
    free(d->avg_db);
    free(d->floor_db);
    free(d->tmp);
    free(d);
}

void detector_reset(detector_t *d) { d->primed = false; }

void detector_feed(detector_t *d, const float *db, int n, float tau_frames)
{
    if (n != d->n)
        return;
    if (!d->primed) {
        for (int i = 0; i < n; i++)
            d->avg_lin[i] = powf(10.f, db[i] / 10.f);
        d->primed = true;
        return;
    }
    const float a = 1.f / fmaxf(tau_frames, 1.f);
    for (int i = 0; i < n; i++)
        d->avg_lin[i] += a * (powf(10.f, db[i] / 10.f) - d->avg_lin[i]);
}

/* k-ésimo menor elemento (selección rápida de Hoare, O(n) de media). */
static float quickselect(float *v, int n, int k)
{
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        float pivot = v[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) {
                float t = v[i]; v[i] = v[j]; v[j] = t;
                i++;
                j--;
            }
        }
        if (k <= j)
            hi = j;
        else if (k >= i)
            lo = i;
        else
            break;
    }
    return v[k];
}

int detector_run(detector_t *d, double rate, float snr_min, det_signal_t *out, int max, float *noise_db)
{
    if (!d->primed)
        return 0;
    const int n = d->n;
    for (int i = 0; i < n; i++)
        d->avg_db[i] = power_to_db(d->avg_lin[i]);

    /* Suelo por segmento: percentil 20 */
    const int seg = n / NSEG;
    float segfloor[NSEG];
    for (int s = 0; s < NSEG; s++) {
        memcpy(d->tmp, d->avg_db + s * seg, sizeof(float) * (size_t)seg);
        segfloor[s] = quickselect(d->tmp, seg, seg / 5);
    }
    /* Mínimo con los vecinos e interpolación lineal entre centros de segmento */
    float smooth[NSEG];
    for (int s = 0; s < NSEG; s++) {
        float m = segfloor[s];
        if (s > 0) m = fminf(m, segfloor[s - 1] + 1.f);
        if (s < NSEG - 1) m = fminf(m, segfloor[s + 1] + 1.f);
        smooth[s] = m;
    }
    for (int i = 0; i < n; i++) {
        float pos = (i + 0.5f) / seg - 0.5f;
        int s0 = (int)floorf(pos);
        float t = pos - s0;
        int a = CLAMP(s0, 0, NSEG - 1), b = CLAMP(s0 + 1, 0, NSEG - 1);
        d->floor_db[i] = smooth[a] * (1 - t) + smooth[b] * t;
    }
    if (noise_db) {
        memcpy(d->tmp, segfloor, sizeof segfloor);
        *noise_db = quickselect(d->tmp, NSEG, NSEG / 2);
    }

    /* Búsqueda de tramos ocupados */
    const double binhz = rate / n;
    const int dc = n / 2;
    int count = 0;
    int i = 0;
    while (i < n && count < max) {
        if (d->avg_db[i] - d->floor_db[i] < snr_min || abs(i - dc) <= 1) {
            i++;
            continue;
        }
        int start = i, end = i, gap = 0, peak = i;
        for (int j = i + 1; j < n; j++) {
            bool occ = d->avg_db[j] - d->floor_db[j] >= snr_min && abs(j - dc) > 1;
            if (occ) {
                end = j;
                gap = 0;
                if (d->avg_db[j] > d->avg_db[peak])
                    peak = j;
            } else if (++gap > 2) {
                break;
            }
        }
        float snr = d->avg_db[peak] - d->floor_db[peak];
        /* Un único bin con poco margen suele ser una espuria, no una señal. */
        if (end > start || snr >= snr_min + 6.f) {
            det_signal_t *s = &out[count++];
            s->offset_hz = (peak - n / 2) * binhz;
            s->bw_hz = (end - start + 1) * binhz;
            s->peak_db = d->avg_db[peak];
            s->snr_db = snr;
        }
        i = end + 1;
    }
    return count;
}
