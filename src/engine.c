/*
 * engine.c — Hilo DSP principal. Ver engine.h y docs/arquitectura/ARQUITECTURA.md.
 *
 * Dos hilos (ver docs/arquitectura/RENDIMIENTO.md):
 *
 *   hilo lector ── source_read() ──► anillo de bloques (0,25–0,5 s) ──► hilo DSP
 *
 * El lector solo hace bladerf_sync_rx (o lee el fichero a ritmo real) y nunca
 * espera al procesado: así una pausa del DSP (un GC de la GPU, un decodificador
 * pesado, un cambio de tasa) no se convierte en un overrun del USB. Si el DSP se
 * atrasa más de lo que cabe en el anillo, el lector descarta el bloque más viejo
 * sin leer (cuenta como "drop"); los overruns que declara el hardware se cuentan
 * aparte ("overruns"). Los dos salen en status.stats.
 */
#include "engine.h"

#include "decoders/decoder.h"
#include "dsp/detector.h"
#include "dsp/fft.h"
#include "dsp/spectrum.h"
#include "sdr/sweep.h"
#include "util/recorder.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SLOTS 32
#define MAX_SIGNALS 96
/* Cuantificación del espectro (1 byte por bin): v = (dBFS − DB_MIN)/DB_STEP, 0…255.
 * Rango −145 … +8 dBFS: el techo deja margen sobre 0 dBFS (un tono recortado o I y Q a fondo
 * de escala llegan a ≈ +3 dBFS por bin) y el suelo queda por debajo del ruido de cuantificación
 * del ADC en la FFT más fina (≈ −119 dBFS por bin a 65536 con 12 bits). Paso 0,6 dB. */
#define DB_MIN -145.0f
#define DB_STEP 0.6f

/* Tipos de mensajes binarios (primer byte). Ver docs/arquitectura/WEBSOCKET.md. */
enum { BIN_SPECTRUM = 1, BIN_AUDIO = 2, BIN_METER = 3 };

typedef struct {
    bool used;
    int client;
    bool dirty;
    chan_params_t p;
    channel_t *ch;
    engine_t *e;
} slot_t;

struct engine {
    source_t *src;
    server_t *srv;
    recorder_t *rec;
    pthread_t thread;
    atomic_bool running;
    bool use_gpu;

    pthread_mutex_t lock;  /* protege todo lo de este bloque */
    engine_cfg_t cfg;      /* configuración aplicada */
    engine_cfg_t want;     /* configuración pedida */
    unsigned dirty;        /* campos pendientes de aplicar */
    int rec_cmd;           /* 0 nada, 1 empezar, 2 parar */
    slot_t slots[MAX_SLOTS];
    /* estadísticas */
    double meas_rate;
    _Atomic unsigned long hw_overruns; /* el hardware/USB perdió muestras (lector) */
    _Atomic unsigned long dsp_drops;   /* bloques descartados porque el DSP iba atrasado */
    _Atomic uint64_t rd_samples;       /* muestras entregadas por la fuente (para la tasa medida) */
    double stall_s;        /* pruebas: el DSP duerme esto antes del próximo bloque (engine_debug_stall) */
    float ring_ms;         /* cola del anillo en ms (valor máximo del último periodo) */
    float temp_c;
    int gain_now;
    bool dbm_valid;        /* hay calibración de ganancia: dbm = dBFS + dbm_off */
    double dbm_off;
    int sweep_cmd;         /* 0 nada, 1 empezar, 2 parar */
    sweep_cfg_t sweep_req;
    bool sweep_active;     /* publicado en "status" */
    sweep_info_t sweep_info;
    float dsp_load;
    /* --- espectro generado en la FPGA (imagen "lab"; ver fpga_lab.h) ---
     * lab y los campos fpga_* los toca solo el hilo DSP (y engine_create antes de
     * arrancarlo); fpga_active/fpga_info se publican bajo e->lock para el estado. */
    fpga_lab_t lab;
    bool lab_present_pub; /* copias de lab.present/version y del motor de espectro para "status" (bajo e->lock) */
    uint32_t lab_version_pub;
    bool spec_gpu;
    int ssrc_cmd;          /* 0 nada, 1 pasar a FPGA, 2 volver a CPU/GPU (bajo e->lock) */
    bool fpga_active;
    fpga_lab_parser_t *fpar;
    int fpga_n, fpga_k;
    float *fpga_db;        /* FPGA_SPEC_NMAX valores dBFS de la trama en curso */
    double fpga_t_last;    /* instante (monotónico) de la última trama útil */
    uint64_t fpga_frames, fpga_gaps; /* publicados en "status" (copia de las estadísticas del parser) */
    char spec_backend[160];
    char last_error[300];
    det_signal_t sigs[MAX_SIGNALS];
    int nsigs;
    float noise_db;
    char profile[40];
    dec_host_t *host;

    void (*cfg_cb)(void *, const engine_cfg_t *);
    void *cfg_cb_ctx;

    /* --- anillo lector → DSP (protegido por rg_m) --- */
    pthread_mutex_t rg_m;
    pthread_cond_t rg_data;     /* el lector avisa al DSP de un bloque nuevo */
    pthread_cond_t rg_ctl;      /* pausa/reanudación del lector */
    int16_t *ring;              /* rg_n bloques de rg_block muestras (I,Q Q11) */
    double *rg_t;               /* hora (Unix) a la que acabó de leerse cada bloque */
    uint64_t *rg_s0;            /* índice de muestra (desde que arrancó la fuente) de la 1.ª muestra de cada bloque */
    uint64_t rd_next;           /* próximo índice de muestra a leer (lo toca solo el lector) */
    bool fast;                  /* --replay-fast: sin descartes, el lector espera al DSP; acaba en SOURCE_EOF */
    bool rd_eof;                /* el lector llegó al final del fichero (bajo rg_m) */
    atomic_bool finished;       /* fast: EOF y todos los bloques ya entregados al host */
    unsigned rg_n, rg_block;
    uint64_t rg_head, rg_tail;  /* sin leer = [head, tail); el módulo rg_n da la ranura */
    bool rg_busy;               /* el DSP trabaja con la ranura head−1 */
    int rd_pause;               /* peticiones de pausa pendientes */
    bool rd_busy;               /* el lector está dentro de source_read */
    bool rd_run;
    bool rd_started;
    pthread_t rd_thread;
    unsigned long rd_err_seq;   /* el lector sube esto al fallar una lectura (y rd_err guarda el texto) */
    char rd_err[300];
    bool sweep_paused;          /* el lector está en pausa por un barrido */

    /* --- propiedad exclusiva del hilo DSP --- */
    spectrum_t *spec;
    detector_t *det;
    /* Detector de etiquetado (--labels): imagen propia a cada 0,5 s de SEÑAL (no de reloj), así que
     * funciona igual con --replay-fast. Ver engine_set_label_sink(). */
    engine_label_fn lab_fn;
    void *lab_ctx;
    detector_t *ldet;
    unsigned lab_frames;
    uint64_t cur_send;          /* índice de muestra tras el bloque en curso */
    double enbw_bins;           /* anchura de ruido equivalente de la ventana del espectro */
    double t_spec;              /* instante de la última trama de espectro */
    unsigned long rd_err_seen;
    cf32 *buf;
    unsigned block;
    float dc_re, dc_im;         /* continua estimada (Q11), media de bloque suavizada */
    bool dc_valid;
    double hb_t;                /* última consulta de "¿algún decodificador necesita buf?" */
    bool hb_val;
    float ring_peak_ms;
    uint8_t *specmsg;
    size_t specmsg_cap;
    uint32_t seq;
    sweep_t *sw;
    double sw_t_status;

    /* Toma de IQ para tx_cal (otro hilo la arma, el DSP la rellena). Mutex propio:
     * así el hilo DSP nunca espera por e->lock ni por el que arma la toma. */
    pthread_mutex_t tap_lock;
    pthread_cond_t tap_cv;
    int16_t *tap_buf;
    unsigned tap_n, tap_pos;
    bool tap_on;
    double tap_t;          /* hora (Unix) en que se armó la toma: solo vale lo leído después */
};

/* ======================================================================= */
/*  Utilidades                                                             */
/* ======================================================================= */
static void broadcast_json(engine_t *e, int client, sb_t *sb)
{
    server_send_text(e->srv, client, sb->data, sb->len);
}

static void send_error(engine_t *e, int client, const char *msg)
{
    sb_t sb = {0};
    sb_puts(&sb, "{\"type\":\"error\",\"msg\":");
    sb_json_str(&sb, msg);
    sb_puts(&sb, "}");
    broadcast_json(e, client, &sb);
    sb_free(&sb);
}

static void refresh_dbm(engine_t *e);

source_t *engine_source(engine_t *e) { return e->src; }

void engine_set_replay_fast(engine_t *e, bool on)
{
    e->fast = on;
    source_set_fast(e->src, on);
}

bool engine_finished(engine_t *e) { return atomic_load(&e->finished); }

uint64_t engine_sample_count(engine_t *e)
{
    pthread_mutex_lock(&e->rg_m);
    uint64_t n = e->rd_next;
    pthread_mutex_unlock(&e->rg_m);
    return n;
}

void engine_set_label_sink(engine_t *e, engine_label_fn fn, void *ctx)
{
    e->lab_ctx = ctx;
    e->lab_fn = fn;
    if (fn && !e->ldet)
        e->ldet = detector_create(e->cfg.fft_size);
}

void engine_set_host(engine_t *e, dec_host_t *host) { e->host = host; }

void engine_set_profile(engine_t *e, const char *id)
{
    pthread_mutex_lock(&e->lock);
    snprintf(e->profile, sizeof e->profile, "%s", id ? id : "");
    pthread_mutex_unlock(&e->lock);
}

void engine_on_config_change(engine_t *e, void (*fn)(void *, const engine_cfg_t *), void *ctx)
{
    e->cfg_cb = fn;
    e->cfg_cb_ctx = ctx;
}

void engine_sanitize(engine_t *e, engine_cfg_t *c)
{
    const source_info_t *in = source_info(e->src);
    if (in->can_tune) {
        c->center_hz = CLAMP(c->center_hz, in->freq_min, in->freq_max);
        /* Por encima de 61,44 Msps solo con el modo oversample (SC8, FPGA >= 0.15) */
        uint32_t rmax = in->rate_max_oversample ? in->rate_max_oversample : MIN(in->rate_max, SOURCE_RATE_NATIVE_MAX);
        c->rate = CLAMP(c->rate, in->rate_min, rmax);
        if (c->rf_bw)
            c->rf_bw = CLAMP(c->rf_bw, in->bw_min, in->bw_max);
        c->gain_db = CLAMP(c->gain_db, in->gain_min, in->gain_max);
    } else {
        c->center_hz = source_frequency(e->src);
        c->rate = source_rate(e->src);
    }
    int maxfft = e->use_gpu ? 65536 : 32768;
    int f = 512;
    while (f < c->fft_size && f < maxfft)
        f <<= 1;
    c->fft_size = f;
    c->fps = CLAMP(c->fps, 2, 60);
    c->det_snr = CLAMP(c->det_snr, 3.f, 40.f);
    if (c->loc_valid && (fabs(c->lat) > 90 || fabs(c->lon) > 180))
        c->loc_valid = false;
}

/* ======================================================================= */
/*  Salidas: espectro y audio                                              */
/* ======================================================================= */
static void on_audio(void *ctx, const int16_t *pcm, int nframes, float level, unsigned flags);

static void on_spectrum(void *ctx, const float *db_in, int n, int navg)
{
    (void)navg;
    engine_t *e = ctx;
    float *db = (float *)db_in; /* el búfer pertenece al módulo y se puede retocar */

    /* Pico de continua (DC): se sustituye por la media de sus vecinos */
    const int c = n / 2;
    float fill = 0.5f * (db[c - 3] + db[c + 3]);
    for (int k = -2; k <= 2; k++)
        db[c + k] = fill;

    detector_feed(e->det, db, n, (float)e->cfg.fps * 1.0f);
    if (e->lab_fn && e->ldet) {
        /* Etiquetado: una imagen del detector cada 0,5 s de señal (fps/2 tramas) */
        detector_feed(e->ldet, db, n, (float)e->cfg.fps * 1.0f);
        const unsigned every = (unsigned)MAX(1, e->cfg.fps / 2);
        if (++e->lab_frames % every == 0) {
            det_signal_t sg[MAX_SIGNALS];
            float noise = 0;
            int ns = detector_run(e->ldet, source_rate(e->src), e->cfg.det_snr, sg, MAX_SIGNALS, &noise);
            e->lab_fn(e->lab_ctx, e->cur_send, (double)source_rate(e->src), source_frequency(e->src), sg, ns);
        }
    }

    /* S-meter de los clientes que NO escuchan: se calcula a partir de esta trama
     * (cero trabajo por muestra; ver channel_feed_spectrum). */
    double tnow = now_monotonic();
    double dt = e->t_spec > 0 ? CLAMP(tnow - e->t_spec, 0.001, 1.0) : 1.0 / MAX(e->cfg.fps, 1);
    e->t_spec = tnow;
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < MAX_SLOTS; i++) {
        slot_t *s = &e->slots[i];
        if (s->used && !s->p.audio_on && !s->dirty)
            channel_feed_spectrum(s->ch, db, n, e->enbw_bins, dt, on_audio, s);
    }
    pthread_mutex_unlock(&e->lock);

    size_t need = 32 + (size_t)n;
    if (need > e->specmsg_cap) {
        e->specmsg = realloc(e->specmsg, need);
        e->specmsg_cap = need;
    }
    uint8_t *m = e->specmsg;
    const int src = e->fpga_active ? SPEC_SRC_FPGA : spectrum_using_gpu(e->spec) ? SPEC_SRC_GPU : SPEC_SRC_CPU;
    need = spectrum_msg_pack(m, db, n, src, (double)source_frequency(e->src), (double)source_rate(e->src), DB_MIN,
                             DB_STEP);
    server_send_binary(e->srv, -1, m, need, true);
}

static void on_audio(void *ctx, const int16_t *pcm, int nframes, float level, unsigned flags)
{
    slot_t *s = ctx;
    int ch = (flags & CHAN_FLAG_STEREO) ? 2 : 1;
    size_t bytes = pcm ? (size_t)nframes * (size_t)ch * 2 : 0;
    uint8_t hdr_and_data[8 + 1024 * 2 * 2];
    if (8 + bytes > sizeof hdr_and_data)
        return;
    hdr_and_data[0] = pcm ? BIN_AUDIO : BIN_METER;
    hdr_and_data[1] = (uint8_t)flags;
    hdr_and_data[2] = (uint8_t)ch;
    hdr_and_data[3] = 0;
    memcpy(hdr_and_data + 4, &level, 4);
    if (bytes)
        memcpy(hdr_and_data + 8, pcm, bytes);
    server_send_binary(s->e->srv, s->client, hdr_and_data, 8 + bytes, true);
}

/* ======================================================================= */
/*  Aplicación de la configuración (solo en el hilo DSP)                   */
/* ======================================================================= */
static void rebuild_spectrum(engine_t *e, const engine_cfg_t *c)
{
    spectrum_destroy(e->spec);
    detector_destroy(e->det);
    e->spec = spectrum_create(c->fft_size, c->rate, c->fps, e->use_gpu);
    e->det = detector_create(c->fft_size);
    detector_destroy(e->ldet);
    e->ldet = e->lab_fn ? detector_create(c->fft_size) : NULL;
    e->lab_frames = 0;
    /* ENBW = N·Σw²/(Σw)² de la ventana del espectro (Blackman-Harris: ≈ 2,0044 bins) */
    {
        float *w = malloc(sizeof(float) * (size_t)c->fft_size);
        e->enbw_bins = 2.0044;
        if (w) {
            window_blackman_harris(w, c->fft_size);
            double sw = 0, sw2 = 0;
            for (int i = 0; i < c->fft_size; i++) {
                sw += w[i];
                sw2 += (double)w[i] * w[i];
            }
            if (sw > 0)
                e->enbw_bins = c->fft_size * sw2 / (sw * sw);
            free(w);
        }
        e->t_spec = 0;
    }
    pthread_mutex_lock(&e->lock);
    snprintf(e->spec_backend, sizeof e->spec_backend, "%s", e->spec ? spectrum_backend(e->spec) : "?");
    e->spec_gpu = e->spec && spectrum_using_gpu(e->spec);
    e->nsigs = 0;
    pthread_mutex_unlock(&e->lock);
}

/* ======================================================================= */
/*  Anillo y hilo lector                                                   */
/* ======================================================================= */
/* Redimensiona el anillo (solo con el lector parado o en pausa). Profundidad:
 * 0,5 s a tasas bajas, 0,3 s hasta 64 Msps y 0,25 s por encima (a 122,88 Msps son
 * ~126 MB: la memoria manda). Las páginas se tocan aquí para que el primer
 * recorrido del lector no pague fallos de página. */
static void ring_alloc(engine_t *e, unsigned block, double rate)
{
    double sec = rate <= 16e6 ? 0.5 : rate <= 64e6 ? 0.3 : 0.25;
    unsigned n = (unsigned)ceil(sec * rate / block);
    n = MAX(n, 8u);
    if (e->ring && n == e->rg_n && block == e->rg_block)
        return;
    free(e->ring);
    free(e->rg_t);
    free(e->rg_s0);
    /* n+1 bloques: el último (índice n) es la ranura de derrame del lector cuando el
     * anillo está lleno y el DSP tiene la única ranura libre en uso (ver reader_main). */
    e->ring = malloc(sizeof(int16_t) * 2 * (size_t)block * (n + 1));
    e->rg_t = calloc(n + 1, sizeof(double));
    e->rg_s0 = calloc(n + 1, sizeof(uint64_t));
    if (!e->ring || !e->rg_t || !e->rg_s0) {
        LOG_E("Sin memoria para el anillo de lectura (%u bloques de %u muestras)", n, block);
        abort();
    }
    memset(e->ring, 0, sizeof(int16_t) * 2 * (size_t)block * (n + 1));
    e->rg_n = n;
    e->rg_block = block;
    e->rg_head = e->rg_tail = 0;
    e->rg_busy = false;
}

/* Hilo lector: solo source_read(). Reserva una ranura libre (si el anillo está
 * lleno descarta la más vieja sin leer), lee fuera del cerrojo y la publica. */
static void *reader_main(void *arg)
{
    engine_t *e = arg;
    int consecutive_errors = 0;
    for (;;) {
        pthread_mutex_lock(&e->rg_m);
        /* --replay-fast (rd_busy sigue en false: una pausa del DSP no se bloquea): con el anillo lleno se espera al DSP (no se pierde ni un bloque) */
        while (e->fast && e->rd_run && e->rd_pause == 0 &&
               (e->rg_busy ? (e->rg_tail - (e->rg_head - 1) >= e->rg_n) : (e->rg_tail - e->rg_head >= e->rg_n)))
            pthread_cond_wait(&e->rg_ctl, &e->rg_m);
        if (!e->rd_run) {
            pthread_mutex_unlock(&e->rg_m);
            break;
        }
        while (e->rd_pause > 0 && e->rd_run)
            pthread_cond_wait(&e->rg_ctl, &e->rg_m);
        if (!e->rd_run) {
            pthread_mutex_unlock(&e->rg_m);
            break;
        }
        e->rd_busy = true;
        /* Anillo lleno y DSP atrasado: se pierde un bloque. Si el DSP tiene una ranura en
         * uso (head−1), con el anillo lleno tail % n cae justo en ella: descartar el más
         * viejo no la libera y escribir ahí sería una carrera de datos con el DSP (la
         * detectó TSan). En ese caso el bloque nuevo se lee en la ranura de derrame (n)
         * y se descarta; si el DSP no tiene ranura, se descarta el más viejo sin leer. */
        bool spill = false;
        if (e->rg_busy ? (e->rg_tail - (e->rg_head - 1) >= e->rg_n) : (e->rg_tail - e->rg_head >= e->rg_n)) {
            if (e->rg_busy)
                spill = true;
            else
                e->rg_head++;
            atomic_fetch_add(&e->dsp_drops, 1);
        }
        unsigned slot = spill ? e->rg_n : (unsigned)(e->rg_tail % e->rg_n), block = e->rg_block;
        const uint64_t s0 = e->rd_next; /* índice de la primera muestra de este bloque */
        pthread_mutex_unlock(&e->rg_m);

        unsigned ovr = 0;
        int rc = source_read(e->src, e->ring + 2 * (size_t)slot * block, block, &ovr);
        double t = now_wall();
        if (ovr)
            atomic_fetch_add(&e->hw_overruns, ovr);

        pthread_mutex_lock(&e->rg_m);
        if (rc == 0) {
            consecutive_errors = 0;
            atomic_fetch_add(&e->rd_samples, block);
            e->rd_next += block; /* también si el bloque se derrama: esas muestras se perdieron */
            if (!spill) {
                e->rg_t[slot] = t;
                e->rg_s0[slot] = s0;
                e->rg_tail++;
                pthread_cond_signal(&e->rg_data);
            }
        } else if (rc == SOURCE_EOF) {
            /* Fin del fichero (solo con --replay-fast): ya no hay más que leer */
            e->rd_eof = true;
            pthread_cond_broadcast(&e->rg_data);
            pthread_cond_broadcast(&e->rg_ctl);
            pthread_mutex_unlock(&e->rg_m);
            break;
        } else {
            if (++consecutive_errors == 1 || consecutive_errors % 20 == 0) {
                LOG_E("Error de lectura: %s", source_error(e->src));
                snprintf(e->rd_err, sizeof e->rd_err, "%s", source_error(e->src));
                e->rd_err_seq++;
            }
        }
        pthread_mutex_unlock(&e->rg_m);
        if (rc != 0) {
            usleep(250000);
            /* Reintentar el flujo: cubre desconexiones breves del USB */
            source_stop(e->src);
            source_start(e->src);
        }
        pthread_mutex_lock(&e->rg_m);
        e->rd_busy = false;
        pthread_cond_broadcast(&e->rg_ctl);
        pthread_mutex_unlock(&e->rg_m);
    }
    return NULL;
}

/* Espera a que el lector salga de source_read() y lo mantiene parado. Mientras
 * dure la pausa el DSP puede tocar la fuente y el anillo. Anidable. */
static void reader_pause(engine_t *e)
{
    if (!e->rd_started)
        return;
    pthread_mutex_lock(&e->rg_m);
    e->rd_pause++;
    while (e->rd_busy)
        pthread_cond_wait(&e->rg_ctl, &e->rg_m);
    pthread_mutex_unlock(&e->rg_m);
}

/* Descarta lo que haya en el anillo (muestras de la configuración anterior) y
 * reanuda. Hay que haber hecho reader_pause() antes. */
static void reader_resume(engine_t *e, bool flush)
{
    if (!e->rd_started)
        return;
    pthread_mutex_lock(&e->rg_m);
    if (flush) {
        e->rg_head = e->rg_tail;
        e->rg_busy = false;
    }
    e->rd_pause--;
    pthread_cond_broadcast(&e->rg_ctl);
    pthread_mutex_unlock(&e->rg_m);
}

/* Toma el bloque más viejo del anillo (espera hasta timeout_s). Devuelve su
 * ranura en *slot y el nº de bloques que quedan detrás; <0 si no hay nada. */
static int ring_pop(engine_t *e, unsigned *slot, double timeout_s, unsigned *queued)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    double t = dl.tv_nsec * 1e-9 + timeout_s;
    dl.tv_sec += (time_t)t;
    dl.tv_nsec = (long)((t - (double)(time_t)t) * 1e9);
    pthread_mutex_lock(&e->rg_m);
    e->rg_busy = false;
    int rc = 0;
    while (e->rg_head == e->rg_tail && e->rd_run && !e->rd_eof && rc == 0)
        rc = pthread_cond_timedwait(&e->rg_data, &e->rg_m, &dl);
    if (e->rg_head == e->rg_tail) {
        pthread_mutex_unlock(&e->rg_m);
        return -1;
    }
    *slot = (unsigned)(e->rg_head % e->rg_n);
    e->rg_head++;
    e->rg_busy = true; /* la ranura sigue siendo nuestra hasta el próximo ring_pop */
    *queued = (unsigned)(e->rg_tail - e->rg_head);
    pthread_cond_broadcast(&e->rg_ctl);
    pthread_mutex_unlock(&e->rg_m);
    return 0;
}

static void alloc_block(engine_t *e, uint32_t rate)
{
    /* ~20 ms por bloque, múltiplo de 1024, entre 16 Ki y 1 Mi muestras (a 122,88 Msps
     * oversample el tope de 1 Mi son 8,5 ms). Sin oversample disponible no se
     * dimensiona por encima de 61,44 Msps. */
    const source_info_t *in = source_info(e->src);
    if (in->can_tune && rate > SOURCE_RATE_NATIVE_MAX && !in->rate_max_oversample)
        rate = SOURCE_RATE_NATIVE_MAX;
    unsigned b = (unsigned)(rate / 50);
    b = (b + 1023) & ~1023u;
    b = CLAMP(b, 16384u, 1048576u);
    /* El anillo se redimensiona con el lector parado (apply_config lo pausa). */
    ring_alloc(e, b, (double)rate);
    if (b == e->block)
        return;
    e->block = b;
    e->buf = realloc(e->buf, sizeof(cf32) * b);
}

static void send_recording(engine_t *e, int client);
static void send_status(engine_t *e);

/* ======================================================================= */
/*  Espectro generado en la FPGA (imagen "lab")                            */
/* ======================================================================= */
/* Con rx_mux = 3 el flujo SC16 que sale de la placa no son muestras IQ sino tramas de
 * espectro (fpga_lab.h): la FPGA hace la FFT y promedia K tramas, así que el USB solo
 * lleva ~0,2 Mpalabras/s aunque el ADC corra a 61,44 Msps. El lector y el anillo no
 * cambian (siguen entregando "muestras" que ahora son palabras de 32 bits); el hilo
 * DSP las pasa al intérprete y de cada trama sale el mismo mensaje binario tipo 1 que
 * produce la FFT de CPU/GPU, con el byte [1] = 2. Sin IQ no hay audio, grabación ni
 * decodificadores: el motor lo dice con un aviso y el host los pone en "waiting". */
#define FPGA_BLOCK_WORDS 4096u      /* palabras por bloque del anillo en modo FPGA (~20 ms) */
#define FPGA_WARMUP_READS 4         /* búferes leídos con rx_mux = 0 antes de activar el espectro (late-mux) */
#define FPGA_FRAME_TIMEOUT_S 3.0    /* sin tramas durante este tiempo → vuelta a CPU/GPU */
#define FPGA_SUSPEND_MSG "Espectro FPGA activo: sin muestras IQ (el audio y los decodificadores esperan)"

static void fpga_frame_cb(void *ctx, const fpga_lab_frame_t *f)
{
    engine_t *e = ctx;
    e->fpga_t_last = now_monotonic();
    if (f->n != e->fpga_n || server_client_count(e->srv) == 0)
        return; /* una trama de otro N (llegada tras un cambio de configuración) no se dibuja */
    fpga_lab_frame_to_dbfs(f, e->fpga_db);
    on_spectrum(e, e->fpga_db, f->n, f->k);
}

static void fpga_publish(engine_t *e, bool active)
{
    pthread_mutex_lock(&e->lock);
    e->fpga_active = active;
    pthread_mutex_unlock(&e->lock);
    if (e->host)
        dec_host_set_suspended(e->host, active ? FPGA_SUSPEND_MSG : NULL);
}

/* Pasa a espectro FPGA con la configuración c. Hilo DSP. < 0 con err si no se puede
 * (en ese caso la fuente queda como estaba: modo IQ normal). */
static int fpga_enter(engine_t *e, const engine_cfg_t *c, char *err, size_t el)
{
    const source_info_t *in = source_info(e->src);
    if (strcmp(in->kind, "bladerf") != 0) {
        snprintf(err, el, "el espectro FPGA exige un bladeRF con la imagen lab (la fuente es un fichero)");
        return -1;
    }
    if (c->rate > SOURCE_RATE_NATIVE_MAX || source_oversample(e->src)) {
        snprintf(err, el, "el espectro FPGA no se combina con el modo oversample (máx. %.2f Msps)",
                 SOURCE_RATE_NATIVE_MAX / 1e6);
        return -1;
    }
    int log2n, k;
    if (fpga_lab_choose_params(c->fft_size, (double)c->rate, c->fps, &log2n, &k) < 0) {
        snprintf(err, el, "parámetros de espectro FPGA no válidos");
        return -1;
    }
    if (!e->fpga_db) {
        e->fpga_db = malloc(sizeof(float) * FPGA_SPEC_NMAX);
        if (!e->fpga_db) {
            snprintf(err, el, "sin memoria");
            return -1;
        }
    }
    reader_pause(e);
    int rc = -1;
    bool started = false;
    /* 1) RX parado: es seguro tocar la imagen. Detección (también si la lab se cargó con el servidor ya en marcha). */
    source_stop(e->src);
    /* Reconfiguración con el modo ya activo (cambio de fps/N/tasa): el FPGA sigue con rx_mux = 3. Se deja
     * en 0 AHORA, con RX parado, para que el arranque del flujo sea el late-mux de siempre; hacerlo
     * después de arrancar (como hacía fpga_leave) deja el flujo IQ a 30 Msps sobre USB 2, que no lo
     * sostiene, y la lectura se queda esperando hasta el «sync_rx: Operation timed out». */
    if (e->lab.present)
        fpga_lab_set_spectrum_mode(&e->lab, false);
    char perr[160] = "";
    fpga_lab_io_t io;
    if (source_fpga_lab_io(e->src, &io) < 0) {
        snprintf(err, el, "no se pudo acceder a la FPGA");
    } else if (fpga_lab_probe(&io, &e->lab, perr, sizeof perr) < 0) {
        snprintf(err, el, "FPGA: %s", perr);
    } else if (!e->lab.present) {
        snprintf(err, el, "la FPGA cargada no es la imagen lab (FPGA %s): carga hosted-xA9-lab con bladeRF-cli -l",
                 in->fpga_version);
    } else if (fpga_lab_config_spectrum(&e->lab, log2n, k, true) < 0) {
        snprintf(err, el, "no se pudo configurar el motor de espectro de la FPGA");
    } else {
        /* 2) late-mux (fpga_lab.h): RX con rx_mux = 0, unas lecturas para que el flujo USB arranque y
         *    solo entonces rx_mux = 3, para no perder la primera trama. */
        if (source_start(e->src) < 0) {
            snprintf(err, el, "no se pudo arrancar el flujo: %s", source_error(e->src));
        } else {
            started = true;
            const unsigned nb = MIN(e->rg_block, 16384u);
            unsigned ovr = 0;
            int r = 0;
            /* Un intento más si el flujo no arranca: parar, rx_mux = 0 (con RX parado) y volver a empezar */
            for (int intento = 0; intento < 2; intento++) {
                r = 0;
                for (int i = 0; i < FPGA_WARMUP_READS && r == 0; i++)
                    r = source_read(e->src, e->ring, nb, &ovr);
                if (r == 0)
                    break;
                LOG_W("Espectro FPGA: la lectura de arranque falló (%s)%s", source_error(e->src),
                      intento == 0 ? ", reintento" : "");
                if (intento == 0) {
                    source_stop(e->src);
                    fpga_lab_set_spectrum_mode(&e->lab, false);
                    if (source_start(e->src) < 0)
                        break;
                }
            }
            if (r < 0)
                snprintf(err, el, "lectura de arranque: %s", source_error(e->src));
            else if (fpga_lab_set_spectrum_mode(&e->lab, true) < 0)
                snprintf(err, el, "no se pudo activar rx_mux = 3");
            else
                rc = 0;
        }
    }
    if (rc < 0) {
        if (e->lab.present)
            fpga_lab_set_spectrum_mode(&e->lab, false);
        if (!started)
            source_start(e->src); /* recupera el flujo IQ que se paró para configurar */
        pthread_mutex_lock(&e->lock);
        e->lab_present_pub = e->lab.present;
        e->lab_version_pub = e->lab.version;
        pthread_mutex_unlock(&e->lock);
        reader_resume(e, true);
        return -1;
    }
    fpga_lab_parser_destroy(e->fpar);
    e->fpar = fpga_lab_parser_create(fpga_frame_cb, e);
    e->fpga_n = 1 << log2n;
    e->fpga_k = k;
    e->fpga_t_last = now_monotonic();
    e->fpga_frames = e->fpga_gaps = 0;
    ring_alloc(e, FPGA_BLOCK_WORDS, 1e6);
    /* Ventana Blackman-Harris de N puntos (FFT de la FPGA) y detector del tamaño de la trama */
    detector_destroy(e->det);
    e->det = detector_create(e->fpga_n);
    e->enbw_bins = 2.0044;
    e->t_spec = 0;
    recorder_stop(e->rec);
    send_recording(e, -1);
    pthread_mutex_lock(&e->lock);
    e->nsigs = 0;
    e->lab_present_pub = true;
    e->lab_version_pub = e->lab.version;
    snprintf(e->spec_backend, sizeof e->spec_backend, "FPGA lab (N=%d, K=%d, %.1f imágenes/s)", e->fpga_n, k,
             (double)c->rate / ((double)e->fpga_n * k));
    pthread_mutex_unlock(&e->lock);
    fpga_publish(e, true);
    reader_resume(e, true);
    LOG_I("Espectro FPGA: N=%d K=%d a %.2f Msps (%.1f imágenes/s)", e->fpga_n, k, c->rate / 1e6,
          (double)c->rate / ((double)e->fpga_n * k));
    return 0;
}

/* Vuelve al modo IQ con la configuración c. Hilo DSP. */
static void fpga_leave(engine_t *e, const engine_cfg_t *c)
{
    reader_pause(e);
    /* Con RX parado: rx_mux = 0 y solo entonces se reinicia el flujo (las palabras de espectro que
     * quedan en los búferes de la FPGA/FX3 no son IQ) */
    source_stop(e->src);
    if (e->lab.present)
        fpga_lab_set_spectrum_mode(&e->lab, false);
    source_start(e->src);
    fpga_lab_parser_destroy(e->fpar);
    e->fpar = NULL;
    fpga_publish(e, false);
    alloc_block(e, c->rate);
    rebuild_spectrum(e, c);
    reader_resume(e, true);
}

/* Estado de la FPGA para el mensaje "status" (bajo e->lock). */
static void fpga_status_json(engine_t *e, sb_t *sb)
{
    sb_printf(sb, ",\"spectrum_src\":\"%s\"", e->fpga_active ? "fpga" : e->spec_gpu ? "gpu" : "cpu");
    sb_printf(sb, ",\"fpga_lab\":{\"present\":%s,\"active\":%s", e->lab_present_pub ? "true" : "false",
              e->fpga_active ? "true" : "false");
    if (e->lab_present_pub)
        sb_printf(sb, ",\"version\":\"%u.%u\"", (e->lab_version_pub >> 16) & 0xFFFFu, e->lab_version_pub & 0xFFFFu);
    if (e->fpga_active)
        sb_printf(sb, ",\"n\":%d,\"k\":%d,\"frames\":%llu,\"gaps\":%llu", e->fpga_n, e->fpga_k,
                  (unsigned long long)e->fpga_frames, (unsigned long long)e->fpga_gaps);
    sb_puts(sb, "}");
}

static void apply_config(engine_t *e, bool first)
{
    pthread_mutex_lock(&e->lock);
    engine_cfg_t w = e->want;
    unsigned d = first ? CFG_ALL : e->dirty;
    e->dirty = 0;
    pthread_mutex_unlock(&e->lock);
    if (!d)
        return;

    /* Todo lo que toca la fuente o cambia la tasa: lector en pausa (así nadie
     * llama a libbladeRF a la vez que sync_rx) y, al acabar, se tira lo que
     * hubiera en el anillo: son muestras de la sintonía/tasa anteriores. */
    const unsigned touch = CFG_RATE | CFG_CENTER | CFG_RFBW | CFG_GAIN | CFG_BIASTEE;
    const bool paused = !first && (d & touch) && e->rd_started;
    if (paused)
        reader_pause(e);

    engine_cfg_t a = e->cfg; /* lo que queda aplicado */
    const source_info_t *in = source_info(e->src);
    char err[300] = "";

    if (d & CFG_RATE) {
        uint32_t act = w.rate;
        if (source_set_rate(e->src, w.rate, &act) < 0)
            snprintf(err, sizeof err, "No se pudo fijar la tasa: %s", source_error(e->src));
        else
            a.rate = act;
        alloc_block(e, a.rate);
        d |= CFG_FFT | CFG_RFBW;
    }
    if (d & CFG_CENTER) {
        if (in->can_tune && source_set_frequency(e->src, w.center_hz) < 0)
            snprintf(err, sizeof err, "No se pudo sintonizar: %s", source_error(e->src));
        else
            a.center_hz = in->can_tune ? w.center_hz : source_frequency(e->src);
        if (e->det)
            detector_reset(e->det);
    }
    if (d & CFG_RFBW) {
        a.rf_bw = w.rf_bw;
        if (in->can_tune) {
            uint32_t bw = w.rf_bw ? w.rf_bw : (uint32_t)(a.rate * 0.85);
            bw = CLAMP(bw, in->bw_min, in->bw_max);
            source_set_bandwidth(e->src, bw, NULL);
        }
    }
    if (d & CFG_GAIN) {
        if (source_set_gain(e->src, w.gain_mode, w.gain_db) < 0)
            snprintf(err, sizeof err, "No se pudo fijar la ganancia: %s", source_error(e->src));
        a.gain_mode = w.gain_mode;
        a.gain_db = w.gain_db;
    }
    if (d & CFG_BIASTEE) {
        if (w.biastee != a.biastee || first) {
            if (in->has_biastee && source_set_biastee(e->src, w.biastee) == 0)
                a.biastee = w.biastee;
            else if (w.biastee)
                snprintf(err, sizeof err, "Este equipo no admite bias-tee");
        }
    }
    if (d & CFG_FFT) {
        a.fft_size = w.fft_size;
        a.fps = w.fps;
        rebuild_spectrum(e, &a);
    }
    if (d & CFG_LOC) {
        a.loc_valid = w.loc_valid;
        a.lat = w.lat;
        a.lon = w.lon;
    }
    if (d & CFG_DET)
        a.det_snr = w.det_snr;

    /* Espectro FPGA activo: N y K dependen de tasa, FFT y fps; se reconfigura el motor. Si ya no
     * se puede (p. ej. la nueva tasa pide oversample) se vuelve a CPU/GPU con un aviso. */
    if (e->fpga_active && (d & (CFG_RATE | CFG_FFT))) {
        char ferr[300] = "";
        /* Directo a fpga_enter (que ya para, pone rx_mux = 0 y rearranca): pasar antes por fpga_leave
         * rearrancaba un flujo IQ completo que luego se paraba de inmediato (causa del timeout). */
        if (fpga_enter(e, &a, ferr, sizeof ferr) < 0) {
            fpga_leave(e, &a);
            snprintf(err, sizeof err, "Espectro FPGA desactivado: %s. Vuelvo a CPU/GPU.", ferr);
            send_status(e);
        }
    }
    if (paused)
        reader_resume(e, (d & (CFG_RATE | CFG_CENTER)) != 0);
    pthread_mutex_lock(&e->lock);
    e->cfg = a;
    /* Los canales deben conocer la nueva tasa/centro */
    if (d & (CFG_RATE | CFG_CENTER))
        for (int i = 0; i < MAX_SLOTS; i++)
            if (e->slots[i].used)
                e->slots[i].dirty = true;
    if (err[0])
        snprintf(e->last_error, sizeof e->last_error, "%s", err);
    pthread_mutex_unlock(&e->lock);

    if (err[0]) {
        LOG_W("%s", err);
        send_error(e, -1, err);
    }
    refresh_dbm(e);
    sb_t sb = {0};
    engine_status_json(e, &sb);
    broadcast_json(e, -1, &sb);
    sb_free(&sb);
    if (e->cfg_cb && !first)
        e->cfg_cb(e->cfg_cb_ctx, &a);
}

/* Desplazamiento dBFS → dBm de la calibración de ganancia (si existe). */
static void refresh_dbm(engine_t *e)
{
    double off = 0;
    bool ok = source_dbm_offset(e->src, &off) == 0;
    pthread_mutex_lock(&e->lock);
    e->dbm_valid = ok;
    e->dbm_off = off;
    pthread_mutex_unlock(&e->lock);
}

/* ======================================================================= */
/*  Barrido rápido (src/sdr/sweep.h). Todo en el hilo DSP.                 */
/* ======================================================================= */
static void send_status(engine_t *e)
{
    sb_t sb = {0};
    engine_status_json(e, &sb);
    broadcast_json(e, -1, &sb);
    sb_free(&sb);
}

/* Cierra el barrido y devuelve el motor a su configuración normal: se marca todo
 * como pendiente para que apply_config() reponga tasa, frecuencia, ganancia… */
static void sweep_end(engine_t *e)
{
    sweep_destroy(e->sw); /* cancela los retunes pendientes y para el flujo */
    e->sw = NULL;
    /* El barrido lee la fuente él mismo: el lector sigue en pausa hasta aquí */
    pthread_mutex_lock(&e->lock);
    e->sweep_active = false;
    e->dirty |= CFG_ALL;
    pthread_mutex_unlock(&e->lock);
    if (source_start(e->src) < 0)
        send_error(e, -1, source_error(e->src));
    if (e->sweep_paused) {
        e->sweep_paused = false;
        reader_resume(e, true);
    }
    send_status(e);
}

static void sweep_begin(engine_t *e, const sweep_cfg_t *req)
{
    if (e->sw)
        sweep_end(e);
    reader_pause(e); /* el barrido hace sus propias lecturas */
    e->sweep_paused = e->rd_started;
    /* La ganancia debe ser fija durante todo el barrido (con AGC cada salto tendría
     * otro nivel y la trama no sería comparable): manual, con la ganancia actual. */
    pthread_mutex_lock(&e->lock);
    int g = e->cfg.gain_mode == GAIN_MANUAL ? e->cfg.gain_db : e->gain_now;
    pthread_mutex_unlock(&e->lock);
    source_set_gain(e->src, GAIN_MANUAL, g);

    sweep_io_t io = sweep_io_source(e->src);
    char err[300] = "";
    e->sw = sweep_create(req, &io, err, sizeof err);
    if (!e->sw) {
        LOG_W("%s", err);
        send_error(e, -1, err);
        pthread_mutex_lock(&e->lock);
        e->dirty |= CFG_ALL;
        pthread_mutex_unlock(&e->lock);
        source_start(e->src); /* sweep_create dejó el flujo parado */
        if (e->sweep_paused) {
            e->sweep_paused = false;
            reader_resume(e, true);
        }
        return;
    }
    pthread_mutex_lock(&e->lock);
    e->sweep_active = true;
    sweep_get_info(e->sw, &e->sweep_info);
    pthread_mutex_unlock(&e->lock);
    sweep_info_t *i = &e->sweep_info;
    LOG_I("Barrido %.3f–%.3f MHz: %d saltos a %.2f Msps, FFT %d, modo %s", i->start_hz / 1e6, i->stop_hz / 1e6,
          i->hops, i->rate / 1e6, i->fft, i->quick ? "quick-tune" : "lento");
    e->sw_t_status = now_monotonic();
    send_status(e);
}

static void sweep_iteration(engine_t *e)
{
    sweep_frame_t fr;
    char err[300];
    int r = sweep_step(e->sw, &fr, err, sizeof err);
    if (r < 0) {
        LOG_W("%s", err);
        send_error(e, -1, err);
        sweep_end(e);
        usleep(100000);
        return;
    }
    if (r > 0 && server_client_count(e->srv) > 0) {
        size_t need = sweep_msg_size(fr.n);
        if (need > e->specmsg_cap) {
            e->specmsg = realloc(e->specmsg, need);
            e->specmsg_cap = need;
        }
        server_send_binary(e->srv, -1, e->specmsg, sweep_pack(&fr, e->specmsg), true);
    }
    double now = now_monotonic();
    if (now - e->sw_t_status >= 2.0) {
        e->sw_t_status = now;
        float t = 0;
        bool have_t = source_get_temperature(e->src, &t) == 0;
        pthread_mutex_lock(&e->lock);
        if (have_t)
            e->temp_c = t;
        sweep_get_info(e->sw, &e->sweep_info);
        pthread_mutex_unlock(&e->lock);
        send_status(e);
    }
}

int engine_sweep_start(engine_t *e, int64_t start_hz, int64_t stop_hz, int fft, char *err, size_t errlen)
{
    const source_info_t *in = source_info(e->src);
    pthread_mutex_lock(&e->lock);
    const bool fpga_on = e->fpga_active;
    pthread_mutex_unlock(&e->lock);
    if (fpga_on) {
        snprintf(err, errlen, "el barrido no es compatible con el espectro FPGA: vuelva antes a spectrum_src=cpu");
        return -1;
    }
    if (!in->can_tune) {
        snprintf(err, errlen, "el barrido requiere el bladeRF (no funciona con ficheros)");
        return -1;
    }
    if (stop_hz <= start_hz || stop_hz <= in->freq_min || start_hz >= in->freq_max) {
        snprintf(err, errlen, "barrido: rango no válido (el equipo sintoniza %lld–%lld Hz)", (long long)in->freq_min,
                 (long long)in->freq_max);
        return -1;
    }
    int f = 256;
    while (f < fft && f < 8192)
        f <<= 1;
    sweep_cfg_t c = {.start_hz = start_hz, .stop_hz = stop_hz, .fmin_hz = in->freq_min, .fmax_hz = in->freq_max,
                     .fft = f, .allow_quick = true};
    pthread_mutex_lock(&e->lock);
    e->sweep_req = c;
    e->sweep_cmd = 1;
    pthread_mutex_unlock(&e->lock);
    return 0;
}

int engine_set_spectrum_src(engine_t *e, const char *src, char *err, size_t errlen)
{
    const bool fpga = src && strcmp(src, "fpga") == 0;
    if (!src || (!fpga && strcmp(src, "cpu") != 0 && strcmp(src, "gpu") != 0)) {
        snprintf(err, errlen, "spectrum_src: use cpu, gpu o fpga");
        return -1;
    }
    if (fpga && strcmp(source_info(e->src)->kind, "bladerf") != 0) {
        snprintf(err, errlen, "spectrum_src=fpga exige un bladeRF con la imagen lab (la fuente actual es un fichero)");
        return -1;
    }
    pthread_mutex_lock(&e->lock);
    e->ssrc_cmd = fpga ? 1 : 2; /* "cpu" y "gpu" equivalen: el motor FFT lo elige --no-gpu */
    pthread_mutex_unlock(&e->lock);
    return 0;
}

void engine_sweep_stop(engine_t *e)
{
    pthread_mutex_lock(&e->lock);
    e->sweep_cmd = 2;
    pthread_mutex_unlock(&e->lock);
}

/* ======================================================================= */
/*  Tareas periódicas                                                      */
/* ======================================================================= */
static void send_signals(engine_t *e, int client)
{
    pthread_mutex_lock(&e->lock);
    sb_t sb = {0};
    sb_printf(&sb, "{\"type\":\"signals\",\"center\":%lld,\"noise\":%.1f,\"list\":[", (long long)e->cfg.center_hz,
              e->noise_db);
    for (int i = 0; i < e->nsigs; i++)
        sb_printf(&sb, "%s[%.0f,%.0f,%.1f,%.1f]", i ? "," : "", e->sigs[i].offset_hz, e->sigs[i].bw_hz,
                  e->sigs[i].peak_db, e->sigs[i].snr_db);
    sb_puts(&sb, "]}");
    pthread_mutex_unlock(&e->lock);
    broadcast_json(e, client, &sb);
    sb_free(&sb);
}

static void send_recording(engine_t *e, int client)
{
    recorder_status_t st;
    recorder_status(e->rec, &st);
    sb_t sb = {0};
    sb_printf(&sb, "{\"type\":\"recording\",\"on\":%s,\"name\":", st.active ? "true" : "false");
    sb_json_str(&sb, st.name);
    sb_printf(&sb, ",\"bytes\":%llu,\"seconds\":%.1f,\"dropped\":%llu}", st.bytes, st.seconds, st.dropped);
    broadcast_json(e, client, &sb);
    sb_free(&sb);
}

static void periodic(engine_t *e, double now, double *t_sig, double *t_status, uint64_t *samples, double *t_rate)
{
    if (now - *t_sig >= 0.5) {
        *t_sig = now;
        det_signal_t tmp[MAX_SIGNALS];
        float noise = 0;
        int n = e->det ? detector_run(e->det, source_rate(e->src), e->cfg.det_snr, tmp, MAX_SIGNALS, &noise) : 0;
        if (e->host) { /* perfiles auto: el host crea decodificadores sobre estas señales */
            dec_sig_t ds[MAX_SIGNALS];
            int64_t c = source_frequency(e->src);
            for (int i = 0; i < n; i++)
                ds[i] = (dec_sig_t){c + (int64_t)llround(tmp[i].offset_hz), tmp[i].bw_hz, tmp[i].snr_db};
            dec_host_auto_signals(e->host, ds, n, now_wall());
        }
        pthread_mutex_lock(&e->lock);
        memcpy(e->sigs, tmp, sizeof(det_signal_t) * (size_t)n);
        e->nsigs = n;
        e->noise_db = noise;
        pthread_mutex_unlock(&e->lock);
        send_signals(e, -1);
    }
    if (now - *t_status >= 2.0) {
        float t = 0;
        int g = 0;
        bool have_t = source_get_temperature(e->src, &t) == 0;
        bool have_g = source_get_gain(e->src, &g) == 0;
        refresh_dbm(e);
        pthread_mutex_lock(&e->lock);
        /* En modo FPGA lo que entrega el lector son palabras de espectro, no muestras: se publica la tasa nominal */
        e->meas_rate = e->fpga_active ? (double)source_rate(e->src) : *samples / (now - *t_rate);
        if (have_t)
            e->temp_c = t;
        if (have_g)
            e->gain_now = g;
        pthread_mutex_unlock(&e->lock);
        *samples = 0;
        *t_rate = now;
        *t_status = now;
        sb_t sb = {0};
        engine_status_json(e, &sb);
        broadcast_json(e, -1, &sb);
        sb_free(&sb);
        send_recording(e, -1);
    }
}

/* ======================================================================= */
/*  Hilo DSP                                                               */
/* ======================================================================= */
/* Continua (offset de los convertidores) en cuentas Q11, para restarla antes de
 * demodular o decodificar.
 *
 * Se estima como la MEDIA DEL BLOQUE suavizada entre bloques (paso bajo de un polo
 * con τ = 0,25 s; el primer bloque tras no haberla usado la toma directa). Antes
 * era un IIR de un polo por muestra: dependencia serie, no vectorizable, 3,4 ms por
 * bloque de 1 Mi muestras (0,40 núcleos a 122,88 Msps). Ahora la suma es un bucle
 * sin dependencias que GCC vectoriza con -O2 -march=native (vpaddd sobre ymm,
 * comprobado en el ensamblador): int32 por trozos de 4096 muestras (4096·2048 < 2^31)
 * y double entre trozos. Basta con las primeras 256 Ki muestras del bloque: el error
 * típico de la media es σ/√N (< 0,5 cuentas con σ = 100) y así no se recorre toda la
 * RAM en bloques de 1 Mi. */
/* Vectores de GCC: con -O2 el autovectorizador no entra en estos bucles (suma con
 * ensanchado int16→int32 y conversión de pares entrelazados: "relevant stmt not
 * supported" / "no vectype", comprobado con -fopt-info-vec-all). */
typedef int16_t v16h __attribute__((vector_size(32), aligned(2), may_alias));
typedef int32_t v16i __attribute__((vector_size(64)));
typedef float v16f __attribute__((vector_size(64), aligned(4), may_alias));

static void estimate_dc(engine_t *e, const int16_t *iq, unsigned n)
{
    const unsigned ns = MIN(n, 1u << 18) & ~15u; /* múltiplo de 16 muestras */
    double sr = 0, si = 0;
    /* 32 valores int16 por vuelta; las 16 lanas int32 alternan I,Q,I,Q… Cada lana suma
     * a lo sumo 1024·2048 < 2^31 antes de volcarse a double. */
    for (unsigned i0 = 0; i0 < ns; i0 += 8192) {
        const unsigned m = MIN(8192u, ns - i0);
        const int16_t *p = iq + 2 * (size_t)i0;
        v16i acc = {0};
        for (unsigned i = 0; i < 2 * m; i += 16)
            acc += __builtin_convertvector(*(const v16h *)(const void *)(p + i), v16i);
        for (int l = 0; l < 16; l += 2) {
            sr += acc[l];
            si += acc[l + 1];
        }
    }
    const double fs = (double)source_rate(e->src);
    const double a = e->dc_valid ? 1.0 - exp(-((double)n / fs) / 0.25) : 1.0;
    if (ns) {
        e->dc_re += (float)(a * (sr / ns - e->dc_re));
        e->dc_im += (float)(a * (si / ns - e->dc_im));
    }
    e->dc_valid = true;
}

/* Q11 → float normalizado (±1,0 = fondo de escala) sin la continua, para los
 * decodificadores NARROW que no usan el DDC de GPU: out = in·(1/2048) − dc con dc
 * constante en todo el bloque (16 valores por vuelta, ~0,3 ns/muestra). */
static void convert_block(engine_t *e, const int16_t *iq, unsigned n)
{
    const float k = 1.0f / 2048.0f, dr = e->dc_re * k, di = e->dc_im * k;
    float *o = (float *)e->buf;
    const v16f kv = {k, k, k, k, k, k, k, k, k, k, k, k, k, k, k, k};
    const v16f dv = {dr, di, dr, di, dr, di, dr, di, dr, di, dr, di, dr, di, dr, di};
    unsigned i = 0;
    for (; i + 8 <= n; i += 8) /* 8 muestras = 16 valores */
        *(v16f *)(void *)(o + 2 * (size_t)i) =
            __builtin_convertvector(*(const v16h *)(const void *)(iq + 2 * (size_t)i), v16f) * kv - dv;
    for (; i < n; i++) {
        o[2 * i] = (float)iq[2 * i] * k - dr;
        o[2 * i + 1] = (float)iq[2 * i + 1] * k - di;
    }
}

/* ¿Algún decodificador NARROW sin DDC en GPU necesita las muestras en float?
 * (los WIDE y los de GPU usan el Q11 crudo). host.h no lo expone, así que se
 * deduce de dec_list (protocolos → tipo) y del nº de canales de GPU; se consulta
 * a 4 Hz. Es conservador: cuenta también las instancias en espera/error. */
static bool host_wants_float(engine_t *e, double now)
{
    if (!e->host)
        return false;
    if (now - e->hb_t < 0.25)
        return e->hb_val;
    e->hb_t = now;
    sb_t sb = {0};
    dec_host_list_json(e->host, &sb);
    int narrow = 0;
    const char *p = sb.data;
    while (p && (p = strstr(p, "\"proto\":\"")) != NULL) {
        p += 9;
        char id[32];
        size_t k = 0;
        while (*p && *p != '"' && k < sizeof id - 1)
            id[k++] = *p++;
        id[k] = 0;
        const decoder_desc_t *d = decoder_find(id);
        if (d && d->kind == DEC_NARROW)
            narrow++;
    }
    sb_free(&sb);
    int gch = 0;
    dec_host_gpu_info(e->host, NULL, 0, &gch, NULL, NULL, NULL, NULL);
    e->hb_val = narrow > gch;
    return e->hb_val;
}

/* Configura los canales con cambios pendientes y dice si alguno necesita IQ. */
static bool sync_slots(engine_t *e)
{
    bool any = false;
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < MAX_SLOTS; i++) {
        slot_t *s = &e->slots[i];
        if (!s->used)
            continue;
        if (s->dirty) {
            channel_configure(s->ch, source_rate(e->src), source_frequency(e->src), &s->p);
            s->dirty = false;
        }
        any |= channel_wants_samples(s->ch);
    }
    pthread_mutex_unlock(&e->lock);
    return any;
}

static void *dsp_main(void *arg)
{
    engine_t *e = arg;
    apply_config(e, true);
    alloc_block(e, e->cfg.rate);
    if (source_start(e->src) < 0) {
        LOG_E("No se pudo arrancar la recepción: %s", source_error(e->src));
        send_error(e, -1, source_error(e->src));
    }
    e->rd_run = true;
    if (pthread_create(&e->rd_thread, NULL, reader_main, e) == 0)
        e->rd_started = true;
    else
        LOG_E("No se pudo crear el hilo lector");
    double t_sig = 0, t_status = 0, t_rate = now_monotonic(), busy = 0, wall_t0 = now_monotonic();
    uint64_t samples = 0, delivered_prev = 0;

    while (e->running) {
        /* Órdenes de barrido: mientras barre no se hace nada de la ruta normal */
        pthread_mutex_lock(&e->lock);
        int sc = e->sweep_cmd;
        sweep_cfg_t sreq = e->sweep_req;
        e->sweep_cmd = 0;
        pthread_mutex_unlock(&e->lock);
        if (sc == 1)
            sweep_begin(e, &sreq);
        else if (sc == 2 && e->sw)
            sweep_end(e);
        if (e->sw) {
            sweep_iteration(e);
            continue;
        }

        apply_config(e, false);

        /* Fuente del espectro (CPU/GPU ↔ FPGA) */
        pthread_mutex_lock(&e->lock);
        int ssc = e->ssrc_cmd;
        e->ssrc_cmd = 0;
        engine_cfg_t scfg = e->cfg;
        pthread_mutex_unlock(&e->lock);
        if (ssc == 1 && !e->fpga_active) {
            char ferr[300] = "";
            if (fpga_enter(e, &scfg, ferr, sizeof ferr) < 0) {
                LOG_W("%s", ferr);
                send_error(e, -1, ferr);
            }
            send_status(e);
            if (e->fpga_active)
                send_error(e, -1, "Espectro FPGA activo: no hay IQ, el audio y los decodificadores esperan");
        } else if (ssc == 2 && e->fpga_active) {
            fpga_leave(e, &scfg);
            send_status(e);
        }
        if (e->fpga_active && now_monotonic() - e->fpga_t_last > FPGA_FRAME_TIMEOUT_S) {
            const char *m = "FPGA: no llegan tramas de espectro; vuelvo al espectro de CPU/GPU";
            LOG_W("%s", m);
            send_error(e, -1, m);
            fpga_leave(e, &scfg);
            send_status(e);
        }

        /* Órdenes de grabación (solo este hilo arranca/para el grabador) */
        pthread_mutex_lock(&e->lock);
        int rc = e->rec_cmd;
        e->rec_cmd = 0;
        pthread_mutex_unlock(&e->lock);
        if (rc == 1 && e->fpga_active) {
            send_error(e, -1, "No se puede grabar IQ con el espectro FPGA activo (no hay muestras)");
            send_recording(e, -1);
        } else if (rc == 1) {
            char err[256] = "";
            const source_info_t *in = source_info(e->src);
            char hw[128];
            snprintf(hw, sizeof hw, "%s %s", in->board, in->serial);
            if (recorder_start(e->rec, source_rate(e->src), source_frequency(e->src), hw, 0, err, sizeof err) < 0)
                send_error(e, -1, err);
            send_recording(e, -1);
        } else if (rc == 2) {
            recorder_stop(e->rec);
            send_recording(e, -1);
        }

        /* Errores de lectura del hilo lector (con el mismo ritmo de aviso de siempre) */
        pthread_mutex_lock(&e->rg_m);
        unsigned long eseq = e->rd_err_seq;
        char emsg[300];
        snprintf(emsg, sizeof emsg, "%s", e->rd_err);
        pthread_mutex_unlock(&e->rg_m);
        if (eseq != e->rd_err_seen) {
            e->rd_err_seen = eseq;
            send_error(e, -1, emsg);
        }

        /* La tasa medida cuenta lo que entrega la fuente (no lo que consume el DSP) */
        uint64_t delivered = atomic_load(&e->rd_samples);
        samples += delivered - delivered_prev;
        delivered_prev = delivered;

        pthread_mutex_lock(&e->lock);
        double stall = e->stall_s;
        e->stall_s = 0;
        pthread_mutex_unlock(&e->lock);
        if (stall > 0)
            usleep((useconds_t)(stall * 1e6));

        unsigned slot, queued = 0;
        if (ring_pop(e, &slot, 0.1, &queued) < 0) {
            /* Sin datos (fuente parada o con error): se mantienen estado y avisos */
            double tn = now_monotonic();
            periodic(e, tn, &t_sig, &t_status, &samples, &t_rate);
            pthread_mutex_lock(&e->rg_m);
            bool done = e->fast && e->rd_eof && e->rg_head == e->rg_tail;
            pthread_mutex_unlock(&e->rg_m);
            if (done)
                atomic_store(&e->finished, true); /* todo el fichero ya está en el host */
            continue;
        }
        double t1 = now_monotonic();
        const unsigned n = e->rg_block;
        const int16_t *iq = e->ring + 2 * (size_t)slot * n;
        const double t_blk = e->rg_t[slot]; /* instante en que acabó de leerse */
        {
            float ms = (float)(1e3 * (double)queued * n / (double)source_rate(e->src));
            if (ms > e->ring_peak_ms)
                e->ring_peak_ms = ms;
        }

        if (e->fpga_active) {
            /* Modo FPGA: el bloque son palabras de espectro. Cada trama completa sale por
             * fpga_frame_cb → on_spectrum (detector, S-meter y difusión). Nada más usa el bloque. */
            sync_slots(e);
            fpga_lab_parser_feed_iq(e->fpar, iq, n);
            uint64_t fr = 0, sk = 0, gp = 0;
            fpga_lab_parser_stats(e->fpar, &fr, &sk, &gp);
            pthread_mutex_lock(&e->lock);
            e->fpga_frames = fr;
            e->fpga_gaps = gp;
            pthread_mutex_unlock(&e->lock);
            pthread_mutex_lock(&e->rg_m);
            e->rg_busy = false;
            pthread_cond_broadcast(&e->rg_ctl);
            pthread_mutex_unlock(&e->rg_m);
            double t2f = now_monotonic();
            busy += t2f - t1;
            periodic(e, t2f, &t_sig, &t_status, &samples, &t_rate);
            if (e->host)
                dec_host_periodic(e->host, now_wall());
            continue;
        }
        bool chan_needs = sync_slots(e);

        /* 1) Espectro (trabaja con los enteros crudos) */
        e->cur_send = e->rg_s0[slot] + n;
        if (e->spec && (server_client_count(e->srv) > 0 || e->lab_fn))
            spectrum_feed(e->spec, iq, (int)n, on_spectrum, e);

        /* 2) Grabación */
        recorder_push(e->rec, iq, n);

        /* 2b) Toma para tx_cal: un memcpy corto, sin esperas */
        pthread_mutex_lock(&e->tap_lock);
        /* Con el anillo puede haber bloques en cola anteriores a la orden: se ignoran
         * los que empezaron antes de armar la toma (la calibración del TX necesita
         * muestras de DESPUÉS de empezar a emitir). */
        if (e->tap_on && t_blk - (double)n / (double)source_rate(e->src) >= e->tap_t) {
            unsigned take = MIN(n, e->tap_n - e->tap_pos);
            memcpy(e->tap_buf + 2 * (size_t)e->tap_pos, iq, sizeof(int16_t) * 2 * take);
            e->tap_pos += take;
            if (e->tap_pos >= e->tap_n)
                pthread_cond_broadcast(&e->tap_cv);
        }
        pthread_mutex_unlock(&e->tap_lock);

        /* 3) Continua y conversión a flotante: solo si alguien lo usa. Los canales con
         *    audio (dentro de banda) leen el Q11 crudo y restan la continua en su
         *    mezclador; el float de todo el bloque solo lo piden los decodificadores
         *    NARROW sin DDC en GPU (la ruta GPU y los WIDE usan Q11 crudo). */
        const bool host_needs = host_wants_float(e, t1);
        const bool need_float = host_needs;
        if (chan_needs || host_needs)
            estimate_dc(e, iq, n);
        else
            e->dc_valid = false; /* la estimación se vuelve vieja: se reinicia al volver */
        if (need_float)
            convert_block(e, iq, n);

        /* 4) Decodificadores: dec_host_feed() solo encola punteros (no bloquea).
         *    'now' es la hora de la primera muestra del bloque. */
        if (e->host) {
            double fs = (double)source_rate(e->src);
            dec_host_feed_at(e->host, iq, need_float ? e->buf : NULL, n, fs, source_frequency(e->src),
                             t_blk - n / fs, e->rg_s0[slot]);
        }

        /* 5) Canales de audio (los que no escuchan ya han recibido su nivel con la
         *    trama de espectro: ver on_spectrum) */
        pthread_mutex_lock(&e->lock);
        for (int i = 0; i < MAX_SLOTS; i++) {
            slot_t *s = &e->slots[i];
            /* Con el canal fuera de banda channel_process solo emite su trama de
             * nivel y no mira las muestras (e->buf puede estar sin convertir). */
            if (s->used && !s->dirty && s->p.audio_on)
                channel_process_q11(s->ch, iq, (int)n, e->dc_re, e->dc_im, on_audio, s);
        }
        pthread_mutex_unlock(&e->lock);

        /* Fin del bloque: la ranura vuelve al lector */
        pthread_mutex_lock(&e->rg_m);
        e->rg_busy = false;
        pthread_cond_broadcast(&e->rg_ctl);
        pthread_mutex_unlock(&e->rg_m);

        double t2 = now_monotonic();
        busy += t2 - t1;
        if (t2 - wall_t0 >= 1.0) {
            pthread_mutex_lock(&e->lock);
            e->dsp_load = (float)(busy / (t2 - wall_t0));
            e->ring_ms = e->ring_peak_ms;
            pthread_mutex_unlock(&e->lock);
            e->ring_peak_ms = 0;
            busy = 0;
            wall_t0 = t2;
        }
        periodic(e, t2, &t_sig, &t_status, &samples, &t_rate);
        if (e->host)
            dec_host_periodic(e->host, now_wall());
    }
    sweep_destroy(e->sw);
    e->sw = NULL;
    if (e->rd_started) {
        pthread_mutex_lock(&e->rg_m);
        e->rd_run = false;
        pthread_cond_broadcast(&e->rg_ctl);
        pthread_mutex_unlock(&e->rg_m);
        pthread_join(e->rd_thread, NULL);
        e->rd_started = false;
    }
    source_stop(e->src);
    recorder_stop(e->rec);
    return NULL;
}

/* ======================================================================= */
/*  API pública                                                            */
/* ======================================================================= */
engine_t *engine_create(source_t *src, server_t *srv, const engine_cfg_t *initial, bool use_gpu, const char *recdir)
{
    engine_t *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->src = src;
    e->srv = srv;
    e->use_gpu = use_gpu;
    pthread_mutex_init(&e->lock, NULL);
    pthread_mutex_init(&e->tap_lock, NULL);
    pthread_mutex_init(&e->rg_m, NULL);
    pthread_cond_init(&e->rg_data, NULL);
    pthread_cond_init(&e->rg_ctl, NULL);
    pthread_cond_init(&e->tap_cv, NULL);
    e->want = *initial;
    engine_sanitize(e, &e->want);
    e->cfg = e->want;
    e->rec = recorder_create(recdir);
    /* ¿Está cargada la imagen lab? La detección solo lee la versión y config_gpio (seguro en la
     * imagen de serie); el hilo DSP todavía no existe, nadie más usa la placa. */
    fpga_lab_io_t io;
    char perr[160];
    if (source_fpga_lab_io(src, &io) == 0 && fpga_lab_probe(&io, &e->lab, perr, sizeof perr) > 0) {
        e->lab_present_pub = true;
        e->lab_version_pub = e->lab.version;
        LOG_I("FPGA: imagen lab detectada (versión %u.%u): espectro FPGA disponible", e->lab.version >> 16,
              e->lab.version & 0xFFFFu);
    }
    return e;
}

int engine_start(engine_t *e)
{
    e->running = true;
    return pthread_create(&e->thread, NULL, dsp_main, e);
}

void engine_stop(engine_t *e)
{
    if (!e->running)
        return;
    e->running = false;
    pthread_join(e->thread, NULL);
}

void engine_destroy(engine_t *e)
{
    if (!e)
        return;
    engine_stop(e);
    for (int i = 0; i < MAX_SLOTS; i++)
        channel_destroy(e->slots[i].ch);
    spectrum_destroy(e->spec);
    detector_destroy(e->det);
    detector_destroy(e->ldet);
    recorder_destroy(e->rec);
    free(e->ring);
    free(e->rg_t);
    free(e->rg_s0);
    free(e->buf);
    free(e->specmsg);
    free(e->fpga_db);
    fpga_lab_parser_destroy(e->fpar);
    pthread_mutex_destroy(&e->lock);
    pthread_mutex_destroy(&e->tap_lock);
    pthread_mutex_destroy(&e->rg_m);
    pthread_cond_destroy(&e->rg_data);
    pthread_cond_destroy(&e->rg_ctl);
    pthread_cond_destroy(&e->tap_cv);
    free(e);
}

void engine_request(engine_t *e, const engine_cfg_t *w, unsigned fields)
{
    engine_cfg_t c = *w;
    engine_sanitize(e, &c);
    pthread_mutex_lock(&e->lock);
    if (fields & CFG_CENTER) e->want.center_hz = c.center_hz;
    if (fields & CFG_RATE) e->want.rate = c.rate;
    if (fields & CFG_RFBW) e->want.rf_bw = c.rf_bw;
    if (fields & CFG_GAIN) { e->want.gain_mode = c.gain_mode; e->want.gain_db = c.gain_db; }
    if (fields & CFG_BIASTEE) e->want.biastee = c.biastee;
    if (fields & CFG_FFT) { e->want.fft_size = c.fft_size; e->want.fps = c.fps; }
    if (fields & CFG_LOC) { e->want.loc_valid = c.loc_valid; e->want.lat = c.lat; e->want.lon = c.lon; }
    if (fields & CFG_DET) e->want.det_snr = c.det_snr;
    e->dirty |= fields;
    pthread_mutex_unlock(&e->lock);
}

static void abs_deadline(struct timespec *ts, double s)
{
    clock_gettime(CLOCK_REALTIME, ts);
    double t = ts->tv_nsec * 1e-9 + s;
    ts->tv_sec += (time_t)t;
    ts->tv_nsec = (long)((t - (double)(time_t)t) * 1e9);
}

bool engine_wait_tuned(engine_t *e, int64_t freq, uint32_t rate, double timeout_s)
{
    engine_cfg_t c;
    engine_get_config(e, &c);
    c.center_hz = freq;
    c.rate = rate;
    engine_request(e, &c, CFG_CENTER | CFG_RATE);
    for (double t = 0; t < timeout_s; t += 0.05) {
        if (source_rate(e->src) == rate && source_frequency(e->src) == freq && !e->sw)
            return true;
        usleep(50000);
    }
    return false;
}

int engine_capture_iq(engine_t *e, int64_t freq, uint32_t rate, int16_t *iq, unsigned n, double timeout_s, char *err,
                      size_t errlen)
{
    if (source_rate(e->src) != rate || source_frequency(e->src) != freq) {
        snprintf(err, errlen, "el receptor no está en %.6f MHz / %u sps", freq / 1e6, rate);
        return -1;
    }
    pthread_mutex_lock(&e->tap_lock);
    if (e->tap_on) {
        pthread_mutex_unlock(&e->tap_lock);
        snprintf(err, errlen, "ya hay una captura en curso");
        return -1;
    }
    e->tap_buf = iq;
    e->tap_n = n;
    e->tap_pos = 0;
    e->tap_t = now_wall();
    e->tap_on = true;
    struct timespec dl;
    abs_deadline(&dl, timeout_s);
    int rc = 0;
    while (e->tap_pos < e->tap_n && rc == 0)
        rc = pthread_cond_timedwait(&e->tap_cv, &e->tap_lock, &dl);
    bool ok = e->tap_pos >= e->tap_n;
    e->tap_on = false;
    pthread_mutex_unlock(&e->tap_lock);
    if (!ok)
        snprintf(err, errlen, "el receptor no entregó muestras a tiempo (¿barrido activo o fuente parada?)");
    return ok ? 0 : -1;
}

void engine_debug_stall(engine_t *e, double seconds)
{
    pthread_mutex_lock(&e->lock);
    e->stall_s = seconds;
    pthread_mutex_unlock(&e->lock);
}

void engine_get_config(engine_t *e, engine_cfg_t *out)
{
    pthread_mutex_lock(&e->lock);
    *out = e->want;
    pthread_mutex_unlock(&e->lock);
}

void engine_add_client(engine_t *e, int client)
{
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < MAX_SLOTS; i++) {
        slot_t *s = &e->slots[i];
        if (s->used)
            continue;
        if (!s->ch)
            s->ch = channel_create();
        s->used = true;
        s->client = client;
        s->e = e;
        /* Canal inicial: el centro de la banda en WFM o NFM según la tasa */
        s->p.freq_hz = e->cfg.center_hz;
        s->p.mode = MODE_NFM;
        s->p.bw_hz = mode_default_bw(MODE_NFM);
        s->p.squelch_db = NAN;
        s->p.deemph_us = 50;
        s->p.stereo = true;
        s->p.audio_on = false;
        s->dirty = true;
        break;
    }
    pthread_mutex_unlock(&e->lock);
}

void engine_remove_client(engine_t *e, int client)
{
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < MAX_SLOTS; i++)
        if (e->slots[i].used && e->slots[i].client == client)
            e->slots[i].used = false; /* el canal se reutiliza en la próxima conexión */
    pthread_mutex_unlock(&e->lock);
}

/* Copia de las señales del último ciclo del detector (offsets relativos al centro). */
int engine_get_signals(engine_t *e, det_signal_t *out, int max, int64_t *center_hz)
{
    pthread_mutex_lock(&e->lock);
    int n = MIN(e->nsigs, max);
    memcpy(out, e->sigs, sizeof(det_signal_t) * (size_t)n);
    if (center_hz)
        *center_hz = e->cfg.center_hz;
    pthread_mutex_unlock(&e->lock);
    return n;
}

bool engine_get_channel(engine_t *e, int client, chan_params_t *p)
{
    bool ok = false;
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < MAX_SLOTS; i++)
        if (e->slots[i].used && e->slots[i].client == client) {
            *p = e->slots[i].p;
            ok = true;
        }
    pthread_mutex_unlock(&e->lock);
    return ok;
}

void engine_set_channel(engine_t *e, int client, const chan_params_t *p)
{
    pthread_mutex_lock(&e->lock);
    for (int i = 0; i < MAX_SLOTS; i++)
        if (e->slots[i].used && e->slots[i].client == client) {
            e->slots[i].p = *p;
            e->slots[i].dirty = true;
        }
    pthread_mutex_unlock(&e->lock);
}

void engine_record(engine_t *e, bool on)
{
    pthread_mutex_lock(&e->lock);
    e->rec_cmd = on ? 1 : 2;
    pthread_mutex_unlock(&e->lock);
}

int engine_delete_recording(engine_t *e, const char *name)
{
    recorder_status_t st;
    recorder_status(e->rec, &st);
    if (st.active && strncmp(name, st.name, strlen(st.name)) == 0)
        return -1; /* no se borra lo que se está grabando */
    return recorder_delete(e->rec, name);
}

void engine_status_json(engine_t *e, sb_t *sb)
{
    const source_info_t *in = source_info(e->src);
    int clients = server_client_count(e->srv);
    unsigned long long sent = server_bytes_sent(e->srv);
    pthread_mutex_lock(&e->lock);
    const engine_cfg_t *c = &e->cfg;
    sb_puts(sb, "{\"type\":\"status\",\"app\":\"" APP_NAME "\",\"version\":\"" APP_VERSION "\",\"device\":{");
    sb_puts(sb, "\"kind\":");
    sb_json_str(sb, in->kind);
    sb_puts(sb, ",\"board\":");
    sb_json_str(sb, in->board);
    sb_puts(sb, ",\"serial\":");
    sb_json_str(sb, in->serial);
    sb_printf(sb, ",\"fw\":\"%s\",\"fpga\":\"%s\",\"lib\":\"%s\"", in->fw_version, in->fpga_version, in->lib_version);
    sb_printf(sb, ",\"freq_min\":%lld,\"freq_max\":%lld,\"rate_min\":%u,\"rate_max\":%u", (long long)in->freq_min,
              (long long)in->freq_max, in->rate_min, MIN(in->rate_max, SOURCE_RATE_NATIVE_MAX));
    if (in->rate_max_oversample)
        sb_printf(sb, ",\"rate_max_oversample\":%u", in->rate_max_oversample);
    else
        sb_puts(sb, ",\"rate_max_oversample\":null");
    sb_printf(sb, ",\"bw_min\":%u,\"bw_max\":%u,\"gain_min\":%d,\"gain_max\":%d", in->bw_min, in->bw_max,
              in->gain_min, in->gain_max);
    sb_printf(sb, ",\"can_tune\":%s,\"biastee\":%s,\"can_tx\":%s}", in->can_tune ? "true" : "false",
              in->has_biastee ? "true" : "false", in->can_tx ? "true" : "false");
    sb_printf(sb, ",\"center\":%lld,\"rate\":%u,\"rf_bw\":%u,\"gain_mode\":\"%s\",\"gain\":%d,\"gain_now\":%d",
              (long long)c->center_hz, c->rate, c->rf_bw, gain_mode_name(c->gain_mode), c->gain_db, e->gain_now);
    /* Formato de las muestras en el USB/fichero (a las demás partes siempre llegan en Q11 int16) */
    sb_printf(sb, ",\"format\":\"%s\",\"oversample\":%s", source_format(e->src),
              source_oversample(e->src) ? "true" : "false");
    /* dBm = dBFS + dbm_offset, solo con tabla de calibración de ganancia de libbladeRF */
    if (e->dbm_valid)
        sb_printf(sb, ",\"dbm_offset\":%.2f", e->dbm_off);
    else
        sb_puts(sb, ",\"dbm_offset\":null");
    if (e->sweep_active) {
        const sweep_info_t *w = &e->sweep_info;
        sb_printf(sb, ",\"sweep\":{\"start\":%lld,\"stop\":%lld,\"fft\":%d,\"rate\":%u,\"hops\":%d,\"bins\":%d,"
                      "\"mode\":\"%s\",\"sweeps\":%llu,\"restarts\":%llu}",
                  (long long)w->start_hz, (long long)w->stop_hz, w->fft, w->rate, w->hops, w->bins,
                  w->quick ? "quick" : "slow", (unsigned long long)w->sweeps, (unsigned long long)w->restarts);
    } else {
        sb_puts(sb, ",\"sweep\":null");
    }
    fpga_status_json(e, sb);
    sb_printf(sb, ",\"biastee\":%s,\"fft\":%d,\"fps\":%d,\"det_snr\":%.1f",
              c->biastee ? "true" : "false", c->fft_size, c->fps, c->det_snr);
    if (e->profile[0]) {
        sb_puts(sb, ",\"profile\":");
        sb_json_str(sb, e->profile);
    } else {
        sb_puts(sb, ",\"profile\":null");
    }
    if (c->loc_valid)
        sb_printf(sb, ",\"loc\":{\"lat\":%.6f,\"lon\":%.6f}", c->lat, c->lon);
    else
        sb_puts(sb, ",\"loc\":null");
    /* overruns = muestras perdidas por el hardware/USB; drops = bloques que el DSP no
     * llegó a consumir (anillo lleno); ring_ms = cola máxima del anillo en el último segundo */
    sb_printf(sb, ",\"stats\":{\"rate\":%.0f,\"overruns\":%lu,\"drops\":%lu,\"ring_ms\":%.0f,\"temp\":%.1f,"
                  "\"load\":%.3f,\"clients\":%d,\"sent\":%llu",
              e->meas_rate, atomic_load(&e->hw_overruns), atomic_load(&e->dsp_drops), e->ring_ms, e->temp_c,
              e->dsp_load, clients, sent);
    sb_puts(sb, ",\"backend\":");
    sb_json_str(sb, e->spec_backend);
    sb_puts(sb, "}}");
    pthread_mutex_unlock(&e->lock);
}

void engine_send_snapshot(engine_t *e, int client)
{
    sb_t sb = {0};
    engine_status_json(e, &sb);
    broadcast_json(e, client, &sb);
    sb_free(&sb);
    send_signals(e, client);
    send_recording(e, client);
}
