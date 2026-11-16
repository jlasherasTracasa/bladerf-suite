/*
 * tx.h — Emisión de señales con política de seguridad (AGENTS.md §6).
 *
 * Tres modos:
 *   file      Escribe la señal en un SigMF dentro de recordings/ (siempre
 *             permitido). Sirve para probar de extremo a extremo con --replay.
 *   loopback  Con --enable-tx. Usa el bucle interno del RFIC del bladeRF
 *             (BLADERF_LB_RFIC_BIST) con la ganancia de TX al mínimo: la señal
 *             se genera y vuelve al receptor sin salir por la antena.
 *   air       Con --enable-tx-air Y todas las comprobaciones de tx_check_air().
 *
 * El nivel de seguridad es deliberadamente pesimista: ante la duda se rechaza.
 * La lista blanca de bandas (ISM 433, SRD 868/869 y radioaficionado con
 * indicativo) impide por construcción emitir en aviación, marítimo, GNSS,
 * celular, radiodifusión o emergencias (406 MHz).
 *
 * Cada emisión se escribe en el log (frecuencia, protocolo, duración) y en
 * <recdir>/tx_log.jsonl. Reproducción de grabaciones (tx_play) y calibración
 * (tx_cal_run) NUNCA usan el modo air.
 */
#ifndef BLADERF_LAB_TX_H
#define BLADERF_LAB_TX_H

#include "../net/server.h"
#include "../sdr/source.h"
#include "cal.h"
#include "mod.h"

typedef enum { TX_FILE = 0, TX_LOOPBACK, TX_AIR } tx_mode_t;

typedef struct {
    bool enabled;          /* --enable-tx  (permite loopback) */
    bool air;              /* --enable-tx-air (permite air) */
    char callsign[16];     /* --callsign (indicativo de radioaficionado) */
    int max_gain_db;       /* --tx-max-gain (por defecto 0) */
} tx_policy_t;

typedef struct {
    char proto[16];
    tx_mode_t mode;
    int64_t freq;          /* frecuencia RF de la señal */
    int64_t center;        /* centro del hardware/fichero; 0 = automático */
    uint32_t rate;         /* tasa de muestreo; 0 = automática */
    int gain_db;
    int repeat;            /* nº de repeticiones (≥ 1) */
    const char *params;    /* objeto JSON para el modulador, o NULL */
} tx_req_t;

/* Resultado de tx_render(). lo/hi: bordes del 99,98 % de la potencia respecto
 * a la portadora (Hz); occ_bw = hi − lo. */
typedef struct {
    int64_t center;
    double rate;
    double lo_hz, hi_hz, occ_bw_hz;
    double seconds;        /* duración de UNA repetición */
} tx_meta_t;

/* Modula y coloca la señal: a 'rate', con el canal desplazado a freq dentro
 * de la captura centrada en 'center' (0 = automático, ver tx.c). La salida
 * queda en out (cf32, |x| ≤ 0,9). */
int tx_render(const modulator_desc_t *m, const char *params, int64_t freq, int64_t center, double rate,
              iqbuf_t *out, tx_meta_t *meta, char *err, size_t errlen);
/* Escribe prefix.sigmf-data y prefix.sigmf-meta (ci16_le, escala Q11). */
int tx_write_sigmf(const char *prefix, const iqbuf_t *iq, int repeat, int64_t center, const char *desc, char *err,
                   size_t errlen);
/* Comprueba la política de «air». Devuelve 0 si se permite; <0 con el motivo.
 * recent_s: segundos ya emitidos en la última hora en 868,0–868,6 MHz. */
int tx_check_air(const tx_policy_t *pol, const modulator_desc_t *m, int64_t freq, int64_t center,
                 const tx_meta_t *meta, int gain_db, double total_seconds, double recent_s, char *err,
                 size_t errlen);
/* Indicativo plausible: 3–12 caracteres [A-Z0-9/-], con al menos una letra y un dígito. */
bool tx_callsign_valid(const char *cs);

typedef struct tx tx_t;
/* src puede ser NULL (solo modo file). srv puede ser NULL. */
tx_t *tx_create(source_t *src, server_t *srv, const char *recdir, const tx_policy_t *pol);
void tx_destroy(tx_t *t);
/* Procesa una orden. Envía siempre un mensaje tx_result (ok o error). Devuelve
 * 0 si se aceptó (el envío real puede seguir en un hilo) o <0 con err. */
int tx_submit(tx_t *t, const tx_req_t *req, char *err, size_t errlen);
/* Igual con el modulador ya resuelto (para pruebas: la política se aplica igual). */
int tx_submit_with(tx_t *t, const modulator_desc_t *m, const tx_req_t *req, char *err, size_t errlen);
void tx_stop(tx_t *t);
bool tx_busy(tx_t *t);
void tx_wait(tx_t *t);
void tx_policy_json(tx_t *t, sb_t *sb);

/* --- Reproducción de grabaciones SigMF como emisión ({"cmd":"tx_play"}) ---
 * SOLO file y loopback: un ataque de repetición (replay) por antena queda
 * excluido por construcción (mode == TX_AIR se rechaza). */
typedef struct {
    char name[200];        /* nombre dentro de recdir (con o sin .sigmf-meta/.sigmf-data) */
    tx_mode_t mode;        /* TX_FILE o TX_LOOPBACK */
    int repeat;
} tx_play_req_t;
int tx_play(tx_t *t, const tx_play_req_t *req, char *err, size_t errlen);

/* --- Calibración del TX (cal.h) --- */
/* Captura del receptor durante la calibración: debe sintonizar freq/rate (el
 * bucle digital exige la misma tasa) y devolver n muestras Q11 intercaladas.
 * La instala quien posee el receptor (el motor); sin ella tx_cal_run falla. */
typedef int (*tx_rx_capture_fn)(void *ctx, int64_t freq, uint32_t rate, int16_t *iq, unsigned n, char *err,
                                size_t errlen);
void tx_set_rx_capture(tx_t *t, tx_rx_capture_fn fn, void *ctx);
/* Orden {"cmd":"tx_cal"}: exige --enable-tx y SIEMPRE usa loopback. Mide el TX
 * (q y fuga de LO), lo verifica con una segunda pasada precorregida y, si mejora,
 * guarda la corrección para ese centro/tasa. freq/rate 0 → 433,92 MHz / 1 Msps. */
int tx_cal_run(tx_t *t, int64_t freq, uint32_t rate, char *err, size_t errlen);
/* Corrección vigente (false si no hay). */
bool tx_cal_get(tx_t *t, cal_t *c, int64_t *center, uint32_t *rate);
void tx_cal_clear(tx_t *t);

/* --- Sustituto del hardware (pruebas): si se instala, los modos loopback y air
 * usan estas funciones en lugar de source_tx_*(). Nunca lo instala main.c. --- */
typedef struct {
    int (*configure)(void *ctx, int64_t freq, uint32_t rate, int gain_db, bool loopback);
    int (*write)(void *ctx, const int16_t *iq, unsigned n);
    int (*stop)(void *ctx);
    void *ctx;
} tx_sink_t;
void tx_set_sink(tx_t *t, const tx_sink_t *s);

#endif
