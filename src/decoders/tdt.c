/*
 * tdt.c — Decodificador «tdt»: televisión digital terrestre (DVB-T, 8 MHz).
 *
 * Es el enlace entre el receptor de TV1 (src/tv/dvbt_rx.h: OFDM → Viterbi → RS →
 * paquetes TS de 188 bytes) y el gestor de televisión (src/tv/tv.c: demux, PSI,
 * audio MP2 y envío al navegador).
 *
 *   host (DDC a 64/7 MHz) ─► tdt_process ─► dvbt_rx_process ─► sink ─► tv_ts_in
 *
 * El canal de DVB-T de 8 MHz usa una tasa elemental T = 7/64 µs, es decir
 * fs = 64/7 MHz ≈ 9,142857 Msps, que es la tasa compleja que el host entrega a
 * los decodificadores narrow de este descriptor; el filtro de canal es de 7,61 MHz
 * (portadoras útiles del modo 8k: 6817 · 1,116 kHz ≈ 7,61 MHz).
 *
 * Cada 250 ms se pasan al gestor las estadísticas del receptor (enganche, MER, BER
 * antes de Viterbi y de RS, paquetes RS corregidos o perdidos, tasa de TS) y se emite
 * un evento cuando cambia el enganche.
 *
 * Si todavía no existe src/tv/dvbt_rx.h (lo escribe TV1) se compila un sustituto que
 * rechaza la creación con un mensaje claro: así el resto del árbol sigue compilando.
 */
#include "decoder.h"
#include "../tv/tv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include("../tv/dvbt_rx.h")
#include "../tv/dvbt_rx.h"
#define TDT_HAVE_RX 1
#endif
#endif

#ifndef TDT_HAVE_RX
/* Sustituto mientras no exista el receptor (misma API que AGENTS.md §8c). */
typedef void (*ts_sink_fn)(void *ctx, const uint8_t *pkts, int npkts);
typedef struct {
    bool lock;
    int mode_k, guard_den, qam, cr_num, cr_den, cell_id;
    float cfo_hz, mer_db, snr_db;
    double ber_pre_viterbi, ber_pre_rs;
    unsigned long rs_ok, rs_corrected, rs_failed;
    double ts_bitrate;
} dvbt_rx_stats_t;
typedef struct dvbt_rx dvbt_rx_t;
static dvbt_rx_t *dvbt_rx_create(double rate_in, char *err, size_t errlen)
{
    (void)rate_in;
    snprintf(err, errlen, "receptor DVB-T (src/tv/dvbt_rx.h) todavía no disponible");
    return NULL;
}
static void dvbt_rx_destroy(dvbt_rx_t *r) { (void)r; }
static void dvbt_rx_process(dvbt_rx_t *r, const cf32 *iq, int n, ts_sink_fn sink, void *ctx)
{
    (void)r; (void)iq; (void)n; (void)sink; (void)ctx;
}
static void dvbt_rx_get_stats(dvbt_rx_t *r, dvbt_rx_stats_t *st) { (void)r; memset(st, 0, sizeof *st); }
#endif

typedef struct {
    dvbt_rx_t *rx;
    double t_stats;
    bool last_lock;
    dvbt_rx_stats_t st;
} tdt_t;

static void *tdt_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    tdt_t *w = calloc(1, sizeof *w);
    if (!w) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    w->rx = dvbt_rx_create(cfg->rate, err, errlen);
    if (!w->rx) {
        free(w);
        return NULL;
    }
    return w;
}

static void tdt_destroy(void *st)
{
    tdt_t *w = st;
    if (!w)
        return;
    dvbt_rx_destroy(w->rx);
    free(w);
}

static void ts_sink(void *ctx, const uint8_t *pkts, int npkts)
{
    (void)ctx;
    tv_t *tv = tv_global();
    if (tv)
        tv_ts_in(tv, pkts, npkts);
}

static void publish(tdt_t *w)
{
    dvbt_rx_get_stats(w->rx, &w->st);
    tv_t *tv = tv_global();
    if (!tv)
        return;
    tv_rx_stats_t s;
    s.lock = w->st.lock;
    s.mode_k = w->st.mode_k;
    s.guard_den = w->st.guard_den;
    s.qam = w->st.qam;
    s.cr_num = w->st.cr_num;
    s.cr_den = w->st.cr_den;
    s.cell_id = w->st.cell_id;
    s.cfo_hz = w->st.cfo_hz;
    s.mer_db = w->st.mer_db;
    s.snr_db = w->st.snr_db;
    s.ber_pre_viterbi = w->st.ber_pre_viterbi;
    s.ber_pre_rs = w->st.ber_pre_rs;
    s.rs_ok = w->st.rs_ok;
    s.rs_corrected = w->st.rs_corrected;
    s.rs_failed = w->st.rs_failed;
    s.ts_bitrate = w->st.ts_bitrate;
    tv_set_rx_stats(tv, &s);
}

static void tdt_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    tdt_t *w = st;
    dvbt_rx_process(w->rx, iq, n, ts_sink, w);
    if (now - w->t_stats < 0.25 && w->t_stats > 0 && now >= w->t_stats)
        return;
    w->t_stats = now;
    publish(w);
    if (w->st.lock != w->last_lock) {
        w->last_lock = w->st.lock;
        char js[300];
        int l;
        if (w->st.lock)
            l = snprintf(js, sizeof js,
                         "{\"text\":\"TDT enganchada: %dk, %d-QAM, FEC %d/%d, MER %.1f dB\",\"lock\":true,\"mer_db\":%.1f}",
                         w->st.mode_k, w->st.qam, w->st.cr_num, w->st.cr_den, (double)w->st.mer_db, (double)w->st.mer_db);
        else
            l = snprintf(js, sizeof js, "{\"text\":\"TDT: señal perdida\",\"lock\":false}");
        emit(ctx, js, (size_t)l);
    }
}

static void tdt_state_json(void *st, sb_t *sb, double now)
{
    (void)now;
    tdt_t *w = st;
    dvbt_rx_get_stats(w->rx, &w->st);
    sb_printf(sb, "{\"lock\":%s,\"mer_db\":%.1f,\"snr_db\":%.1f,\"qam\":%d,\"rs_ok\":%lu,\"rs_failed\":%lu,\"ts_bitrate\":%.0f}",
              w->st.lock ? "true" : "false", (double)w->st.mer_db, (double)w->st.snr_db, w->st.qam, w->st.rs_ok,
              w->st.rs_failed, w->st.ts_bitrate);
}

const decoder_desc_t DEC_TDT = {
    .id = "tdt",
    .name = "TDT (vídeo y audio)",
    .group = "Televisión",
    .desc = "Recibe un múltiplex DVB-T completo y lo entrega a la pestaña TV (vídeo H.264, audio MP2).",
    .kind = DEC_NARROW,
    .rate = 64e6 / 7.0,
    .bw_hz = 7610000,
    .freqs = {474000000LL, 498000000LL, 530000000LL, 770000000LL},
    .create = tdt_create,
    .destroy = tdt_destroy,
    .process = tdt_process,
    .state_json = tdt_state_json,
};
