/*
 * engine.h — Motor de recepción: el hilo DSP que mueve todo.
 *
 * Es dueño exclusivo del hardware (source_t): solo sus hilos (el lector para
 * source_read, el DSP para configurar, y siempre con el lector en pausa si se toca
 * la tasa o la sintonía) llaman a libbladeRF en recepción, así no hay carreras.
 * Los demás hilos (red) piden cambios con engine_request_*() y el motor los
 * aplica entre dos bloques de muestras.
 *
 * Hilos: un LECTOR (source_read en bucle sobre un anillo de 0,25–0,5 s) y el DSP, que
 * consume del anillo. Si el DSP se atrasa más de lo que cabe en el anillo, se
 * descartan bloques enteros (status.stats.drops); los overruns que declara el
 * hardware se cuentan aparte (status.stats.overruns). Ver docs/arquitectura/RENDIMIENTO.md.
 *
 * Por cada bloque de muestras (~20 ms):
 *   1. Toma el bloque más viejo del anillo.
 *   2. Calcula el espectro (CPU o GPU) y lo difunde a todos los clientes. De la
 *      misma trama sale el S-meter de los clientes que no escuchan (sin trabajo por
 *      muestra).
 *   3. Alimenta el detector de señales.
 *   4. dec_host_feed(): reparte el bloque a los decodificadores (no bloquea). El
 *      float sin continua solo se calcula si algún decodificador NARROW lo usa.
 *   5. El canal de audio de los clientes con audio_on (lee el Q11 crudo).
 *   6. Si se está grabando, copia las muestras al grabador SigMF.
 * Y periódicamente envía estado y lista de señales (y dec_host_periodic()).
 */
#ifndef BLADERF_LAB_ENGINE_H
#define BLADERF_LAB_ENGINE_H

#include "common.h"
#include "dsp/channel.h"
#include "decoders/host.h"
#include "net/server.h"
#include "sdr/source.h"
#include "util/strbuf.h"

typedef struct {
    int64_t center_hz;
    uint32_t rate;
    uint32_t rf_bw;        /* ancho de banda analógico; 0 = automático */
    gain_mode_t gain_mode;
    int gain_db;
    bool biastee;
    int fft_size;
    int fps;
    bool loc_valid;
    double lat, lon;
    float det_snr;         /* umbral del detector de señales (dB) */
} engine_cfg_t;

/* Campos de engine_cfg_t que se quieren cambiar */
enum {
    CFG_CENTER = 1 << 0,
    CFG_RATE = 1 << 1,
    CFG_RFBW = 1 << 2,
    CFG_GAIN = 1 << 3,
    CFG_BIASTEE = 1 << 4,
    CFG_FFT = 1 << 5,
    CFG_LOC = 1 << 6,
    CFG_DET = 1 << 7,
    CFG_ALL = 0xFF,
};

typedef struct engine engine_t;

engine_t *engine_create(source_t *src, server_t *srv, const engine_cfg_t *initial, bool use_gpu, const char *recdir);
void engine_destroy(engine_t *e);
int engine_start(engine_t *e);
void engine_stop(engine_t *e);

/* Normaliza y valida una configuración (rangos del hardware, potencias de 2…). */
void engine_sanitize(engine_t *e, engine_cfg_t *c);
void engine_request(engine_t *e, const engine_cfg_t *want, unsigned fields);
void engine_get_config(engine_t *e, engine_cfg_t *out);

/* Barrido rápido (src/sdr/sweep.h). Mientras barre, el motor no hace la ruta normal
 * (espectro, detector, decodificadores, audio, grabación) y difunde mensajes
 * binarios de tipo 4. start valida y encola la orden (la ejecuta el hilo DSP):
 * < 0 con err si el rango o la fuente no lo permiten. stop devuelve al motor a
 * su configuración normal (la última pedida). */
int engine_sweep_start(engine_t *e, int64_t start_hz, int64_t stop_hz, int fft, char *err, size_t errlen);
void engine_sweep_stop(engine_t *e);

/* Canales de audio (uno por cliente WebSocket) */
void engine_add_client(engine_t *e, int client);
void engine_remove_client(engine_t *e, int client);
bool engine_get_channel(engine_t *e, int client, chan_params_t *p);
void engine_set_channel(engine_t *e, int client, const chan_params_t *p);

/* Grabación IQ */
void engine_record(engine_t *e, bool on);
int engine_delete_recording(engine_t *e, const char *name);

/* JSON del estado completo (mensaje "status" y GET /api/status). */
void engine_status_json(engine_t *e, sb_t *sb);
/* Envía a un cliente lo último conocido (estado, señales…) al conectar. */
void engine_send_snapshot(engine_t *e, int client);

/* Se llama (desde el hilo DSP) cada vez que la configuración aplicada cambia:
 * main.c lo usa para guardarla en disco y recuperarla al reiniciar. */
void engine_on_config_change(engine_t *e, void (*fn)(void *ctx, const engine_cfg_t *cfg), void *ctx);

/* Host de decodificadores al que se alimenta cada bloque (puede ser NULL). */
void engine_set_host(engine_t *e, dec_host_t *host);
/* Perfil activo que se publica en "status" (NULL o "" = ninguno). */
void engine_set_profile(engine_t *e, const char *id);

/* Captura de IQ crudo para la calibración del TX (tx_set_rx_capture, ver main.c).
 * engine_capture_iq() la llama un hilo AJENO al DSP: arma una "toma" y espera a que
 * el hilo DSP copie en ella las n primeras muestras (Q11 intercaladas) que lea
 * después. El hilo DSP solo hace un memcpy bajo un mutex propio: no espera a nadie.
 * Exige que la fuente esté ya en freq/rate (engine_wait_tuned). < 0 con err. */
int engine_capture_iq(engine_t *e, int64_t freq, uint32_t rate, int16_t *iq, unsigned n, double timeout_s, char *err,
                      size_t errlen);
/* Pide centro y tasa y espera a que el hilo DSP los aplique (false si pasa timeout_s). */
bool engine_wait_tuned(engine_t *e, int64_t freq, uint32_t rate, double timeout_s);

/* Pruebas: el hilo DSP deja de consumir durante 'seconds' (el lector sigue). Sirve
 * para comprobar que un DSP atrasado descarta bloques enteros y los cuenta en
 * status.stats.drops sin provocar overruns del hardware. */
void engine_debug_stall(engine_t *e, double seconds);

/* Acceso a la fuente para el módulo de transmisión. */
source_t *engine_source(engine_t *e);

#endif
