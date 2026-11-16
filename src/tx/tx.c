/*
 * tx.c — Emisión con política de seguridad. Ver tx.h y docs/TX.md.
 *
 * Flujo de una orden:
 *   1. modulator_find() + modulate()  → banda base compleja a la tasa del modulador.
 *   2. Medida del ancho ocupado (Welch con FFT de 1024, bordes al 0,01 % de
 *      la potencia acumulada por cada lado, sobre TODA la señal).
 *   3. Remuestreo complejo (cresamp_t) a la tasa de captura/emisión y
 *      desplazamiento (freq − center) con un NCO.
 *   4. Según el modo: escribir SigMF, o comprobar la política y enviar al
 *      bladeRF por source_tx_*() desde un hilo propio (precorregida con la
 *      calibración de cal.h si hay una vigente para ese centro/tasa).
 *
 * Revisión de seguridad (T1) sobre la versión de A1; huecos cerrados:
 *   - Una señal de < 1024 muestras medía ancho 0 y pasaba cualquier banda; la
 *     medida solo miraba el principio de señales largas; un fallo de memoria
 *     devolvía ancho 0. Ahora se rellena a 1024, se barre la señal entera y un
 *     fallo es un error.
 *   - La imagen especular del IQ (2·centro − f) no se comprobaba: ahora debe
 *     caer también dentro de la banda.
 *   - El ciclo de trabajo se anotaba al terminar (y no si fallaba o se
 *     interrumpía), se perdía al llenarse la tabla de 64 y al reiniciar el
 *     programa. Ahora se anota al lanzar, la tabla se funde (conservador) y se
 *     persiste en <recdir>/tx_log.jsonl.
 *   - La duración se calculaba antes del remuestreo; ahora sale de las muestras
 *     realmente enviadas.
 *   - Dos órdenes simultáneas podían lanzar dos hilos de emisión (sin cerrojo
 *     en tx_submit); tasa, repeat y tamaños sin acotar (memoria); indicativo
 *     sin validar; modo desconocido.
 */
#include "tx.h"

#include "../decoders/decoder.h"
#include "../dsp/fft.h"
#include "../dsp/filter.h"
#include "../util/json.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TX_MAX_SECONDS 5.0          /* por orden en air */
#define TX_LOOPBACK_MAX_SECONDS 30.0
#define TX_LOOPBACK_GAIN_DB (-20)   /* cerca del mínimo del rango de TX del bladeRF 2.0 */
#define DUTY_WINDOW_S 3600.0
#define DUTY_LIMIT 0.01
#define CHUNK 8192
#define TX_MAX_SAMPLES 40000000     /* una señal renderizada: 320 MB como cf32 */
#define TX_MAX_FILE_SAMPLES 100000000LL /* total escrito en un fichero (repeat incluido) */
#define TX_MIN_RATE 8000.0
#define TX_MAX_RATE 61440000.0
#define TX_MAX_FREQ 6000000000LL
#define HIST_N 64
#define OCC_TAIL 1e-4               /* fracción de potencia fuera, por lado */

typedef struct {
    int64_t lo, hi;
    const char *name;
    bool ham;    /* exige indicativo */
    bool duty;   /* ciclo de trabajo ≤ 1 % por hora */
} band_t;

/* Lista blanca (AGENTS.md §6). Todo lo que no esté aquí se rechaza. */
static const band_t BANDS[] = {
    {433050000LL, 434790000LL, "ISM 433 MHz", false, false},
    {868000000LL, 868600000LL, "SRD 868,0-868,6 MHz (1 % ciclo)", false, true},
    {869400000LL, 869650000LL, "SRD 869,4-869,65 MHz", false, false},
    {144000000LL, 146000000LL, "Radioaficionado 2 m", true, false},
    {430000000LL, 440000000LL, "Radioaficionado 70 cm", true, false},
};

typedef struct {
    double t_end; /* hora de pared (s desde 1970) del final de la emisión */
    double seconds;
} emission_t;

struct tx {
    source_t *src;
    server_t *srv;
    char recdir[512];
    tx_policy_t pol;
    pthread_mutex_t lock;   /* estado compartido con el hilo de emisión */
    pthread_mutex_t submit; /* serializa tx_submit / tx_play / tx_cal_run */
    pthread_t th;
    bool th_valid, busy;
    volatile bool stop;
    emission_t hist[HIST_N]; /* emisiones recientes en 868,0–868,6 (ciclo de trabajo) */
    int nhist;
    tx_sink_t sink;
    bool has_sink;
    tx_rx_capture_fn rx_cap;
    void *rx_ctx;
    cal_t cal;
    bool cal_valid;
    int64_t cal_center;
    uint32_t cal_rate;
};

/* ======================================================================= */
/*  Medida del ancho ocupado                                               */
/* ======================================================================= */
#define OCC_N 1024

/* Devuelve 0 o <0 si no se pudo medir (el llamador debe rechazar la emisión). */
static int measure_occupancy(const iqbuf_t *b, double *lo, double *hi)
{
    int rc = -1;
    fft_plan_t *plan = fft_plan_create(OCC_N);
    float *w = malloc(sizeof(float) * OCC_N);
    cf32 *x = malloc(sizeof(cf32) * OCC_N);
    double *p = calloc(OCC_N, sizeof(double));
    *lo = *hi = 0;
    if (!plan || !w || !x || !p || b->n <= 0)
        goto out;
    window_blackman_harris(w, OCC_N);
    /* Segmentos con 50 % de solape. Las señales largas se barren enteras con
     * un paso mayor (≤ 4000 segmentos); las cortas (< OCC_N) se rellenan con
     * ceros: antes medían ancho 0. */
    int step = OCC_N / 2;
    if (b->n > 4000 * step)
        step = b->n / 4000;
    int nseg = b->n <= OCC_N ? 1 : (b->n - OCC_N) / step + 1;
    for (int sg = 0; sg < nseg; sg++) {
        int s = b->n <= OCC_N ? 0 : MIN(sg * step, b->n - OCC_N);
        for (int i = 0; i < OCC_N; i++) {
            bool in = s + i < b->n;
            x[i].re = in ? b->s[s + i].re * w[i] : 0.f;
            x[i].im = in ? b->s[s + i].im * w[i] : 0.f;
        }
        fft_exec(plan, x);
        for (int k = 0; k < OCC_N; k++)
            p[k] += (double)x[k].re * x[k].re + (double)x[k].im * x[k].im;
    }
    rc = 0;
    double tot = 0;
    for (int k = 0; k < OCC_N; k++)
        tot += p[k];
    if (!(tot > 0))
        goto out; /* señal nula: ancho 0 (no radia nada salvo la fuga del LO, que ya se controla) */
    /* Índices con fftshift: bin k ↔ frecuencia (k − N/2)·fs/N */
    double acc = 0, bin = b->rate / OCC_N;
    int klo = 0, khi = OCC_N - 1;
    for (int j = 0; j < OCC_N; j++) {
        int k = (j + OCC_N / 2) % OCC_N; /* de −fs/2 a +fs/2 */
        acc += p[k];
        if (acc >= OCC_TAIL * tot) {
            klo = j;
            break;
        }
    }
    acc = 0;
    for (int j = OCC_N - 1; j >= 0; j--) {
        int k = (j + OCC_N / 2) % OCC_N;
        acc += p[k];
        if (acc >= OCC_TAIL * tot) {
            khi = j;
            break;
        }
    }
    *lo = (klo - OCC_N / 2) * bin;
    *hi = (khi + 1 - OCC_N / 2) * bin;
out:
    free(p);
    free(x);
    free(w);
    fft_plan_destroy(plan);
    return rc;
}

/* ======================================================================= */
/*  Renderizado                                                            */
/* ======================================================================= */
int tx_render(const modulator_desc_t *m, const char *params, int64_t freq, int64_t center, double rate,
              iqbuf_t *out, tx_meta_t *meta, char *err, size_t errlen)
{
    if (!(rate == 0 || (rate >= TX_MIN_RATE && rate <= TX_MAX_RATE)) || freq < 0 || freq > TX_MAX_FREQ || center < 0 ||
        center > TX_MAX_FREQ) {
        snprintf(err, errlen, "frecuencia, centro o tasa fuera de rango");
        return -1;
    }
    iqbuf_t base = {0};
    err[0] = '\0';
    if (m->modulate(params, &base, err, errlen) < 0 || base.n <= 0 || !(base.rate > 0)) {
        if (!err[0])
            snprintf(err, errlen, "el modulador %s no generó señal", m->id);
        iqbuf_free(&base);
        return -1;
    }
    const decoder_desc_t *dd = decoder_find(m->id);
    bool wide = dd && dd->kind == DEC_WIDE;
    if (freq <= 0 && dd && dd->freqs[0])
        freq = dd->freqs[0];
    if (rate <= 0)
        rate = base.rate >= 1e6 ? base.rate : 1e6;
    if ((double)base.n * rate / base.rate > TX_MAX_SAMPLES) {
        snprintf(err, errlen, "la señal sería demasiado larga (> %d muestras)", TX_MAX_SAMPLES);
        iqbuf_free(&base);
        return -1;
    }
    /* Centro automático: los decodificadores estrechos se desplazan a propósito
     * para no caer sobre el pico de continua; los de banda ancha se quedan en
     * el centro (su señal ocupa toda la banda y un desplazamiento la plegaría). */
    if (center == 0)
        center = wide ? freq : freq - (int64_t)(rate / 4.0);
    tx_meta_t mt = {.center = center, .rate = rate, .seconds = base.n / base.rate};
    if (measure_occupancy(&base, &mt.lo_hz, &mt.hi_hz) < 0) {
        snprintf(err, errlen, "no se pudo medir el ancho ocupado");
        iqbuf_free(&base);
        return -1;
    }
    mt.occ_bw_hz = mt.hi_hz - mt.lo_hz;

    double shift = (double)(freq - center);
    if (fabs(shift) + MAX(fabs(mt.lo_hz), fabs(mt.hi_hz)) > 0.5 * rate * 1.0001) {
        snprintf(err, errlen, "la señal (%.0f Hz de ancho a %+.0f Hz del centro) no cabe en %.0f sps", mt.occ_bw_hz,
                 shift, rate);
        iqbuf_free(&base);
        return -1;
    }

    /* Remuestreo (si hace falta) y desplazamiento */
    iqbuf_t *src = &base;
    iqbuf_t rs = {0};
    if (fabs(base.rate - rate) > 1e-6) {
        cresamp_t r;
        double nyq = 0.5 * MIN(base.rate, rate);
        if (cresamp_init(&r, base.rate, rate, 0.8 * nyq, 0.2 * nyq) < 0) {
            snprintf(err, errlen, "no se pudo crear el remuestreador");
            iqbuf_free(&base);
            return -1;
        }
        int pad = r.L + 16; /* ceros finales: vacían la línea de retardo del filtro */
        iqbuf_reserve(&base, pad);
        memset(base.s + base.n, 0, sizeof(cf32) * (size_t)pad);
        base.n += pad;
        int cap = (int)((double)base.n * rate / base.rate) + 16;
        rs.s = malloc(sizeof(cf32) * (size_t)cap);
        if (!rs.s) {
            snprintf(err, errlen, "sin memoria");
            cresamp_free(&r);
            iqbuf_free(&base);
            return -1;
        }
        rs.cap = cap;
        rs.n = cresamp_process(&r, base.s, base.n, rs.s);
        rs.rate = rate;
        cresamp_free(&r);
        src = &rs;
    }
    if (shift != 0.0) {
        nco_t o;
        nco_init(&o, shift, rate);
        nco_mix(&o, src->s, src->n, src->s);
    }
    *out = *src;
    out->rate = rate;
    if (src == &rs)
        iqbuf_free(&base);
    else
        base.s = NULL; /* la memoria pasa a 'out' */
    if (meta)
        *meta = mt;
    return 0;
}

/* ======================================================================= */
/*  SigMF                                                                  */
/* ======================================================================= */
static int16_t to_q11(float v)
{
    float s = v * 2047.0f;
    return (int16_t)(s > 2047.f ? 2047 : (s < -2047.f ? -2047 : lrintf(s)));
}

int tx_write_sigmf(const char *prefix, const iqbuf_t *iq, int repeat, int64_t center, const char *desc, char *err,
                   size_t errlen)
{
    char path[1100];
    snprintf(path, sizeof path, "%s.sigmf-data", prefix);
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errlen, "no se puede crear %s: %s", path, strerror(errno));
        return -1;
    }
    int16_t *buf = malloc(sizeof(int16_t) * 2 * (size_t)iq->n);
    if (!buf) {
        fclose(f);
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    for (int i = 0; i < iq->n; i++) {
        buf[2 * i] = to_q11(iq->s[i].re);
        buf[2 * i + 1] = to_q11(iq->s[i].im);
    }
    for (int r = 0; r < MAX(repeat, 1); r++)
        fwrite(buf, sizeof(int16_t) * 2, (size_t)iq->n, f);
    free(buf);
    if (fclose(f) != 0) {
        snprintf(err, errlen, "error escribiendo %s", path);
        return -1;
    }
    time_t t = time(NULL);
    struct tm utc;
    gmtime_r(&t, &utc);
    sb_t s = {0};
    sb_puts(&s, "{\n  \"global\": {\n    \"core:datatype\": \"ci16_le\",\n");
    sb_printf(&s, "    \"core:sample_rate\": %.0f,\n    \"core:version\": \"1.0.0\",\n", iq->rate);
    sb_puts(&s, "    \"core:hw\": \"BladeRF Lab (generado)\",\n    \"core:recorder\": \"" APP_NAME " " APP_VERSION
                "\",\n    \"core:description\": ");
    sb_json_str(&s, desc ? desc : "");
    sb_puts(&s, ",\n    \"bladerf_lab:full_scale\": 2048,\n    \"bladerf_lab:dropped_samples\": 0\n  },\n");
    sb_printf(&s,
              "  \"captures\": [\n    {\n      \"core:sample_start\": 0,\n      \"core:frequency\": %lld,\n"
              "      \"core:datetime\": \"%04d-%02d-%02dT%02d:%02d:%02dZ\"\n    }\n  ],\n  \"annotations\": []\n}\n",
              (long long)center, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
              utc.tm_sec);
    snprintf(path, sizeof path, "%s.sigmf-meta", prefix);
    f = fopen(path, "w");
    if (!f) {
        snprintf(err, errlen, "no se puede crear %s: %s", path, strerror(errno));
        sb_free(&s);
        return -1;
    }
    fwrite(s.data, 1, s.len, f);
    fclose(f);
    sb_free(&s);
    return 0;
}

/* ======================================================================= */
/*  Política                                                               */
/* ======================================================================= */
static const band_t *band_for(int64_t lo, int64_t hi)
{
    if (lo > hi)
        return NULL;
    for (size_t i = 0; i < ARRAY_LEN(BANDS); i++)
        if (lo >= BANDS[i].lo && hi <= BANDS[i].hi)
            return &BANDS[i];
    return NULL;
}

bool tx_callsign_valid(const char *cs)
{
    if (!cs)
        return false;
    size_t n = strnlen(cs, 32);
    if (n < 3 || n > 12)
        return false;
    bool letter = false, digit = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)cs[i];
        if (isalpha(c))
            letter = true;
        else if (isdigit(c))
            digit = true;
        else if (c != '/' && c != '-')
            return false;
    }
    return letter && digit;
}

/* Núcleo de la comprobación; devuelve la banda en *bout para el ciclo de trabajo. */
static int check_air(const tx_policy_t *pol, const modulator_desc_t *m, int64_t freq, int64_t center,
                     const tx_meta_t *meta, int gain_db, double total_seconds, double recent_s, const band_t **bout,
                     char *err, size_t errlen)
{
    if (bout)
        *bout = NULL;
    if (!pol->enabled || !pol->air) {
        snprintf(err, errlen, "emisión por antena desactivada (arrancar con --enable-tx --enable-tx-air)");
        return -1;
    }
    if (!m->air_ok) {
        snprintf(err, errlen, "el protocolo %s no se puede radiar (solo file o loopback)", m->id);
        return -1;
    }
    if (gain_db > pol->max_gain_db) {
        snprintf(err, errlen, "ganancia %d dB por encima del máximo permitido (%d dB, --tx-max-gain)", gain_db,
                 pol->max_gain_db);
        return -1;
    }
    /* '!(x <= y)' rechaza también NaN */
    if (!(total_seconds > 0) || !(total_seconds <= TX_MAX_SECONDS)) {
        snprintf(err, errlen, "duración %.1f s fuera de (0, %.0f] s por orden", total_seconds, TX_MAX_SECONDS);
        return -1;
    }
    if (!(isfinite(meta->lo_hz) && isfinite(meta->hi_hz)) || meta->lo_hz > meta->hi_hz || freq <= 0 || center <= 0) {
        snprintf(err, errlen, "medida del ancho ocupado no válida");
        return -1;
    }
    /* La señal entera (± su ancho ocupado) debe caer dentro de UNA banda */
    int64_t lo = freq + (int64_t)floor(meta->lo_hz), hi = freq + (int64_t)ceil(meta->hi_hz);
    const band_t *b = band_for(lo, hi);
    if (!b) {
        snprintf(err, errlen, "%.4f–%.4f MHz queda fuera de las bandas permitidas", lo / 1e6, hi / 1e6);
        return -1;
    }
    /* La fuga de la portadora (LO) sale a la frecuencia 'center': también debe estar permitida */
    if (band_for(center, center) != b) {
        snprintf(err, errlen, "el centro del hardware (%.4f MHz) debe estar en la misma banda permitida",
                 center / 1e6);
        return -1;
    }
    /* Imagen especular por desequilibrio IQ: la señal en f aparece también en
     * 2·centro − f (atenuada solo 25–40 dB sin calibrar). */
    int64_t ilo = 2 * center - hi, ihi = 2 * center - lo;
    if (band_for(ilo, ihi) != b) {
        snprintf(err, errlen, "la imagen especular (%.4f–%.4f MHz) saldría de la banda permitida; usa centro = frecuencia",
                 ilo / 1e6, ihi / 1e6);
        return -1;
    }
    if (b->ham && !tx_callsign_valid(pol->callsign)) {
        snprintf(err, errlen, "la banda %s exige un indicativo válido (--callsign)", b->name);
        return -1;
    }
    if (b->duty && recent_s + total_seconds > DUTY_LIMIT * DUTY_WINDOW_S) {
        snprintf(err, errlen, "ciclo de trabajo: máximo %.0f s por hora en %s (ya %.1f s)",
                 DUTY_LIMIT * DUTY_WINDOW_S, b->name, recent_s);
        return -1;
    }
    if (bout)
        *bout = b;
    return 0;
}

int tx_check_air(const tx_policy_t *pol, const modulator_desc_t *m, int64_t freq, int64_t center,
                 const tx_meta_t *meta, int gain_db, double total_seconds, double recent_s, char *err,
                 size_t errlen)
{
    return check_air(pol, m, freq, center, meta, gain_db, total_seconds, recent_s, NULL, err, errlen);
}

/* ---- Historial del ciclo de trabajo (868,0–868,6 MHz) ------------------- */
static double recent_868(tx_t *t, double now)
{
    double sum = 0;
    int w = 0;
    for (int i = 0; i < t->nhist; i++)
        if (now - t->hist[i].t_end < DUTY_WINDOW_S) {
            sum += t->hist[i].seconds;
            t->hist[w++] = t->hist[i];
        }
    t->nhist = w;
    return sum;
}

/* Anota una emisión. Si la tabla está llena se funden las dos más antiguas
 * (suman segundos y conservan el fin más tardío): sobrestima, nunca subestima. */
static void hist_add(tx_t *t, double now, double seconds)
{
    if (t->nhist >= HIST_N) {
        t->hist[1].seconds += t->hist[0].seconds;
        memmove(&t->hist[0], &t->hist[1], sizeof(emission_t) * (HIST_N - 1));
        t->nhist = HIST_N - 1;
    }
    t->hist[t->nhist++] = (emission_t){now + seconds, seconds};
}

/* Registro persistente de emisiones: una línea JSON por emisión en loopback/air.
 * Al arrancar se recargan las de air en la banda con ciclo de trabajo de la
 * última hora (reiniciar el programa no reinicia el 1 %). */
static void log_emission(tx_t *t, const char *mode, const char *proto, int64_t freq, int64_t center, double rate,
                         int gain, double seconds, bool duty)
{
    LOG_I("TX REGISTRO: modo=%s proto=%s freq=%.6f MHz centro=%.6f MHz tasa=%.0f ganancia=%d dB duración=%.2f s", mode,
          proto, freq / 1e6, center / 1e6, rate, gain, seconds);
    if (!t->recdir[0])
        return;
    char path[600];
    snprintf(path, sizeof path, "%s/tx_log.jsonl", t->recdir);
    mkdir(t->recdir, 0755);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "{\"t\":%.3f,\"mode\":\"%s\",\"proto\":\"%s\",\"freq\":%lld,\"center\":%lld,\"rate\":%.0f,\"gain\":%d,"
               "\"seconds\":%.3f,\"duty\":%d}\n",
            now_wall(), mode, proto, (long long)freq, (long long)center, rate, gain, seconds, duty ? 1 : 0);
    fclose(f);
}

static void hist_load(tx_t *t)
{
    char path[600], line[512];
    snprintf(path, sizeof path, "%s/tx_log.jsonl", t->recdir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    double now = now_wall();
    while (fgets(line, sizeof line, f)) {
        double ts, sec;
        int duty = 0;
        char *p = strstr(line, "\"seconds\":");
        char *q = strstr(line, "\"duty\":");
        if (sscanf(line, "{\"t\":%lf", &ts) != 1 || !p || !q || !strstr(line, "\"mode\":\"air\""))
            continue;
        if (sscanf(p, "\"seconds\":%lf", &sec) != 1 || sscanf(q, "\"duty\":%d", &duty) != 1 || !duty)
            continue;
        if (now - (ts + sec) < DUTY_WINDOW_S && sec > 0)
            hist_add(t, ts, sec);
    }
    fclose(f);
}

/* ======================================================================= */
/*  Objeto tx_t                                                            */
/* ======================================================================= */
tx_t *tx_create(source_t *src, server_t *srv, const char *recdir, const tx_policy_t *pol)
{
    tx_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->src = src;
    t->srv = srv;
    snprintf(t->recdir, sizeof t->recdir, "%s", recdir ? recdir : "");
    t->pol = *pol;
    t->pol.callsign[sizeof t->pol.callsign - 1] = '\0';
    pthread_mutex_init(&t->lock, NULL);
    pthread_mutex_init(&t->submit, NULL);
    if (t->recdir[0])
        hist_load(t);
    return t;
}

void tx_wait(tx_t *t)
{
    if (t->th_valid) {
        pthread_join(t->th, NULL);
        t->th_valid = false;
    }
}

void tx_stop(tx_t *t) { t->stop = true; }

void tx_destroy(tx_t *t)
{
    if (!t)
        return;
    t->stop = true;
    tx_wait(t);
    pthread_mutex_destroy(&t->lock);
    pthread_mutex_destroy(&t->submit);
    free(t);
}

bool tx_busy(tx_t *t)
{
    pthread_mutex_lock(&t->lock);
    bool b = t->busy;
    pthread_mutex_unlock(&t->lock);
    return b;
}

void tx_set_sink(tx_t *t, const tx_sink_t *s)
{
    t->has_sink = s != NULL;
    if (s)
        t->sink = *s;
}

void tx_set_rx_capture(tx_t *t, tx_rx_capture_fn fn, void *ctx)
{
    t->rx_cap = fn;
    t->rx_ctx = ctx;
}

bool tx_cal_get(tx_t *t, cal_t *c, int64_t *center, uint32_t *rate)
{
    pthread_mutex_lock(&t->lock);
    bool v = t->cal_valid;
    if (v) {
        if (c)
            *c = t->cal;
        if (center)
            *center = t->cal_center;
        if (rate)
            *rate = t->cal_rate;
    }
    pthread_mutex_unlock(&t->lock);
    return v;
}

void tx_cal_clear(tx_t *t)
{
    pthread_mutex_lock(&t->lock);
    t->cal_valid = false;
    pthread_mutex_unlock(&t->lock);
}

/* Envoltorios del hardware: el sustituto de pruebas o source_tx_*() */
static bool hw_can_tx(tx_t *t) { return t->has_sink || (t->src && source_info(t->src)->can_tx); }

static int hw_configure(tx_t *t, int64_t freq, uint32_t rate, int gain, bool lb)
{
    return t->has_sink ? t->sink.configure(t->sink.ctx, freq, rate, gain, lb)
                       : source_tx_configure(t->src, freq, rate, gain, lb);
}

static int hw_write(tx_t *t, const int16_t *iq, unsigned n)
{
    return t->has_sink ? t->sink.write(t->sink.ctx, iq, n) : source_tx_write(t->src, iq, n);
}

static void hw_stop(tx_t *t)
{
    if (t->has_sink)
        t->sink.stop(t->sink.ctx);
    else
        source_tx_stop(t->src);
}

static const char *hw_error(tx_t *t) { return t->has_sink ? "error del dispositivo de prueba" : source_error(t->src); }

void tx_policy_json(tx_t *t, sb_t *sb)
{
    sb_printf(sb, "{\"type\":\"tx_policy\",\"enabled\":%s,\"air\":%s,\"callsign\":", t->pol.enabled ? "true" : "false",
              t->pol.air ? "true" : "false");
    sb_json_str(sb, t->pol.callsign);
    sb_printf(sb, ",\"max_gain\":%d,\"bands\":[", t->pol.max_gain_db);
    for (size_t i = 0; i < ARRAY_LEN(BANDS); i++) {
        sb_printf(sb, "%s[%lld,%lld,", i ? "," : "", (long long)BANDS[i].lo, (long long)BANDS[i].hi);
        sb_json_str(sb, BANDS[i].name);
        sb_puts(sb, "]");
    }
    sb_printf(sb, "],\"cal\":%s}", t->cal_valid ? "true" : "false");
}

static void send_json(tx_t *t, sb_t *sb)
{
    if (t->srv)
        server_send_text(t->srv, -1, sb->data, sb->len);
}

static void send_result(tx_t *t, bool ok, const char *msg, const char *file, double seconds)
{
    if (ok)
        LOG_I("TX: %s", msg);
    else
        LOG_W("TX rechazada: %s", msg);
    if (!t->srv)
        return;
    sb_t sb = {0};
    sb_printf(&sb, "{\"type\":\"tx_result\",\"ok\":%s,\"msg\":", ok ? "true" : "false");
    sb_json_str(&sb, msg);
    sb_puts(&sb, ",\"file\":");
    if (file)
        sb_json_str(&sb, file);
    else
        sb_puts(&sb, "null");
    sb_printf(&sb, ",\"seconds\":%.3f}", seconds);
    send_json(t, &sb);
    sb_free(&sb);
}

/* Inserta "callsign" en el objeto de parámetros si no lo trae. */
static char *params_with_callsign(const char *params, const char *callsign)
{
    sb_t sb = {0};
    if (!params || !params[0]) {
        sb_puts(&sb, "{\"callsign\":");
        sb_json_str(&sb, callsign);
        sb_puts(&sb, "}");
    } else {
        jtok_t tk[128];
        int n = json_parse(params, strlen(params), tk, 128);
        if (n < 1 || tk[0].type != J_OBJ || json_obj_get(params, tk, 0, "callsign") >= 0) {
            sb_puts(&sb, params);
        } else {
            const char *p = strchr(params, '{');
            sb_append(&sb, params, (size_t)(p - params) + 1);
            sb_puts(&sb, "\"callsign\":");
            sb_json_str(&sb, callsign);
            sb_puts(&sb, tk[0].size ? "," : "");
            sb_puts(&sb, p + 1);
        }
    }
    return sb.data; /* el llamador hace free() */
}

/* ======================================================================= */
/*  Hilo de emisión (loopback / air) y de calibración                      */
/* ======================================================================= */
typedef struct {
    tx_t *t;
    bool is_cal;
    iqbuf_t iq;
    int repeat, gain_db;
    int64_t center;
    bool loopback;
    char proto[16];
    int64_t freq;
    double seconds;
    uint32_t cal_rate; /* is_cal */
} job_t;

typedef struct {
    tx_t *t;
    int64_t freq;
    uint32_t rate;
    int16_t *buf;
    unsigned n;
    int rc;
    char err[160];
} cap_t;

static void *cap_main(void *arg)
{
    cap_t *c = arg;
    c->err[0] = '\0';
    c->rc = c->t->rx_cap(c->t->rx_ctx, c->freq, c->rate, c->buf, c->n, c->err, sizeof c->err);
    return NULL;
}

/* Una pasada de calibración: emite [ceros][ráfaga (precorregida con 'pre')][ceros]
 * en bucle interno mientras el receptor captura, y estima q y c. */
static int cal_pass(job_t *j, const cal_t *pre, cal_t *est, cal_meas_t *meas, char *err, size_t errlen)
{
    tx_t *t = j->t;
    iqbuf_t burst;
    if (cal_burst_make(&burst, j->cal_rate, 0.5) < 0) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    if (pre)
        cal_apply(pre, burst.s, burst.n);
    int total = CAL_PRE_ZEROS + burst.n + CAL_POST_ZEROS;
    int16_t *tx = calloc(2 * (size_t)total, sizeof(int16_t));
    int16_t *rx = malloc(sizeof(int16_t) * 2 * CAL_CAPTURE_LEN);
    cf32 *rf = malloc(sizeof(cf32) * CAL_CAPTURE_LEN);
    int rc = -1;
    if (!tx || !rx || !rf) {
        snprintf(err, errlen, "sin memoria");
        goto out;
    }
    for (int i = 0; i < burst.n; i++) {
        tx[2 * (CAL_PRE_ZEROS + i)] = to_q11(burst.s[i].re);
        tx[2 * (CAL_PRE_ZEROS + i) + 1] = to_q11(burst.s[i].im);
    }
    /* Las muestras con precorrección de continua pueden salirse de ±0,95·2047
     * por el DC: to_q11 satura, y la medida lo delataría como residuo. */
    if (hw_configure(t, j->center, j->cal_rate, TX_LOOPBACK_GAIN_DB, true) < 0) {
        snprintf(err, errlen, "no se pudo configurar TX: %s", hw_error(t));
        hw_stop(t);
        goto out;
    }
    cap_t cap = {.t = t, .freq = j->center, .rate = j->cal_rate, .buf = rx, .n = CAL_CAPTURE_LEN};
    pthread_t th;
    bool cap_started = pthread_create(&th, NULL, cap_main, &cap) == 0;
    if (!cap_started) {
        snprintf(err, errlen, "no se pudo crear el hilo de captura");
        hw_stop(t);
        goto out;
    }
    bool ok = true;
    for (int off = 0; off < total && ok && !t->stop; off += CHUNK) {
        unsigned c = (unsigned)MIN(CHUNK, total - off);
        if (hw_write(t, tx + 2 * (size_t)off, c) < 0) {
            snprintf(err, errlen, "error de TX: %s", hw_error(t));
            ok = false;
        }
    }
    pthread_join(th, NULL);
    hw_stop(t);
    if (!ok)
        goto out;
    if (t->stop) {
        snprintf(err, errlen, "calibración interrumpida");
        goto out;
    }
    if (cap.rc < 0) {
        snprintf(err, errlen, "captura del receptor: %s", cap.err[0] ? cap.err : "error");
        goto out;
    }
    for (int i = 0; i < CAL_CAPTURE_LEN; i++) {
        rf[i].re = rx[2 * i] / 2048.0f;
        rf[i].im = rx[2 * i + 1] / 2048.0f;
    }
    rc = cal_estimate(rf, CAL_CAPTURE_LEN, 0.5, est, meas, err, errlen);
out:
    free(tx);
    free(rx);
    free(rf);
    iqbuf_free(&burst);
    return rc;
}

static void cal_job(job_t *j)
{
    tx_t *t = j->t;
    char msg[300] = "";
    cal_t e1, e2;
    cal_meas_t m1 = {0}, m2 = {0};
    bool ok = false, saved = false;
    log_emission(t, "loopback", "cal", j->freq, j->center, j->cal_rate, TX_LOOPBACK_GAIN_DB, 2 * (CAL_PRE_ZEROS + cal_burst_len() + CAL_POST_ZEROS) / (double)j->cal_rate, false);
    if (cal_pass(j, NULL, &e1, &m1, msg, sizeof msg) == 0) {
        if (m1.irr_db > 50.0 && m1.lo_dbc < -50.0) {
            snprintf(msg, sizeof msg,
                     "TX ya limpio en el bucle (imagen −%.1f dB, fuga de LO %.1f dBc): no se aplica corrección. En el "
                     "bucle digital RFIC_BIST esto es lo esperado (no atraviesa el RF).",
                     m1.irr_db, m1.lo_dbc);
            ok = true;
        } else if (cal_pass(j, &e1, &e2, &m2, msg, sizeof msg) == 0) {
            ok = true;
            if (m2.irr_db >= m1.irr_db && m2.lo_dbc <= m1.lo_dbc + 1.0) {
                pthread_mutex_lock(&t->lock);
                t->cal = e1;
                t->cal_valid = true;
                t->cal_center = j->center;
                t->cal_rate = j->cal_rate;
                pthread_mutex_unlock(&t->lock);
                saved = true;
                snprintf(msg, sizeof msg,
                         "calibración guardada: rechazo de imagen %.1f → %.1f dB, fuga de LO %.1f → %.1f dBc",
                         m1.irr_db, m2.irr_db, m1.lo_dbc, m2.lo_dbc);
            } else {
                snprintf(msg, sizeof msg,
                         "la corrección no mejora la medida (imagen %.1f → %.1f dB, LO %.1f → %.1f dBc): descartada",
                         m1.irr_db, m2.irr_db, m1.lo_dbc, m2.lo_dbc);
            }
        } else {
            char tmp[300];
            snprintf(tmp, sizeof tmp, "verificación: %s", msg);
            memcpy(msg, tmp, sizeof msg);
        }
    }
    if (ok)
        LOG_I("TX cal: %s", msg);
    else
        LOG_W("TX cal falló: %s", msg);
    if (t->srv) {
        sb_t sb = {0};
        sb_printf(&sb, "{\"type\":\"tx_cal\",\"ok\":%s,\"saved\":%s,\"msg\":", ok ? "true" : "false",
                  saved ? "true" : "false");
        sb_json_str(&sb, msg);
        sb_printf(&sb, ",\"irr_before_db\":%.2f,\"irr_after_db\":%.2f,\"lo_before_dbc\":%.2f,\"lo_after_dbc\":%.2f,"
                       "\"center\":%lld,\"rate\":%u}",
                  m1.irr_db, m2.irr_db, m1.lo_dbc, m2.lo_dbc, (long long)j->center, j->cal_rate);
        send_json(t, &sb);
        sb_free(&sb);
    }
}

static void tx_job(job_t *j)
{
    tx_t *t = j->t;
    char msg[256];
    /* Precorrección de la calibración (solo si es para este centro y tasa) */
    cal_t cal;
    int64_t cc;
    uint32_t cr;
    if (tx_cal_get(t, &cal, &cc, &cr) && cc == j->center && cr == (uint32_t)j->iq.rate)
        cal_apply(&cal, j->iq.s, j->iq.n);
    int16_t *buf = malloc(sizeof(int16_t) * 2 * (size_t)j->iq.n);
    bool ok = buf != NULL;
    if (ok) {
        for (int i = 0; i < j->iq.n; i++) {
            buf[2 * i] = to_q11(j->iq.s[i].re);
            buf[2 * i + 1] = to_q11(j->iq.s[i].im);
        }
        if (hw_configure(t, j->center, (uint32_t)j->iq.rate, j->gain_db, j->loopback) < 0) {
            snprintf(msg, sizeof msg, "no se pudo configurar TX: %s", hw_error(t));
            ok = false;
        }
    } else {
        snprintf(msg, sizeof msg, "sin memoria");
    }
    if (ok) {
        double t0 = now_monotonic();
        for (int r = 0; r < j->repeat && ok && !t->stop; r++)
            for (int off = 0; off < j->iq.n && !t->stop; off += CHUNK) {
                unsigned c = (unsigned)MIN(CHUNK, j->iq.n - off);
                if (hw_write(t, buf + 2 * (size_t)off, c) < 0) {
                    snprintf(msg, sizeof msg, "error de TX: %s", hw_error(t));
                    ok = false;
                    break;
                }
            }
        static const int16_t zeros[2 * CHUNK];
        if (ok)
            hw_write(t, zeros, CHUNK); /* cola de silencio: vacía el búfer del hardware */
        hw_stop(t);
        double dt = now_monotonic() - t0;
        if (ok)
            snprintf(msg, sizeof msg, "emitido %s en %.6f MHz (%s) durante %.2f s%s", j->proto, j->freq / 1e6,
                     j->loopback ? "bucle interno del RFIC" : "antena", dt, t->stop ? " (interrumpido)" : "");
    } else {
        hw_stop(t); /* garantiza TX apagado y loopback desactivado */
    }
    send_result(t, ok, msg, NULL, j->seconds);
    free(buf);
}

static void *job_main(void *arg)
{
    job_t *j = arg;
    tx_t *t = j->t;
    if (j->is_cal)
        cal_job(j);
    else
        tx_job(j);
    iqbuf_free(&j->iq);
    pthread_mutex_lock(&t->lock);
    t->busy = false;
    pthread_mutex_unlock(&t->lock);
    free(j);
    return NULL;
}

/* Lanza el hilo. Toma posesión de j (y de j->iq) incluso si falla. Con t->submit tomado. */
static int launch_job(tx_t *t, job_t *j, char *err, size_t errlen)
{
    t->stop = false;
    pthread_mutex_lock(&t->lock);
    t->busy = true;
    pthread_mutex_unlock(&t->lock);
    if (pthread_create(&t->th, NULL, job_main, j) != 0) {
        pthread_mutex_lock(&t->lock);
        t->busy = false;
        pthread_mutex_unlock(&t->lock);
        iqbuf_free(&j->iq);
        free(j);
        snprintf(err, errlen, "no se pudo crear el hilo de emisión");
        return -1;
    }
    t->th_valid = true;
    return 0;
}

/* ======================================================================= */
/*  Comprobaciones comunes de las órdenes                                  */
/* ======================================================================= */
static int fail(tx_t *t, const char *msg)
{
    send_result(t, false, msg, NULL, 0);
    return -1;
}

/* Modo loopback/air: TX permitido, hardware presente, nada en curso. */
static int precheck_hw(tx_t *t, char *err, size_t errlen)
{
    if (!t->pol.enabled) {
        snprintf(err, errlen, "la transmisión está desactivada (arrancar con --enable-tx)");
        return -1;
    }
    if (!hw_can_tx(t)) {
        snprintf(err, errlen, "esta fuente no puede transmitir (hace falta el bladeRF)");
        return -1;
    }
    return 0;
}

static int precheck_idle(tx_t *t, char *err, size_t errlen)
{
    pthread_mutex_lock(&t->lock);
    bool was_busy = t->busy;
    pthread_mutex_unlock(&t->lock);
    if (was_busy) {
        snprintf(err, errlen, "ya hay una emisión en curso");
        return -1;
    }
    tx_wait(t);
    return 0;
}

/* Prefijo único tx_<tag>_<fecha>[_ms] dentro de recdir. */
static void make_prefix(tx_t *t, const char *tag, char *prefix, size_t n, char *base, size_t nb)
{
    mkdir(t->recdir, 0755);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char datap[900];
    snprintf(base, nb, "tx_%s_%04d%02d%02d_%02d%02d%02d", tag, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(prefix, n, "%s/%s", t->recdir, base);
    snprintf(datap, sizeof datap, "%s.sigmf-data", prefix);
    if (access(datap, F_OK) == 0) { /* no pisar un fichero del mismo segundo */
        snprintf(base + strlen(base), nb - strlen(base), "_%d", (int)(now_monotonic() * 1000) % 1000);
        snprintf(prefix, n, "%s/%s", t->recdir, base);
    }
}

/* ======================================================================= */
/*  tx_submit                                                              */
/* ======================================================================= */
static int submit_locked(tx_t *t, const modulator_desc_t *m, const tx_req_t *req, char *err, size_t errlen)
{
    if (req->mode != TX_FILE && req->mode != TX_LOOPBACK && req->mode != TX_AIR) {
        snprintf(err, errlen, "modo de emisión desconocido");
        return fail(t, err);
    }
    if (req->mode != TX_FILE && precheck_hw(t, err, errlen) < 0)
        return fail(t, err);
    if (precheck_idle(t, err, errlen) < 0)
        return fail(t, err);
    if (req->freq < 0 || req->freq > TX_MAX_FREQ || req->center < 0 || req->center > TX_MAX_FREQ ||
        (req->rate != 0 && (req->rate < TX_MIN_RATE || req->rate > TX_MAX_RATE)) || req->gain_db < -100 ||
        req->gain_db > 100) {
        snprintf(err, errlen, "frecuencia, centro, tasa o ganancia fuera de rango");
        return fail(t, err);
    }

    int repeat = CLAMP(req->repeat, 1, 1000);
    char *params2 = NULL;
    const char *params = req->params;
    if (req->mode == TX_AIR && t->pol.callsign[0]) {
        params2 = params_with_callsign(params, t->pol.callsign); /* el indicativo va en la trama si el protocolo lo admite */
        params = params2;
    }
    iqbuf_t iq = {0};
    tx_meta_t mt;
    int64_t center = req->center;
    /* En loopback/air el centro por defecto es la propia frecuencia (sin
     * desplazamiento): así la fuga de portadora cae en el canal emitido. */
    if (req->mode != TX_FILE && center == 0)
        center = req->freq;
    if (tx_render(m, params, req->freq, center, req->rate, &iq, &mt, err, errlen) < 0) {
        free(params2);
        return fail(t, err);
    }
    free(params2);
    int64_t freq = req->freq > 0 ? req->freq : mt.center;
    const decoder_desc_t *dd = decoder_find(m->id);
    if (req->freq <= 0 && dd && dd->freqs[0])
        freq = dd->freqs[0];
    /* Duración REAL: muestras renderizadas (tras el remuestreo) × repeticiones */
    double total = (double)iq.n / iq.rate * repeat;
    char msg[1400];

    if (req->mode == TX_FILE) {
        if ((double)iq.n * repeat > (double)TX_MAX_FILE_SAMPLES) {
            snprintf(err, errlen, "el fichero resultante sería demasiado grande (> %lld muestras)",
                     (long long)TX_MAX_FILE_SAMPLES);
            iqbuf_free(&iq);
            return fail(t, err);
        }
        char base[200], prefix[800];
        make_prefix(t, m->id, prefix, sizeof prefix, base, sizeof base);
        char desc[200];
        snprintf(desc, sizeof desc, "%s sintético: canal en %lld Hz", m->name, (long long)freq);
        int rc = tx_write_sigmf(prefix, &iq, repeat, mt.center, desc, err, errlen);
        iqbuf_free(&iq);
        if (rc < 0)
            return fail(t, err);
        char fname[260];
        snprintf(fname, sizeof fname, "%s.sigmf-meta", base);
        snprintf(msg, sizeof msg, "%s guardado en %s (centro %.6f MHz, %.0f sps, %.2f s)", m->name, fname,
                 mt.center / 1e6, mt.rate, total);
        send_result(t, true, msg, fname, total);
        return 0;
    }

    int gain = req->mode == TX_LOOPBACK ? TX_LOOPBACK_GAIN_DB : req->gain_db;
    bool duty = false;
    if (req->mode == TX_AIR) {
        const band_t *b = NULL;
        pthread_mutex_lock(&t->lock);
        double recent = recent_868(t, now_wall());
        pthread_mutex_unlock(&t->lock);
        if (check_air(&t->pol, m, freq, mt.center, &mt, gain, total, recent, &b, err, errlen) < 0) {
            iqbuf_free(&iq);
            return fail(t, err);
        }
        duty = b && b->duty;
    } else if (!(total <= TX_LOOPBACK_MAX_SECONDS)) {
        snprintf(err, errlen, "duración %.1f s por encima del máximo del bucle interno (%.0f s)", total,
                 TX_LOOPBACK_MAX_SECONDS);
        iqbuf_free(&iq);
        return fail(t, err);
    }
    LOG_I("TX %s (%s): %.6f MHz, centro %.6f MHz, %.0f sps, ganancia %d dB, %.2f s (ancho ocupado %.0f Hz)", m->id,
          req->mode == TX_AIR ? "air" : "loopback", freq / 1e6, mt.center / 1e6, mt.rate, gain, total, mt.occ_bw_hz);

    job_t *j = calloc(1, sizeof *j);
    if (!j) {
        iqbuf_free(&iq);
        snprintf(err, errlen, "sin memoria");
        return fail(t, err);
    }
    j->t = t;
    j->iq = iq;
    j->repeat = repeat;
    j->gain_db = gain;
    j->center = mt.center;
    j->loopback = req->mode == TX_LOOPBACK;
    snprintf(j->proto, sizeof j->proto, "%s", m->id);
    j->freq = freq;
    j->seconds = total;
    /* Se anota ANTES de emitir (aunque falle o se interrumpa después): sobrestima, no subestima */
    log_emission(t, req->mode == TX_AIR ? "air" : "loopback", m->id, freq, mt.center, mt.rate, gain, total, duty);
    if (duty) {
        pthread_mutex_lock(&t->lock);
        hist_add(t, now_wall(), total);
        pthread_mutex_unlock(&t->lock);
    }
    if (launch_job(t, j, err, errlen) < 0)
        return fail(t, err);
    return 0;
}

int tx_submit_with(tx_t *t, const modulator_desc_t *m, const tx_req_t *req, char *err, size_t errlen)
{
    err[0] = '\0';
    pthread_mutex_lock(&t->submit);
    int rc = submit_locked(t, m, req, err, errlen);
    pthread_mutex_unlock(&t->submit);
    return rc;
}

int tx_submit(tx_t *t, const tx_req_t *req, char *err, size_t errlen)
{
    err[0] = '\0';
    char proto[sizeof req->proto + 1];
    snprintf(proto, sizeof proto, "%.*s", (int)sizeof req->proto, req->proto);
    const modulator_desc_t *m = modulator_find(proto);
    if (!m) {
        snprintf(err, errlen, "no hay modulador para '%s'", proto);
        return fail(t, err);
    }
    return tx_submit_with(t, m, req, err, errlen);
}

/* ======================================================================= */
/*  Reproducción de grabaciones SigMF (file y loopback; nunca air)         */
/* ======================================================================= */
static bool name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 180 || s[0] == '.')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
            return false; /* nada de '/' ni '\\': no se sale de recdir */
    }
    return strstr(s, "..") == NULL;
}

static bool regular_file(const char *path, off_t *size)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) /* lstat: no se siguen enlaces simbólicos */
        return false;
    *size = st.st_size;
    return true;
}

/* Lee una grabación de recdir como cf32 con |x| ≤ 0,9. base_out: nombre sin extensión. */
static int read_sigmf(const char *recdir, const char *name_in, iqbuf_t *out, int64_t *center, char *base_out,
                      size_t nb, char *err, size_t errlen)
{
    char base[200];
    snprintf(base, sizeof base, "%s", name_in);
    size_t l = strlen(base);
    if (l > 11 && (strcmp(base + l - 11, ".sigmf-meta") == 0 || strcmp(base + l - 11, ".sigmf-data") == 0))
        base[l - 11] = '\0';
    if (!name_ok(base)) {
        snprintf(err, errlen, "nombre de grabación no válido");
        return -1;
    }
    char mp[600], dp[600];
    snprintf(mp, sizeof mp, "%s/%s.sigmf-meta", recdir, base);
    snprintf(dp, sizeof dp, "%s/%s.sigmf-data", recdir, base);
    off_t msize, dsize;
    if (!regular_file(mp, &msize) || !regular_file(dp, &dsize) || msize <= 0 || msize > 65536) {
        snprintf(err, errlen, "grabación '%s' no encontrada", base);
        return -1;
    }
    char *js = malloc((size_t)msize + 1);
    FILE *f = fopen(mp, "rb");
    if (!js || !f || fread(js, 1, (size_t)msize, f) != (size_t)msize) {
        if (f)
            fclose(f);
        free(js);
        snprintf(err, errlen, "no se pudo leer %s", mp);
        return -1;
    }
    fclose(f);
    js[msize] = '\0';
    jtok_t tk[256];
    int nt = json_parse(js, (size_t)msize, tk, 256);
    char dt[24] = "";
    double rate = 0, fc = 0;
    int g = nt > 0 ? json_obj_get(js, tk, 0, "global") : -1;
    if (g >= 0) {
        int v = json_obj_get(js, tk, g, "core:datatype");
        if (v >= 0)
            json_str(js, tk, v, dt, sizeof dt);
        v = json_obj_get(js, tk, g, "core:sample_rate");
        if (v >= 0)
            json_num(js, tk, v, &rate);
    }
    int ca = nt > 0 ? json_obj_get(js, tk, 0, "captures") : -1;
    if (ca >= 0) {
        int c0 = json_arr_get(tk, ca, 0);
        int v = c0 >= 0 ? json_obj_get(js, tk, c0, "core:frequency") : -1;
        if (v >= 0)
            json_num(js, tk, v, &fc);
    }
    free(js);
    int bps = !strcmp(dt, "ci16_le") ? 4 : !strcmp(dt, "cf32_le") ? 8 : (!strcmp(dt, "ci8") || !strcmp(dt, "cu8")) ? 2 : 0;
    if (!bps || !(rate >= TX_MIN_RATE && rate <= TX_MAX_RATE) || !(fc >= 0 && fc <= (double)TX_MAX_FREQ)) {
        snprintf(err, errlen, "metadatos SigMF no válidos o no soportados (datatype %s)", dt[0] ? dt : "?");
        return -1;
    }
    long long ns = dsize / bps;
    if (ns <= 0 || ns > TX_MAX_SAMPLES) {
        snprintf(err, errlen, "grabación vacía o demasiado larga (%lld muestras)", ns);
        return -1;
    }
    unsigned char *raw = malloc((size_t)ns * (size_t)bps);
    cf32 *s = malloc(sizeof(cf32) * (size_t)ns);
    f = fopen(dp, "rb");
    if (!raw || !s || !f || fread(raw, (size_t)bps, (size_t)ns, f) != (size_t)ns) {
        if (f)
            fclose(f);
        free(raw);
        free(s);
        snprintf(err, errlen, "no se pudo leer %s", dp);
        return -1;
    }
    fclose(f);
    double peak = 0;
    for (long long i = 0; i < ns; i++) {
        float re, im;
        if (bps == 4) {
            const int16_t *p = (const int16_t *)(raw + 4 * i);
            re = p[0] / 2048.0f;
            im = p[1] / 2048.0f;
        } else if (bps == 8) {
            memcpy(&re, raw + 8 * i, 4);
            memcpy(&im, raw + 8 * i + 4, 4);
            if (!isfinite(re) || !isfinite(im))
                re = im = 0;
        } else if (dt[1] == 'i') { /* ci8 */
            re = (int8_t)raw[2 * i] / 128.0f;
            im = (int8_t)raw[2 * i + 1] / 128.0f;
        } else { /* cu8 */
            re = (raw[2 * i] - 127.5f) / 127.5f;
            im = (raw[2 * i + 1] - 127.5f) / 127.5f;
        }
        s[i] = (cf32){re, im};
        peak = fmax(peak, hypot(re, im));
    }
    free(raw);
    if (peak > 0.9) { /* contrato de los moduladores: |x| ≤ 0,9 */
        float k = (float)(0.9 / peak);
        for (long long i = 0; i < ns; i++) {
            s[i].re *= k;
            s[i].im *= k;
        }
    }
    out->s = s;
    out->n = out->cap = (int)ns;
    out->rate = rate;
    *center = (int64_t)fc;
    snprintf(base_out, nb, "%s", base);
    return 0;
}

int tx_play(tx_t *t, const tx_play_req_t *req, char *err, size_t errlen)
{
    err[0] = '\0';
    pthread_mutex_lock(&t->submit);
    int rc = -1;
    iqbuf_t iq = {0};
    int64_t center = 0;
    char base[200];
    /* REGLA: la reproducción jamás sale por antena (riesgo de ataques de repetición) */
    if (req->mode != TX_FILE && req->mode != TX_LOOPBACK) {
        snprintf(err, errlen, "tx_play solo admite file o loopback: reproducir grabaciones por antena está prohibido");
        fail(t, err);
        goto out;
    }
    if (req->mode == TX_LOOPBACK && precheck_hw(t, err, errlen) < 0) {
        fail(t, err);
        goto out;
    }
    if (precheck_idle(t, err, errlen) < 0) {
        fail(t, err);
        goto out;
    }
    if (read_sigmf(t->recdir, req->name, &iq, &center, base, sizeof base, err, errlen) < 0) {
        fail(t, err);
        goto out;
    }
    int repeat = CLAMP(req->repeat, 1, 1000);
    double total = (double)iq.n / iq.rate * repeat;
    char msg[1400];
    if (req->mode == TX_FILE) {
        if ((double)iq.n * repeat > (double)TX_MAX_FILE_SAMPLES) {
            snprintf(err, errlen, "el fichero resultante sería demasiado grande");
            fail(t, err);
            goto out;
        }
        char tag[220], b2[200], prefix[800], desc[260];
        snprintf(tag, sizeof tag, "play");
        make_prefix(t, tag, prefix, sizeof prefix, b2, sizeof b2);
        snprintf(desc, sizeof desc, "copia de %s (tx_play)", base);
        if (tx_write_sigmf(prefix, &iq, repeat, center, desc, err, errlen) < 0) {
            fail(t, err);
            goto out;
        }
        char fname[260];
        snprintf(fname, sizeof fname, "%s.sigmf-meta", b2);
        snprintf(msg, sizeof msg, "%s reproducido a fichero %s (%.0f sps, %.2f s)", base, fname, iq.rate, total);
        send_result(t, true, msg, fname, total);
        rc = 0;
        goto out;
    }
    if (!(total <= TX_LOOPBACK_MAX_SECONDS)) {
        snprintf(err, errlen, "duración %.1f s por encima del máximo del bucle interno (%.0f s)", total,
                 TX_LOOPBACK_MAX_SECONDS);
        fail(t, err);
        goto out;
    }
    job_t *j = calloc(1, sizeof *j);
    if (!j) {
        snprintf(err, errlen, "sin memoria");
        fail(t, err);
        goto out;
    }
    j->t = t;
    j->iq = iq;
    iq.s = NULL; /* ahora es del job */
    j->repeat = repeat;
    j->gain_db = TX_LOOPBACK_GAIN_DB;
    j->center = center;
    j->loopback = true;
    snprintf(j->proto, sizeof j->proto, "play");
    j->freq = center;
    j->seconds = total;
    LOG_I("TX play %s (loopback): centro %.6f MHz, %.0f sps, %.2f s", base, center / 1e6, j->iq.rate, total);
    log_emission(t, "loopback", "play", center, center, j->iq.rate, TX_LOOPBACK_GAIN_DB, total, false);
    rc = launch_job(t, j, err, errlen);
    if (rc < 0)
        fail(t, err);
out:
    iqbuf_free(&iq);
    pthread_mutex_unlock(&t->submit);
    return rc;
}

/* ======================================================================= */
/*  Calibración                                                            */
/* ======================================================================= */
int tx_cal_run(tx_t *t, int64_t freq, uint32_t rate, char *err, size_t errlen)
{
    err[0] = '\0';
    pthread_mutex_lock(&t->submit);
    int rc = -1;
    if (freq == 0)
        freq = 433920000LL;
    if (rate == 0)
        rate = 1000000;
    if (freq < 70000000LL || freq > TX_MAX_FREQ || rate < 520834 || rate > TX_MAX_RATE) {
        snprintf(err, errlen, "frecuencia o tasa fuera de rango para calibrar");
        fail(t, err);
    } else if (precheck_hw(t, err, errlen) < 0) { /* --enable-tx y hardware; siempre loopback */
        fail(t, err);
    } else if (!t->rx_cap) {
        snprintf(err, errlen, "no hay captura del receptor para calibrar (el motor debe instalar tx_set_rx_capture)");
        fail(t, err);
    } else if (precheck_idle(t, err, errlen) < 0) {
        fail(t, err);
    } else {
        job_t *j = calloc(1, sizeof *j);
        if (!j) {
            snprintf(err, errlen, "sin memoria");
            fail(t, err);
        } else {
            j->t = t;
            j->is_cal = true;
            j->loopback = true;
            j->center = freq;
            j->freq = freq;
            j->cal_rate = rate;
            snprintf(j->proto, sizeof j->proto, "cal");
            rc = launch_job(t, j, err, errlen);
            if (rc < 0)
                fail(t, err);
        }
    }
    pthread_mutex_unlock(&t->submit);
    return rc;
}
