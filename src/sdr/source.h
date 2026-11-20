/*
 * source.h — Fuente de muestras IQ: el bladeRF real o un fichero grabado.
 *
 * El resto del programa no sabe de dónde vienen las muestras: el motor DSP
 * solo llama a source_read() y recibe bloques de enteros de 16 bits I,Q,I,Q…
 * en formato Q11 (±2048 = fondo de escala), que es el nativo del bladeRF 2.0.
 *
 * Fuentes disponibles:
 *   - bladeRF:  vía libbladeRF (API síncrona, ver docs/arquitectura/ARQUITECTURA.md).
 *   - fichero:  "cs16" crudo (como el que guarda bladeRF-cli o la propia app)
 *               o una grabación SigMF (.sigmf-meta + .sigmf-data). Se lee a
 *               ritmo real y en bucle, ideal para probar sin hardware.
 *
 * Todas las funciones devuelven 0 si van bien y un valor negativo si no; el
 * texto del error queda en src->err.
 */
#ifndef BLADERF_LAB_SOURCE_H
#define BLADERF_LAB_SOURCE_H

#include "../common.h"
#include "fpga_lab.h"

/* Modos de ganancia del receptor (AD9361, ver libbladeRF: bladerf_gain_mode).
 *   AUTO    = el modo "por defecto" de libbladeRF (en el 2.0 micro: AGC lenta)
 *   MANUAL  = ganancia fija (MGC)
 *   FAST    = AGC de ataque rápido: reacciona en microsegundos; buena con ráfagas
 *             (ADS-B, buscas) pero "bombea" el suelo de ruido.
 *   SLOW    = AGC de ataque lento: estable para señales continuas (FM, TV).
 *   HYBRID  = AGC híbrida: lenta, pero con ataque rápido ante sobrecarga. */
typedef enum { GAIN_AUTO = 0, GAIN_MANUAL = 1, GAIN_FAST = 2, GAIN_SLOW = 3, GAIN_HYBRID = 4 } gain_mode_t;

/* Nombre JSON del modo ("auto", "manual", "fast", "slow", "hybrid") y lo inverso.
 * parse acepta también "mgc"/"agc_fast"/"agc_slow"/"agc_hybrid"; false si no existe. */
const char *gain_mode_name(gain_mode_t m);
bool gain_mode_parse(const char *s, gain_mode_t *m);

/* Modo oversample (FPGA >= 0.15, libbladeRF >= 2.5.1): el AD9361 funciona al
 * doble de reloj y entrega hasta 122,88 Msps en 8 bits (SC8_Q7). Se activa solo
 * cuando la tasa pedida supera este umbral. */
#define SOURCE_RATE_NATIVE_MAX 61440000u
#define SOURCE_RATE_OVERSAMPLE_MAX 122880000u

typedef struct {
    char kind[16];        /* "bladerf" o "file" */
    char board[64];       /* nombre de la placa */
    char serial[40];
    char fw_version[32];
    char fpga_version[32];
    char lib_version[32];
    int64_t freq_min, freq_max;
    uint32_t rate_min, rate_max;
    uint32_t rate_max_oversample; /* 0 si el modo oversample no está disponible */
    uint32_t bw_min, bw_max;
    int gain_min, gain_max;
    bool can_tune;        /* falso en reproducción de ficheros */
    bool has_biastee;
    bool can_tx;
    bool gain_cal;        /* hay tabla de calibración de ganancia cargada (dBm) */
} source_info_t;

typedef struct source source_t;

/* Abre el primer bladeRF (o el de 'serial' si no es NULL/""). */
source_t *source_open_bladerf(const char *serial, char *err, size_t errlen);
/* Abre un fichero. Si es .sigmf-meta/.sigmf-data se leen sus metadatos; si
 * no, se usan rate y center_hz. */
source_t *source_open_file(const char *path, uint32_t rate, int64_t center_hz, char *err, size_t errlen);

const source_info_t *source_info(const source_t *s);
const char *source_error(const source_t *s);

int source_set_frequency(source_t *s, int64_t hz);
int source_set_rate(source_t *s, uint32_t rate, uint32_t *actual);
int source_set_bandwidth(source_t *s, uint32_t bw, uint32_t *actual);
int source_set_gain(source_t *s, gain_mode_t mode, int gain_db);
int source_get_gain(source_t *s, int *gain_db);
int source_set_biastee(source_t *s, bool on);
int source_get_temperature(source_t *s, float *celsius);
/* Valores actuales (para la fuente de fichero son fijos). */
int64_t source_frequency(const source_t *s);
uint32_t source_rate(const source_t *s);
/* true si el modo oversample (8 bit, > 61,44 Msps) está activo. */
bool source_oversample(const source_t *s);
/* Formato de las muestras en el USB/fichero: "sc16q11", "sc8q7", "cs16", "cs8", "cu8"
 * (siempre se entregan a source_read() como Q11 de 16 bit). */
const char *source_format(const source_t *s);
/* Desplazamiento dBFS → dBm (dBm = dBFS + off) si hay tabla de calibración de
 * ganancia cargada y el modo la admite; devuelve 0, o <0 si no hay dato.
 * Con la calibración de libbladeRF dBm = dBFS − gain_target (ver docs/hardware/HARDWARE.md). */
int source_dbm_offset(source_t *s, double *off_db);
/* Conversión SC8_Q7 (int8, ±128 = fondo de escala) → Q11 (×16); n_values = 2·muestras. */
void source_sc8_to_q11(const int8_t *in, int16_t *out, size_t n_values);
/* Flujo de baja latencia (búferes pequeños) para el barrido; surte efecto en el
 * próximo source_start(). */
void source_set_low_latency(source_t *s, bool on);
/* Búferes y timeout de sync_rx adecuados al flujo del espectro FPGA (ver source.c); frame_period_s =
 * K·N/tasa. Se aplica en el siguiente source_start(); false restaura los valores de IQ. */
void source_set_spectrum_stream(source_t *s, bool on, double frame_period_s);

/* E/S hacia la FPGA para fpga_lab (versión, config_gpio, Wishbone) con el dispositivo de
 * esta fuente. 0 si va bien; < 0 si la fuente no es un bladeRF. Los accesos a la
 * placa los serializa libbladeRF, pero quien los llame debe ser el hilo que ya
 * gobierna la fuente (el DSP del motor, con el lector en pausa). */
int source_fpga_lab_io(source_t *s, fpga_lab_io_t *io);

/* Fichero a máxima velocidad (--replay-fast): sin dormir para guardar el ritmo real y sin bucle.
 * Al llegar al final source_read() devuelve SOURCE_EOF (el último bloque va completado con ceros).
 * Sin efecto en el bladeRF. Llamar antes de source_start(). */
#define SOURCE_EOF (-2000)
void source_set_fast(source_t *s, bool on);

/* Arranca/para el flujo de recepción. */
int source_start(source_t *s);
int source_stop(source_t *s);
/* Lee exactamente n muestras complejas en iq[2n]. Bloquea lo necesario.
 * *overruns se incrementa si el hardware perdió muestras. */
int source_read(source_t *s, int16_t *iq, unsigned n, unsigned *overruns);

/* Como source_read() pero devuelve también la marca de tiempo (en muestras, reloj
 * del FPGA) de la primera muestra. Solo bladeRF; en fichero *ts = nº de muestra. */
int source_read_ts(source_t *s, int16_t *iq, unsigned n, uint64_t *ts, unsigned *overruns);

/* --- Quick-tune / retune programado (solo bladeRF 2.0; ver sdr/sweep.h) ---
 * El 2.0 micro guarda hasta 256 perfiles de sintonía rápida por sesión (no se
 * pueden reciclar), así que se cachean por frecuencia. */
int source_qt_prepare(source_t *s, int64_t freq_hz);       /* → identificador >= 0, o < 0 */
int source_qt_free(const source_t *s);                     /* perfiles aún disponibles */
int source_retune_at(source_t *s, uint64_t ts, int qt_id); /* SOURCE_QUEUE_FULL si la cola está llena */
int source_retune_cancel(source_t *s);
int source_timestamp(source_t *s, uint64_t *ts);           /* reloj de muestras actual del FPGA */
#define SOURCE_QUEUE_FULL (-1000)

/* --- Transmisión (solo bladeRF). Ver tx/tx.h para la capa de seguridad. --- */
int source_tx_configure(source_t *s, int64_t freq_hz, uint32_t rate, int gain_db, bool rfic_loopback);
int source_tx_write(source_t *s, const int16_t *iq, unsigned n);
int source_tx_stop(source_t *s);

void source_close(source_t *s);

#endif
