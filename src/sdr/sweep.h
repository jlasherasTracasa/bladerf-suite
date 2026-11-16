/*
 * sweep.h — Barrido rápido de espectro con sintonía rápida (quick-tune).
 *
 * Idea: el AD9361 solo ve unos MHz a la vez (tasa de muestreo fs). Para cubrir
 * 70 MHz–6 GHz se salta de frecuencia en frecuencia: en cada "salto" se captura
 * un trozo corto, se calcula su FFT y se pega el tramo útil en un espectro
 * único. Las piezas son:
 *
 *  1. Rejilla común. Con FFT de N puntos cada bin mide bin_hz = fs/N. De los N
 *     bins solo se aprovecha el 70 % central (K bins, K par): fuera de él el
 *     filtro analógico del RFIC atenúa. El salto h se centra en
 *         c_h = inicio + (h·K + K/2)·bin_hz
 *     de modo que su tramo útil empieza exactamente en inicio + h·K·bin_hz: los
 *     tramos de saltos vecinos son contiguos, no se solapan ni dejan huecos
 *     (todos los bins caen en la misma rejilla de paso bin_hz) y no hace falta
 *     interpolar. El pico de continua (fuga del LO) se elimina restando la
 *     media de las muestras antes de la FFT.
 *     Si el último salto cae por encima de la frecuencia máxima del equipo se
 *     desplaza un número entero de bins hacia abajo (sigue en la rejilla) y se
 *     recorta lo que quede fuera de ±42 % de fs.
 *
 *  2. Retune programado. libbladeRF (bladerf_get_quick_tune +
 *     bladerf_schedule_retune) guarda el estado del sintetizador de cada
 *     frecuencia en un perfil del RFIC y lo restaura en un instante de reloj de
 *     muestras (timestamp) fijado de antemano: el cambio de frecuencia ocurre en
 *     la FPGA, sin latencia USB, y mientras tanto el flujo de muestras no se
 *     interrumpe. Se encolan Q = 8 saltos por delante de la lectura; cada salto
 *     dura dwell = settle + N muestras: las primeras `settle` (≈ 250 µs) se
 *     descartan mientras el PLL se asienta y se usan las N siguientes.
 *     Límite del 2.0 micro: 256 perfiles por sesión, no reciclables (libbladeRF
 *     2.6); se cachean por frecuencia y la tasa se elige lo bastante alta para
 *     que los saltos quepan. Si no caben (o no hay quick-tune) se usa el modo
 *     lento: set_frequency + descartar muestras hasta la marca de tiempo
 *     posterior al cambio (milisegundos por salto, pero siempre correcto).
 *
 *  3. Seguridad ante fallos. Si hay overrun, hueco en las marcas de tiempo o la
 *     lectura va retrasada respecto al FPGA (los retunes ya habrían ocurrido), se
 *     cancela la cola y se re-sincroniza; los datos de ese salto se descartan.
 *
 * Mensaje binario tipo 4 (servidor → cliente), little-endian, misma cabecera que
 * el tipo 1 (espectro) pero cubriendo [inicio, fin]:
 *   [0] = 4          [1] = 0
 *   [2] = banderas   bit0: barrido completo (si no, actualización parcial)
 *   [3] = progreso   0..100 (% de saltos de esta pasada)
 *   [4..7]  u32 n    número de bins
 *   [8..15] f64      centro Hz = (inicio+fin)/2
 *   [16..23] f64     "tasa" Hz = fin − inicio = n·bin_hz (como en el tipo 1,
 *                    así inicio = centro − tasa/2 y bin i ↔ inicio + i·tasa/n)
 *   [24..27] f32 dbmin = −140      [28..31] f32 dbstep = 0,5
 *   [32..]  n bytes v: dBFS = dbmin + v·dbstep
 * Los dBFS son los mismos que los del espectro normal (tono de fondo de escala =
 * 0 dBFS) con la ganancia fija durante el barrido (se fuerza ganancia manual).
 */
#ifndef BLADERF_LAB_SWEEP_H
#define BLADERF_LAB_SWEEP_H

#include "../common.h"
#include "source.h"

#define SWEEP_QUEUE_FULL SOURCE_QUEUE_FULL
#define SWEEP_DB_MIN (-140.0f)
#define SWEEP_DB_STEP 0.5f
#define SWEEP_HEADER 32

/* Acceso al hardware (en las pruebas se sustituye por un radio simulado). */
typedef struct {
    void *ctx;
    int (*configure)(void *ctx, uint32_t rate);                  /* para el flujo, fija tasa y ancho, baja latencia */
    int (*start)(void *ctx);
    void (*stop)(void *ctx);
    int (*set_freq)(void *ctx, int64_t hz);
    int (*qt_prepare)(void *ctx, int64_t hz);                    /* id >= 0 o < 0 */
    int (*qt_free)(void *ctx);
    int (*retune_at)(void *ctx, uint64_t ts, int qt_id);         /* 0, SWEEP_QUEUE_FULL o < 0 */
    int (*retune_cancel)(void *ctx);
    int (*timestamp)(void *ctx, uint64_t *ts);
    int (*read)(void *ctx, int16_t *iq, unsigned n, uint64_t *ts, unsigned *overruns);
    const char *(*error)(void *ctx);
} sweep_io_t;

typedef struct {
    int64_t start_hz, stop_hz;
    int64_t fmin_hz, fmax_hz;    /* rango sintonizable del equipo */
    uint32_t rate;               /* 0 = elegir la menor que permita usar quick-tune */
    int fft;                     /* tamaño de FFT por salto (potencia de 2, 256..8192) */
    double settle_us;            /* 0 = 250 µs */
    bool allow_quick;            /* false = forzar modo lento */
} sweep_cfg_t;

typedef struct {
    const float *db;             /* n valores dBFS */
    int n;
    int64_t start_hz;            /* frecuencia del bin 0 */
    double bin_hz;
    bool complete;
    int progress_pct;
} sweep_frame_t;

typedef struct sweep sweep_t;

/* Planifica, configura el hardware, prepara los perfiles y arranca el flujo. */
sweep_t *sweep_create(const sweep_cfg_t *cfg, const sweep_io_t *io, char *err, size_t errlen);
/* Para el flujo y cancela los retunes pendientes. */
void sweep_destroy(sweep_t *w);
/* Un salto (≈ dwell + procesado). 1 = hay trama en *fr (válida hasta la próxima
 * llamada), 0 = nada que emitir aún, < 0 = error irrecuperable (texto en err). */
int sweep_step(sweep_t *w, sweep_frame_t *fr, char *err, size_t errlen);

/* Escribe el mensaje tipo 4 en out (capacidad ≥ sweep_msg_size(fr->n)). */
size_t sweep_msg_size(int n);
size_t sweep_pack(const sweep_frame_t *fr, uint8_t *out);

typedef struct {
    int hops, bins;
    uint32_t rate;
    int fft;
    bool quick;
    int64_t start_hz, stop_hz;   /* cobertura real */
    uint64_t sweeps, restarts;
} sweep_info_t;
void sweep_get_info(const sweep_t *w, sweep_info_t *out);

/* Envoltorio sobre source_t (bladeRF real). */
sweep_io_t sweep_io_source(source_t *s);

#endif
