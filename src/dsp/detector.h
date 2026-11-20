/*
 * detector.h — Detección automática de señales en el espectro.
 *
 * Idea (detector de energía con umbral adaptativo, familia CFAR):
 *   1. Se promedia el espectro en el tiempo (media exponencial ~1 s) para que
 *      el ruido sea casi plano y las señales intermitentes dejen huella.
 *   2. Se estima el suelo de ruido LOCAL: el espectro se divide en 32
 *      segmentos y en cada uno se toma el percentil 20 de la potencia (un
 *      estimador de "estadístico ordenado", OS-CFAR, que ignora las señales
 *      porque ocupan pocos bins). Se usa el mínimo con los segmentos vecinos
 *      para que una señal ancha no "levante" su propio suelo.
 *   3. Un bin está ocupado si supera el suelo en más de 'snr_min' dB. Los bins
 *      ocupados contiguos (con huecos de ≤ 2 bins) forman una señal.
 *   3b. Bordes: se ignora el DET_EDGE_FRAC (5 %) de cada lado de la banda. El
 *      filtro antialias del AD9361 cae unos dB a partir de ~0,45·fs y en el
 *      borde el "suelo" baja, de modo que el ruido adyacente parece señal.
 *   3c. Fusión: tramos separados por menos de 0,3 veces el ancho del más estrecho se
 *      unen (0,3 veces; una emisora FM de 200 kHz no es un racimo de marcadores). La
 *      frecuencia es el centroide de potencia (exceso sobre el suelo) y la
 *      anchura el tramo con SNR ≥ snr_min (−snr_min dB bajo el pico, típ. 8 dB).
 *   3d. Histéresis: una señal se muestra tras DET_N_ON imágenes seguidas (cada
 *      llamada a detector_run es una imagen) y se retira tras DET_M_OFF
 *      ausencias; con run cada 0,5 s: aparece en 1 s y desaparece en 2 s.
 *   4. De cada señal se informa la frecuencia del pico, la anchura ocupada,
 *      el nivel de pico y la relación señal/ruido (SNR).
 *
 * Referencias de inspiración: literatura de "energy detection" con umbral
 * adaptativo al SNR para acceso dinámico al espectro (p. ej. arXiv:2607.00754)
 * y los detectores OS-CFAR clásicos del radar.
 */
#ifndef BLADERF_LAB_DETECTOR_H
#define BLADERF_LAB_DETECTOR_H

#include "../common.h"

#define DET_EDGE_FRAC 0.05f /* fracción de cada borde excluida */
#define DET_N_ON 2          /* imágenes seguidas para aparecer */
#define DET_M_OFF 4         /* imágenes ausentes para desaparecer */

typedef struct {
    double offset_hz;  /* frecuencia central (centroide de potencia) relativa al centro */
    double bw_hz;      /* anchura ocupada estimada */
    float peak_db;     /* nivel de pico (dBFS por bin) */
    float snr_db;      /* pico respecto al suelo de ruido local */
} det_signal_t;

typedef struct detector detector_t;

detector_t *detector_create(int n);
void detector_destroy(detector_t *d);
/* Incorpora una trama de espectro (dBFS). tau_frames: constante de tiempo de
 * la media exponencial expresada en tramas. */
void detector_feed(detector_t *d, const float *db, int n, float tau_frames);
/* Busca señales. Devuelve cuántas escribió en out (ordenadas por frecuencia). */
int detector_run(detector_t *d, double rate, float snr_min, det_signal_t *out, int max, float *noise_db);
void detector_reset(detector_t *d);
/* Ajusta fracción de borde excluida (0–0,45) y la histéresis (imágenes). */
void detector_config(detector_t *d, float edge_frac, int n_on, int m_off);

#endif
