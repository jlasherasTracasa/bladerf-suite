/*
 * tv.c — Gestor de televisión (ver tv.h).
 *
 * Detalles que importan:
 *
 *  - Tiempo. Los PTS de 33 bits dan la vuelta cada 26,5 h; se «desenrollan» a 64
 *    bits sumando ±2^33 cuando el salto es mayor que medio ciclo. Si el reloj salta
 *    de verdad (fin del bucle de un fichero, cambio de programa) se abre una época
 *    nueva con un desplazamiento común a audio y vídeo que garantiza que el tiempo
 *    nunca retrocede: el navegador no tiene que tratar discontinuidades.
 *  - Audio. MP2 se decodifica aquí (mp2.c); un PES puede llevar varias tramas de
 *    1152 muestras (24 ms a 48 kHz) y una trama puede partirse entre PES. El PTS de
 *    cada trama sale de un reloj propio que avanza 1152/fs por trama y solo se
 *    reajusta con el PTS del PES si difiere más de 40 ms.
 *  - Vídeo. H.264 se corta en unidades de acceso (pes.c). Antes de mandar un
 *    fotograma clave se garantiza que lleva SPS y PPS (se guardan los últimos): el
 *    VideoDecoder del navegador los necesita en el flujo (Annex B sin «description»).
 *    No se emite nada hasta el primer fotograma clave.
 *  - Exploración (tv_scan): recorre los canales sintonizando el hardware, espera el
 *    enganche del receptor y a que lleguen PAT/PMT/SDT, y anota el resultado.
 */
#include "tv.h"

#include "../common.h"
#include "../util/strbuf.h"
#include "mp2.h"
#include "pes.h"
#include "psi.h"
#include "ts.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PTS_WRAP (1LL << 33)
#define MAX_FOUND 32

typedef struct {
    int64_t last_raw, unw, last_out;
    bool init, have_out;
    unsigned epoch;
} clk_t;

typedef struct {
    int64_t freq;
    int ch;
    float mer;
    int nsvc;
    char names[8][96];
    char net[64];
} found_t;

struct tv {
    server_t *srv;
    tv_ops_t ops;
    pthread_mutex_t mu;
    tv_sink_fn sink;
    void *sink_ctx;

    /* pipeline */
    ts_demux_t *dmx;
    psi_t *psi;
    pes_asm_t *vpes, *apes;
    h264_split_t *split;
    mp2_t *mp2;

    /* servicio activo */
    int want_sid;   /* −1 = ninguno */
    int cur_sid;    /* activado (PMT conocida) o −1 */
    int vpid, apid, vcodec;
    bool audio_ok;  /* hay audio MP2 decodificable */
    int acodec;

    /* hardware */
    int64_t freq;
    int inst;
    tv_rx_stats_t rx;
    bool have_rx;
    double rx_t;
    bool file_mode;

    /* tiempo */
    unsigned epoch, shift_epoch;
    int64_t shift, max_out;
    clk_t vclk, aclk;
    double a_clock;
    bool a_init;
    int64_t last_vpts;

    /* vídeo */
    uint8_t *sps, *pps;
    size_t sps_len, pps_len;
    bool got_key;
    uint8_t vseq;
    uint8_t *msg;
    size_t msg_cap;
    int16_t pcm2[1152 * 2];

    /* contadores */
    unsigned long vframes, kframes, aframes, pkts;
    unsigned long bytes_win;
    double win_t, bitrate;
    unsigned long v_bytes_win;

    /* hilos */
    pthread_t pub_th, file_th, scan_th;
    atomic_bool pub_run, file_run, scan_run; /* los leen sus hilos sin cerrojo */
    bool scan_started, file_started;
    char file_path[600];

    /* exploración */
    bool scanning;
    int scan_pos, scan_total, scan_ch;
    found_t found[MAX_FOUND];
    int nfound;
    int64_t scan_start, scan_stop;
};

static tv_t *g_tv;
void tv_set_global(tv_t *t) { g_tv = t; }
tv_t *tv_global(void) { return g_tv; }

int64_t tv_channel_freq(int ch) { return (ch >= 21 && ch <= 69) ? (306 + 8 * (int64_t)ch) * 1000000LL : 0; }
int tv_freq_channel(int64_t hz)
{
    int64_t mhz = (hz + 500000) / 1000000;
    if (mhz < 306 + 8 * 21 - 4 || mhz > 306 + 8 * 69 + 4)
        return 0;
    return (int)((mhz - 306 + 4) / 8);
}

/* ---- envío ---- */
static void emit_text(tv_t *t, int client, const char *s, size_t n)
{
    if (t->srv)
        server_send_text(t->srv, client, s, n);
    if (t->sink)
        t->sink(t->sink_ctx, 0, s, n);
}
static void emit_bin(tv_t *t, int kind, const void *d, size_t n, bool droppable)
{
    if (t->srv)
        server_send_binary(t->srv, -1, d, n, droppable);
    if (t->sink)
        t->sink(t->sink_ctx, kind, d, n);
}

static uint8_t *msg_buf(tv_t *t, size_t n)
{
    if (n > t->msg_cap) {
        size_t nc = n * 2;
        uint8_t *nb = realloc(t->msg, nc);
        if (!nb)
            return NULL;
        t->msg = nb;
        t->msg_cap = nc;
    }
    return t->msg;
}

static void put_i64(uint8_t *p, int64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((uint64_t)v >> (8 * i));
}

/* ---- tiempo ---- */
static int64_t clk_conv(tv_t *t, clk_t *c, int64_t raw)
{
    if (c->epoch != t->epoch) {
        c->epoch = t->epoch;
        c->init = false;
        c->have_out = false;
    }
    if (!c->init) {
        c->unw = raw;
        c->init = true;
    } else {
        int64_t d = raw - c->last_raw;
        if (d > PTS_WRAP / 2)
            d -= PTS_WRAP;
        else if (d < -PTS_WRAP / 2)
            d += PTS_WRAP;
        c->unw += d;
    }
    c->last_raw = raw;
    int64_t out = c->unw + t->shift;
    if (c->have_out && (out - c->last_out < -180000 || out - c->last_out > 900000) && t->shift_epoch == t->epoch) {
        /* salto real del reloj: época nueva, el tiempo sigue creciendo */
        t->epoch++;
        c->epoch = t->epoch;
        c->init = true;
        c->unw = raw;
        c->have_out = false;
    }
    if (t->shift_epoch != t->epoch) {
        t->shift = t->max_out + 3600 - c->unw;
        t->shift_epoch = t->epoch;
        out = c->unw + t->shift;
    }
    c->last_out = out;
    c->have_out = true;
    if (out > t->max_out)
        t->max_out = out;
    return out;
}

/* ---- PES de audio ---- */
static void on_pcm(void *ctx, const int16_t *pcm, int n, int ch, int rate)
{
    tv_t *t = ctx;
    const int16_t *src = pcm;
    if (ch == 1) { /* mono → estéreo */
        for (int i = 0; i < n; i++)
            t->pcm2[2 * i] = t->pcm2[2 * i + 1] = pcm[i];
        src = t->pcm2;
    }
    size_t bytes = (size_t)n * 2 * sizeof(int16_t);
    uint8_t *m = msg_buf(t, 16 + bytes);
    if (!m)
        return;
    memset(m, 0, 16);
    m[0] = 6;
    m[1] = 2;
    m[4] = (uint8_t)rate;
    m[5] = (uint8_t)(rate >> 8);
    m[6] = (uint8_t)(rate >> 16);
    m[7] = (uint8_t)(rate >> 24);
    put_i64(m + 8, (int64_t)t->a_clock);
    memcpy(m + 16, src, bytes); /* little-endian en x86/ARM: el mismo orden que el protocolo */
    emit_bin(t, 6, m, 16 + bytes, true);
    t->a_clock += (double)n * 90000.0 / rate;
    t->aframes++;
}

static void audio_pes(void *ctx, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    (void)dts;
    tv_t *t = ctx;
    if (pts != PTS_NONE) {
        int64_t out = clk_conv(t, &t->aclk, pts);
        if (!t->a_init || t->a_clock - (double)out > 3600 || (double)out - t->a_clock > 3600) {
            t->a_clock = (double)out;
            t->a_init = true;
        }
    }
    if (!t->a_init)
        return;
    mp2_decode(t->mp2, es, len, on_pcm, t);
}

/* ---- vídeo ---- */
static void keep_nal(uint8_t **dst, size_t *dl, const uint8_t *nal, size_t n)
{
    uint8_t *b = realloc(*dst, n + 4);
    if (!b)
        return;
    b[0] = b[1] = b[2] = 0;
    b[3] = 1;
    memcpy(b + 4, nal, n);
    *dst = b;
    *dl = n + 4;
}

static void send_video(tv_t *t, const uint8_t *d, size_t n, bool key, int codec, int64_t pts, int64_t dts)
{
    size_t extra = key ? t->sps_len + t->pps_len : 0;
    uint8_t *m = msg_buf(t, 20 + extra + n);
    if (!m)
        return;
    memset(m, 0, 20);
    m[0] = 5;
    m[1] = key ? 1 : 0;
    m[2] = (uint8_t)codec;
    m[3] = t->vseq++;
    put_i64(m + 4, pts);
    put_i64(m + 12, dts);
    size_t o = 20;
    if (extra) {
        memcpy(m + o, t->sps, t->sps_len);
        o += t->sps_len;
        memcpy(m + o, t->pps, t->pps_len);
        o += t->pps_len;
    }
    memcpy(m + o, d, n);
    /* los fotogramas clave nunca se descartan; los demás sí si el cliente va atrasado
     * (el hueco se nota en m[3] y el navegador espera al siguiente clave) */
    emit_bin(t, 5, m, o + n, !key);
    t->vframes++;
    if (key)
        t->kframes++;
    t->v_bytes_win += n;
}

static void h264_au(void *ctx, const h264_au_t *au)
{
    tv_t *t = ctx;
    size_t nl;
    const uint8_t *sps = annexb_find_nal(au->data, au->len, 7, &nl);
    if (sps) {
        keep_nal(&t->sps, &t->sps_len, sps, nl);
        int w, h;
        if (h264_sps_size(sps, nl, &w, &h)) {
            psi_service_t *sv = psi_find_mut(t->psi, t->cur_sid);
            if (sv)
                for (int i = 0; i < sv->nes; i++)
                    if (sv->es[i].pid == t->vpid) {
                        sv->es[i].w = w;
                        sv->es[i].h = h;
                    }
        }
    }
    const uint8_t *pps = annexb_find_nal(au->data, au->len, 8, &nl);
    if (pps)
        keep_nal(&t->pps, &t->pps_len, pps, nl);
    bool key = au->key;
    if (key && !t->sps_len)
        return; /* sin SPS no se puede arrancar */
    if (!t->got_key) {
        if (!key)
            return;
        t->got_key = true;
    }
    int64_t pts = au->pts, dts = au->dts;
    if (pts == PTS_NONE)
        pts = t->last_vpts + 3600; /* AU sin PTS (varias en un PES): se supone 25 fps */
    if (dts == PTS_NONE)
        dts = pts;
    int64_t opts = clk_conv(t, &t->vclk, pts);
    int64_t odts = opts - (pts - dts);
    t->last_vpts = pts;
    /* has_sps && PPS en la AU: no hace falta anteponerlos */
    bool need = key && !au->has_sps;
    if (need)
        send_video(t, au->data, au->len, true, CODEC_H264, opts, odts);
    else {
        size_t save_s = t->sps_len, save_p = t->pps_len;
        t->sps_len = t->pps_len = 0; /* ya van dentro */
        send_video(t, au->data, au->len, key, CODEC_H264, opts, odts);
        t->sps_len = save_s;
        t->pps_len = save_p;
    }
}

/* MPEG-2 y HEVC: un PES = una AU; la clave se detecta de forma simple. */
static void other_video_pes(void *ctx, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    tv_t *t = ctx;
    if (pts == PTS_NONE)
        return;
    bool key = false;
    if (t->vcodec == CODEC_MPEG2V) {
        for (size_t i = 0; i + 4 <= len && !key; i++)
            if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1 && es[i + 3] == 0xB3)
                key = true; /* sequence_header */
    } else {
        for (size_t i = 0; i + 4 <= len && !key; i++)
            if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
                int ty = (es[i + 3] >> 1) & 0x3F;
                if (ty >= 16 && ty <= 21)
                    key = true; /* IRAP */
            }
    }
    if (!t->got_key) {
        if (!key)
            return;
        t->got_key = true;
    }
    if (dts == PTS_NONE)
        dts = pts;
    int64_t opts = clk_conv(t, &t->vclk, pts);
    size_t s = t->sps_len, p = t->pps_len;
    t->sps_len = t->pps_len = 0;
    send_video(t, es, len, key, t->vcodec, opts, opts - (pts - dts));
    t->sps_len = s;
    t->pps_len = p;
}

static void video_pes(void *ctx, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    tv_t *t = ctx;
    if (t->vcodec == CODEC_H264)
        h264_split_feed(t->split, es, len, pts, dts);
    else
        other_video_pes(t, es, len, pts, dts);
}

/* ---- servicio activo ---- */
static void pipeline_reset(tv_t *t)
{
    pes_reset(t->vpes);
    pes_reset(t->apes);
    h264_split_reset(t->split);
    mp2_reset(t->mp2);
    t->got_key = false;
    t->a_init = false;
}

static void deactivate(tv_t *t)
{
    t->cur_sid = -1;
    t->vpid = t->apid = -1;
    t->audio_ok = false;
    pipeline_reset(t);
}

static void try_activate(tv_t *t)
{
    if (t->want_sid < 0 || t->cur_sid == t->want_sid)
        return;
    const psi_service_t *s = psi_find(t->psi, t->want_sid);
    if (!s || !s->have_pmt)
        return;
    int v = -1, a = -1, vc = 0, ac = 0, anyaud = -1;
    for (int i = 0; i < s->nes; i++) {
        const psi_es_t *e = &s->es[i];
        if (e->kind == ES_VIDEO && v < 0) {
            v = e->pid;
            vc = e->codec;
        } else if (e->kind == ES_AUDIO) {
            if (anyaud < 0)
                anyaud = e->pid;
            if (e->codec == CODEC_MP2 && a < 0) {
                a = e->pid;
                ac = e->codec;
            }
        }
    }
    deactivate(t);
    t->cur_sid = t->want_sid;
    t->vpid = v;
    t->vcodec = vc;
    t->apid = a;
    t->acodec = ac;
    t->audio_ok = a >= 0;
    LOG_I("tv: servicio %d (%s): vídeo PID %d, audio PID %d%s", s->sid, s->name, v, a,
          (a < 0 && anyaud >= 0) ? " (el audio de este servicio no es MP2: sin sonido)" : "");
}

/* ---- entrada de paquetes ---- */
static void on_pkt(void *ctx, const ts_pkt_t *k)
{
    tv_t *t = ctx;
    t->pkts++;
    if (psi_wants_pid(t->psi, k->pid))
        psi_feed(t->psi, k);
    if (t->want_sid >= 0 && t->cur_sid != t->want_sid)
        try_activate(t);
    if (t->cur_sid < 0)
        return;
    if (k->pid == t->vpid)
        pes_feed(t->vpes, k);
    else if (k->pid == t->apid)
        pes_feed(t->apes, k);
}

void tv_ts_in(tv_t *t, const uint8_t *pkts, int npkts)
{
    pthread_mutex_lock(&t->mu);
    t->bytes_win += (unsigned long)npkts * TS_PKT;
    ts_demux_feed(t->dmx, pkts, (size_t)npkts * TS_PKT);
    pthread_mutex_unlock(&t->mu);
}

void tv_set_rx_stats(tv_t *t, const tv_rx_stats_t *st)
{
    pthread_mutex_lock(&t->mu);
    t->rx = *st;
    t->have_rx = true;
    t->rx_t = now_monotonic();
    pthread_mutex_unlock(&t->mu);
}

void tv_set_sink(tv_t *t, tv_sink_fn fn, void *ctx)
{
    pthread_mutex_lock(&t->mu);
    t->sink = fn;
    t->sink_ctx = ctx;
    pthread_mutex_unlock(&t->mu);
}

void tv_counters(tv_t *t, tv_counters_t *c)
{
    pthread_mutex_lock(&t->mu);
    c->video_frames = t->vframes;
    c->key_frames = t->kframes;
    c->audio_frames = t->aframes;
    c->ts_packets = t->pkts;
    c->cur_sid = t->cur_sid;
    pthread_mutex_unlock(&t->mu);
}

/* ======================================================================= */
/*  JSON                                                                   */
/* ======================================================================= */
static const char *codec_name(int c)
{
    switch (c) {
    case CODEC_H264: return "h264";
    case CODEC_MPEG2V: return "mpeg2";
    case CODEC_HEVC: return "hevc";
    case CODEC_MP2: return "mp2";
    case CODEC_AAC: return "aac";
    case CODEC_AC3: return "ac3";
    case CODEC_EAC3: return "eac3";
    case CODEC_SUB: return "sub";
    case CODEC_TXT: return "txt";
    default: return "otro";
    }
}

static void json_services(const psi_t *p, sb_t *sb)
{
    sb_puts(sb, "[");
    bool first = true;
    for (int i = 0; i < psi_nservices(p); i++) {
        const psi_service_t *s = psi_service(p, i);
        if (!s->pmt_pid) /* anunciado solo en el SDT de otro múltiplex o sin PAT */
            continue;
        const psi_es_t *v = NULL;
        for (int k = 0; k < s->nes && !v; k++)
            if (s->es[k].kind == ES_VIDEO)
                v = &s->es[k];
        sb_puts(sb, first ? "{" : ",{");
        first = false;
        sb_printf(sb, "\"sid\":%d,\"name\":", s->sid);
        if (s->name[0])
            sb_json_str(sb, s->name);
        else {
            char tmp[32];
            snprintf(tmp, sizeof tmp, "Servicio %d", s->sid);
            sb_json_str(sb, tmp);
        }
        sb_puts(sb, ",\"provider\":");
        sb_json_str(sb, s->provider);
        sb_printf(sb, ",\"type\":%d,\"lcn\":%d,\"ca\":%s,\"pmt\":%s", s->svc_type, s->lcn, s->ca ? "true" : "false",
                  s->have_pmt ? "true" : "false");
        if (v)
            sb_printf(sb, ",\"video\":{\"pid\":%d,\"codec\":\"%s\",\"w\":%d,\"h\":%d}", v->pid, codec_name(v->codec), v->w,
                      v->h);
        else
            sb_puts(sb, ",\"video\":null");
        sb_puts(sb, ",\"audio\":[");
        bool fa = true;
        for (int k = 0; k < s->nes; k++)
            if (s->es[k].kind == ES_AUDIO) {
                sb_printf(sb, "%s{\"pid\":%d,\"codec\":\"%s\",\"lang\":", fa ? "" : ",", s->es[k].pid,
                          codec_name(s->es[k].codec));
                sb_json_str(sb, s->es[k].lang);
                sb_puts(sb, "}");
                fa = false;
            }
        sb_puts(sb, "]}");
    }
    sb_puts(sb, "]");
}

void tv_json(tv_t *t, sb_t *sb)
{
    sb_clear(sb);
    sb_printf(sb, "{\"type\":\"tv\",\"freq\":%lld,\"ch\":%d,\"file\":%s,\"lock\":%s,", (long long)t->freq,
              tv_freq_channel(t->freq), t->file_mode ? "true" : "false",
              (t->file_mode || (t->have_rx && t->rx.lock)) ? "true" : "false");
    const tv_rx_stats_t *r = &t->rx;
    sb_puts(sb, "\"stats\":{");
    sb_printf(sb, "\"lock\":%s,\"mode_k\":%d,\"guard_den\":%d,\"qam\":%d,\"cr_num\":%d,\"cr_den\":%d,\"cell_id\":%d,",
              r->lock ? "true" : "false", r->mode_k, r->guard_den, r->qam, r->cr_num, r->cr_den, r->cell_id);
    sb_puts(sb, "\"cfo_hz\":");
    sb_json_num(sb, r->cfo_hz, 1);
    sb_puts(sb, ",\"mer_db\":");
    sb_json_num(sb, r->mer_db, 1);
    sb_puts(sb, ",\"snr_db\":");
    sb_json_num(sb, r->snr_db, 1);
    sb_printf(sb, ",\"ber_pre_viterbi\":%.3g,\"ber_pre_rs\":%.3g,\"rs_ok\":%lu,\"rs_corrected\":%lu,\"rs_failed\":%lu,",
              r->ber_pre_viterbi, r->ber_pre_rs, r->rs_ok, r->rs_corrected, r->rs_failed);
    sb_puts(sb, "\"ts_bitrate\":");
    sb_json_num(sb, t->bitrate > 0 ? t->bitrate : r->ts_bitrate, 0);
    sb_puts(sb, "},");
    ts_stats_t ts;
    ts_demux_stats(t->dmx, &ts);
    sb_printf(sb, "\"ts\":{\"packets\":%lu,\"cc_errors\":%lu,\"tei\":%lu,\"sync_losses\":%lu,\"video_frames\":%lu,"
                  "\"key_frames\":%lu,\"audio_frames\":%lu,\"pes_dropped\":%lu},",
              ts.packets, ts.cc_errors, ts.tei, ts.sync_losses, t->vframes, t->kframes, t->aframes,
              pes_dropped(t->vpes) + pes_dropped(t->apes));
    sb_puts(sb, "\"network\":");
    sb_json_str(sb, psi_network_name(t->psi));
    sb_puts(sb, ",\"services\":");
    json_services(t->psi, sb);
    if (t->cur_sid >= 0 || t->want_sid >= 0)
        sb_printf(sb, ",\"current\":%d", t->want_sid >= 0 ? t->want_sid : t->cur_sid);
    else
        sb_puts(sb, ",\"current\":null");
    sb_printf(sb, ",\"audio_ok\":%s,\"scanning\":%s}", t->audio_ok ? "true" : "false", t->scanning ? "true" : "false");
}

static void json_scan(tv_t *t, sb_t *sb, bool done)
{
    sb_clear(sb);
    sb_printf(sb, "{\"type\":\"tv_scan\",\"running\":%s,\"done\":%s,\"pos\":%d,\"total\":%d,\"ch\":%d,\"freq\":%lld,\"found\":[",
              t->scanning ? "true" : "false", done ? "true" : "false", t->scan_pos, t->scan_total, t->scan_ch,
              (long long)tv_channel_freq(t->scan_ch));
    for (int i = 0; i < t->nfound; i++) {
        const found_t *f = &t->found[i];
        sb_printf(sb, "%s{\"freq\":%lld,\"ch\":%d,\"mer_db\":", i ? "," : "", (long long)f->freq, f->ch);
        sb_json_num(sb, f->mer, 1);
        sb_printf(sb, ",\"n\":%d,\"network\":", f->nsvc);
        sb_json_str(sb, f->net);
        sb_puts(sb, ",\"names\":[");
        for (int k = 0; k < f->nsvc && k < 8; k++) {
            if (k)
                sb_puts(sb, ",");
            sb_json_str(sb, f->names[k]);
        }
        sb_puts(sb, "]}");
    }
    sb_puts(sb, "]}");
}

void tv_send_snapshot(tv_t *t, int client)
{
    sb_t sb = {0};
    pthread_mutex_lock(&t->mu);
    tv_json(t, &sb);
    emit_text(t, client, sb.data, sb.len);
    if (t->nfound || t->scanning) {
        json_scan(t, &sb, !t->scanning);
        emit_text(t, client, sb.data, sb.len);
    }
    pthread_mutex_unlock(&t->mu);
    sb_free(&sb);
}

/* Hilo de publicación: 1 Hz, y cálculo de la tasa de TS medida. */
static void *pub_main(void *arg)
{
    tv_t *t = arg;
    sb_t sb = {0};
    while (t->pub_run) {
        for (int i = 0; i < 10 && t->pub_run; i++)
            usleep(100000);
        pthread_mutex_lock(&t->mu);
        double now = now_monotonic();
        if (t->win_t > 0 && now > t->win_t) {
            double dt = now - t->win_t;
            t->bitrate = 0.5 * t->bitrate + 0.5 * (double)t->bytes_win * 8.0 / dt;
            if (t->bitrate < 1)
                t->bitrate = 0;
        }
        t->win_t = now;
        t->bytes_win = 0;
        tv_json(t, &sb);
        emit_text(t, -1, sb.data, sb.len);
        pthread_mutex_unlock(&t->mu);
    }
    sb_free(&sb);
    return NULL;
}

/* ======================================================================= */
/*  Ciclo de vida                                                          */
/* ======================================================================= */
tv_t *tv_create(server_t *srv, const tv_ops_t *ops)
{
    tv_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->srv = srv;
    if (ops)
        t->ops = *ops;
    pthread_mutex_init(&t->mu, NULL);
    t->dmx = ts_demux_create(on_pkt, t);
    t->psi = psi_create();
    t->vpes = pes_create(video_pes, t);
    t->apes = pes_create(audio_pes, t);
    t->split = h264_split_create(h264_au, t);
    t->mp2 = mp2_create();
    if (!t->dmx || !t->psi || !t->vpes || !t->apes || !t->split || !t->mp2) {
        tv_destroy(t);
        return NULL;
    }
    t->want_sid = t->cur_sid = -1;
    t->vpid = t->apid = -1;
    t->inst = -1;
    t->pub_run = true;
    if (pthread_create(&t->pub_th, NULL, pub_main, t) != 0) {
        t->pub_run = false;
        tv_destroy(t);
        return NULL;
    }
    return t;
}

static void *file_main(void *arg);

void tv_quiesce(tv_t *t)
{
    t->file_run = false;
    t->scan_run = false;
    if (t->file_started) {
        pthread_join(t->file_th, NULL);
        t->file_started = false;
    }
    if (t->scan_started) {
        pthread_join(t->scan_th, NULL);
        t->scan_started = false;
    }
}

void tv_destroy(tv_t *t)
{
    if (!t)
        return;
    if (g_tv == t)
        g_tv = NULL;
    if (t->pub_run) {
        t->pub_run = false;
        pthread_join(t->pub_th, NULL);
    }
    tv_quiesce(t);
    ts_demux_destroy(t->dmx);
    psi_destroy(t->psi);
    pes_destroy(t->vpes);
    pes_destroy(t->apes);
    h264_split_destroy(t->split);
    mp2_destroy(t->mp2);
    free(t->sps);
    free(t->pps);
    free(t->msg);
    pthread_mutex_destroy(&t->mu);
    free(t);
}

/* Descarta todo lo aprendido del múltiplex anterior (cambio de frecuencia). */
static void reset_mux_locked(tv_t *t)
{
    ts_demux_reset(t->dmx);
    psi_reset(t->psi);
    deactivate(t);
    t->want_sid = -1;
    free(t->sps);
    free(t->pps);
    t->sps = t->pps = NULL;
    t->sps_len = t->pps_len = 0;
    t->epoch++;
    t->bitrate = 0;
    t->have_rx = false;
    memset(&t->rx, 0, sizeof t->rx);
}

/* ======================================================================= */
/*  Órdenes                                                                */
/* ======================================================================= */
int tv_cmd_tune(tv_t *t, int64_t freq_hz, int sid, char *err, size_t errlen)
{
    if (!t->ops.tune) {
        snprintf(err, errlen, "tv: sin acceso al receptor");
        return -1;
    }
    pthread_mutex_lock(&t->mu);
    if (t->file_mode) {
        pthread_mutex_unlock(&t->mu);
        snprintf(err, errlen, "tv: reproduciendo un fichero .ts (--tv-ts); no se sintoniza");
        return -1;
    }
    if (t->scanning) {
        pthread_mutex_unlock(&t->mu);
        snprintf(err, errlen, "tv: exploración en curso");
        return -1;
    }
    int old = t->inst;
    t->inst = -1;
    reset_mux_locked(t);
    t->freq = freq_hz;
    pthread_mutex_unlock(&t->mu);
    if (old > 0 && t->ops.untune)
        t->ops.untune(t->ops.app, old);
    int inst = t->ops.tune(t->ops.app, freq_hz, err, errlen);
    if (inst < 0)
        return -1;
    pthread_mutex_lock(&t->mu);
    t->inst = inst;
    t->want_sid = sid > 0 ? sid : -1;
    pthread_mutex_unlock(&t->mu);
    return 0;
}

int tv_cmd_service(tv_t *t, int sid, char *err, size_t errlen)
{
    pthread_mutex_lock(&t->mu);
    const psi_service_t *s = psi_find(t->psi, sid);
    if (!s || !s->pmt_pid) {
        pthread_mutex_unlock(&t->mu);
        snprintf(err, errlen, "tv_service: servicio %d desconocido", sid);
        return -1;
    }
    if (s->ca) {
        /* cifrado: no se descifra nada; se avisa pero se intenta (a veces es solo un descriptor) */
        LOG_W("tv: el servicio %d declara acceso condicional", sid);
    }
    t->want_sid = sid;
    deactivate(t);
    t->epoch++; /* otro programa: su reloj puede no tener relación con el anterior */
    try_activate(t);
    pthread_mutex_unlock(&t->mu);
    return 0;
}

void tv_cmd_stop(tv_t *t)
{
    pthread_mutex_lock(&t->mu);
    if (t->scanning)
        t->scan_run = false;
    int old = t->file_mode ? -1 : t->inst;
    t->inst = -1;
    t->want_sid = -1;
    deactivate(t);
    if (!t->file_mode)
        reset_mux_locked(t);
    pthread_mutex_unlock(&t->mu);
    if (old > 0 && t->ops.untune)
        t->ops.untune(t->ops.app, old);
}

/* ---- exploración ---- */
static bool wait_cond(tv_t *t, double secs, bool (*cond)(tv_t *))
{
    double t0 = now_monotonic();
    while (t->scan_run && now_monotonic() - t0 < secs) {
        pthread_mutex_lock(&t->mu);
        bool ok = cond(t);
        pthread_mutex_unlock(&t->mu);
        if (ok)
            return true;
        usleep(100000);
    }
    return false;
}
static bool cond_lock(tv_t *t) { return t->have_rx && t->rx.lock; }
static bool cond_names(tv_t *t)
{
    int n = 0, ok = 0;
    for (int i = 0; i < psi_nservices(t->psi); i++) {
        const psi_service_t *s = psi_service(t->psi, i);
        if (!s->pmt_pid)
            continue;
        n++;
        if (s->have_pmt && s->have_sdt)
            ok++;
    }
    return n > 0 && ok == n;
}
static bool cond_any_pat(tv_t *t) { return psi_nservices(t->psi) > 0; }

static void scan_publish(tv_t *t, bool done)
{
    sb_t sb = {0};
    pthread_mutex_lock(&t->mu);
    json_scan(t, &sb, done);
    emit_text(t, -1, sb.data, sb.len);
    pthread_mutex_unlock(&t->mu);
    sb_free(&sb);
}

static void *scan_main(void *arg)
{
    tv_t *t = arg;
    int64_t f0 = t->scan_start, f1 = t->scan_stop;
    int n = 0;
    for (int64_t f = f0; f <= f1; f += 8000000)
        n++;
    pthread_mutex_lock(&t->mu);
    t->scan_total = n;
    t->nfound = 0;
    pthread_mutex_unlock(&t->mu);
    int i = 0;
    for (int64_t f = f0; f <= f1 && t->scan_run; f += 8000000, i++) {
        pthread_mutex_lock(&t->mu);
        t->scan_pos = i;
        t->scan_ch = tv_freq_channel(f);
        pthread_mutex_unlock(&t->mu);
        scan_publish(t, false);
        char err[200] = "";
        pthread_mutex_lock(&t->mu);
        int old = t->inst;
        t->inst = -1;
        reset_mux_locked(t);
        t->freq = f;
        pthread_mutex_unlock(&t->mu);
        if (old > 0 && t->ops.untune)
            t->ops.untune(t->ops.app, old);
        int inst = t->ops.tune(t->ops.app, f, err, sizeof err);
        if (inst < 0) {
            LOG_W("tv_scan: %s", err);
            break;
        }
        pthread_mutex_lock(&t->mu);
        t->inst = inst;
        pthread_mutex_unlock(&t->mu);
        /* el hardware tarda en asentarse y el receptor en sincronizar */
        double lw = t->ops.lock_wait_s > 0 ? t->ops.lock_wait_s : 5.0;
        double pw = t->ops.psi_wait_s > 0 ? t->ops.psi_wait_s : 6.0;
        if (!wait_cond(t, lw, cond_lock))
            continue;
        if (!wait_cond(t, pw, cond_any_pat))
            continue;
        wait_cond(t, pw + 2.0, cond_names);
        pthread_mutex_lock(&t->mu);
        if (t->nfound < MAX_FOUND && psi_nservices(t->psi) > 0) {
            found_t *fd = &t->found[t->nfound++];
            memset(fd, 0, sizeof *fd);
            fd->freq = f;
            fd->ch = tv_freq_channel(f);
            fd->mer = t->rx.mer_db;
            snprintf(fd->net, sizeof fd->net, "%s", psi_network_name(t->psi));
            for (int k = 0; k < psi_nservices(t->psi); k++) {
                const psi_service_t *s = psi_service(t->psi, k);
                if (!s->pmt_pid)
                    continue;
                if (fd->nsvc < 8)
                    snprintf(fd->names[fd->nsvc], sizeof fd->names[0], "%s", s->name[0] ? s->name : "?");
                fd->nsvc++;
            }
        }
        pthread_mutex_unlock(&t->mu);
    }
    pthread_mutex_lock(&t->mu);
    int old = t->inst;
    t->inst = -1;
    t->scanning = false;
    pthread_mutex_unlock(&t->mu);
    if (old > 0 && t->ops.untune)
        t->ops.untune(t->ops.app, old);
    scan_publish(t, true);
    return NULL;
}

int tv_cmd_scan(tv_t *t, int64_t start_hz, int64_t stop_hz, char *err, size_t errlen)
{
    if (!t->ops.tune) {
        snprintf(err, errlen, "tv_scan: sin acceso al receptor");
        return -1;
    }
    if (start_hz <= 0)
        start_hz = tv_channel_freq(21);
    if (stop_hz <= 0 || stop_hz < start_hz)
        stop_hz = tv_channel_freq(48);
    /* Q3 (fuzz): el rango viene del WebSocket sin acotar. Con stop = 9e18 el bucle de
     * scan_main contaba ~10^12 canales y desbordaba el 'int n' (comportamiento
     * indefinido, y un hilo ocupado durante horas). El hardware llega a 6 GHz. */
    if (start_hz > 6000000000LL)
        start_hz = 6000000000LL;
    if (stop_hz > 6000000000LL)
        stop_hz = 6000000000LL;
    pthread_mutex_lock(&t->mu);
    if (t->file_mode || t->scanning) {
        pthread_mutex_unlock(&t->mu);
        snprintf(err, errlen, t->file_mode ? "tv_scan: no hay radio mientras se reproduce un fichero .ts"
                                           : "tv_scan: ya hay una exploración en curso");
        return -1;
    }
    t->scanning = true;
    t->scan_run = true;
    t->scan_start = start_hz;
    t->scan_stop = stop_hz;
    pthread_mutex_unlock(&t->mu);
    if (t->scan_started) { /* hilo anterior ya terminado */
        pthread_join(t->scan_th, NULL);
        t->scan_started = false;
    }
    if (pthread_create(&t->scan_th, NULL, scan_main, t) != 0) {
        t->scanning = false;
        snprintf(err, errlen, "tv_scan: no se pudo crear el hilo");
        return -1;
    }
    t->scan_started = true;
    return 0;
}

/* ======================================================================= */
/*  Fichero .ts en bucle                                                   */
/* ======================================================================= */
#define FILE_CHUNK 64 /* paquetes por llamada */

static void *file_main(void *arg)
{
    tv_t *t = arg;
    FILE *f = fopen(t->file_path, "rb");
    if (!f) {
        LOG_E("tv: no se pudo abrir %s: %s", t->file_path, strerror(errno));
        return NULL;
    }
    uint8_t buf[FILE_CHUNK * TS_PKT];
    int pace_pid = -1;
    double wall0 = 0, pcr0 = 0;
    unsigned long long fed = 0;
    double t_start = now_monotonic();
    while (t->file_run) {
        size_t n = fread(buf, TS_PKT, FILE_CHUNK, f);
        if (n == 0) {
            /* fin: volver al principio con un reloj nuevo */
            rewind(f);
            pthread_mutex_lock(&t->mu);
            t->epoch++;
            ts_demux_reset(t->dmx);
            pipeline_reset(t);
            pthread_mutex_unlock(&t->mu);
            pace_pid = -1;
            fed = 0;
            t_start = now_monotonic();
            continue;
        }
        /* ritmo: por PCR del primer PID que lo lleve; antes de verlo, 15 Mb/s */
        for (size_t i = 0; i < n; i++) {
            ts_pkt_t k;
            if (ts_parse(buf + i * TS_PKT, &k) < 0 || !k.has_pcr)
                continue;
            if (pace_pid < 0) {
                pace_pid = k.pid;
                wall0 = now_monotonic();
                pcr0 = ts_pcr_sec(k.pcr27);
            }
            if (k.pid == pace_pid) {
                double due = wall0 + (ts_pcr_sec(k.pcr27) - pcr0);
                double wait = due - now_monotonic();
                if (wait > 2.0 || wait < -2.0) { /* discontinuidad del PCR: reajustar */
                    wall0 = now_monotonic();
                    pcr0 = ts_pcr_sec(k.pcr27);
                } else if (wait > 0) {
                    tv_ts_in(t, buf, (int)i + 1);
                    usleep((useconds_t)(wait * 1e6));
                    tv_ts_in(t, buf + (i + 1) * TS_PKT, (int)(n - i - 1));
                    fed += n;
                    goto next;
                }
            }
        }
        tv_ts_in(t, buf, (int)n);
        fed += n;
        if (pace_pid < 0) {
            double due = t_start + (double)fed * TS_PKT * 8.0 / 15e6;
            double wait = due - now_monotonic();
            if (wait > 0)
                usleep((useconds_t)(wait * 1e6));
        }
    next:;
    }
    fclose(f);
    return NULL;
}

int tv_play_file(tv_t *t, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "no se pudo abrir %s: %s", path, strerror(errno));
        return -1;
    }
    uint8_t b[TS_PKT];
    bool ok = fread(b, 1, TS_PKT, f) == TS_PKT && b[0] == 0x47;
    fclose(f);
    if (!ok) {
        snprintf(err, errlen, "%s no parece un TS de 188 bytes (falta 0x47)", path);
        return -1;
    }
    snprintf(t->file_path, sizeof t->file_path, "%s", path);
    pthread_mutex_lock(&t->mu);
    t->file_mode = true;
    reset_mux_locked(t);
    pthread_mutex_unlock(&t->mu);
    t->file_run = true;
    if (pthread_create(&t->file_th, NULL, file_main, t) != 0) {
        t->file_run = false;
        snprintf(err, errlen, "no se pudo crear el hilo del fichero");
        return -1;
    }
    t->file_started = true;
    return 0;
}
