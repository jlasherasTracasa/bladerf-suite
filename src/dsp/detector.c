/*
 * detector.c — Detector de señales OS-CFAR por segmentos. Ver detector.h.
 */
#include "detector.h"

#include <stdlib.h>
#include <string.h>

#define NSEG 32
#define MAX_RAW 128
#define MAX_TRK 128

typedef struct { int start, end, peak; } raw_t;

/* Pista temporal de una señal (histéresis de aparición y desaparición). */
typedef struct {
    det_signal_t s;
    int hits, misses;
    bool shown;
} track_t;

struct detector {
    int n;
    bool primed;
    float *avg_lin;  /* potencia media por bin (lineal) */
    float *avg_db;
    float *floor_db; /* suelo de ruido estimado por bin */
    float *tmp;
    float edge_frac;
    int n_on, m_off;
    double last_rate;
    track_t trk[MAX_TRK];
    int ntrk;
};

detector_t *detector_create(int n)
{
    detector_t *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->n = n;
    d->edge_frac = DET_EDGE_FRAC;
    d->n_on = DET_N_ON;
    d->m_off = DET_M_OFF;
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

void detector_reset(detector_t *d)
{
    d->primed = false;
    d->ntrk = 0;
}

void detector_config(detector_t *d, float edge_frac, int n_on, int m_off)
{
    d->edge_frac = CLAMP(edge_frac, 0.f, 0.45f);
    d->n_on = MAX(n_on, 1);
    d->m_off = MAX(m_off, 1);
}

/* ¿El bin cuenta como ocupado? Excluye bordes (|i-dc| > half_keep) y la continua. */
static bool bin_occ(const detector_t *d, int i, int dc, int half_keep, float snr_min)
{
    return abs(i - dc) > 1 && abs(i - dc) <= half_keep && d->avg_db[i] - d->floor_db[i] >= snr_min;
}

/* Asocia las detecciones de esta imagen con las pistas y aplica la histéresis.
 * Una detección coincide con una pista si sus centros distan menos que la mitad
 * del mayor ancho (mín. 3 bins). Las pistas nuevas se muestran tras n_on
 * imágenes seguidas; las confirmadas se retiran tras m_off ausencias. Mientras
 * una pista confirmada está ausente se sigue informando su último valor. El
 * centro y el ancho se suavizan (media exponencial 0,5) para que el marcador
 * no tiemble. Devuelve las pistas visibles ordenadas por frecuencia. */
static int track_update(detector_t *d, const det_signal_t *cur, int nc, double binhz, det_signal_t *out, int max)
{
    bool used[MAX_RAW] = {0};
    for (int t = 0; t < d->ntrk; t++) {
        track_t *k = &d->trk[t];
        int best = -1;
        double bd = 1e300;
        for (int c = 0; c < nc; c++) {
            double tol = fmax(0.5 * fmax(k->s.bw_hz, cur[c].bw_hz), 3 * binhz);
            double dist = fabs(cur[c].offset_hz - k->s.offset_hz);
            if (!used[c] && dist < tol && dist < bd) {
                bd = dist;
                best = c;
            }
        }
        if (best >= 0) {
            used[best] = true;
            k->s.offset_hz += 0.5 * (cur[best].offset_hz - k->s.offset_hz);
            k->s.bw_hz += 0.5 * (cur[best].bw_hz - k->s.bw_hz);
            k->s.peak_db = cur[best].peak_db;
            k->s.snr_db = cur[best].snr_db;
            k->hits++;
            k->misses = 0;
            if (k->hits >= d->n_on)
                k->shown = true;
        } else {
            k->misses++;
            if (!k->shown)
                k->hits = 0, k->misses = d->m_off; /* tentativa fallida: se descarta */
        }
    }
    int w = 0;
    for (int t = 0; t < d->ntrk; t++)
        if (d->trk[t].misses < d->m_off)
            d->trk[w++] = d->trk[t];
    d->ntrk = w;
    for (int c = 0; c < nc && d->ntrk < MAX_TRK; c++)
        if (!used[c])
            d->trk[d->ntrk++] = (track_t){cur[c], 1, 0, d->n_on <= 1};
    /* Orden por frecuencia (inserción) y copia de las visibles. */
    for (int a = 1; a < d->ntrk; a++) {
        track_t v = d->trk[a];
        int b = a - 1;
        while (b >= 0 && d->trk[b].s.offset_hz > v.s.offset_hz) {
            d->trk[b + 1] = d->trk[b];
            b--;
        }
        d->trk[b + 1] = v;
    }
    int count = 0;
    for (int t = 0; t < d->ntrk && count < max; t++)
        if (d->trk[t].shown)
            out[count++] = d->trk[t].s;
    return count;
}

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
    if (rate != d->last_rate) {
        d->ntrk = 0;
        d->last_rate = rate;
    }
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
    /* Segmentos cuyo centro cae en la zona de borde (caída del filtro): su suelo
     * no es representativo (más bajo), se sustituye por el del interior más cercano. */
    {
        int lo_s = (int)ceilf(d->edge_frac * NSEG - 0.5f), hi_s = NSEG - 1 - lo_s;
        if (lo_s > 0 && lo_s < hi_s) {
            for (int s = 0; s < lo_s; s++) segfloor[s] = segfloor[lo_s];
            for (int s = hi_s + 1; s < NSEG; s++) segfloor[s] = segfloor[hi_s];
        }
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

    /* Búsqueda de tramos ocupados (fuera de los bordes y del pico de continua) */
    const double binhz = rate / n;
    const int dc = n / 2;
    const int half_keep = (int)((0.5f - d->edge_frac) * (float)n);
    raw_t raw[MAX_RAW];
    int nraw = 0;
    for (int i = 0; i < n && nraw < MAX_RAW;) {
        if (!bin_occ(d, i, dc, half_keep, snr_min)) {
            i++;
            continue;
        }
        int start = i, end = i, gap = 0, peak = i;
        for (int j = i + 1; j < n; j++) {
            if (bin_occ(d, j, dc, half_keep, snr_min)) {
                end = j;
                gap = 0;
                if (d->avg_db[j] > d->avg_db[peak])
                    peak = j;
            } else if (++gap > 2) {
                break;
            }
        }
        float snr = d->avg_db[peak] - d->floor_db[peak];
        /* Uno o dos bins con poco margen suelen ser una espuria, no una señal. */
        if (end - start >= 2 || snr >= snr_min + 6.f)
            raw[nraw++] = (raw_t){start, end, peak};
        i = end + 1;
    }

    /* Fusión: dos tramos se unen si el hueco entre ellos es menor que 0,3 veces
     * del más estrecho x0,3 (más 2 bins). Una emisora FM con poca modulación deja un
     * valle de portadora y la rampa del filtro parte el tramo en varios. */
    int nm = 0;
    for (int k = 0; k < nraw; k++) {
        if (nm > 0) {
            raw_t *p = &raw[nm - 1];
            int gapb = raw[k].start - p->end - 1;
            int wa = p->end - p->start + 1, wb = raw[k].end - raw[k].start + 1;
            if (gapb <= (3 * MIN(wa, wb) / 10) + 2) {
                if (d->avg_db[raw[k].peak] > d->avg_db[p->peak])
                    p->peak = raw[k].peak;
                p->end = raw[k].end;
                continue;
            }
        }
        raw[nm++] = raw[k];
    }

    det_signal_t cur[MAX_RAW];
    for (int k = 0; k < nm; k++) {
        /* Centroide de potencia del exceso sobre el suelo (lineal) en el tramo. */
        double sw = 0, sx = 0;
        for (int i = raw[k].start; i <= raw[k].end; i++) {
            double ex = pow(10.0, d->avg_db[i] / 10.0) - pow(10.0, d->floor_db[i] / 10.0);
            if (ex <= 0)
                continue;
            sw += ex;
            sx += ex * i;
        }
        double c = sw > 0 ? sx / sw : 0.5 * (raw[k].start + raw[k].end);
        cur[k].offset_hz = (c - n / 2) * binhz;
        cur[k].bw_hz = (raw[k].end - raw[k].start + 1) * binhz;
        cur[k].peak_db = d->avg_db[raw[k].peak];
        cur[k].snr_db = d->avg_db[raw[k].peak] - d->floor_db[raw[k].peak];
    }
    return track_update(d, cur, nm, binhz, out, max);
}
