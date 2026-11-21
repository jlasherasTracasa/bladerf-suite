/*
 * main.c — Punto de entrada de BladeRF Lab.
 *
 * Une todos los módulos:
 *
 *   source (bladeRF o fichero) ─► engine (hilo DSP) ─► dec_host (decodificadores)
 *                                    ▲                         │
 *   navegador ⇄ server (HTTP + WebSocket) ◄──── eventos ◄──────┘
 *                         │ órdenes JSON (cmd)
 *                         └► este fichero: config, channel, profile, dec_*, record, tx…
 *
 * Responsabilidades de main.c:
 *   - Opciones de línea de órdenes (AGENTS.md §7).
 *   - Configuración persistente en ~/.config/bladerf-lab/config.json: se
 *     guarda cada vez que el motor aplica un cambio (engine_on_config_change)
 *     y se recupera al arrancar. Con una fuente de fichero no se guarda el
 *     centro ni la tasa (los fija la grabación, no el usuario).
 *   - Las órdenes del protocolo WebSocket (docs/arquitectura/WEBSOCKET.md) y el
 *     «snapshot» que recibe cada cliente al conectar.
 *   - Modo --gen: genera una señal SigMF con un modulador y termina.
 *   - Cierre limpio con SIGINT/SIGTERM o --duration.
 */
#include "common.h"
#include "decoders/host.h"
#include "engine.h"
#include "net/server.h"
#include "profiles.h"
#include "decoders/nr_phy.h"
#include "sdr/source.h"
#include "tx/tx.h"
#include "tv/tv.h"
#include "util/json.h"
#include "util/labels.h"
#include "util/strbuf.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 8090
#define MAX_TOKENS 256

/* ======================================================================= */
/*  Estado de la aplicación                                                */
/* ======================================================================= */
typedef struct {
    server_t *srv;
    source_t *src;
    engine_t *eng;
    dec_host_t *host;
    tx_t *tx;
    tv_t *tv;
    char cfg_path[600];   /* "" = sin persistencia */
    bool can_tune;
    int64_t saved_center; /* se conservan al guardar si la fuente no sintoniza */
    uint32_t saved_rate;
    pthread_mutex_t out_lock;
    bool print_events;
    FILE *log_events;
    labels_t *labels;     /* --labels: reúne eventos y señales para las anotaciones SigMF */
    bool tx_enabled;      /* --enable-tx: sin él tx_cal se rechaza y no se toca el receptor */
    pthread_mutex_t cal_lock; /* una calibración a la vez */
    /* Hilo de tx_cal: se une al cerrar (antes iba detached y podía usar a->eng/a->tx
     * ya liberados si el servidor se destruía durante la calibración). */
    pthread_t cal_th;
    bool cal_started;         /* cal_th es un hilo sin unir (solo lo toca el hilo de red y main) */
    volatile int cal_running; /* 1 mientras cal_thread no ha terminado */
    volatile int closing;     /* 1 al cerrar: las calibraciones pendientes no empiezan */
} app_t;

static volatile sig_atomic_t g_stop;
static server_t *volatile g_srv;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    if (g_srv)
        server_stop(g_srv); /* escribe en un eventfd: seguro dentro de un manejador */
}

/* ======================================================================= */
/*  Utilidades                                                             */
/* ======================================================================= */
/* Frecuencia con sufijo opcional: 161975000, 161.975M, 868.3M, 2.4G, 500k. */
static bool parse_hz(const char *s, int64_t *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s)
        return false;
    switch (*end) {
    case 'k': case 'K': v *= 1e3; end++; break;
    case 'm': case 'M': v *= 1e6; end++; break;
    case 'g': case 'G': v *= 1e9; end++; break;
    default: break;
    }
    if (*end || v < 0)
        return false;
    *out = (int64_t)(v + 0.5);
    return true;
}

static void send_error(app_t *a, int client, const char *msg)
{
    sb_t sb = {0};
    sb_puts(&sb, "{\"type\":\"error\",\"msg\":");
    sb_json_str(&sb, msg);
    sb_puts(&sb, "}");
    server_send_text(a->srv, client, sb.data, sb.len);
    sb_free(&sb);
    LOG_W("%s", msg);
}

static void send_sb(app_t *a, int client, sb_t *sb)
{
    server_send_text(a->srv, client, sb->data, sb->len);
    sb_free(sb);
}

static bool get_num(const char *js, const jtok_t *t, const char *key, double *v)
{
    int i = json_obj_get(js, t, 0, key);
    return i >= 0 && json_num(js, t, i, v);
}

/* Número JSON acotado: 1 si está y es válido, 0 si no está, −1 si está pero es NaN/fuera de rango
 * (convertir a entero un valor fuera de rango es comportamiento indefinido en C). */
static int num_in(const char *js, const jtok_t *t, const char *key, double lo, double hi, double *v)
{
    if (json_obj_get(js, t, 0, key) < 0)
        return 0;
    if (!get_num(js, t, key, v) || !(*v >= lo && *v <= hi))
        return -1;
    return 1;
}

/* Como num_in, pero avisa al cliente cuando el valor existe y no es válido. Devuelve igual que num_in. */
static int num_chk(app_t *a, int client, const char *cmd, const char *js, const jtok_t *t, const char *key, double lo,
                   double hi, double *v)
{
    int k = num_in(js, t, key, lo, hi, v);
    if (k < 0) {
        char m[96];
        snprintf(m, sizeof m, "%s: '%s' no es un número finito en [%g, %g]", cmd, key, lo, hi);
        send_error(a, client, m);
    }
    return k;
}

static bool get_bool(const char *js, const jtok_t *t, const char *key, bool *v)
{
    int i = json_obj_get(js, t, 0, key);
    return i >= 0 && json_bool(js, t, i, v);
}

static bool get_str(const char *js, const jtok_t *t, const char *key, char *buf, size_t n)
{
    int i = json_obj_get(js, t, 0, key);
    return i >= 0 && json_str(js, t, i, buf, n);
}

/* Copia el texto de un objeto JSON (params, opts) o NULL si no hay o es null. */
static char *get_obj_text(const char *js, const jtok_t *t, const char *key)
{
    int i = json_obj_get(js, t, 0, key);
    if (i < 0 || t[i].type != J_OBJ)
        return NULL;
    return strndup(js + t[i].start, (size_t)(t[i].end - t[i].start));
}

/* ======================================================================= */
/*  Configuración persistente                                              */
/* ======================================================================= */
static void default_cfg(engine_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->center_hz = 100000000;
    c->rate = 2400000;
    c->gain_mode = GAIN_AUTO;
    c->gain_db = 30;
    c->fft_size = 4096;
    c->fps = 20;
    c->det_snr = 10.f;
}

static void mkdir_p(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
}

static void load_config(const char *path, engine_cfg_t *c)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char *buf = malloc(1 << 16);
    size_t n = buf ? fread(buf, 1, (1 << 16) - 1, f) : 0;
    fclose(f);
    if (!buf)
        return;
    buf[n] = '\0';
    jtok_t t[128];
    if (json_parse(buf, n, t, 128) < 1 || t[0].type != J_OBJ) {
        LOG_W("Configuración %s ilegible: se usan los valores por defecto", path);
        free(buf);
        return;
    }
    double v;
    char s[16];
    bool b;
    if (num_in(buf, t, "center", 0, 6e+09, &v) > 0) c->center_hz = (int64_t)v;
    if (num_in(buf, t, "rate", 0, 1.2288e+08, &v) > 0) c->rate = (uint32_t)v;
    if (num_in(buf, t, "rf_bw", 0, 1.2288e+08, &v) > 0) c->rf_bw = (uint32_t)v;
    if (get_str(buf, t, "gain_mode", s, sizeof s) && !gain_mode_parse(s, &c->gain_mode)) c->gain_mode = GAIN_AUTO;
    if (num_in(buf, t, "gain", -100, 100, &v) > 0) c->gain_db = (int)v;
    if (get_bool(buf, t, "biastee", &b)) c->biastee = b;
    if (num_in(buf, t, "fft", 0, 1.04858e+06, &v) > 0) c->fft_size = (int)v;
    if (num_in(buf, t, "fps", 0, 1000, &v) > 0) c->fps = (int)v;
    if (num_in(buf, t, "det_snr", -100, 200, &v) > 0) c->det_snr = (float)v;
    int l = json_obj_get(buf, t, 0, "loc");
    if (l >= 0 && t[l].type == J_OBJ) {
        double la, lo;
        int ia = json_obj_get(buf, t, l, "lat"), io = json_obj_get(buf, t, l, "lon");
        if (ia >= 0 && io >= 0 && json_num(buf, t, ia, &la) && json_num(buf, t, io, &lo)) {
            c->loc_valid = true;
            c->lat = la;
            c->lon = lo;
        }
    }
    free(buf);
}

/* Llamado desde el hilo DSP cada vez que el motor aplica un cambio. */
static void save_config(void *ctx, const engine_cfg_t *c)
{
    app_t *a = ctx;
    if (!a->cfg_path[0])
        return;
    sb_t sb = {0};
    int64_t center = a->can_tune ? c->center_hz : a->saved_center;
    uint32_t rate = a->can_tune ? c->rate : a->saved_rate;
    sb_printf(&sb, "{\n  \"center\": %lld,\n  \"rate\": %u,\n  \"rf_bw\": %u,\n  \"gain_mode\": \"%s\",\n", (long long)center,
              rate, c->rf_bw, gain_mode_name(c->gain_mode));
    sb_printf(&sb, "  \"gain\": %d,\n  \"biastee\": %s,\n  \"fft\": %d,\n  \"fps\": %d,\n  \"det_snr\": %.1f,\n", c->gain_db,
              c->biastee ? "true" : "false", c->fft_size, c->fps, c->det_snr);
    if (c->loc_valid)
        sb_printf(&sb, "  \"loc\": {\"lat\": %.6f, \"lon\": %.6f}\n}\n", c->lat, c->lon);
    else
        sb_puts(&sb, "  \"loc\": null\n}\n");
    char dir[600], tmp[620];
    snprintf(dir, sizeof dir, "%s", a->cfg_path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = '\0';
        mkdir_p(dir);
        mkdir(dir, 0755);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", a->cfg_path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fwrite(sb.data, 1, sb.len, f);
        if (fclose(f) == 0)
            rename(tmp, a->cfg_path); /* atómico: nunca queda a medias */
    } else {
        LOG_W("No se pudo guardar la configuración en %s: %s", a->cfg_path, strerror(errno));
    }
    sb_free(&sb);
}

/* ======================================================================= */
/*  Eventos de decodificadores: --print y --log-events                     */
/* ======================================================================= */
static void on_dec_event(void *ctx, const char *line)
{
    app_t *a = ctx;
    pthread_mutex_lock(&a->out_lock);
    if (a->print_events) {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    if (a->log_events) {
        fputs(line, a->log_events);
        fputc('\n', a->log_events);
        fflush(a->log_events);
    }
    pthread_mutex_unlock(&a->out_lock);
    if (a->labels)
        labels_on_event(a->labels, line);
}

/* ======================================================================= */
/*  Órdenes WebSocket                                                      */
/* ======================================================================= */
static void cmd_config(app_t *a, const char *js, const jtok_t *t)
{
    engine_cfg_t c;
    engine_get_config(a->eng, &c);
    unsigned f = 0;
    double v;
    char s[16];
    bool b;
    if (num_chk(a, -1, "config", js, t, "center", 0, 6e+09, &v) > 0) { c.center_hz = (int64_t)v; f |= CFG_CENTER; }
    if (num_chk(a, -1, "config", js, t, "rate", 0, 1.2288e+08, &v) > 0) { c.rate = (uint32_t)v; f |= CFG_RATE; }
    if (num_chk(a, -1, "config", js, t, "rf_bw", 0, 1.2288e+08, &v) > 0) { c.rf_bw = (uint32_t)v; f |= CFG_RFBW; }
    /* "auto" | "manual" | "fast" | "slow" | "hybrid" (AGC rápida/lenta/híbrida del AD9361) */
    if (get_str(js, t, "gain_mode", s, sizeof s)) {
        if (gain_mode_parse(s, &c.gain_mode))
            f |= CFG_GAIN;
        else
            send_error(a, -1, "gain_mode: use auto, manual, fast, slow o hybrid");
    }
    if (num_chk(a, -1, "config", js, t, "gain", -100, 100, &v) > 0) { c.gain_db = (int)v; f |= CFG_GAIN; }
    if (get_bool(js, t, "biastee", &b)) { c.biastee = b; f |= CFG_BIASTEE; }
    if (num_chk(a, -1, "config", js, t, "fft", 0, 1.04858e+06, &v) > 0) { c.fft_size = (int)v; f |= CFG_FFT; }
    if (num_chk(a, -1, "config", js, t, "fps", 0, 1000, &v) > 0) { c.fps = (int)v; f |= CFG_FFT; }
    if (num_chk(a, -1, "config", js, t, "det_snr", -100, 200, &v) > 0) { c.det_snr = (float)v; f |= CFG_DET; }
    /* Formato de RX por el USB: "auto" | "sc16" | "packed12" | "sc8g"; sc8g_shift = -1 (auto) | 0..3 (GAIN8) */
    if (get_str(js, t, "format", s, sizeof s)) {
        if (rxfmt_parse(s, &c.format))
            f |= CFG_FORMAT;
        else
            send_error(a, -1, "format: use auto, sc16, packed12 o sc8g");
    }
    if (num_chk(a, -1, "config", js, t, "sc8g_shift", -1, 3, &v) > 0) {
        c.sc8g_manual = v >= 0;
        c.sc8g_shift = v >= 0 ? (int)v : 0;
        f |= CFG_FORMAT;
    }
    /* Fuente del espectro: "cpu" | "gpu" | "fpga" (imagen lab). Se aplica en el motor tras el resto de campos. */
    char ssrc[16];
    bool has_ssrc = get_str(js, t, "spectrum_src", ssrc, sizeof ssrc);
    int l = json_obj_get(js, t, 0, "loc");
    if (l >= 0) {
        f |= CFG_LOC;
        double la, lo;
        int ia = t[l].type == J_OBJ ? json_obj_get(js, t, l, "lat") : -1;
        int io = t[l].type == J_OBJ ? json_obj_get(js, t, l, "lon") : -1;
        if (ia >= 0 && io >= 0 && json_num(js, t, ia, &la) && json_num(js, t, io, &lo) && fabs(la) <= 90 &&
            fabs(lo) <= 180) {
            c.loc_valid = true;
            c.lat = la;
            c.lon = lo;
        } else {
            c.loc_valid = false;
        }
        dec_host_set_location(a->host, c.loc_valid, c.lat, c.lon);
    }
    /* Un cambio manual de centro o tasa saca de la configuración de perfil */
    if (f & (CFG_CENTER | CFG_RATE))
        engine_set_profile(a->eng, NULL);
    if (f)
        engine_request(a->eng, &c, f);
    if (has_ssrc) {
        char err[200];
        if (engine_set_spectrum_src(a->eng, ssrc, err, sizeof err) < 0)
            send_error(a, -1, err);
    }
}

static void cmd_channel(app_t *a, int client, const char *js, const jtok_t *t)
{
    chan_params_t p;
    if (!engine_get_channel(a->eng, client, &p))
        return;
    double v;
    bool b;
    char s[16];
    bool mode_set = false;
    if (get_str(js, t, "mode", s, sizeof s)) {
        int m = mode_from_name(s);
        if (m >= 0) {
            p.mode = (demod_mode_t)m;
            mode_set = true;
        }
    }
    int kf = num_chk(a, client, "channel", js, t, "freq", 0, 6e9, &v);
    if (kf > 0) p.freq_hz = (int64_t)v;
    int kb = num_chk(a, client, "channel", js, t, "bw", 0, 2e6, &v);
    if (kb > 0) p.bw_hz = (int)v;
    else if (mode_set) p.bw_hz = mode_default_bw(p.mode);
    p.bw_hz = CLAMP(p.bw_hz, 100, mode_max_bw(p.mode));
    int sq = json_obj_get(js, t, 0, "squelch");
    if (sq >= 0) {
        if (json_is_null(t, sq)) p.squelch_db = NAN;
        else if (json_num(js, t, sq, &v) && v >= -200 && v <= 50) p.squelch_db = (float)v;
    }
    if (num_chk(a, client, "channel", js, t, "deemph", 0, 1000, &v) > 0) p.deemph_us = (int)v;
    if (get_bool(js, t, "stereo", &b)) p.stereo = b;
    if (get_bool(js, t, "audio", &b)) p.audio_on = b;
    engine_set_channel(a->eng, client, &p);
}

/* "file" (por defecto), "loopback" o "air"; cualquier otra cosa es un error (no se degrada en silencio). */
static bool parse_tx_mode(const char *js, const jtok_t *t, tx_mode_t *m)
{
    char mode[16] = "file";
    *m = TX_FILE;
    if (json_obj_get(js, t, 0, "mode") < 0)
        return true;
    if (!get_str(js, t, "mode", mode, sizeof mode))
        return false;
    if (strcmp(mode, "file") == 0) *m = TX_FILE;
    else if (strcmp(mode, "loopback") == 0) *m = TX_LOOPBACK;
    else if (strcmp(mode, "air") == 0) *m = TX_AIR;
    else return false;
    return true;
}

static void cmd_tx(app_t *a, const char *js, const jtok_t *t)
{
    tx_req_t r;
    memset(&r, 0, sizeof r);
    r.repeat = 1;
    double v;
    if (!get_str(js, t, "proto", r.proto, sizeof r.proto)) {
        send_error(a, -1, "tx: falta 'proto'");
        return;
    }
    if (!parse_tx_mode(js, t, &r.mode)) {
        send_error(a, -1, "tx: 'mode' debe ser file, loopback o air");
        return;
    }
    int bad = 0, k;
    if ((k = num_in(js, t, "freq", 0, 6e9, &v)) > 0) r.freq = (int64_t)v; else bad |= k < 0;
    if ((k = num_in(js, t, "center", 0, 6e9, &v)) > 0) r.center = (int64_t)v; else bad |= k < 0;
    if ((k = num_in(js, t, "rate", 0, 61.44e6, &v)) > 0) r.rate = (uint32_t)v; else bad |= k < 0;
    if ((k = num_in(js, t, "gain", -100, 100, &v)) > 0) r.gain_db = (int)v; else bad |= k < 0;
    if ((k = num_in(js, t, "repeat", 1, 1000, &v)) > 0) r.repeat = (int)v; else bad |= k < 0;
    if (bad) {
        send_error(a, -1, "tx: freq, center, rate, gain o repeat fuera de rango");
        return;
    }
    char *params = get_obj_text(js, t, "params");
    r.params = params;
    char err[300];
    tx_submit(a->tx, &r, err, sizeof err); /* envía tx_result por sí mismo, con éxito o error */
    free(params);
}

/* {"cmd":"tx_play","name":"<grabación>","mode":"file"|"loopback","repeat":N}
 * Solo file/loopback (tx_play rechaza air: ataque de repetición). */
static void cmd_tx_play(app_t *a, const char *js, const jtok_t *t)
{
    tx_play_req_t r;
    memset(&r, 0, sizeof r);
    r.repeat = 1;
    double v;
    if (!get_str(js, t, "name", r.name, sizeof r.name)) {
        send_error(a, -1, "tx_play: falta 'name'");
        return;
    }
    if (!parse_tx_mode(js, t, &r.mode)) {
        send_error(a, -1, "tx_play: 'mode' debe ser file o loopback");
        return;
    }
    if (num_in(js, t, "repeat", 1, 1000, &v) > 0) r.repeat = (int)v;
    char err[300];
    tx_play(a->tx, &r, err, sizeof err); /* envía tx_result por sí mismo */
}

/* Captura del receptor para tx_cal (tx_set_rx_capture). La llama el hilo de TX
 * mientras emite la ráfaga; el motor copia las muestras desde el hilo DSP. */
static int cal_rx_capture(void *ctx, int64_t freq, uint32_t rate, int16_t *iq, unsigned n, char *err, size_t errlen)
{
    app_t *a = ctx;
    return engine_capture_iq(a->eng, freq, rate, iq, n, 5.0, err, errlen);
}

typedef struct {
    app_t *a;
    int64_t freq;
    uint32_t rate;
} cal_arg_t;

/* Hilo de calibración: sintoniza el receptor en freq/rate (el TX en bucle interno
 * RFIC_BIST ve la señal en la misma frecuencia que el RX), lanza la calibración,
 * espera a que acabe y devuelve el receptor a su configuración anterior.
 * Va en un hilo propio para no bloquear el hilo de red mientras el motor retunea. */
static void *cal_thread(void *arg)
{
    cal_arg_t c = *(cal_arg_t *)arg;
    free(arg);
    app_t *a = c.a;
    pthread_mutex_lock(&a->cal_lock);
    if (a->closing) {
        pthread_mutex_unlock(&a->cal_lock);
        a->cal_running = 0;
        return NULL;
    }
    engine_cfg_t prev;
    engine_get_config(a->eng, &prev);
    char err[300] = "";
    bool retune = a->tx_enabled && a->can_tune; /* un fichero no se sintoniza; tx_cal_run lo rechazará */
    if (retune && !engine_wait_tuned(a->eng, c.freq, c.rate, 4.0))
        LOG_W("tx_cal: el receptor no llegó a %.3f MHz / %u sps a tiempo", c.freq / 1e6, c.rate);
    if (tx_cal_run(a->tx, c.freq, c.rate, err, sizeof err) == 0)
        tx_wait(a->tx);
    if (retune)
        engine_request(a->eng, &prev, CFG_CENTER | CFG_RATE | CFG_RFBW);
    pthread_mutex_unlock(&a->cal_lock);
    a->cal_running = 0;
    return NULL;
}

/* {"cmd":"tx_cal","freq":Hz,"rate":sps}: calibración del TX en bucle interno.
 * Exige --enable-tx; nunca emite por antena. El resultado llega como mensaje
 * "tx_cal" y los rechazos como "tx_result". AVISO: el bucle RFIC_BIST es digital
 * y no pasa por el LO de TX; la calibración real exige cable TX→RX con
 * atenuador ≥ 30 dB y permiso del usuario (docs/arquitectura/TX.md). */
static void cmd_tx_cal(app_t *a, const char *js, const jtok_t *t)
{
    double v;
    int64_t freq = 433920000LL; /* mismos valores por omisión que tx_cal_run */
    uint32_t rate = 1000000;
    if (num_in(js, t, "freq", 0, 6e9, &v) > 0 && v > 0) freq = (int64_t)v;
    if (num_in(js, t, "rate", 0, 61.44e6, &v) > 0 && v > 0) rate = (uint32_t)v;
    if (a->cal_running) {
        send_error(a, -1, "tx_cal: ya hay una calibración en curso");
        return;
    }
    if (a->cal_started) { /* la anterior terminó: se une antes de lanzar otra */
        pthread_join(a->cal_th, NULL);
        a->cal_started = false;
    }
    cal_arg_t *c = malloc(sizeof *c);
    if (!c) {
        send_error(a, -1, "tx_cal: sin memoria");
        return;
    }
    *c = (cal_arg_t){a, freq, rate};
    a->cal_running = 1;
    if (pthread_create(&a->cal_th, NULL, cal_thread, c) != 0) {
        a->cal_running = 0;
        free(c);
        send_error(a, -1, "tx_cal: no se pudo crear el hilo");
        return;
    }
    a->cal_started = true;
}

/* ---- TV (src/tv/tv.c): el gestor pide sintonizar y crear/quitar la instancia "tdt" ---- */
static int tv_op_tune(void *app, int64_t freq, char *err, size_t errlen)
{
    app_t *a = app;
    if (!a->can_tune) {
        snprintf(err, errlen, "la fuente no se puede sintonizar (fichero): para probar la TV sin radio usa --tv-ts");
        return -1;
    }
    /* DVB-T ocupa 8 MHz: 10 Msps cubren el canal con margen para el DDC de 64/7 MHz */
    engine_cfg_t c;
    engine_get_config(a->eng, &c);
    c.center_hz = freq;
    c.rate = 10000000;
    c.rf_bw = 8000000;
    engine_set_profile(a->eng, NULL);
    engine_request(a->eng, &c, CFG_CENTER | CFG_RATE | CFG_RFBW);
    return dec_host_add(a->host, "tdt", freq, NULL, 2, err, errlen);
}

static void tv_op_untune(void *app, int inst) { dec_host_remove(((app_t *)app)->host, inst); }

static void on_text(void *app, int client, const char *msg, size_t len)
{
    app_t *a = app;
    jtok_t t[MAX_TOKENS];
    int n = json_parse(msg, len, t, MAX_TOKENS);
    if (n < 1 || t[0].type != J_OBJ) {
        send_error(a, client, "mensaje JSON no válido");
        return;
    }
    char cmd[24];
    if (!get_str(msg, t, "cmd", cmd, sizeof cmd)) {
        send_error(a, client, "falta 'cmd'");
        return;
    }
    double v;
    char err[300];
    if (strcmp(cmd, "config") == 0) {
        cmd_config(a, msg, t);
    } else if (strcmp(cmd, "channel") == 0) {
        cmd_channel(a, client, msg, t);
    } else if (strcmp(cmd, "profile") == 0) {
        char id[32];
        const profile_t *p = get_str(msg, t, "id", id, sizeof id) ? profile_find(id) : NULL;
        if (!p)
            send_error(a, client, "perfil desconocido");
        else {
            int pr = profile_apply(p, a->eng, a->host, client, err, sizeof err);
            if (pr < 0) {
                send_error(a, client, err);
            } else if (pr == 1) {
                /* Canal de audio movido: aviso informativo (no error) y estado con la frecuencia elegida */
                chan_params_t cp;
                bool have = engine_get_channel(a->eng, client, &cp);
                sb_t sb = {0};
                sb_puts(&sb, "{\"type\":\"error\",\"level\":\"info\",\"msg\":");
                sb_json_str(&sb, err);
                if (have)
                    sb_printf(&sb, ",\"chan_freq\":%lld", (long long)cp.freq_hz);
                sb_puts(&sb, "}");
                server_send_text(a->srv, client, sb.data, sb.len);
                sb_free(&sb);
                sb_t st = {0};
                engine_status_json(a->eng, &st);
                if (have && st.len > 0 && st.data[st.len - 1] == '}') {
                    st.len--;
                    sb_printf(&st, ",\"chan_freq\":%lld}", (long long)cp.freq_hz);
                }
                server_send_text(a->srv, client, st.data, st.len);
                sb_free(&st);
            }
        }
    } else if (strcmp(cmd, "dec_add") == 0) {
        char proto[24];
        if (!get_str(msg, t, "proto", proto, sizeof proto) || num_chk(a, client, "dec_add", msg, t, "freq", 0, 6e9, &v) <= 0) {
            if (json_obj_get(msg, t, 0, "freq") < 0 || !get_str(msg, t, "proto", proto, sizeof proto))
                send_error(a, client, "dec_add: 'proto' y 'freq' son obligatorios");
        } else {
            char *opts = get_obj_text(msg, t, "opts");
            if (dec_host_add(a->host, proto, (int64_t)v, opts, 0, err, sizeof err) < 0)
                send_error(a, client, err);
            free(opts);
        }
    } else if (strcmp(cmd, "dec_remove") == 0) {
        if (num_in(msg, t, "inst", 0, 1e6, &v) <= 0 || dec_host_remove(a->host, (int)v) < 0)
            send_error(a, client, "dec_remove: instancia inexistente");
    } else if (strcmp(cmd, "dec_tune") == 0) {
        double f;
        if (num_in(msg, t, "inst", 0, 1e6, &v) <= 0 || num_in(msg, t, "freq", 0, 6e9, &f) <= 0 ||
            dec_host_retune(a->host, (int)v, (int64_t)f) < 0)
            send_error(a, client, "dec_tune: instancia o frecuencia no válida");
    } else if (strcmp(cmd, "record") == 0) {
        bool on = false;
        get_bool(msg, t, "on", &on);
        engine_record(a->eng, on);
    } else if (strcmp(cmd, "rec_delete") == 0) {
        char name[200];
        if (!get_str(msg, t, "name", name, sizeof name) || engine_delete_recording(a->eng, name) < 0)
            send_error(a, client, "no se pudo borrar la grabación");
        else
            engine_send_snapshot(a->eng, -1);
    } else if (strcmp(cmd, "tx") == 0) {
        cmd_tx(a, msg, t);
    } else if (strcmp(cmd, "tx_play") == 0) {
        cmd_tx_play(a, msg, t);
    } else if (strcmp(cmd, "tx_cal") == 0) {
        cmd_tx_cal(a, msg, t);
    } else if (strcmp(cmd, "tx_stop") == 0) {
        tx_stop(a->tx);
    } else if (strcmp(cmd, "sweep") == 0) {
        /* Barrido rápido: {"cmd":"sweep","start":Hz,"stop":Hz,"fft":N} (ver docs/hardware/HARDWARE.md) */
        double f0, f1, nfft = 1024;
        if (num_chk(a, client, "sweep", msg, t, "fft", 0, 1 << 20, &nfft) < 0)
            return;
        if (num_in(msg, t, "start", 0, 6e9, &f0) <= 0 || num_in(msg, t, "stop", 0, 6e9, &f1) <= 0)
            send_error(a, client, "sweep: 'start' y 'stop' (Hz, 0..6e9) son obligatorios");
        else if (engine_sweep_start(a->eng, (int64_t)f0, (int64_t)f1, (int)nfft, err, sizeof err) < 0)
            send_error(a, client, err);
    } else if (strcmp(cmd, "sweep_stop") == 0) {
        engine_sweep_stop(a->eng);
    } else if (strcmp(cmd, "tv") == 0) {
        /* {"cmd":"tv","freq":Hz[,"sid":N]}: sintoniza un múltiplex y crea la instancia "tdt" */
        double f, sid = 0;
        if (num_chk(a, client, "tv", msg, t, "sid", 0, 65535, &sid) < 0)
            return;
        if (!a->tv || num_in(msg, t, "freq", 0, 6e9, &f) <= 0)
            send_error(a, client, "tv: 'freq' (Hz, 0..6e9) es obligatorio");
        else if (tv_cmd_tune(a->tv, (int64_t)f, (int)sid, err, sizeof err) < 0)
            send_error(a, client, err);
    } else if (strcmp(cmd, "tv_service") == 0) {
        if (!a->tv || num_in(msg, t, "sid", 0, 65535, &v) <= 0)
            send_error(a, client, "tv_service: 'sid' (0..65535) es obligatorio");
        else if (tv_cmd_service(a->tv, (int)v, err, sizeof err) < 0)
            send_error(a, client, err);
    } else if (strcmp(cmd, "tv_stop") == 0) {
        if (a->tv)
            tv_cmd_stop(a->tv);
    } else if (strcmp(cmd, "tv_scan") == 0) {
        /* {"cmd":"tv_scan"[,"start":Hz,"stop":Hz]}: por omisión canales 21–48 */
        double f0 = 0, f1 = 0;
        if (num_chk(a, client, "tv_scan", msg, t, "start", 0, 6e9, &f0) < 0 ||
            num_chk(a, client, "tv_scan", msg, t, "stop", 0, 6e9, &f1) < 0)
            return;
        if (!a->tv || tv_cmd_scan(a->tv, (int64_t)f0, (int64_t)f1, err, sizeof err) < 0)
            send_error(a, client, a->tv ? err : "tv: no disponible");
    } else {
        send_error(a, client, "orden desconocida");
    }
}

static void on_open(void *app, int client)
{
    app_t *a = app;
    engine_add_client(a->eng, client);
    /* status, signals, recording */
    engine_send_snapshot(a->eng, client);
    sb_t sb = {0};
    dec_host_catalog_json(&sb);
    send_sb(a, client, &sb);
    profiles_json(&sb);
    send_sb(a, client, &sb);
    tx_policy_json(a->tx, &sb);
    send_sb(a, client, &sb);
    /* dec_list, dec_state cacheados y los últimos 100 eventos (replay:true) */
    dec_host_send_snapshot(a->host, client);
    if (a->tv)
        tv_send_snapshot(a->tv, client);
}

static void on_close(void *app, int client) { engine_remove_client(((app_t *)app)->eng, client); }

static void on_status(void *app, sb_t *body) { engine_status_json(((app_t *)app)->eng, body); }

/* ======================================================================= */
/*  --gen                                                                  */
/* ======================================================================= */
static int do_gen(const char *id, const char *params, int64_t freq, int64_t center, int64_t rate, const char *out)
{
    const modulator_desc_t *m = modulator_find(id);
    if (!m) {
        fprintf(stderr, "No hay modulador '%s'\n", id);
        return 1;
    }
    if (strcmp(id, "nr") == 0 && freq > 0) {
        /* El decodificador NR solo busca SSB en la rejilla GSCN (TS 38.104 tabla 5.4.3.1-1):
         * una frecuencia fuera de ella genera un fichero que --decode nr no encontrará. */
        int64_t cand[512];
        int nc = nr_gscn_candidates((double)freq - 30e6, (double)freq + 30e6, cand, 512), best = -1;
        for (int i = 0; i < nc; i++)
            if (best < 0 || llabs(cand[i] - freq) < llabs(cand[best] - freq))
                best = i;
        if (best >= 0 && cand[best] != freq)
            fprintf(stderr,
                    "Aviso: %.6f MHz no está en la rejilla GSCN; el decodificador NR no lo encontrará. "
                    "La frecuencia GSCN más cercana es %.6f MHz (--gen-freq %lld).\n",
                    freq / 1e6, cand[best] / 1e6, (long long)cand[best]);
        else if (best < 0)
            fprintf(stderr, "Aviso: no hay ninguna frecuencia GSCN a ±30 MHz de %.6f MHz.\n", freq / 1e6);
    }
    char err[300] = "";
    iqbuf_t iq = {0};
    tx_meta_t mt;
    if (tx_render(m, params, freq, center, (double)rate, &iq, &mt, err, sizeof err) < 0) {
        fprintf(stderr, "No se pudo generar %s: %s\n", id, err);
        return 1;
    }
    char prefix[700];
    snprintf(prefix, sizeof prefix, "%s", out && out[0] ? out : "gen");
    size_t l = strlen(prefix);
    if (l > 11 && (strcmp(prefix + l - 11, ".sigmf-meta") == 0 || strcmp(prefix + l - 11, ".sigmf-data") == 0))
        prefix[l - 11] = '\0';
    char desc[200];
    snprintf(desc, sizeof desc, "%s sintético (--gen)", m->name);
    if (tx_write_sigmf(prefix, &iq, 1, mt.center, desc, err, sizeof err) < 0) {
        fprintf(stderr, "%s\n", err);
        iqbuf_free(&iq);
        return 1;
    }
    printf("Generado %s.sigmf-meta: %s, centro %.6f MHz, %.0f sps, %.3f s, ancho ocupado %.0f Hz\n", prefix, m->name,
           mt.center / 1e6, mt.rate, mt.seconds, mt.occ_bw_hz);
    iqbuf_free(&iq);
    return 0;
}

/* ======================================================================= */
/*  Opciones                                                               */
/* ======================================================================= */
static void usage(const char *prog)
{
    printf("%s %s — servidor SDR para bladeRF 2.0 micro\n\n"
           "Uso: %s [opciones]\n\n"
           "  --host IP            dirección de escucha (127.0.0.1; 0.0.0.0 para la LAN)\n"
           "  --port N             puerto HTTP/WebSocket (%d)\n"
           "  --serial S           número de serie del bladeRF\n"
           "  --replay FICHERO     reproduce un .sigmf-meta/.cs16/.cs8/.cu8 en bucle (sin hardware)\n"
           "  --rate Hz --freq Hz  tasa y frecuencia central (en ficheros crudos, su tasa y centro)\n"
           "  --no-gpu             espectro en CPU\n"
           "  --web DIR            carpeta de la interfaz web (por defecto web/)\n"
           "  --recdir DIR         carpeta de grabaciones (recordings/)\n"
           "  --config FICHERO     configuración persistente (~/.config/bladerf-lab/config.json; 'none' = sin)\n"
           "  --profile ID         aplica un perfil al arrancar\n"
           "  --decode ID[@Hz]     añade un decodificador (repetible)\n"
           "  --print              imprime los eventos de decodificadores por stdout (JSONL)\n"
           "  --log-events DIR     guarda los eventos en DIR/events_<fecha>.jsonl\n"
           "  --replay-fast        con --replay: sin ritmo real ni bucle, sin descartar bloques (contrapresion);\n"
           "                       al acabar el fichero vacia las colas y sale con 0\n"
           "  --labels F.sigmf-meta  con --replay: escribe anotaciones SigMF en tres niveles (verified/unknown/\n"
           "                       empty implicito, ver docs); implica --replay-fast\n"
           "  --duration S         termina tras S segundos\n"
           "  --tv-ts FICHERO      reproduce un .ts (H.264 + MP2) en bucle en la pestaña TV, sin radio\n"
           "  --gen ID             genera una señal SigMF con el modulador ID y termina\n"
           "    --gen-params JSON --gen-freq Hz --gen-center Hz --gen-rate Hz --out PREFIJO\n"
           "  --enable-tx          permite TX en bucle interno (loopback)\n"
           "  --enable-tx-air      permite TX por antena (política de docs/arquitectura/TX.md)\n"
           "  --callsign C         indicativo de radioaficionado\n"
           "  --tx-max-gain dB     ganancia máxima de TX (0)\n"
           "  -v                   mensajes de depuración\n",
           APP_NAME, APP_VERSION, prog, DEFAULT_PORT);
}

enum {
    O_HOST = 256, O_PORT, O_SERIAL, O_REPLAY, O_RATE, O_FREQ, O_NOGPU, O_WEB, O_RECDIR, O_CONFIG, O_PROFILE,
    O_DECODE, O_PRINT, O_LOGEV, O_DURATION, O_GEN, O_GENP, O_GENF, O_GENC, O_GENR, O_OUT, O_ETX, O_ETXA, O_CALL,
    O_TXG, O_TVTS, O_RFAST, O_LABELS
};

static const struct option LONGOPTS[] = {
    {"host", 1, 0, O_HOST}, {"port", 1, 0, O_PORT}, {"serial", 1, 0, O_SERIAL}, {"replay", 1, 0, O_REPLAY},
    {"rate", 1, 0, O_RATE}, {"freq", 1, 0, O_FREQ}, {"no-gpu", 0, 0, O_NOGPU}, {"web", 1, 0, O_WEB},
    {"recdir", 1, 0, O_RECDIR}, {"config", 1, 0, O_CONFIG}, {"profile", 1, 0, O_PROFILE}, {"decode", 1, 0, O_DECODE},
    {"print", 0, 0, O_PRINT}, {"log-events", 1, 0, O_LOGEV}, {"duration", 1, 0, O_DURATION}, {"gen", 1, 0, O_GEN},
    {"gen-params", 1, 0, O_GENP}, {"gen-freq", 1, 0, O_GENF}, {"gen-center", 1, 0, O_GENC},
    {"gen-rate", 1, 0, O_GENR}, {"out", 1, 0, O_OUT}, {"enable-tx", 0, 0, O_ETX}, {"enable-tx-air", 0, 0, O_ETXA},
    {"callsign", 1, 0, O_CALL}, {"tx-max-gain", 1, 0, O_TXG}, {"tv-ts", 1, 0, O_TVTS}, {"replay-fast", 0, 0, O_RFAST}, {"labels", 1, 0, O_LABELS}, {"help", 0, 0, 'h'}, {0, 0, 0, 0},
};

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Busca web/ junto al cwd o al ejecutable (build/x/bladerf-lab → ../../web). */
static void find_webroot(const char *argv0, char *out, size_t n)
{
    char exe[600] = "";
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k > 0)
        exe[k] = '\0';
    else
        snprintf(exe, sizeof exe, "%s", argv0);
    char *sl = strrchr(exe, '/');
    if (sl)
        *sl = '\0';
    char cand[4][1024];
    snprintf(cand[0], sizeof cand[0], "web");
    snprintf(cand[1], sizeof cand[1], "%s/web", exe);
    snprintf(cand[2], sizeof cand[2], "%s/../web", exe);
    snprintf(cand[3], sizeof cand[3], "%s/../../web", exe);
    for (int i = 0; i < 4; i++)
        if (is_dir(cand[i])) {
            size_t l = MIN(strlen(cand[i]), n - 1); /* recorte voluntario */
            memcpy(out, cand[i], l);
            out[l] = '\0';
            return;
        }
    snprintf(out, n, "web");
}

static void *duration_thread(void *arg)
{
    double secs = *(double *)arg;
    double t0 = now_monotonic();
    while (!g_stop && now_monotonic() - t0 < secs)
        usleep(20000);
    g_stop = 1;
    if (g_srv)
        server_stop(g_srv);
    return NULL;
}

/* --replay-fast: cuando el motor ha leído todo el fichero y lo ha entregado al host, espera a que
 * las instancias vacíen sus colas y pide el cierre (salida 0 tras volcar eventos y etiquetas). */
static void *finish_thread(void *arg)
{
    app_t *a = arg;
    while (!g_stop && !engine_finished(a->eng))
        usleep(5000);
    if (!g_stop)
        dec_host_flush(a->host, 600.0);
    g_stop = 1;
    if (g_srv)
        server_stop(g_srv);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *serial = NULL, *replay = NULL, *web = NULL, *recdir = "recordings";
    const char *cfg_arg = NULL, *profile_id = NULL, *logev = NULL, *gen = NULL, *gen_params = NULL, *out = NULL;
    const char *decodes[32], *tv_ts = NULL;
    int ndec = 0, port = DEFAULT_PORT;
    int64_t rate_opt = 0, freq_opt = 0, gen_freq = 0, gen_center = 0, gen_rate = 0;
    bool no_gpu = false, print = false, replay_fast = false;
    const char *labels_path = NULL;
    double duration = 0;
    tx_policy_t pol = {0};
    int c;

    while ((c = getopt_long(argc, argv, "vh", LONGOPTS, NULL)) != -1) {
        switch (c) {
        case O_HOST: host = optarg; break;
        case O_PORT: port = atoi(optarg); break;
        case O_SERIAL: serial = optarg; break;
        case O_REPLAY: replay = optarg; break;
        case O_RATE: if (!parse_hz(optarg, &rate_opt)) { fprintf(stderr, "--rate no válido\n"); return 2; } break;
        case O_FREQ: if (!parse_hz(optarg, &freq_opt)) { fprintf(stderr, "--freq no válido\n"); return 2; } break;
        case O_NOGPU: no_gpu = true; break;
        case O_WEB: web = optarg; break;
        case O_RECDIR: recdir = optarg; break;
        case O_CONFIG: cfg_arg = optarg; break;
        case O_PROFILE: profile_id = optarg; break;
        case O_DECODE:
            if (ndec < 32) decodes[ndec++] = optarg;
            break;
        case O_PRINT: print = true; break;
        case O_LOGEV: logev = optarg; break;
        case O_DURATION: duration = atof(optarg); break;
        case O_GEN: gen = optarg; break;
        case O_GENP: gen_params = optarg; break;
        case O_GENF: if (!parse_hz(optarg, &gen_freq)) { fprintf(stderr, "--gen-freq no válido\n"); return 2; } break;
        case O_GENC: if (!parse_hz(optarg, &gen_center)) { fprintf(stderr, "--gen-center no válido\n"); return 2; } break;
        case O_GENR: if (!parse_hz(optarg, &gen_rate)) { fprintf(stderr, "--gen-rate no válido\n"); return 2; } break;
        case O_OUT: out = optarg; break;
        case O_RFAST: replay_fast = true; break;
        case O_LABELS: labels_path = optarg; replay_fast = true; break;
        case O_ETX: pol.enabled = true; break;
        case O_ETXA: pol.air = true; pol.enabled = true; break;
        case O_CALL: snprintf(pol.callsign, sizeof pol.callsign, "%s", optarg); break;
        case O_TXG: pol.max_gain_db = atoi(optarg); break;
        case O_TVTS: tv_ts = optarg; break;
        case 'v': g_log_level = LOG_LVL_DEBUG; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (gen)
        return do_gen(gen, gen_params, gen_freq, gen_center, gen_rate, out);

    static app_t app;
    memset(&app, 0, sizeof app);
    pthread_mutex_init(&app.out_lock, NULL);
    app.print_events = print;

    /* --- configuración --- */
    engine_cfg_t cfg;
    default_cfg(&cfg);
    if (cfg_arg && strcmp(cfg_arg, "none") == 0) {
        app.cfg_path[0] = '\0';
    } else if (cfg_arg) {
        snprintf(app.cfg_path, sizeof app.cfg_path, "%s", cfg_arg);
    } else {
        const char *home = getenv("HOME");
        if (home && home[0])
            snprintf(app.cfg_path, sizeof app.cfg_path, "%s/.config/bladerf-lab/config.json", home);
    }
    if (app.cfg_path[0])
        load_config(app.cfg_path, &cfg);
    app.saved_center = cfg.center_hz;
    app.saved_rate = cfg.rate;
    if ((replay_fast || labels_path) && !replay) {
        fprintf(stderr, "--replay-fast y --labels exigen --replay FICHERO\n");
        return 2;
    }
    if (freq_opt && !replay) cfg.center_hz = freq_opt;
    if (rate_opt && !replay) cfg.rate = (uint32_t)rate_opt;

    /* --- fuente --- */
    char err[300] = "";
    if (replay)
        app.src = source_open_file(replay, (uint32_t)rate_opt, freq_opt, err, sizeof err);
    else
        app.src = source_open_bladerf(serial, err, sizeof err);
    if (!app.src) {
        if (replay)
            fprintf(stderr, "No se pudo abrir el fichero: %s\n", err);
        else
            fprintf(stderr,
                    "No se pudo abrir el bladeRF: %s\n"
                    "Puede que otro programa lo tenga abierto (p. ej. el servicio openwebrx: "
                    "'systemctl status openwebrx'); páralo o usa --replay <fichero> para trabajar sin hardware.\n",
                    err);
        return 1;
    }
    app.can_tune = source_info(app.src)->can_tune;

    /* --- servidor, host, motor, TX --- */
    char webroot[700];
    if (web)
        snprintf(webroot, sizeof webroot, "%s", web);
    else
        find_webroot(argv[0], webroot, sizeof webroot);
    server_callbacks_t cb = {.on_open = on_open, .on_text = on_text, .on_close = on_close, .on_status = on_status};
    app.srv = server_create(host, port, webroot, recdir, &cb, &app, err, sizeof err);
    if (!app.srv) {
        fprintf(stderr, "No se pudo iniciar el servidor: %s\n", err);
        source_close(app.src);
        return 1;
    }
    app.host = dec_host_create(app.srv);
    app.eng = engine_create(app.src, app.srv, &cfg, !no_gpu, recdir);
    app.tx = tx_create(app.src, app.srv, recdir, &pol);
    app.tx_enabled = pol.enabled;
    pthread_mutex_init(&app.cal_lock, NULL);
    if (app.tx)
        tx_set_rx_capture(app.tx, cal_rx_capture, &app);
    if (no_gpu && app.host)
        dec_host_set_gpu(app.host, false);
    if (!app.host || !app.eng || !app.tx) {
        fprintf(stderr, "Sin memoria al iniciar\n");
        return 1;
    }
    engine_set_host(app.eng, app.host);
    if (replay_fast) {
        /* Sin ritmo real: el lector espera al DSP y el DSP al host (nunca se descarta un bloque) */
        engine_set_replay_fast(app.eng, true);
        dec_host_set_backpressure(app.host, true);
    }
    if (labels_path) {
        const char *fmt = source_format(app.src);
        const char *dt = !strcmp(fmt, "cs8") || !strcmp(fmt, "sc8q7") ? "ci8" : !strcmp(fmt, "cu8") ? "cu8" : "ci16_le";
        char ds[700];
        const char *base = strrchr(replay, '/');
        base = base ? base + 1 : replay;
        snprintf(ds, sizeof ds, "%s", base);
        char *dot = strstr(ds, ".sigmf-meta");
        if (dot)
            strcpy(dot, ".sigmf-data");
        app.labels = labels_create(dt, ds);
        if (!app.labels) {
            fprintf(stderr, "Sin memoria al iniciar\n");
            return 1;
        }
        labels_set_source(app.labels, (double)source_rate(app.src), source_frequency(app.src));
        engine_set_label_sink(app.eng, labels_on_signals, app.labels);
    }
    tv_ops_t tvops = {.app = &app, .tune = tv_op_tune, .untune = tv_op_untune};
    app.tv = tv_create(app.srv, &tvops);
    tv_set_global(app.tv);
    if (app.tv && tv_ts && tv_play_file(app.tv, tv_ts, err, sizeof err) < 0) {
        fprintf(stderr, "--tv-ts: %s\n", err);
        return 2;
    }
    engine_on_config_change(app.eng, save_config, &app);
    engine_cfg_t eff;
    engine_get_config(app.eng, &eff);
    dec_host_set_location(app.host, eff.loc_valid, eff.lat, eff.lon);
    dec_host_on_event(app.host, on_dec_event, &app);
    if (logev) {
        mkdir_p(logev);
        mkdir(logev, 0755);
        char path[700];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(path, sizeof path, "%s/events_%04d%02d%02d_%02d%02d%02d.jsonl", logev, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        app.log_events = fopen(path, "a");
        if (!app.log_events)
            LOG_W("No se pudo abrir %s: %s", path, strerror(errno));
        else
            LOG_I("Registrando eventos en %s", path);
    }

    if (profile_id) {
        const profile_t *p = profile_find(profile_id);
        if (!p) {
            fprintf(stderr, "Perfil desconocido: %s\n", profile_id);
            return 2;
        }
        profile_apply(p, app.eng, app.host, -1, err, sizeof err);
    }
    for (int i = 0; i < ndec; i++) {
        char spec[120], *at;
        snprintf(spec, sizeof spec, "%s", decodes[i]);
        int64_t f = 0;
        if ((at = strchr(spec, '@'))) {
            *at = '\0';
            if (!parse_hz(at + 1, &f)) {
                fprintf(stderr, "--decode %s: frecuencia no válida\n", decodes[i]);
                return 2;
            }
        }
        const decoder_desc_t *d = decoder_find(spec);
        if (d && d->kind == DEC_WIDE && f == 0)
            f = source_frequency(app.src);
        if (dec_host_add(app.host, spec, f, NULL, 0, err, sizeof err) < 0) {
            fprintf(stderr, "--decode %s: %s\n", decodes[i], err);
            return 2;
        }
    }

    /* --- señales y arranque --- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_srv = app.srv;

    LOG_I("%s %s — %s (%s)", APP_NAME, APP_VERSION, source_info(app.src)->board, source_info(app.src)->kind);
    LOG_I("Interfaz en http://%s:%d/  (web: %s, grabaciones: %s)", host, port, webroot, recdir);
    if (engine_start(app.eng) != 0) {
        fprintf(stderr, "No se pudo arrancar el motor DSP\n");
        return 1;
    }
    pthread_t fin;
    bool fin_started = false;
    if (replay_fast && pthread_create(&fin, NULL, finish_thread, &app) == 0)
        fin_started = true;
    pthread_t dur;
    bool dur_started = false;
    if (duration > 0 && pthread_create(&dur, NULL, duration_thread, &duration) == 0)
        dur_started = true;

    if (!g_stop)
        server_run(app.srv);

    /* --- cierre ordenado: primero lo que produce datos --- */
    LOG_I("Cerrando…");
    g_stop = 1;
    if (dur_started)
        pthread_join(dur, NULL);
    if (fin_started)
        pthread_join(fin, NULL);
    /* La calibración usa el motor y el TX: se señaliza y se une antes de pararlos. */
    app.closing = 1;
    if (app.cal_started) {
        tx_stop(app.tx);
        pthread_join(app.cal_th, NULL);
        app.cal_started = false;
    }
    if (app.tv)
        tv_quiesce(app.tv);
    engine_stop(app.eng);
    dec_host_flush(app.host, replay_fast ? 600.0 : 1.0);
    int rc = 0;
    if (app.labels) {
        int nv = 0, nu = 0, ng = 0;
        char lerr[300] = "";
        if (labels_write(app.labels, labels_path, engine_sample_count(app.eng), lerr, sizeof lerr, &nv, &nu, &ng) < 0) {
            fprintf(stderr, "--labels: %s\n", lerr);
            rc = 1;
        } else {
            fprintf(stderr, "%s: %d verified, %d unknown, %d gap (empty implicito)\n", labels_path, nv, nu, ng);
        }
    }
    dec_host_destroy(app.host);
    tv_destroy(app.tv);
    tx_destroy(app.tx);
    engine_destroy(app.eng);
    g_srv = NULL;
    server_destroy(app.srv);
    source_close(app.src);
    if (app.log_events)
        fclose(app.log_events);
    labels_destroy(app.labels);
    return rc;
}
