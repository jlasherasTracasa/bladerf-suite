/*
 * host.c — Host de decodificadores. Ver host.h y docs/arquitectura/ARQUITECTURA.md.
 *
 * Hilos y cerrojos:
 *   - h->lock protege la tabla de instancias, la memoria de eventos, el
 *     callback de eventos y la última (fs, centro) alimentada.
 *   - inst->m protege la cola, las banderas de orden y los contadores.
 *   - Orden de adquisición: h->lock → inst->m. Un hilo de instancia nunca
 *     toma h->lock mientras tiene inst->m.
 *   - dec_host_feed() (hilo DSP) solo toma cerrojos durante microsegundos:
 *     copia el bloque una vez y encola punteros.
 */
#include "host.h"

#include "../dsp/ddc.h"
#include "../gpu/ddc_mgr.h"
#include "../tx/mod.h"
#include "../util/json.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INST 64
#define QDEPTH 16          /* bloques en cola por instancia (~0,3 s a 20 ms/bloque) */
#define EV_PER_SEC 200.0   /* límite de eventos por instancia */
#define HIST_N 100         /* eventos recordados para quien conecta */
#define POOL_MAX 6

typedef enum { ST_WAITING = 0, ST_OK, ST_ERROR } inst_state_t;
static const char *const STATE_NAMES[] = {"waiting", "ok", "error"};

/* Bloque de muestras compartido entre todas las instancias. */
typedef struct block {
    atomic_int refs;
    int16_t *iq;
    cf32 *buf;
    size_t iq_cap, buf_cap; /* en muestras */
    unsigned n;
    double fs, now;
    int64_t center;
    /* Qué datos son válidos en ESTE uso del bloque. Los bloques se reciclan en un
     * pool y conservan punteros (y capacidades) del uso anterior: un 'buf' rancho
     * de una salida 'mid' (pocas muestras) leído con n = 8192 es un desbordamiento.
     * Si el motor llamó a dec_host_feed sin float (needs_float() se evaluó antes de
     * que una instancia pasara a elegible) b->buf no se rellena: sin esta marca el
     * consumidor leía el residuo del pool. */
    bool has_iq, has_buf;
    bool mid; /* buf = salidas del DDC de GPU a tasa intermedia (no la captura) */
} block_t;

typedef struct inst {
    dec_host_t *h;
    int id, owner;
    const decoder_desc_t *d;
    char *opts;
    pthread_t th;
    bool started;

    pthread_mutex_t m;
    pthread_cond_t cv;
    block_t *q[QDEPTH];
    int qh, qn;
    bool stop, cfg_dirty, want_state, loc_dirty, working;
    int64_t freq;
    bool loc_valid;
    double lat, lon;
    inst_state_t state;
    char err[160];
    double rate_now;
    unsigned long events, drops;
    double busy_acc, load, load_t0;
    char *last_state;
    size_t last_state_len;

    /* --- privado del hilo de la instancia --- */
    void *st;
    ddc_t *ddc;
    /* DDC en GPU (gpu_attach): la GPU entrega la tasa intermedia y 'fine' hace
     * filtro de canal + remuestreo exacto. on_gpu lo lee dec_host_feed (inst->m). */
    ddc_fine_t *fine;
    int gpu_ch;
    bool on_gpu;
    cf32 *ddc_out;
    size_t ddc_cap;
    double cur_fs;
    int64_t cur_center, cur_freq;
    bool cfg_pending, create_failed, ever_cfg;
    double tokens, tok_t;
    sb_t evsb;
} inst_t;

#define AUTO_RULES 8
#define AUTO_SIGS 64
typedef struct {
    char proto[24];
    int64_t lo_hz, hi_hz;
} auto_rule_t;
typedef struct {
    int inst, rule;
    int64_t freq_hz;
    double last_seen;
} auto_track_t;

struct dec_host {
    server_t *srv;
    /* instancias automáticas: reglas, seguimiento y hilo de trabajo */
    pthread_mutex_t am; /* protege todo lo de este bloque */
    pthread_cond_t acv, aidle;
    pthread_t ath;
    bool ath_on, a_stop, a_pending, a_busy;
    auto_rule_t arule[AUTO_RULES];
    int nrule;
    auto_track_t atrk[DEC_AUTO_MAX];
    int ntrk;
    dec_sig_t asig[AUTO_SIGS];
    int nasig;
    double anow;
    unsigned gen; /* sube con cada auto_clear */
    pthread_mutex_t lock;
    inst_t *inst[MAX_INST];
    int next_id;
    bool loc_valid;
    double lat, lon;
    double fs;
    int64_t center;
    char *hist[HIST_N];
    int hist_head, hist_n;
    void (*ev_fn)(void *, const char *);
    void *ev_ctx;
    block_t *pool[POOL_MAX];
    int npool;
    double t_state, t_list, t_load;
    atomic_int list_dirty;
    char suspended[120]; /* no hay IQ (ver dec_host_set_suspended); protegido por h->lock */
    /* DDC en GPU compartido (se crea la primera vez que hace falta) */
    pthread_mutex_t gpu_lock;
    ddc_mgr_t *mgr;
    bool gpu_tried, gpu_enabled;
    atomic_ulong gpu_drops;
};

/* ======================================================================= */
/*  Bloques compartidos                                                    */
/* ======================================================================= */
static block_t *block_get(dec_host_t *h, unsigned n, bool need_iq, bool need_buf)
{
    block_t *b = NULL;
    if (h->npool > 0)
        b = h->pool[--h->npool];
    if (!b)
        b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    if (need_iq && b->iq_cap < n) {
        free(b->iq);
        b->iq = malloc(sizeof(int16_t) * 2 * (size_t)n);
        b->iq_cap = b->iq ? n : 0;
    }
    if (need_buf && b->buf_cap < n) {
        free(b->buf);
        b->buf = malloc(sizeof(cf32) * (size_t)n);
        b->buf_cap = b->buf ? n : 0;
    }
    b->has_iq = b->has_buf = false;
    atomic_store(&b->refs, 1);
    return b;
}

static void block_free(block_t *b)
{
    free(b->iq);
    free(b->buf);
    free(b);
}

static void block_unref(dec_host_t *h, block_t *b)
{
    if (atomic_fetch_sub(&b->refs, 1) != 1)
        return;
    pthread_mutex_lock(&h->lock);
    if (h->npool < POOL_MAX) {
        h->pool[h->npool++] = b;
        b = NULL;
    }
    pthread_mutex_unlock(&h->lock);
    if (b)
        block_free(b);
}

/* ======================================================================= */
/*  Eventos                                                                */
/* ======================================================================= */
/* emit() de los decodificadores: envuelve, limita, guarda y difunde. */
static void inst_emit(void *ctx, const char *json, size_t len)
{
    inst_t *in = ctx;
    dec_host_t *h = in->h;

    /* Cubo de fichas: 200 eventos/s sostenidos, ráfagas de hasta 200 */
    double t = now_monotonic();
    in->tokens = MIN(EV_PER_SEC, in->tokens + (t - in->tok_t) * EV_PER_SEC);
    in->tok_t = t;
    if (in->tokens < 1.0) {
        pthread_mutex_lock(&in->m);
        in->drops++;
        pthread_mutex_unlock(&in->m);
        return;
    }
    in->tokens -= 1.0;

    int64_t freq;
    pthread_mutex_lock(&in->m);
    in->events++;
    freq = in->freq;
    pthread_mutex_unlock(&in->m);

    sb_t *s = &in->evsb;
    sb_clear(s);
    sb_printf(s, "{\"type\":\"dec\",\"inst\":%d,\"proto\":\"%s\",\"freq\":%lld,\"t\":%.3f,\"msg\":", in->id,
              in->d->id, (long long)freq, now_wall());
    sb_append(s, json, len);
    sb_puts(s, "}");

    pthread_mutex_lock(&h->lock);
    char *copy = strdup(s->data);
    if (copy) {
        int slot = (h->hist_head + h->hist_n) % HIST_N;
        if (h->hist_n == HIST_N) {
            free(h->hist[h->hist_head]);
            slot = h->hist_head;
            h->hist_head = (h->hist_head + 1) % HIST_N;
        } else {
            h->hist_n++;
        }
        h->hist[slot] = copy;
    }
    if (h->ev_fn)
        h->ev_fn(h->ev_ctx, s->data);
    pthread_mutex_unlock(&h->lock);
    if (h->srv)
        server_send_text(h->srv, -1, s->data, s->len);
}

/* ======================================================================= */
/*  Hilo de una instancia                                                  */
/* ======================================================================= */
static void set_state(inst_t *in, inst_state_t st, const char *err)
{
    pthread_mutex_lock(&in->m);
    in->state = st;
    snprintf(in->err, sizeof in->err, "%s", err ? err : "");
    pthread_mutex_unlock(&in->m);
    atomic_store(&in->h->list_dirty, 1);
}

static void fmt_msps(char *out, size_t n, double hz)
{
    if (hz >= 1e6)
        snprintf(out, n, "%g Msps", hz / 1e6);
    else
        snprintf(out, n, "%g ksps", hz / 1e3);
}

/* ======================================================================= */
/*  DDC en GPU (C1, ver docs/arquitectura/CUDA.md)                                    */
/* ======================================================================= */
#define GPU_MIN_NARROW 2 /* con menos instancias narrow no compensa */

/* Entrega de la GPU (hilo ddc-gpu): copia las salidas a un bloque 'mid' y lo
 * encola en la instancia. Nunca bloquea mucho: si la cola está llena se
 * descarta, como en dec_host_feed. */
static void gpu_deliver(void *ctx, const cf32 *y, int n, double now, double fs, int64_t center)
{
    inst_t *in = ctx;
    dec_host_t *h = in->h;
    pthread_mutex_lock(&h->lock);
    block_t *b = block_get(h, (unsigned)n, false, true);
    pthread_mutex_unlock(&h->lock);
    if (!b)
        return;
    memcpy(b->buf, y, sizeof(cf32) * (size_t)n);
    b->has_buf = true;
    b->n = (unsigned)n;
    b->fs = fs;
    b->center = center;
    b->now = now;
    b->mid = true;
    pthread_mutex_lock(&in->m);
    if (in->qn < QDEPTH) {
        in->q[(in->qh + in->qn) % QDEPTH] = b;
        in->qn++;
        pthread_cond_signal(&in->cv);
        pthread_mutex_unlock(&in->m);
        return;
    }
    in->drops++;
    pthread_mutex_unlock(&in->m);
    block_unref(h, b);
}

static ddc_mgr_t *gpu_mgr_get(dec_host_t *h)
{
    pthread_mutex_lock(&h->gpu_lock);
    if (!h->mgr && !h->gpu_tried && h->gpu_enabled) {
        char err[160] = "";
        h->gpu_tried = true;
        h->mgr = ddc_mgr_create(1 << 20, err, sizeof err);
        if (h->mgr)
            LOG_I("DDC en GPU: %s", ddc_mgr_device_name(h->mgr));
        else
            LOG_I("DDC en GPU no disponible (%s): se usa la CPU", err);
    }
    ddc_mgr_t *m = h->mgr;
    pthread_mutex_unlock(&h->gpu_lock);
    return m;
}

static void gpu_detach(inst_t *in)
{
    if (in->on_gpu) {
        pthread_mutex_lock(&in->m);
        in->on_gpu = false;
        pthread_mutex_unlock(&in->m);
        ddc_mgr_remove(in->h->mgr, in->gpu_ch);
    }
    ddc_fine_destroy(in->fine);
    in->fine = NULL;
}

/* Pasa la instancia (narrow) al DDC de GPU si compensa: hay GPU, hay al menos
 * GPU_MIN_NARROW instancias narrow y el diezmado es grande. Solo la llama el
 * hilo de la instancia, sin cerrojos tomados. */
static bool gpu_attach(inst_t *in, double fs, int64_t center, int64_t freq)
{
    dec_host_t *h = in->h;
    const decoder_desc_t *d = in->d;
    if (!h->gpu_enabled)
        return false;
    int nn = 0;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++)
        if (h->inst[i] && h->inst[i]->d->kind == DEC_NARROW)
            nn++;
    pthread_mutex_unlock(&h->lock);
    if (nn < GPU_MIN_NARROW || fs / d->rate < DDC_GPU_MIN_DECIM)
        return false;
    ddc_mgr_t *m = gpu_mgr_get(h);
    if (!m)
        return false;
    char err[96];
    double fs_mid = 0;
    int ch = ddc_mgr_add(m, fs, center, freq, d->rate, d->bw_hz, gpu_deliver, in, &fs_mid, err, sizeof err);
    if (ch < 0)
        return false;
    in->fine = ddc_fine_create(fs_mid, d->rate, d->bw_hz);
    if (!in->fine) {
        ddc_mgr_remove(m, ch);
        return false;
    }
    in->gpu_ch = ch;
    pthread_mutex_lock(&in->m);
    in->on_gpu = true;
    pthread_mutex_unlock(&in->m);
    return true;
}

/* Al entrar la segunda instancia narrow, las que ya corrían en CPU deben
 * reevaluar si pasan a la GPU: se les marca la configuración como pendiente. */
static void gpu_kick(dec_host_t *h)
{
    if (!h->gpu_enabled)
        return;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in || in->d->kind != DEC_NARROW)
            continue;
        pthread_mutex_lock(&in->m);
        if (!in->on_gpu && in->state == ST_OK) {
            in->cfg_dirty = true;
            pthread_cond_signal(&in->cv);
        }
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
}

/* (Re)construye DDC y decodificador para la captura (fs, center) y la
 * frecuencia actual de la instancia. Solo la llama el hilo de la instancia. */
static void reconfigure(inst_t *in, double fs, int64_t center)
{
    const decoder_desc_t *d = in->d;
    pthread_mutex_lock(&in->m);
    int64_t freq = in->freq;
    bool lv = in->loc_valid;
    double lat = in->lat, lon = in->lon;
    pthread_mutex_unlock(&in->m);

    bool fs_changed = !in->ever_cfg || fs != in->cur_fs;
    char err[160] = "";
    char a[32], b[32];

    if (d->kind == DEC_WIDE) {
        bool ok = d->wide_rate ? (uint32_t)(fs + 0.5) == d->wide_rate : fs + 0.5 >= d->wide_min_rate;
        if (!ok) {
            if (in->st) {
                d->destroy(in->st);
                in->st = NULL;
            }
            fmt_msps(a, sizeof a, d->wide_rate ? d->wide_rate : d->wide_min_rate);
            fmt_msps(b, sizeof b, fs);
            snprintf(err, sizeof err, "requiere %s %s (ahora %s)", d->wide_rate ? "exactamente" : "al menos", a, b);
            set_state(in, ST_ERROR, err);
            in->create_failed = false;
        } else if (fs_changed || !in->st) {
            if (in->st) {
                d->destroy(in->st);
                in->st = NULL;
            }
            dec_cfg_t cfg = {.rate = fs, .freq_hz = center, .center_hz = center, .loc_valid = lv, .lat = lat,
                             .lon = lon, .opts = in->opts};
            in->st = d->create(&cfg, err, sizeof err);
            if (in->st) {
                set_state(in, ST_OK, NULL);
            } else {
                set_state(in, ST_ERROR, err[0] ? err : "error al crear el decodificador");
                in->create_failed = true;
            }
        }
        pthread_mutex_lock(&in->m);
        in->rate_now = fs;
        pthread_mutex_unlock(&in->m);
    } else {
        /* NARROW: el canal (± la mitad de su ancho) debe caber en la banda
         * capturada; el 96 % deja margen al roll-off del filtro analógico. */
        double off = fabs((double)(freq - center));
        bool inband = off + d->bw_hz / 2.0 <= 0.48 * fs;
        if (fs < d->rate) {
            gpu_detach(in);
            if (in->ddc) {
                ddc_destroy(in->ddc);
                in->ddc = NULL;
            }
            fmt_msps(a, sizeof a, d->rate);
            fmt_msps(b, sizeof b, fs);
            snprintf(err, sizeof err, "la tasa de captura (%s) es menor que la del canal (%s)", b, a);
            set_state(in, ST_ERROR, err);
        } else if (!inband) {
            gpu_detach(in);
            if (in->ddc) {
                ddc_destroy(in->ddc);
                in->ddc = NULL;
            }
            set_state(in, ST_WAITING, "fuera de la banda capturada");
        } else if (in->create_failed && in->st == NULL) {
            /* ya falló create(): se mantiene el error hasta reiniciar la instancia */
        } else {
            gpu_detach(in); /* se reevalúa abajo (cambia fs, centro o frecuencia) */
            bool on_gpu = gpu_attach(in, fs, center, freq);
            if (on_gpu) {
                ddc_destroy(in->ddc);
                in->ddc = NULL;
            } else {
                if (in->ddc && fs_changed) {
                    ddc_destroy(in->ddc);
                    in->ddc = NULL;
                }
                if (!in->ddc) {
                    in->ddc = ddc_create(fs, center, freq, d->rate, d->bw_hz);
                    if (!in->ddc)
                        set_state(in, ST_ERROR, "no se pudo crear el DDC");
                } else {
                    ddc_retune(in->ddc, center, freq);
                }
            }
            if ((in->ddc || on_gpu) && !in->st) {
                dec_cfg_t cfg = {.rate = d->rate, .freq_hz = freq, .center_hz = center, .loc_valid = lv, .lat = lat,
                                 .lon = lon, .opts = in->opts};
                in->st = d->create(&cfg, err, sizeof err);
                if (!in->st) {
                    set_state(in, ST_ERROR, err[0] ? err : "error al crear el decodificador");
                    in->create_failed = true;
                    ddc_destroy(in->ddc);
                    in->ddc = NULL;
                    gpu_detach(in);
                }
            }
            if ((in->ddc || in->fine) && in->st)
                set_state(in, ST_OK, NULL);
        }
        pthread_mutex_lock(&in->m);
        in->rate_now = d->rate;
        pthread_mutex_unlock(&in->m);
    }
    in->cur_fs = fs;
    in->cur_center = center;
    in->cur_freq = freq;
    in->ever_cfg = true;
    in->cfg_pending = false;
}

static void process_block(inst_t *in, const block_t *b)
{
    const decoder_desc_t *d = in->d;
    if (!in->st)
        return;
    if (d->kind == DEC_WIDE) {
        if (d->process_raw && b->has_iq)
            d->process_raw(in->st, b->iq, (int)b->n, b->now, inst_emit, in);
        return;
    }
    if (b->mid) {
        /* Salidas del DDC de GPU a tasa intermedia: solo falta la etapa fina */
        if (!in->fine || !d->process || !b->has_buf)
            return;
        size_t nd = ddc_fine_max_out(in->fine, (int)b->n);
        if (nd > in->ddc_cap) {
            free(in->ddc_out);
            in->ddc_out = malloc(sizeof(cf32) * nd);
            in->ddc_cap = in->ddc_out ? nd : 0;
            if (!in->ddc_out)
                return;
        }
        int k = ddc_fine_process(in->fine, b->buf, (int)b->n, in->ddc_out, (int)in->ddc_cap);
        if (k > 0)
            d->process(in->st, in->ddc_out, k, b->now, inst_emit, in);
        return;
    }
    if (!in->ddc || !d->process || !b->has_buf)
        return;
    size_t need = ddc_max_out(in->ddc, (int)b->n);
    if (need > in->ddc_cap) {
        free(in->ddc_out);
        in->ddc_out = malloc(sizeof(cf32) * need);
        in->ddc_cap = in->ddc_out ? need : 0;
        if (!in->ddc_out)
            return;
    }
    int m = ddc_process(in->ddc, b->buf, (int)b->n, in->ddc_out, (int)in->ddc_cap);
    if (m > 0)
        d->process(in->st, in->ddc_out, m, b->now, inst_emit, in);
}

static void make_state(inst_t *in)
{
    if (!in->st || !in->d->state_json)
        return;
    sb_t sb = {0};
    in->d->state_json(in->st, &sb, now_wall());
    if (!sb.data) {
        return;
    }
    sb_t w = {0};
    sb_printf(&w, "{\"type\":\"dec_state\",\"inst\":%d,\"proto\":\"%s\",\"t\":%.3f,\"state\":", in->id, in->d->id,
              now_wall());
    sb_append(&w, sb.data, sb.len);
    sb_puts(&w, "}");
    char *copy = strdup(sb.data);
    pthread_mutex_lock(&in->m);
    free(in->last_state);
    in->last_state = copy;
    in->last_state_len = sb.len;
    pthread_mutex_unlock(&in->m);
    if (in->h->srv)
        server_send_text(in->h->srv, -1, w.data, w.len);
    sb_free(&sb);
    sb_free(&w);
}

static void *inst_main(void *arg)
{
    inst_t *in = arg;
    char nm[16];
    snprintf(nm, sizeof nm, "dec-%.10s", in->d->id);
    pthread_setname_np(pthread_self(), nm);
    in->tokens = EV_PER_SEC;
    in->tok_t = now_monotonic();

    for (;;) {
        pthread_mutex_lock(&in->m);
        while (!in->stop && in->qn == 0 && !in->cfg_dirty && !in->want_state && !in->loc_dirty)
            pthread_cond_wait(&in->cv, &in->m);
        if (in->stop) {
            pthread_mutex_unlock(&in->m);
            break;
        }
        block_t *b = NULL;
        if (in->qn > 0) {
            b = in->q[in->qh];
            in->qh = (in->qh + 1) % QDEPTH;
            in->qn--;
        }
        bool cfg = in->cfg_dirty, ws = in->want_state, ld = in->loc_dirty;
        bool lv = in->loc_valid;
        double lat = in->lat, lon = in->lon;
        in->cfg_dirty = in->want_state = in->loc_dirty = false;
        in->working = true;
        pthread_mutex_unlock(&in->m);

        if (cfg)
            in->cfg_pending = true;
        if (ld && in->st && in->d->set_location)
            in->d->set_location(in->st, lv, lat, lon);
        if (in->cfg_pending && !b) {
            pthread_mutex_lock(&in->h->lock);
            double fs = in->h->fs;
            int64_t c = in->h->center;
            pthread_mutex_unlock(&in->h->lock);
            if (fs > 0)
                reconfigure(in, fs, c);
            else
                in->cfg_pending = false; /* se reevaluará con el primer bloque */
        }
        if (b) {
            if (in->cfg_pending || !in->ever_cfg || b->fs != in->cur_fs || b->center != in->cur_center)
                reconfigure(in, b->fs, b->center);
            double t0 = now_monotonic();
            if (in->st && (in->d->kind == DEC_WIDE || in->ddc || in->fine))
                process_block(in, b);
            double dt = now_monotonic() - t0;
            pthread_mutex_lock(&in->m);
            in->busy_acc += dt;
            pthread_mutex_unlock(&in->m);
            block_unref(in->h, b);
        }
        if (ws)
            make_state(in);
        pthread_mutex_lock(&in->m);
        in->working = false;
        pthread_mutex_unlock(&in->m);
    }
    gpu_detach(in); /* antes de vaciar la cola: así la GPU ya no encola más */
    /* Vaciar lo que quede en la cola */
    for (;;) {
        pthread_mutex_lock(&in->m);
        block_t *b = in->qn > 0 ? in->q[in->qh] : NULL;
        if (b) {
            in->qh = (in->qh + 1) % QDEPTH;
            in->qn--;
        }
        pthread_mutex_unlock(&in->m);
        if (!b)
            break;
        block_unref(in->h, b);
    }
    if (in->st)
        in->d->destroy(in->st);
    ddc_destroy(in->ddc);
    free(in->ddc_out);
    return NULL;
}

/* ======================================================================= */
/*  API                                                                    */
/* ======================================================================= */
dec_host_t *dec_host_create(server_t *srv)
{
    dec_host_t *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->srv = srv;
    h->next_id = 1;
    pthread_mutex_init(&h->lock, NULL);
    pthread_mutex_init(&h->gpu_lock, NULL);
    pthread_mutex_init(&h->am, NULL);
    pthread_cond_init(&h->acv, NULL);
    pthread_cond_init(&h->aidle, NULL);
    /* BLADERF_DDC_GPU=0 desactiva el DDC en GPU (pruebas y medidas) */
    const char *e = getenv("BLADERF_DDC_GPU");
    h->gpu_enabled = !(e && e[0] == '0');
    return h;
}

static void broadcast_list(dec_host_t *h, int client)
{
    if (!h->srv)
        return;
    sb_t sb = {0};
    dec_host_list_json(h, &sb);
    server_send_text(h->srv, client, sb.data, sb.len);
    sb_free(&sb);
}

static void inst_free(inst_t *in)
{
    pthread_mutex_lock(&in->m);
    in->stop = true;
    pthread_cond_signal(&in->cv);
    pthread_mutex_unlock(&in->m);
    if (in->started)
        pthread_join(in->th, NULL);
    free(in->opts);
    free(in->last_state);
    sb_free(&in->evsb);
    pthread_mutex_destroy(&in->m);
    pthread_cond_destroy(&in->cv);
    free(in);
}

void dec_host_destroy(dec_host_t *h)
{
    if (!h)
        return;
    if (h->ath_on) { /* antes que las instancias: el hilo auto las crea y quita */
        pthread_mutex_lock(&h->am);
        h->a_stop = true;
        pthread_cond_signal(&h->acv);
        pthread_mutex_unlock(&h->am);
        pthread_join(h->ath, NULL);
    }
    pthread_mutex_destroy(&h->am);
    pthread_cond_destroy(&h->acv);
    pthread_cond_destroy(&h->aidle);
    for (int i = 0; i < MAX_INST; i++)
        if (h->inst[i]) {
            inst_free(h->inst[i]);
            h->inst[i] = NULL;
        }
    for (int i = 0; i < HIST_N; i++)
        free(h->hist[i]);
    for (int i = 0; i < h->npool; i++)
        block_free(h->pool[i]);
    ddc_mgr_destroy(h->mgr); /* tras las instancias: ellas quitan sus canales al salir */
    pthread_mutex_destroy(&h->gpu_lock);
    pthread_mutex_destroy(&h->lock);
    free(h);
}

int dec_host_add(dec_host_t *h, const char *proto, int64_t freq, const char *opts, int owner, char *err,
                 size_t errlen)
{
    const decoder_desc_t *d = decoder_find(proto);
    if (!d) {
        snprintf(err, errlen, "protocolo desconocido: %s", proto ? proto : "(nulo)");
        return -1;
    }
    if (opts && opts[0]) {
        jtok_t t[128];
        int nt = json_parse(opts, strlen(opts), t, 128);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "opts no es un objeto JSON válido");
            return -1;
        }
    }
    if (d->kind == DEC_NARROW && freq <= 0)
        freq = d->freqs[0];
    if (d->kind == DEC_NARROW && freq <= 0) {
        snprintf(err, errlen, "falta la frecuencia de %s", d->name);
        return -1;
    }
    inst_t *in = calloc(1, sizeof *in);
    if (!in) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    in->h = h;
    in->d = d;
    in->owner = owner;
    in->freq = freq;
    in->opts = opts && opts[0] ? strdup(opts) : NULL;
    in->state = ST_WAITING;
    in->cfg_dirty = true;
    in->load_t0 = now_monotonic();
    pthread_mutex_init(&in->m, NULL);
    pthread_cond_init(&in->cv, NULL);

    pthread_mutex_lock(&h->lock);
    int slot = -1;
    for (int i = 0; i < MAX_INST; i++)
        if (!h->inst[i]) {
            slot = i;
            break;
        }
    if (slot < 0) {
        pthread_mutex_unlock(&h->lock);
        snprintf(err, errlen, "demasiadas instancias (máx. %d)", MAX_INST);
        inst_free(in);
        return -1;
    }
    in->id = h->next_id++;
    in->loc_valid = h->loc_valid;
    in->lat = h->lat;
    in->lon = h->lon;
    h->inst[slot] = in;
    pthread_mutex_unlock(&h->lock);
    if (pthread_create(&in->th, NULL, inst_main, in) != 0) {
        pthread_mutex_lock(&h->lock);
        h->inst[slot] = NULL;
        pthread_mutex_unlock(&h->lock);
        snprintf(err, errlen, "no se pudo crear el hilo");
        inst_free(in);
        return -1;
    }
    in->started = true;
    pthread_mutex_lock(&in->m);
    pthread_cond_signal(&in->cv);
    pthread_mutex_unlock(&in->m);
    if (d->kind == DEC_NARROW)
        gpu_kick(h);
    broadcast_list(h, -1);
    return in->id;
}

static inst_t *take_inst(dec_host_t *h, int id)
{
    inst_t *r = NULL;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++)
        if (h->inst[i] && h->inst[i]->id == id) {
            r = h->inst[i];
            h->inst[i] = NULL;
            break;
        }
    pthread_mutex_unlock(&h->lock);
    return r;
}

int dec_host_remove(dec_host_t *h, int id)
{
    inst_t *in = take_inst(h, id);
    if (!in)
        return -1;
    inst_free(in); /* para y espera al hilo, fuera del cerrojo del host */
    broadcast_list(h, -1);
    return 0;
}

void dec_host_remove_owner(dec_host_t *h, int owner)
{
    for (;;) {
        int id = 0;
        pthread_mutex_lock(&h->lock);
        for (int i = 0; i < MAX_INST; i++)
            if (h->inst[i] && h->inst[i]->owner == owner) {
                id = h->inst[i]->id;
                break;
            }
        pthread_mutex_unlock(&h->lock);
        if (!id)
            break;
        dec_host_remove(h, id);
    }
}

int dec_host_retune(dec_host_t *h, int id, int64_t freq)
{
    int rc = -1;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (in && in->id == id) {
            pthread_mutex_lock(&in->m);
            in->freq = freq;
            in->cfg_dirty = true;
            pthread_cond_signal(&in->cv);
            pthread_mutex_unlock(&in->m);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&h->lock);
    if (rc == 0)
        broadcast_list(h, -1);
    return rc;
}

void dec_host_set_location(dec_host_t *h, bool valid, double lat, double lon)
{
    pthread_mutex_lock(&h->lock);
    h->loc_valid = valid;
    h->lat = lat;
    h->lon = lon;
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in)
            continue;
        pthread_mutex_lock(&in->m);
        in->loc_valid = valid;
        in->lat = lat;
        in->lon = lon;
        in->loc_dirty = true;
        pthread_cond_signal(&in->cv);
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
}

bool dec_host_needs_float(dec_host_t *h)
{
    /* Mismo criterio que dec_host_feed(): solo las instancias NARROW que reciben el bloque
     * float (no las que lleva la GPU ni las WIDE, que usan el Q11 crudo) y que están elegibles. */
    bool need = false;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST && !need; i++) {
        inst_t *in = h->inst[i];
        if (!in)
            continue;
        pthread_mutex_lock(&in->m);
        need = !in->on_gpu && in->d->kind == DEC_NARROW && (in->state == ST_OK || in->cfg_dirty);
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
    return need;
}

void dec_host_feed(dec_host_t *h, const int16_t *iq, const cf32 *buf, unsigned n, double fs, int64_t center,
                   double now)
{
    pthread_mutex_lock(&h->lock);
    bool changed = fs != h->fs || center != h->center;
    h->fs = fs;
    h->center = center;

    /* ¿Qué formatos necesita alguien? Solo copiamos lo que se vaya a usar.
     * Las instancias en espera o con error no reciben bloques (salvo que la
     * captura haya cambiado o estén pendientes de configurar). */
    bool need_iq = false, need_buf = false, any_gpu = false;
    int want[MAX_INST], nw = 0;
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in)
            continue;
        pthread_mutex_lock(&in->m);
        if (changed)
            in->cfg_dirty = true;
        bool eligible = in->state == ST_OK || in->cfg_dirty;
        bool gpu = in->on_gpu;
        if (gpu && in->cfg_dirty)
            pthread_cond_signal(&in->cv); /* no recibe bloques que lo despierten */
        pthread_mutex_unlock(&in->m);
        if (gpu) {
            any_gpu = true; /* su DDC lo hace la GPU con el bloque crudo (abajo) */
            continue;
        }
        if (!eligible)
            continue;
        want[nw++] = i;
        if (in->d->kind == DEC_WIDE)
            need_iq = true;
        else
            need_buf = true;
    }
    ddc_mgr_t *gm = any_gpu ? h->mgr : NULL;
    if (nw == 0) {
        pthread_mutex_unlock(&h->lock);
        goto gpu_feed;
    }
    block_t *b = block_get(h, n, need_iq && iq, need_buf && buf);
    if (!b) {
        pthread_mutex_unlock(&h->lock);
        goto gpu_feed;
    }
    b->mid = false;
    b->n = n;
    b->fs = fs;
    b->center = center;
    b->now = now;
    if (need_iq && iq) {
        memcpy(b->iq, iq, sizeof(int16_t) * 2 * (size_t)n);
        b->has_iq = true;
    }
    if (need_buf && buf) {
        memcpy(b->buf, buf, sizeof(cf32) * (size_t)n);
        b->has_buf = true;
    }
    for (int k = 0; k < nw; k++) {
        inst_t *in = h->inst[want[k]];
        pthread_mutex_lock(&in->m);
        if (in->qn < QDEPTH) {
            atomic_fetch_add(&b->refs, 1);
            in->q[(in->qh + in->qn) % QDEPTH] = b;
            in->qn++;
            pthread_cond_signal(&in->cv);
        } else {
            in->drops++;
        }
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
    block_unref(h, b); /* referencia del propio feed */
gpu_feed:
    /* Una sola copia del bloque crudo para todos los canales de la GPU; fuera
     * del cerrojo del host y sin esperar (si el anillo está lleno, se descarta). */
    if (gm && !ddc_mgr_feed(gm, iq, n, fs, center, now))
        atomic_fetch_add(&h->gpu_drops, 1);
}

/* ======================================================================= */
/*  JSON                                                                   */
/* ======================================================================= */
void dec_host_catalog_json(sb_t *sb)
{
    sb_puts(sb, "{\"type\":\"decoders\",\"list\":[");
    for (int i = 0; DECODERS[i]; i++) {
        const decoder_desc_t *d = DECODERS[i];
        const modulator_desc_t *m = modulator_find(d->id);
        if (i)
            sb_puts(sb, ",");
        sb_puts(sb, "{\"id\":");
        sb_json_str(sb, d->id);
        sb_puts(sb, ",\"name\":");
        sb_json_str(sb, d->name);
        sb_puts(sb, ",\"group\":");
        sb_json_str(sb, d->group);
        sb_puts(sb, ",\"desc\":");
        sb_json_str(sb, d->desc);
        sb_printf(sb, ",\"kind\":\"%s\",\"rate\":%.0f,\"bw\":%d,\"wide_rate\":%u,\"wide_min_rate\":%u,\"freqs\":[",
                  d->kind == DEC_WIDE ? "wide" : "narrow", d->rate, d->bw_hz, d->wide_rate, d->wide_min_rate);
        for (int k = 0; k < 4 && d->freqs[k]; k++)
            sb_printf(sb, "%s%lld", k ? "," : "", (long long)d->freqs[k]);
        sb_printf(sb, "],\"tx\":%s,\"air_ok\":%s}", m ? "true" : "false", m && m->air_ok ? "true" : "false");
    }
    sb_puts(sb, "]}");
}

void dec_host_list_json(dec_host_t *h, sb_t *sb)
{
    sb_puts(sb, "{\"type\":\"dec_list\",\"list\":[");
    bool first = true;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in)
            continue;
        pthread_mutex_lock(&in->m);
        const bool susp = h->suspended[0] && in->state != ST_ERROR;
        double rate = in->rate_now > 0 ? in->rate_now : (in->d->kind == DEC_WIDE ? h->fs : in->d->rate);
        sb_printf(sb, "%s{\"inst\":%d,\"proto\":\"%s\",\"freq\":%lld,\"rate\":%.0f,\"owner\":%d,\"state\":\"%s\","
                      "\"err\":",
                  first ? "" : ",", in->id, in->d->id, (long long)in->freq, rate, in->owner,
                  susp ? STATE_NAMES[ST_WAITING] : STATE_NAMES[in->state]);
        sb_json_str(sb, susp ? h->suspended : in->err);
        sb_printf(sb, ",\"events\":%lu,\"drops\":%lu,\"load\":%.3f}", in->events, in->drops, in->load);
        pthread_mutex_unlock(&in->m);
        first = false;
    }
    pthread_mutex_unlock(&h->lock);
    sb_puts(sb, "]}");
}

void dec_host_on_event(dec_host_t *h, void (*fn)(void *, const char *), void *ctx)
{
    pthread_mutex_lock(&h->lock);
    h->ev_fn = fn;
    h->ev_ctx = ctx;
    pthread_mutex_unlock(&h->lock);
}

void dec_host_send_snapshot(dec_host_t *h, int client)
{
    if (!h->srv)
        return;
    broadcast_list(h, client);
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in)
            continue;
        pthread_mutex_lock(&in->m);
        if (in->last_state) {
            sb_t w = {0};
            sb_printf(&w, "{\"type\":\"dec_state\",\"inst\":%d,\"proto\":\"%s\",\"t\":%.3f,\"state\":", in->id,
                      in->d->id, now_wall());
            sb_append(&w, in->last_state, in->last_state_len);
            sb_puts(&w, "}");
            server_send_text(h->srv, client, w.data, w.len);
            sb_free(&w);
        }
        pthread_mutex_unlock(&in->m);
    }
    /* Últimos eventos, marcados como repetición para que la interfaz no los
     * cuente como nuevos (la línea guardada termina en '}'). */
    for (int k = 0; k < h->hist_n; k++) {
        const char *line = h->hist[(h->hist_head + k) % HIST_N];
        size_t len = strlen(line);
        if (len < 2)
            continue;
        sb_t w = {0};
        sb_append(&w, line, len - 1);
        sb_puts(&w, ",\"replay\":true}");
        server_send_text(h->srv, client, w.data, w.len);
        sb_free(&w);
    }
    pthread_mutex_unlock(&h->lock);
}

/* ======================================================================= */
/*  Periódico y extras                                                     */
/* ======================================================================= */
void dec_host_request_state(dec_host_t *h)
{
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in || !in->d->state_json)
            continue;
        pthread_mutex_lock(&in->m);
        in->want_state = true;
        pthread_cond_signal(&in->cv);
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
}

void dec_host_periodic(dec_host_t *h, double now)
{
    bool clients = h->srv && server_client_count(h->srv) > 0;
    double mono = now_monotonic();
    /* Carga de cada instancia: tiempo ocupado / tiempo transcurrido (1 s) */
    if (mono - h->t_load >= 1.0) {
        h->t_load = mono;
        pthread_mutex_lock(&h->lock);
        for (int i = 0; i < MAX_INST; i++) {
            inst_t *in = h->inst[i];
            if (!in)
                continue;
            pthread_mutex_lock(&in->m);
            double el = mono - in->load_t0;
            if (el > 0) {
                in->load = in->busy_acc / el;
                in->busy_acc = 0;
                in->load_t0 = mono;
            }
            pthread_mutex_unlock(&in->m);
        }
        pthread_mutex_unlock(&h->lock);
    }
    if (clients && now - h->t_state >= 1.0) {
        h->t_state = now;
        dec_host_request_state(h);
    }
    if (clients && (now - h->t_list >= 1.0 || (atomic_load(&h->list_dirty) && now - h->t_list >= 0.2))) {
        h->t_list = now;
        atomic_store(&h->list_dirty, 0);
        broadcast_list(h, -1);
    }
}

bool dec_host_flush(dec_host_t *h, double timeout_s)
{
    double t0 = now_monotonic();
    for (;;) {
        bool busy = false;
        pthread_mutex_lock(&h->lock);
        for (int i = 0; i < MAX_INST; i++) {
            inst_t *in = h->inst[i];
            if (!in)
                continue;
            pthread_mutex_lock(&in->m);
            if (in->qn > 0 || in->working || in->cfg_dirty || in->want_state || in->loc_dirty)
                busy = true;
            pthread_mutex_unlock(&in->m);
        }
        pthread_mutex_unlock(&h->lock);
        if (!busy)
            return true;
        if (now_monotonic() - t0 > timeout_s)
            return false;
        usleep(1000);
    }
}

bool dec_host_inst_state(dec_host_t *h, int id, sb_t *sb)
{
    bool ok = false;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in || in->id != id)
            continue;
        pthread_mutex_lock(&in->m);
        if (in->last_state) {
            sb_append(sb, in->last_state, in->last_state_len);
            ok = true;
        }
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
    return ok;
}

bool dec_host_inst_info(dec_host_t *h, int id, char *state, size_t slen, char *err, size_t elen)
{
    bool ok = false;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST; i++) {
        inst_t *in = h->inst[i];
        if (!in || in->id != id)
            continue;
        pthread_mutex_lock(&in->m);
        const bool susp = h->suspended[0] && in->state != ST_ERROR;
        snprintf(state, slen, "%s", susp ? STATE_NAMES[ST_WAITING] : STATE_NAMES[in->state]);
        snprintf(err, elen, "%s", susp ? h->suspended : in->err);
        pthread_mutex_unlock(&in->m);
        ok = true;
    }
    pthread_mutex_unlock(&h->lock);
    return ok;
}

void dec_host_set_suspended(dec_host_t *h, const char *reason)
{
    pthread_mutex_lock(&h->lock);
    snprintf(h->suspended, sizeof h->suspended, "%s", reason ? reason : "");
    pthread_mutex_unlock(&h->lock);
    atomic_store(&h->list_dirty, 1);
}

void dec_host_set_gpu(dec_host_t *h, bool on)
{
    h->gpu_enabled = on;
}

bool dec_host_gpu_info(dec_host_t *h, char *dev, size_t devlen, int *n_ch, unsigned long *blocks,
                       unsigned long *drops, double *gpu_s, double *cpu_s)
{
    pthread_mutex_lock(&h->gpu_lock);
    ddc_mgr_t *m = h->mgr;
    pthread_mutex_unlock(&h->gpu_lock);
    if (!m)
        return false;
    ddc_mgr_stats_t st;
    ddc_mgr_stats(m, &st);
    if (dev)
        snprintf(dev, devlen, "%s", ddc_mgr_device_name(m));
    if (n_ch)
        *n_ch = st.channels;
    if (blocks)
        *blocks = st.blocks;
    if (drops)
        *drops = st.drops + atomic_load(&h->gpu_drops);
    if (gpu_s)
        *gpu_s = st.gpu_s;
    if (cpu_s)
        *cpu_s = st.thread_s;
    return true;
}

/* ======================================================================= */
/*  Instancias automáticas                                                 */
/* ======================================================================= */
/* Tolerancia de frecuencia para decidir que dos detecciones son la misma señal:
 * el detector tiene resolución de un bin y la frecuencia de una portadora
 * deriva (sondas, osciladores baratos), así que 10 kHz como mínimo o media
 * anchura de la señal. 10 kHz < 25 kHz (separación de canales NFM/AIS). */
static double auto_tol_hz(double bw_hz)
{
    return MAX(10e3, 0.5 * bw_hz);
}

/* ¿Existe ya una instancia de ese protocolo (de cualquier dueño) cerca de f? */
static bool auto_exists(dec_host_t *h, const char *proto, int64_t f, double tol)
{
    bool found = false;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST && !found; i++) {
        inst_t *in = h->inst[i];
        if (!in || strcmp(in->d->id, proto) != 0)
            continue;
        pthread_mutex_lock(&in->m);
        found = fabs((double)(in->freq - f)) <= tol;
        pthread_mutex_unlock(&in->m);
    }
    pthread_mutex_unlock(&h->lock);
    return found;
}

static bool auto_inst_alive(dec_host_t *h, int id)
{
    bool r = false;
    pthread_mutex_lock(&h->lock);
    for (int i = 0; i < MAX_INST && !r; i++)
        r = h->inst[i] && h->inst[i]->id == id;
    pthread_mutex_unlock(&h->lock);
    return r;
}

static int auto_cmp_snr(const void *a, const void *b)
{
    float x = ((const dec_sig_t *)a)->snr_db, y = ((const dec_sig_t *)b)->snr_db;
    return (x < y) - (x > y); /* de mayor a menor SNR: si falta sitio, gana la más clara */
}

static void auto_process(dec_host_t *h, dec_sig_t *sg, int ns, double now)
{
    /* 1) Olvidar las que alguien quitó a mano (dec_remove, cambio de perfil).
     * Se comprueba sin el cerrojo auto (toma el del host); 'gen' detecta que
     * dec_host_auto_clear() cambió las reglas mientras tanto. */
    pthread_mutex_lock(&h->am);
    unsigned gen = h->gen;
    auto_track_t cp[DEC_AUTO_MAX];
    int ncp = h->ntrk;
    memcpy(cp, h->atrk, sizeof cp);
    pthread_mutex_unlock(&h->am);
    int keep = 0;
    for (int i = 0; i < ncp; i++)
        if (auto_inst_alive(h, cp[i].inst))
            cp[keep++] = cp[i];
    pthread_mutex_lock(&h->am);
    if (h->gen != gen) {
        pthread_mutex_unlock(&h->am);
        return;
    }
    memcpy(h->atrk, cp, sizeof cp);
    h->ntrk = keep;
    /* 2) Refrescar "visto" y retirar las que llevan demasiado sin señal */
    int drop[DEC_AUTO_MAX], nd = 0;
    for (int i = 0; i < h->ntrk; i++) {
        auto_track_t *t = &h->atrk[i];
        for (int j = 0; j < ns; j++)
            if (fabs((double)(sg[j].freq_hz - t->freq_hz)) <= auto_tol_hz(sg[j].bw_hz)) {
                t->last_seen = now;
                break;
            }
    }
    int k = 0;
    for (int i = 0; i < h->ntrk; i++) {
        if (now - h->atrk[i].last_seen > DEC_AUTO_TIMEOUT_S)
            drop[nd++] = h->atrk[i].inst;
        else
            h->atrk[k++] = h->atrk[i];
    }
    h->ntrk = k;
    auto_rule_t rules[AUTO_RULES];
    int nr = h->nrule;
    memcpy(rules, h->arule, sizeof rules);
    pthread_mutex_unlock(&h->am);
    for (int i = 0; i < nd; i++)
        dec_host_remove(h, drop[i]);

    /* 3) Altas: señales nuevas dentro de la banda de alguna regla */
    qsort(sg, (size_t)ns, sizeof *sg, auto_cmp_snr);
    for (int j = 0; j < ns; j++)
        for (int r = 0; r < nr; r++) {
            if (sg[j].freq_hz < rules[r].lo_hz || sg[j].freq_hz > rules[r].hi_hz)
                continue;
            pthread_mutex_lock(&h->am);
            bool full = h->ntrk >= DEC_AUTO_MAX;
            pthread_mutex_unlock(&h->am);
            if (full)
                return;
            if (auto_exists(h, rules[r].proto, sg[j].freq_hz, auto_tol_hz(sg[j].bw_hz)))
                continue;
            char err[160];
            int id = dec_host_add(h, rules[r].proto, sg[j].freq_hz, NULL, 1, err, sizeof err);
            if (id < 0) {
                LOG_W("auto %s a %.4f MHz: %s", rules[r].proto, sg[j].freq_hz / 1e6, err);
                continue;
            }
            pthread_mutex_lock(&h->am);
            bool ok = h->gen == gen && h->ntrk < DEC_AUTO_MAX;
            if (ok)
                h->atrk[h->ntrk++] = (auto_track_t){id, r, sg[j].freq_hz, now};
            pthread_mutex_unlock(&h->am);
            if (!ok) { /* cambió el perfil mientras se creaba: no dejar huérfanas */
                dec_host_remove(h, id);
                return;
            }
            LOG_I("auto: %s sobre señal de %.4f MHz (inst %d)", rules[r].proto, sg[j].freq_hz / 1e6, id);
        }
}

static void *auto_main(void *arg)
{
    dec_host_t *h = arg;
    pthread_mutex_lock(&h->am);
    for (;;) {
        while (!h->a_pending && !h->a_stop)
            pthread_cond_wait(&h->acv, &h->am);
        if (h->a_stop)
            break;
        dec_sig_t sg[AUTO_SIGS];
        int ns = h->nasig;
        memcpy(sg, h->asig, sizeof(dec_sig_t) * (size_t)ns);
        double now = h->anow;
        h->a_pending = false;
        h->a_busy = true;
        pthread_mutex_unlock(&h->am);
        auto_process(h, sg, ns, now);
        pthread_mutex_lock(&h->am);
        h->a_busy = false;
        pthread_cond_broadcast(&h->aidle);
    }
    pthread_cond_broadcast(&h->aidle);
    pthread_mutex_unlock(&h->am);
    return NULL;
}

int dec_host_auto_add_rule(dec_host_t *h, const char *proto, int64_t lo_hz, int64_t hi_hz)
{
    const decoder_desc_t *d = decoder_find(proto);
    if (!d || d->kind != DEC_NARROW || hi_hz <= lo_hz)
        return -1;
    int rc = -1;
    pthread_mutex_lock(&h->am);
    if (h->nrule < AUTO_RULES) {
        auto_rule_t *r = &h->arule[h->nrule++];
        snprintf(r->proto, sizeof r->proto, "%s", d->id);
        r->lo_hz = lo_hz;
        r->hi_hz = hi_hz;
        rc = 0;
        if (!h->ath_on && pthread_create(&h->ath, NULL, auto_main, h) == 0)
            h->ath_on = true;
        else if (!h->ath_on)
            rc = -1;
    }
    pthread_mutex_unlock(&h->am);
    return rc;
}

void dec_host_auto_clear(dec_host_t *h)
{
    pthread_mutex_lock(&h->am);
    h->nrule = 0;
    h->ntrk = 0;
    h->gen++;
    h->a_pending = false;
    pthread_mutex_unlock(&h->am);
    dec_host_auto_sync(h, 2.0); /* que no quede un lote a medias creando instancias del perfil viejo */
}

void dec_host_auto_signals(dec_host_t *h, const dec_sig_t *sigs, int n, double now)
{
    if (pthread_mutex_trylock(&h->am) != 0)
        return; /* el hilo auto está copiando: se perderá una lista de 0,5 s, llega otra */
    if (h->nrule > 0 && h->ath_on) {
        if (n > AUTO_SIGS)
            n = AUTO_SIGS;
        memcpy(h->asig, sigs, sizeof(dec_sig_t) * (size_t)n);
        h->nasig = n;
        h->anow = now;
        h->a_pending = true;
        pthread_cond_signal(&h->acv);
    }
    pthread_mutex_unlock(&h->am);
}

bool dec_host_auto_sync(dec_host_t *h, double timeout_s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = ts.tv_nsec * 1e-9 + timeout_s;
    ts.tv_sec += (time_t)t;
    ts.tv_nsec = (long)((t - (double)(time_t)t) * 1e9);
    bool ok = true;
    pthread_mutex_lock(&h->am);
    while ((h->a_pending || h->a_busy) && ok)
        ok = pthread_cond_timedwait(&h->aidle, &h->am, &ts) == 0;
    pthread_mutex_unlock(&h->am);
    return ok;
}
