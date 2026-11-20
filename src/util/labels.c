/*
 * labels.c — Ver labels.h. Reúne eventos verificados, huecos y señales del detector durante la
 * reproducción y escribe el .sigmf-meta con las anotaciones al final.
 */
#include "labels.h"

#include "../decoders/decoder.h"
#include "json.h"
#include "strbuf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char proto[24];
    int64_t freq_hz;
    int64_t s0, s1;   /* s1 < 0: se desconoce el final */
    double snr_db, rssi_dbfs;
    bool has_snr, has_rssi, mark;
    double bw_hz;
    char text[160];
} lab_ev_t;

typedef struct {
    char proto[24];
    int64_t freq_hz;
    uint64_t s0, n;
} lab_gap_t;

typedef struct {
    double freq_hz, bw_hz; /* centroide corriente y anchura máxima vista */
    double snr_db;         /* SNR máxima (= snr_peak) */
    double snr_last;       /* SNR de la última imagen (el detector repite el último valor mientras mantiene la pista) */
    uint64_t first, last;  /* primera y última imagen en que apareció (índice de muestra) */
    bool open;
} lab_reg_t;

struct labels {
    pthread_mutex_t m;
    char datatype[16], dataset[200];
    double rate;
    int64_t center;
    lab_ev_t *ev;
    size_t nev, cap_ev;
    lab_gap_t *gap;
    size_t ngap, cap_gap;
    lab_reg_t *reg;
    size_t nreg, cap_reg;
    uint64_t prev_end, step; /* última imagen del detector y separación entre imágenes */
    jtok_t *tok;
};

#define LAB_MAX_TOK 1024

labels_t *labels_create(const char *datatype, const char *dataset)
{
    labels_t *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    pthread_mutex_init(&l->m, NULL);
    snprintf(l->datatype, sizeof l->datatype, "%s", datatype ? datatype : "ci16_le");
    snprintf(l->dataset, sizeof l->dataset, "%s", dataset ? dataset : "");
    l->tok = malloc(sizeof(jtok_t) * LAB_MAX_TOK);
    if (!l->tok) {
        free(l);
        return NULL;
    }
    return l;
}

void labels_destroy(labels_t *l)
{
    if (!l)
        return;
    free(l->ev);
    free(l->gap);
    free(l->reg);
    free(l->tok);
    pthread_mutex_destroy(&l->m);
    free(l);
}

void labels_set_source(labels_t *l, double rate, int64_t center_hz)
{
    l->rate = rate;
    l->center = center_hz;
}

static void *grow(void *p, size_t *cap, size_t n, size_t elem)
{
    if (n < *cap)
        return p;
    size_t nc = *cap ? *cap * 2 : 256;
    void *q = realloc(p, nc * elem);
    if (!q)
        return NULL;
    *cap = nc;
    return q;
}

/* Ancho ocupado a efectos de solape: el del canal del decodificador; los WIDE, su tasa. */
static double ev_bw_hz(const char *proto, double rate)
{
    const decoder_desc_t *d = decoder_find(proto);
    if (!d)
        return 0;
    if (d->kind == DEC_NARROW)
        return d->bw_hz;
    return d->wide_rate ? (double)d->wide_rate : rate;
}

void labels_on_event(void *lp, const char *line)
{
    labels_t *l = lp;
    size_t len = strlen(line);
    pthread_mutex_lock(&l->m);
    int nt = json_parse(line, len, l->tok, LAB_MAX_TOK);
    if (nt <= 0 || l->tok[0].type != J_OBJ) {
        pthread_mutex_unlock(&l->m);
        return;
    }
    char type[16] = "", proto[24] = "";
    double v, freq = 0;
    int i;
    if ((i = json_obj_get(line, l->tok, 0, "type")) >= 0)
        json_str(line, l->tok, i, type, sizeof type);
    if ((i = json_obj_get(line, l->tok, 0, "proto")) >= 0)
        json_str(line, l->tok, i, proto, sizeof proto);
    if ((i = json_obj_get(line, l->tok, 0, "freq")) >= 0 && json_num(line, l->tok, i, &v))
        freq = v;
    if (strcmp(type, "gap") == 0) {
        l->gap = grow(l->gap, &l->cap_gap, l->ngap, sizeof *l->gap);
        if (l->gap) {
            lab_gap_t *g = &l->gap[l->ngap++];
            memset(g, 0, sizeof *g);
            snprintf(g->proto, sizeof g->proto, "%s", proto);
            g->freq_hz = (int64_t)freq;
            if ((i = json_obj_get(line, l->tok, 0, "sample")) >= 0 && json_num(line, l->tok, i, &v))
                g->s0 = (uint64_t)v;
            if ((i = json_obj_get(line, l->tok, 0, "n")) >= 0 && json_num(line, l->tok, i, &v))
                g->n = (uint64_t)v;
        }
    } else if (strcmp(type, "dec") == 0) {
        int m = json_obj_get(line, l->tok, 0, "msg");
        int is = m >= 0 ? json_obj_get(line, l->tok, m, "sample") : -1;
        if (m >= 0 && is >= 0 && json_num(line, l->tok, is, &v)) {
            l->ev = grow(l->ev, &l->cap_ev, l->nev, sizeof *l->ev);
            if (l->ev) {
                lab_ev_t *e = &l->ev[l->nev++];
                memset(e, 0, sizeof *e);
                snprintf(e->proto, sizeof e->proto, "%s", proto);
                e->freq_hz = (int64_t)freq;
                e->s0 = (int64_t)v;
                e->s1 = -1;
                if ((i = json_obj_get(line, l->tok, m, "sample_end")) >= 0 && json_num(line, l->tok, i, &v))
                    e->s1 = (int64_t)v;
                if ((i = json_obj_get(line, l->tok, m, "snr_db")) >= 0 && json_num(line, l->tok, i, &v)) {
                    e->snr_db = v;
                    e->has_snr = true;
                }
                if ((i = json_obj_get(line, l->tok, m, "rssi_dbfs")) >= 0 && json_num(line, l->tok, i, &v)) {
                    e->rssi_dbfs = v;
                    e->has_rssi = true;
                }
                char src[8] = "";
                if ((i = json_obj_get(line, l->tok, m, "sample_src")) >= 0)
                    json_str(line, l->tok, i, src, sizeof src);
                e->mark = strcmp(src, "mark") == 0;
                if ((i = json_obj_get(line, l->tok, m, "text")) >= 0)
                    json_str(line, l->tok, i, e->text, sizeof e->text);
                e->bw_hz = ev_bw_hz(proto, l->rate);
            }
        }
    }
    pthread_mutex_unlock(&l->m);
}

void labels_on_signals(void *lp, uint64_t sample_end, double rate, int64_t center_hz, const det_signal_t *sigs, int n)
{
    labels_t *l = lp;
    pthread_mutex_lock(&l->m);
    l->rate = rate;
    l->center = center_hz;
    if (l->prev_end && sample_end > l->prev_end)
        l->step = sample_end - l->prev_end;
    l->prev_end = sample_end;
    /* Las regiones que no aparecen en esta imagen se cierran (la histéresis ya la aplicó el detector) */
    bool seen[1024];
    size_t nr = l->nreg < 1024 ? l->nreg : 1024;
    memset(seen, 0, sizeof seen);
    for (int k = 0; k < n; k++) {
        double f = (double)center_hz + sigs[k].offset_hz;
        size_t best = (size_t)-1;
        for (size_t r = 0; r < nr; r++) {
            lab_reg_t *g = &l->reg[r];
            if (!g->open)
                continue;
            if (fabs(g->freq_hz - f) <= 0.5 * (g->bw_hz + sigs[k].bw_hz) + 1.0) {
                best = r;
                break;
            }
        }
        if (best == (size_t)-1) {
            l->reg = grow(l->reg, &l->cap_reg, l->nreg, sizeof *l->reg);
            if (!l->reg)
                continue;
            lab_reg_t *g = &l->reg[l->nreg];
            memset(g, 0, sizeof *g);
            g->freq_hz = f;
            g->bw_hz = sigs[k].bw_hz;
            g->snr_db = sigs[k].snr_db;
            g->snr_last = sigs[k].snr_db;
            g->first = g->last = sample_end;
            g->open = true;
            if (l->nreg < 1024)
                seen[l->nreg] = true;
            l->nreg++;
        } else {
            lab_reg_t *g = &l->reg[best];
            g->freq_hz = 0.8 * g->freq_hz + 0.2 * f;
            g->bw_hz = MAX(g->bw_hz, sigs[k].bw_hz);
            g->snr_db = MAX(g->snr_db, (double)sigs[k].snr_db);
            g->snr_last = sigs[k].snr_db;
            g->last = sample_end;
            seen[best] = true;
        }
    }
    for (size_t r = 0; r < nr; r++)
        if (l->reg[r].open && !seen[r])
            l->reg[r].open = false;
    pthread_mutex_unlock(&l->m);
}

typedef struct {
    int64_t a, b;
} span_t;

static int cmp_span(const void *x, const void *y)
{
    const span_t *p = x, *q = y;
    return (p->a > q->a) - (p->a < q->a);
}

static void ann_common(sb_t *sb, int64_t s0, int64_t n, double flo, double fhi, const char *label, const char *level)
{
    sb_printf(sb, "    {\"core:sample_start\": %lld, \"core:sample_count\": %lld, \"core:freq_lower_edge\": %.0f, "
                  "\"core:freq_upper_edge\": %.0f, \"core:label\": ",
              (long long)s0, (long long)MAX(n, 1), flo, fhi);
    sb_json_str(sb, label);
    sb_printf(sb, ", \"lab:level\": \"%s\"", level);
}

int labels_write(labels_t *l, const char *path, uint64_t total_samples, char *err, size_t errlen, int *n_verified,
                 int *n_unknown, int *n_gap)
{
    pthread_mutex_lock(&l->m);
    const uint64_t step = l->step ? l->step : (uint64_t)(l->rate * 0.5 + 1);

    sb_t sb = {0};
    sb_puts(&sb, "{\n  \"global\": {\n    \"core:datatype\": ");
    sb_json_str(&sb, l->datatype);
    sb_printf(&sb, ",\n    \"core:sample_rate\": %.0f,\n    \"core:version\": \"1.0.0\"", l->rate);
    if (l->dataset[0]) {
        sb_puts(&sb, ",\n    \"core:dataset\": ");
        sb_json_str(&sb, l->dataset);
    }
    sb_puts(&sb, ",\n    \"core:description\": \"Etiquetas en tres niveles generadas por bladerf-lab --labels\","
                 "\n    \"lab:tool\": \"bladerf-lab\",\n"
                 "    \"lab:levels\": \"verified: mensaje decodificado con CRC/FEC (muestra de inicio y fin, SNR). "
                 "unknown: energia sostenida del detector sin evento verificado que la explique. "
                 "gap: tramo que una instancia de decodificador no miro.\",\n"
                 "    \"lab:empty_convention\": \"empty no se anota: toda region tiempo-frecuencia sin anotacion "
                 "verified ni unknown es vacia (solo ruido a la sensibilidad del detector), salvo dentro de un gap.\",\n");
    sb_printf(&sb, "    \"lab:detector_step_samples\": %llu\n  },\n", (unsigned long long)step);
    sb_printf(&sb, "  \"captures\": [{\"core:sample_start\": 0, \"core:frequency\": %lld}],\n  \"annotations\": [\n",
              (long long)l->center);

    int nv = 0, nu = 0, ng = 0, first = 1;
#define SEP() do { if (!first) sb_puts(&sb, ",\n"); first = 0; } while (0)
    /* --- verified --- */
    for (size_t i = 0; i < l->nev; i++) {
        const lab_ev_t *e = &l->ev[i];
        double bw = e->bw_hz > 0 ? e->bw_hz : 0;
        int64_t n = e->s1 >= e->s0 ? e->s1 - e->s0 + 1 : 1;
        SEP();
        char label[64];
        snprintf(label, sizeof label, "%s", e->proto);
        ann_common(&sb, e->s0, n, (double)e->freq_hz - bw / 2, (double)e->freq_hz + bw / 2, label, "verified");
        sb_puts(&sb, ", \"lab:proto\": ");
        sb_json_str(&sb, e->proto);
        sb_printf(&sb, ", \"lab:precision\": \"%s\"", e->mark ? "sample" : "block");
        if (e->has_snr)
            sb_printf(&sb, ", \"lab:snr_db\": %.1f", e->snr_db);
        if (e->has_rssi)
            sb_printf(&sb, ", \"lab:rssi_dbfs\": %.1f", e->rssi_dbfs);
        sb_puts(&sb, ", \"lab:text\": ");
        sb_json_str(&sb, e->text);
        sb_puts(&sb, "}");
        nv++;
    }
    /* --- unknown: cada región del detector menos los tramos cubiertos por eventos --- */
    for (size_t r = 0; r < l->nreg; r++) {
        const lab_reg_t *g = &l->reg[r];
        /* Inicio: la señal se ve tras DET_N_ON imágenes seguidas.
         * Fin: el detector promedia el espectro con constante de tiempo tau = 1 s, así que una señal
         * que se apaga con SNR_pico dB sobre el suelo sigue visible tau·(SNR_pico − SNR_umbral)/8,69 s
         * (decaimiento exponencial, 8,69 dB por constante de tiempo). La SNR de la última imagen es la
         * que tenía la señal en su última detección real (el detector repite ese valor mientras
         * mantiene la pista DET_M_OFF−1 imágenes), así que el instante en que se apagó es
         *   fin = última_detección_real − tau·(SNR_pico − SNR_última)/8,69.
         * Una región abierta al acabar el fichero con SNR estable (caída < 3 dB) dura hasta el final. */
        const double drop_db = g->snr_db - g->snr_last;
        int64_t a = (int64_t)g->first - (int64_t)(DET_N_ON * step);
        int64_t b;
        if (g->open && drop_db < 3.0)
            b = (int64_t)total_samples;
        else {
            int64_t real_last = (int64_t)g->last - (g->open ? 0 : (int64_t)((DET_M_OFF - 1) * step));
            b = real_last - (int64_t)(l->rate * drop_db / 8.69);
        }
        if (a < 0)
            a = 0;
        if (b > (int64_t)total_samples)
            b = (int64_t)total_samples;
        if (b < a + (int64_t)step)
            b = a + (int64_t)step;
        /* Margen alrededor de cada evento: el detector promedia con tau = 1 s, así que una ráfaga corta
         * deja huella en el espectro medio mucho después de acabar: tau·(SNR_pico − 6 dB)/8,69 s como
         * cota (≥ 1 s). Esa huella no es una señal sin explicar y no se anota como unknown. */
        const int64_t pad = MAX((int64_t)(2 * step), (int64_t)(l->rate * (g->snr_db - 6.0) / 8.69));
        double flo = g->freq_hz - g->bw_hz / 2, fhi = g->freq_hz + g->bw_hz / 2;
        /* tramos cubiertos por eventos que solapan en frecuencia */
        size_t nc = 0, cap = 16;
        span_t *cov = malloc(cap * sizeof *cov);
        if (!cov)
            continue;
        for (size_t i = 0; i < l->nev; i++) {
            const lab_ev_t *e = &l->ev[i];
            double elo = (double)e->freq_hz - e->bw_hz / 2, ehi = (double)e->freq_hz + e->bw_hz / 2;
            if (ehi < flo || elo > fhi)
                continue;
            int64_t e1 = e->s1 >= e->s0 ? e->s1 : e->s0;
            if (e1 + pad < a || e->s0 - pad > b)
                continue;
            if (nc == cap) {
                cap *= 2;
                span_t *t = realloc(cov, cap * sizeof *cov);
                if (!t)
                    break;
                cov = t;
            }
            cov[nc++] = (span_t){e->s0 - pad, e1 + pad};
        }
        qsort(cov, nc, sizeof *cov, cmp_span);
        int64_t cur = a;
        for (size_t i = 0; i <= nc; i++) {
            int64_t stop = i < nc ? cov[i].a : b;
            if (stop > b)
                stop = b;
            if (stop - cur >= (int64_t)step) {
                SEP();
                ann_common(&sb, cur, stop - cur, flo, fhi, "unknown", "unknown");
                sb_printf(&sb, ", \"lab:snr_db\": %.1f, \"lab:precision\": \"detector\"}", g->snr_db);
                nu++;
            }
            if (i < nc && cov[i].b > cur)
                cur = cov[i].b;
        }
        free(cov);
    }
    /* --- gap --- */
    for (size_t i = 0; i < l->ngap; i++) {
        const lab_gap_t *g = &l->gap[i];
        double bw = ev_bw_hz(g->proto, l->rate);
        SEP();
        ann_common(&sb, (int64_t)g->s0, (int64_t)g->n, (double)g->freq_hz - bw / 2, (double)g->freq_hz + bw / 2, "gap",
                   "gap");
        sb_puts(&sb, ", \"lab:proto\": ");
        sb_json_str(&sb, g->proto);
        sb_puts(&sb, "}");
        ng++;
    }
#undef SEP
    sb_puts(&sb, "\n  ]\n}\n");
    pthread_mutex_unlock(&l->m);

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errlen, "no se pudo escribir %s", path);
        sb_free(&sb);
        return -1;
    }
    fwrite(sb.data, 1, sb.len, f);
    fclose(f);
    sb_free(&sb);
    if (n_verified)
        *n_verified = nv;
    if (n_unknown)
        *n_unknown = nu;
    if (n_gap)
        *n_gap = ng;
    return 0;
}
