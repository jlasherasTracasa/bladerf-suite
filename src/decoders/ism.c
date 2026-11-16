/*
 * ism.c — Decodificador ISM genérico OOK/FSK (433/868 MHz). Ver docs/decoders/ism.md.
 *
 * Cadena (a 250 kHz complejos, canal de 200 kHz):
 *
 *   IQ ──┬─ |x| (potencia media de 8 muestras → envolvente) ── umbral adaptativo ── OOK
 *        └─ búfer de la ráfaga ── (al terminar) FSK: ver más abajo
 *
 *   Umbral adaptativo (OOK). Se siguen dos niveles de la envolvente: el suelo de
 *   ruido nf (media lenta de lo que queda por debajo del umbral) y el pico pk
 *   (ataque rápido, caída lenta de ~200 ms). Si pk > 2,5·nf hay señal útil y el
 *   umbral está a nf + 0,55·(pk − nf) para subir y nf + 0,40·(pk − nf) para bajar
 *   (histéresis). Funciona sin conocer la ganancia ni la distancia.
 *
 *   Ráfaga. Empieza cuando la envolvente supera el umbral y termina tras 12 ms
 *   sin superarlo. Mientras dura se guarda el IQ y se construye el tren OOK
 *   (marca = envolvente por encima del umbral). Si la envolvente no ha bajado
 *   nunca del umbral (portadora continua) la ráfaga es FSK y se demodula DESPUÉS,
 *   con toda la ráfaga a la vista:
 *     1. desvío de portadora = argumento de Σ x[n]·conj(x[n−1]) (media de las dos
 *        frecuencias, exacta si los bits están equilibrados);
 *     2. se baja a banda base y se filtra paso bajo (±55 kHz): ancho 2× menor que
 *        el canal → +3 dB de relación señal/ruido;
 *     3. discriminador de retardo 3, arg(y[n]·conj(y[n−3])): la señal acumula 3×
 *        más fase y el ruido de "clic" del FM pierde peso (alcance ±41 kHz);
 *     4. mediana de 5 y decisión contra la media: tren de marcas/espacios.
 *
 *   Después se pasa el tren por los protocolos conocidos (ism_protocols.c), que
 *   validan suma/CRC, y, si ninguno encaja y la ráfaga se repite, por el
 *   analizador genérico estilo URH.
 *
 * Eventos: {"kind":"sensor"|"generic", ...}. state_json: tabla de dispositivos.
 */
#include "decoder.h"
#include "ism_protocols.h"
#include "../dsp/filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV_N 8              /* muestras de la envolvente */
#ifndef FSK_LAG
#define FSK_LAG 3
#endif
/* retardo del discriminador de la segunda pasada */
#define HOLD_S 0.012         /* silencio que cierra la ráfaga */
#define MAX_BURST_S 1.5
#define MIN_RUN_OOK_US 40
#ifndef MIN_RUN_FSK_US
#define MIN_RUN_FSK_US 8   /* solo glitches de una o dos muestras: el discriminador ya filtra */
#endif
#ifndef FSK_LP_HZ
#define FSK_LP_HZ 55000.0
#endif
#define MAX_DEV 64
#define DEV_TTL 900.0
#define DUP_WINDOW 1.0       /* s: mismo mensaje repetido no se vuelve a emitir */

typedef struct {
    char model[32];
    uint32_t id;
    int channel;
    ism_msg_t last;
    double t_first, t_last, t_emit;
    unsigned count;
    char hex[48];
} ism_dev_t;

typedef struct {
    double fs;
    /* envolvente */
    float pw[ENV_N];
    int pwi;
    double pwsum;
    float env;
    /* búfer de la ráfaga (IQ) para la demodulación FSK en segunda pasada */
    cf32 *bq;
    int bq_n, bq_cap;
    float lp[63];        /* FIR paso bajo de la segunda pasada */
    int lp_n;
    /* umbral */
    double nf, pk;
    int64_t n;           /* muestras procesadas */
    /* ráfaga */
    bool active;
    int64_t t_start, last_hi;
    bool dipped;         /* la envolvente bajó del umbral durante la ráfaga */
    bool ook_hi;
    int64_t ook_edge;
    ism_train_t ook, fsk;
    double t0;           /* hora de la primera muestra del bloque actual */
    int64_t n_block0;    /* n al empezar el bloque actual */
    double burst_now;
    ism_dev_t dev[MAX_DEV];
    int ndev;
    unsigned bursts, decoded, generic;
    sb_t sb;
} ism_t;

static void *ism_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    ism_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->fs = cfg->rate > 0 ? cfg->rate : 250000.0;
    s->nf = -1;
    s->bq_cap = (int)((MAX_BURST_S + 0.1) * s->fs);
    s->bq = malloc(sizeof(cf32) * (size_t)s->bq_cap);
    s->lp_n = 63;
    fir_design_lowpass(s->lp, s->lp_n, FSK_LP_HZ / s->fs, kaiser_beta(50.0));
    if (!s->bq) {
        free(s);
        return NULL;
    }
    return s;
}

static void ism_destroy(void *st)
{
    ism_t *s = st;
    if (!s)
        return;
    sb_free(&s->sb);
    free(s->bq);
    free(s);
}

static uint32_t to_us(const ism_t *s, int64_t samples) { return (uint32_t)((double)samples * 1e6 / s->fs + 0.5); }

static void push_run(const ism_t *s, ism_train_t *t, int64_t samples)
{
    if (t->n < ISM_MAXD)
        t->d[t->n++] = to_us(s, samples);
}

/* Elimina rachas más cortas que min_us fundiéndolas con sus vecinas (glitches del
 * ruido y flancos del filtro). */
static void clean_train(ism_train_t *t, uint32_t min_us)
{
    int i = 1;
    while (i < t->n - 1) {
        if (t->d[i] < min_us) {
            t->d[i - 1] += t->d[i] + t->d[i + 1];
            memmove(&t->d[i], &t->d[i + 2], sizeof(uint32_t) * (size_t)(t->n - i - 2));
            t->n -= 2;
            if (i > 1)
                i--;
        } else {
            i++;
        }
    }
    if (t->n >= 2 && t->d[0] < min_us)
        t->d[0] = 0;
}

/* ---- Tabla de dispositivos -------------------------------------------- */
static ism_dev_t *dev_get(ism_t *s, const ism_msg_t *m, double now, bool *dup)
{
    for (int i = 0; i < s->ndev; i++) {
        ism_dev_t *d = &s->dev[i];
        if (d->id == m->id && d->channel == m->channel && !strcmp(d->model, m->model)) {
            *dup = (now - d->t_emit < DUP_WINDOW) && !strcmp(d->hex, m->hex);
            return d;
        }
    }
    *dup = false;
    int k = -1;
    if (s->ndev < MAX_DEV)
        k = s->ndev++;
    else {   /* se sustituye la más antigua */
        double old = 1e300;
        for (int i = 0; i < s->ndev; i++)
            if (s->dev[i].t_last < old) {
                old = s->dev[i].t_last;
                k = i;
            }
    }
    ism_dev_t *d = &s->dev[k];
    memset(d, 0, sizeof *d);
    snprintf(d->model, sizeof d->model, "%s", m->model);
    d->id = m->id;
    d->channel = m->channel;
    d->t_first = now;
    return d;
}

static void sensor_text(const ism_msg_t *m, char *txt, size_t cap)
{
    size_t o = (size_t)snprintf(txt, cap, "%s id %u", m->model, m->id);
    if (m->channel && o < cap)
        o += (size_t)snprintf(txt + o, cap - o, " ch %d", m->channel);
    if ((m->has & ISM_HAS_TEMP) && o < cap)
        o += (size_t)snprintf(txt + o, cap - o, "  %.1f °C", m->temp_c);
    if ((m->has & ISM_HAS_HUM) && o < cap)
        o += (size_t)snprintf(txt + o, cap - o, "  %.0f %%", m->humidity);
    if ((m->has & ISM_HAS_PRESS) && o < cap)
        o += (size_t)snprintf(txt + o, cap - o, "  %.0f kPa", m->pressure_kpa);
    if ((m->has & ISM_HAS_WIND) && o < cap)
        o += (size_t)snprintf(txt + o, cap - o, "  viento %.1f m/s", m->wind_avg_ms);
    if ((m->has & ISM_HAS_BATT) && m->batt_low && o < cap)
        snprintf(txt + o, cap - o, "  batería baja");
}

static void emit_sensor(ism_t *s, const ism_msg_t *m, bool fsk, double now, dec_emit_fn emit, void *ctx)
{
    bool dup;
    ism_dev_t *d = dev_get(s, m, now, &dup);
    d->last = *m;
    d->last.model = d->model;
    d->t_last = now;
    d->count++;
    snprintf(d->hex, sizeof d->hex, "%s", m->hex);
    if (dup)
        return;
    d->t_emit = now;
    char txt[200];
    sensor_text(m, txt, sizeof txt);
    sb_t *b = &s->sb;
    sb_clear(b);
    sb_printf(b, "{\"kind\":\"sensor\",\"model\":");
    sb_json_str(b, m->model);
    sb_printf(b, ",\"id\":%u,\"modulation\":\"%s\"", m->id, fsk ? "FSK" : "OOK");
    if (m->channel)
        sb_printf(b, ",\"channel\":%d", m->channel);
    if (m->has & ISM_HAS_TEMP) {
        sb_puts(b, ",\"temperature_c\":");
        sb_json_num(b, m->temp_c, 1);
    }
    if (m->has & ISM_HAS_HUM) {
        sb_puts(b, ",\"humidity\":");
        sb_json_num(b, m->humidity, 0);
    }
    if (m->has & ISM_HAS_PRESS) {
        sb_puts(b, ",\"pressure_kpa\":");
        sb_json_num(b, m->pressure_kpa, 1);
    }
    if (m->has & ISM_HAS_WIND) {
        sb_puts(b, ",\"wind_avg_ms\":");
        sb_json_num(b, m->wind_avg_ms, 2);
        sb_puts(b, ",\"wind_gust_ms\":");
        sb_json_num(b, m->wind_gust_ms, 2);
    }
    if (m->has & ISM_HAS_DIR)
        sb_printf(b, ",\"wind_dir_deg\":%d", m->wind_dir_deg);
    if (m->has & ISM_HAS_RAIN) {
        sb_puts(b, ",\"rain_mm\":");
        sb_json_num(b, m->rain_mm, 1);
    }
    if (m->has & ISM_HAS_BATT)
        sb_printf(b, ",\"battery_low\":%s", m->batt_low ? "true" : "false");
    sb_printf(b, ",\"hex\":\"%s\",\"text\":", m->hex);
    sb_json_str(b, txt);
    sb_puts(b, "}");
    emit(ctx, b->data, b->len);
}

static void emit_generic(ism_t *s, const ism_generic_t *g, bool fsk, dec_emit_fn emit, void *ctx)
{
    (void)fsk;
    char txt[200];
    snprintf(txt, sizeof txt, "%s %d bits ×%d filas, corto %.0f µs, largo %.0f µs: %s", g->mod, g->nbits, g->repeats,
             g->short_us, g->long_us, g->hex);
    sb_t *b = &s->sb;
    sb_clear(b);
    sb_puts(b, "{\"kind\":\"generic\",\"modulation\":");
    sb_json_str(b, g->mod);
    sb_puts(b, ",\"short_us\":");
    sb_json_num(b, g->short_us, 0);
    sb_puts(b, ",\"long_us\":");
    sb_json_num(b, g->long_us, 0);
    sb_puts(b, ",\"gap_us\":");
    sb_json_num(b, g->gap_us, 0);
    sb_puts(b, ",\"bit_us\":");
    sb_json_num(b, g->bit_us, 0);
    sb_printf(b, ",\"bits\":%d,\"repeats\":%d,\"rows\":%d,\"hex\":\"%s\",\"text\":", g->nbits, g->repeats, g->nrows,
              g->hex);
    sb_json_str(b, txt);
    sb_puts(b, "}");
    emit(ctx, b->data, b->len);
}

/* ---- Demodulación FSK de la ráfaga completa --------------------------- */
static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void fsk_demod(ism_t *s)
{
    s->fsk.n = 0;
    /* El filtro (retardo h) y el discriminador (retardo FSK_LAG) recortan los
     * extremos: se incluyen h + FSK_LAG + 20 muestras tras el último instante alto
     * para no perder los últimos bits. */
    int n = (int)MIN((int64_t)s->bq_n, s->last_hi - s->t_start + 1 + s->lp_n / 2 + FSK_LAG + 20);
    if (n < 64 + s->lp_n)
        return;
    cf32 *x = s->bq;
    /* 1) desvío de portadora */
    double sr = 0, si = 0;
    for (int i = 1; i < n; i++) {
        sr += x[i].re * x[i - 1].re + x[i].im * x[i - 1].im;
        si += x[i].im * x[i - 1].re - x[i].re * x[i - 1].im;
    }
    double w0 = atan2(si, sr);   /* rad/muestra */
    /* 2) a banda base (rotación recursiva) y paso bajo */
    cf32 *y = malloc(sizeof(cf32) * (size_t)n);
    float *f = malloc(sizeof(float) * (size_t)n);
    if (!y || !f) {
        free(y);
        free(f);
        return;
    }
    double cr = 1, ci = 0, dr = cos(-w0), di = sin(-w0);
    for (int i = 0; i < n; i++) {
        y[i].re = (float)(x[i].re * cr - x[i].im * ci);
        y[i].im = (float)(x[i].re * ci + x[i].im * cr);
        double nr = cr * dr - ci * di, ni = cr * di + ci * dr;
        double mag = sqrt(nr * nr + ni * ni);
        cr = nr / mag;
        ci = ni / mag;
    }
    int h = s->lp_n / 2;
    for (int i = h; i < n - h; i++) {
        float ar = 0, ai = 0;
        for (int k = 0; k < s->lp_n; k++) {
            ar += s->lp[k] * y[i - h + k].re;
            ai += s->lp[k] * y[i - h + k].im;
        }
        x[i].re = ar;   /* se reutiliza el búfer como salida filtrada */
        x[i].im = ai;
    }
    /* 3) discriminador de retardo FSK_LAG, en Hz */
    int m = 0;
    for (int i = h + FSK_LAG; i < n - h; i++) {
        float re = x[i].re * x[i - FSK_LAG].re + x[i].im * x[i - FSK_LAG].im;
        float im = x[i].im * x[i - FSK_LAG].re - x[i].re * x[i - FSK_LAG].im;
        f[m++] = (float)(atan2f(im, re) * s->fs / (2.0 * M_PI * FSK_LAG));
    }
    /* 4) mediana de 5 y umbral = media */
    for (int i = 0; i + 4 < m; i++) {
        float w[5] = {f[i], f[i + 1], f[i + 2], f[i + 3], f[i + 4]};
        qsort(w, 5, sizeof(float), cmp_f);
        y[i].re = w[2];
    }
    int mm = m - 4;
    double mean = 0;
    for (int i = 0; i < mm; i++)
        mean += y[i].re;
    mean /= mm > 0 ? mm : 1;
    bool lvl = mm > 0 && y[0].re > mean;
    int64_t edge = 0;
    if (!lvl)
        push_run(s, &s->fsk, 0);
    for (int i = 1; i < mm; i++) {
        bool l = y[i].re > mean;
        if (l != lvl) {
            push_run(s, &s->fsk, i - edge);
            edge = i;
            lvl = l;
        }
    }
    push_run(s, &s->fsk, mm - edge);
    clean_train(&s->fsk, MIN_RUN_FSK_US);
    free(y);
    free(f);
}

/* ---- Fin de ráfaga ------------------------------------------------------ */
static void end_burst(ism_t *s, dec_emit_fn emit, void *ctx)
{
    s->active = false;
    /* Cierra la última racha: las marcas terminan en last_hi; el hueco final se descarta. */
    if (s->ook_hi)
        push_run(s, &s->ook, s->last_hi - s->ook_edge);
    clean_train(&s->ook, MIN_RUN_OOK_US);
    bool fsk = !s->dipped || s->ook.n <= 3;
    if (fsk)
        fsk_demod(s);
    if (s->ook.n < 8 && s->fsk.n < 8)
        return;
    s->bursts++;
    ism_train_t *t = fsk ? &s->fsk : &s->ook;
    if (t->n > 0 && (t->n & 1) == 0)
        t->n--;   /* termina en marca */
    double now = s->burst_now;
    ism_msg_t msgs[4];
    int nm = ism_decode_train(t, fsk, msgs, 4);
    for (int i = 0; i < nm; i++) {
        s->decoded++;
        emit_sensor(s, &msgs[i], fsk, now, emit, ctx);
    }
    if (nm == 0) {
        ism_generic_t g;
        if (ism_analyze(t, fsk, &g)) {
            s->generic++;
            emit_generic(s, &g, fsk, emit, ctx);
        }
    }
}

static void begin_burst(ism_t *s, float thr_hi)
{
    (void)thr_hi;
    s->active = true;
    s->t_start = s->n;
    s->last_hi = s->n;
    s->dipped = false;
    s->ook.n = s->fsk.n = 0;
    s->ook_hi = true;
    s->ook_edge = s->n;
    s->bq_n = 0;
    s->burst_now = s->t0 + (double)(s->n - s->n_block0) / s->fs;
}

static void ism_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    ism_t *s = st;
    s->t0 = now;
    s->n_block0 = s->n;
    const double a_nf = 1.0 / 2000.0, a_nf_slow = 1.0 / 8000.0, pk_decay = 1.0 / 50000.0;
    const int hold = (int)(HOLD_S * s->fs), max_burst = (int)(MAX_BURST_S * s->fs);
    for (int i = 0; i < n; i++, s->n++) {
        cf32 x = iq[i];
        /* envolvente: raíz de la potencia media de ENV_N muestras */
        float p = x.re * x.re + x.im * x.im;
        s->pwsum += p - s->pw[s->pwi];
        s->pw[s->pwi] = p;
        s->pwi = (s->pwi + 1) % ENV_N;
        s->env = sqrtf((float)(s->pwsum > 0 ? s->pwsum : 0) / ENV_N);
        /* niveles de ruido y pico */
        if (s->nf < 0)
            s->nf = s->env;
        if (s->env > s->pk)
            s->pk += (s->env - s->pk) * 0.3;
        else
            s->pk += (s->nf - s->pk) * pk_decay;
        bool armed = s->pk > 2.5 * s->nf && s->n > 2000;
        double span = s->pk - s->nf;
        double thr_on = s->nf + 0.55 * span, thr_off = s->nf + 0.40 * span;
        bool hi;
        if (!s->active)
            hi = armed && s->env > thr_on;
        else
            hi = s->ook_hi ? s->env > thr_off : s->env > thr_on;
        if (!hi)
            s->nf += (s->env - s->nf) * (s->active ? a_nf_slow : a_nf);

        if (!s->active) {
            if (hi) {
                begin_burst(s, (float)thr_on);
                s->bq[s->bq_n++] = x;
            }
            continue;
        }
        if (hi)
            s->last_hi = s->n;
        /* tren OOK */
        if (hi != s->ook_hi) {
            push_run(s, &s->ook, s->n - s->ook_edge);
            s->ook_edge = s->n;
            s->ook_hi = hi;
            if (!hi)
                s->dipped = true;
        }
        if (s->bq_n < s->bq_cap)
            s->bq[s->bq_n++] = x;
        if (s->n - s->last_hi > hold || s->n - s->t_start > max_burst)
            end_burst(s, emit, ctx);
    }
}

static void ism_state_json(void *st, sb_t *sb, double now)
{
    ism_t *s = st;
    int w = 0;
    for (int i = 0; i < s->ndev; i++)
        if (now - s->dev[i].t_last <= DEV_TTL)
            s->dev[w++] = s->dev[i];
    s->ndev = w;
    sb_printf(sb, "{\"bursts\":%u,\"decoded\":%u,\"generic\":%u,\"devices\":[", s->bursts, s->decoded, s->generic);
    for (int i = 0; i < s->ndev; i++) {
        const ism_dev_t *d = &s->dev[i];
        if (i)
            sb_puts(sb, ",");
        sb_puts(sb, "{\"model\":");
        sb_json_str(sb, d->model);
        sb_printf(sb, ",\"id\":%u,\"channel\":%d", d->id, d->channel);
        if (d->last.has & ISM_HAS_TEMP) {
            sb_puts(sb, ",\"temperature_c\":");
            sb_json_num(sb, d->last.temp_c, 1);
        }
        if (d->last.has & ISM_HAS_HUM) {
            sb_puts(sb, ",\"humidity\":");
            sb_json_num(sb, d->last.humidity, 0);
        }
        if (d->last.has & ISM_HAS_PRESS) {
            sb_puts(sb, ",\"pressure_kpa\":");
            sb_json_num(sb, d->last.pressure_kpa, 1);
        }
        if (d->last.has & ISM_HAS_BATT)
            sb_printf(sb, ",\"battery_low\":%s", d->last.batt_low ? "true" : "false");
        sb_printf(sb, ",\"count\":%u,\"age_s\":", d->count);
        sb_json_num(sb, now - d->t_last, 0);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_ISM = {
    .id = "ism",
    .name = "ISM genérico",
    .group = "Sensores",
    .desc = "Analizador de pulsos OOK/FSK (estilo URH) y sensores ISM comunes con suma/CRC.",
    .kind = DEC_NARROW,
    .rate = 250000,
    .bw_hz = 200000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {433920000LL, 868300000LL, 0LL, 0LL},
    .create = ism_create,
    .destroy = ism_destroy,
    .process = ism_process,
    .state_json = ism_state_json,
};
