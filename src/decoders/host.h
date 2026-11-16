/*
 * host.h — Host de decodificadores: gestiona las instancias y les reparte IQ.
 *
 * Una INSTANCIA es un decodificador (decoder.h) trabajando sobre una
 * frecuencia concreta. Cada instancia tiene su propio hilo, así 20
 * decodificadores aprovechan 20 núcleos y ninguno puede retrasar al resto.
 *
 * Reparto de muestras (ver docs/ARQUITECTURA.md):
 *
 *   hilo DSP ── dec_host_feed() ──► 1 bloque compartido (copiado UNA vez,
 *                                   con contador de referencias)
 *                                      │ puntero
 *              ┌───────────────────────┼───────────────────────┐
 *              ▼                       ▼                       ▼
 *        cola instancia 1       cola instancia 2       cola instancia N
 *        (hilo: DDC → decodificador → eventos JSON → WebSocket)
 *
 * dec_host_feed() NO bloquea nunca: si la cola de una instancia está llena
 * (decodificador más lento que el tiempo real) se descarta el bloque para ESA
 * instancia y se cuenta en "drops" (nº de bloques).
 *
 * Estados de una instancia:
 *   ok       procesando muestras.
 *   waiting  NARROW cuya frecuencia (± su ancho) cae fuera de la banda
 *            capturada. No es un error: se reactiva si cambian centro o tasa.
 *   error    WIDE con tasa incompatible, create() fallido (p. ej. "no
 *            implementado todavía") o tasa de captura insuficiente. "err" lo
 *            explica.
 *
 * Eventos: máximo 200 por segundo y por instancia; el exceso se descarta y se
 * suma a "drops". Se guardan los 100 últimos para enviarlos a quien conecte.
 */
#ifndef BLADERF_LAB_HOST_H
#define BLADERF_LAB_HOST_H

#include "../net/server.h"
#include "decoder.h"

typedef struct dec_host dec_host_t;

/* srv puede ser NULL (pruebas): entonces no se envía nada por WebSocket. */
dec_host_t *dec_host_create(server_t *srv);
void dec_host_destroy(dec_host_t *h);
/* Devuelve el nº de instancia (>0) o −1 con err. owner: 0 manual, 1 perfil. */
int dec_host_add(dec_host_t *h, const char *proto, int64_t freq, const char *opts, int owner, char *err,
                 size_t errlen);
int dec_host_remove(dec_host_t *h, int inst);
void dec_host_remove_owner(dec_host_t *h, int owner);
int dec_host_retune(dec_host_t *h, int inst, int64_t freq);
void dec_host_set_location(dec_host_t *h, bool valid, double lat, double lon);
/* Hilo DSP. iq: Q11 crudo; buf: complejo normalizado sin continua. No bloquea.
 * now = hora Unix de la primera muestra del bloque. */
void dec_host_feed(dec_host_t *h, const int16_t *iq, const cf32 *buf, unsigned n, double fs, int64_t center,
                   double now);
/* ¿Hay alguna instancia que vaya a leer el bloque float normalizado (buf)? Es verdadero solo si
 * existe una instancia NARROW elegible que no lleva su DDC la GPU. Barato (un cerrojo y un
 * recorrido de las instancias): el motor puede llamarlo cada bloque o cada ~0,25 s y pasar
 * buf = NULL a dec_host_feed() cuando devuelve false. */
bool dec_host_needs_float(dec_host_t *h);
void dec_host_periodic(dec_host_t *h, double now);      /* dec_state ~1 Hz, dec_list */
void dec_host_list_json(dec_host_t *h, sb_t *sb);       /* mensaje "dec_list" */
void dec_host_catalog_json(sb_t *sb);                   /* mensaje "decoders" */
void dec_host_send_snapshot(dec_host_t *h, int client); /* lista + estados + últimos 100 eventos */
/* fn recibe cada evento como una línea JSON {"type":"dec",...} (--print / --log-events).
 * Se llama desde los hilos de las instancias, serializada. */
void dec_host_on_event(dec_host_t *h, void (*fn)(void *ctx, const char *line), void *ctx);

/* --- Instancias automáticas (perfiles con auto:true) ---
 * El host crea instancias del decodificador de la regla sobre las señales que
 * detecta el detector (dsp/detector.h) dentro de la banda [lo_hz, hi_hz] de la
 * regla: como máximo DEC_AUTO_MAX en total, sin duplicados (otra instancia del
 * mismo protocolo a menos de max(10 kHz, bw/2)), con owner = 1 (perfil). Las
 * retira cuando su señal lleva más de DEC_AUTO_TIMEOUT_S sin verse. Solo
 * decodificadores narrow. El trabajo (crear/parar hilos) lo hace un hilo propio:
 * dec_host_auto_signals() solo copia la lista y no bloquea al hilo DSP. */
#define DEC_AUTO_MAX 8
#define DEC_AUTO_TIMEOUT_S 60.0
typedef struct {
    int64_t freq_hz; /* absoluta (centro + offset) */
    double bw_hz;
    float snr_db;
} dec_sig_t;
int dec_host_auto_add_rule(dec_host_t *h, const char *proto, int64_t lo_hz, int64_t hi_hz);
void dec_host_auto_clear(dec_host_t *h); /* olvida reglas e instancias seguidas (no las elimina) */
/* now = hora Unix (el reloj de la prueba o de now_wall()). Llamar ~2 Hz, también con n = 0. */
void dec_host_auto_signals(dec_host_t *h, const dec_sig_t *sigs, int n, double now);
bool dec_host_auto_sync(dec_host_t *h, double timeout_s); /* espera a que el hilo auto acabe lo pendiente */

/* DDC en GPU (C1): activo por omisión si hay CUDA y ≥ 2 instancias narrow.
 * on=false (p. ej. --no-gpu) lo desactiva; BLADERF_DDC_GPU=0 hace lo mismo.
 * Solo tiene efecto sobre instancias que se (re)configuren después. */
void dec_host_set_gpu(dec_host_t *h, bool on);
/* Estado del DDC en GPU: false si no se usa. n_ch canales, bloques crudos
 * procesados y descartados, segundos de GPU y de CPU del hilo gestor. */
bool dec_host_gpu_info(dec_host_t *h, char *dev, size_t devlen, int *n_ch, unsigned long *blocks,
                       unsigned long *drops, double *gpu_s, double *cpu_s);

/* --- Extras (pruebas y herramientas) --- */
/* Espera a que todas las colas se vacíen. false si pasa timeout_s. */
bool dec_host_flush(dec_host_t *h, double timeout_s);
/* Pide a todas las instancias que generen su estado ahora (sin esperar 1 s). */
void dec_host_request_state(dec_host_t *h);
/* Último state_json de una instancia (objeto JSON) en sb. false si no hay. */
bool dec_host_inst_state(dec_host_t *h, int inst, sb_t *sb);
/* Estado de una instancia: "ok", "waiting" o "error" (NULL si no existe). */
bool dec_host_inst_info(dec_host_t *h, int inst, char *state, size_t slen, char *err, size_t elen);

#endif
