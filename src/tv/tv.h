/*
 * tv.h — Gestor de televisión: del TS al navegador (AGENTS.md §8c).
 *
 *   TS (188 B)  ─►  ts_demux ─► PSI/SDT (servicios, nombres)
 *                          └──► PES del servicio elegido
 *                                 ├ vídeo H.264 → unidades de acceso (Annex B) → WS binario tipo 5
 *                                 └ audio MP2   → mp2.c → PCM int16 estéreo      → WS binario tipo 6
 *
 * Las fuentes del TS son dos: el decodificador «tdt» (receptor DVB-T de TV1 sobre
 * el IQ del bladeRF) y un fichero .ts (--tv-ts) para probar el reproductor sin
 * radio; el fichero se reproduce en bucle al ritmo de su PCR.
 *
 * Sincronía A/V: el servidor no retrasa nada; manda cada trama con su PTS (90 kHz,
 * desenrollado a 64 bits y sin saltos atrás) y el navegador usa el audio como
 * reloj y presenta el vídeo según su PTS (web/js/tv.js).
 *
 * Hilos: tv_ts_in() lo llama el hilo del decodificador; las órdenes, el hilo de
 * red. Todo el estado va bajo un cerrojo; el envío al servidor es seguro entre
 * hilos y no vuelve a llamar aquí.
 */
#ifndef BLADERF_LAB_TV_H
#define BLADERF_LAB_TV_H

#include "../net/server.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Espejo de dvbt_rx_stats_t (src/tv/dvbt_rx.h) para no depender de él aquí. */
typedef struct {
    bool lock;
    int mode_k, guard_den, qam, cr_num, cr_den, cell_id;
    float cfo_hz, mer_db, snr_db;
    double ber_pre_viterbi, ber_pre_rs;
    unsigned long rs_ok, rs_corrected, rs_failed;
    double ts_bitrate;
} tv_rx_stats_t;

/* Operaciones del programa principal: sintonizar el hardware y crear/quitar la
 * instancia «tdt» del host. tune devuelve el nº de instancia (>0) o −1 con err. */
typedef struct {
    void *app;
    int (*tune)(void *app, int64_t freq_hz, char *err, size_t errlen);
    void (*untune)(void *app, int inst);
    /* esperas máximas de la exploración (s); 0 = valores por omisión (5 y 6) */
    double lock_wait_s, psi_wait_s;
} tv_ops_t;

typedef struct tv tv_t;

tv_t *tv_create(server_t *srv, const tv_ops_t *ops);
/* Para y une los hilos de exploración y de fichero (antes de destruir el host). */
void tv_quiesce(tv_t *t);
void tv_destroy(tv_t *t);
/* Instancia única usada por el decodificador «tdt» (puede ser NULL en pruebas). */
void tv_set_global(tv_t *t);
tv_t *tv_global(void);

/* ---- entrada ---- */
void tv_ts_in(tv_t *t, const uint8_t *pkts, int npkts);
void tv_set_rx_stats(tv_t *t, const tv_rx_stats_t *st);
/* Reproduce un .ts en bucle como si saliera del receptor. */
int tv_play_file(tv_t *t, const char *path, char *err, size_t errlen);

/* ---- órdenes (hilo de red); las que fallan devuelven < 0 y err ---- */
int tv_cmd_tune(tv_t *t, int64_t freq_hz, int sid, char *err, size_t errlen);
int tv_cmd_service(tv_t *t, int sid, char *err, size_t errlen);
void tv_cmd_stop(tv_t *t);
int tv_cmd_scan(tv_t *t, int64_t start_hz, int64_t stop_hz, char *err, size_t errlen);

/* Mensaje "tv" (y "tv_scan") al conectar un cliente. */
void tv_send_snapshot(tv_t *t, int client);

/* ---- utilidades visibles para pruebas ---- */
/* Canal UHF 21–48 ↔ frecuencia central (8 MHz): 306 + 8·n MHz. */
int64_t tv_channel_freq(int ch);
int tv_freq_channel(int64_t hz);
/* JSON "tv" completo en sb. */
void tv_json(tv_t *t, sb_t *sb);
/* Receptor de todo lo que se enviaría a los clientes (pruebas): kind = 5 vídeo,
 * 6 audio, 0 JSON (texto). Se llama bajo el cerrojo interno. */
typedef void (*tv_sink_fn)(void *ctx, int kind, const void *data, size_t len);
void tv_set_sink(tv_t *t, tv_sink_fn fn, void *ctx);
/* Contadores para pruebas */
typedef struct {
    unsigned long video_frames, key_frames, audio_frames, ts_packets;
    int cur_sid;
} tv_counters_t;
void tv_counters(tv_t *t, tv_counters_t *c);

#endif
