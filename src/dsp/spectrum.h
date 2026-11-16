/*
 * spectrum.h — Cálculo del espectro (FFT promediada) para la cascada.
 *
 * Recibe las muestras crudas del bladeRF (enteros de 16 bits, formato Q11:
 * ±2048 = fondo de escala) y produce "tramas" de espectro a un ritmo fijo
 * (fps). Cada trama es la media de K FFTs consecutivas, lo que reduce la
 * varianza del ruido en un factor K (el suelo de ruido se ve "liso").
 *
 * Dos implementaciones con el mismo resultado:
 *   - CPU: FFT radix-2 propia (dsp/fft.c). Limita K para no saturar un núcleo.
 *   - GPU: FFT Stockham propia en CUDA (gpu/spectrum_gpu.cu). Con la GPU se
 *          pueden promediar TODAS las muestras incluso a 61 Msps.
 *
 * Escala: un tono complejo de amplitud 1,0 (fondo de escala) mide 0 dBFS.
 * Salida: fft_size valores en dBFS ordenados de −fs/2 a +fs/2 (fftshift).
 */
#ifndef BLADERF_LAB_SPECTRUM_H
#define BLADERF_LAB_SPECTRUM_H

#include "../common.h"

typedef struct spectrum spectrum_t;

/* db: fft_size valores; navg: cuántas FFT se promediaron. */
typedef void (*spec_frame_fn)(void *ctx, const float *db, int n, int navg);

spectrum_t *spectrum_create(int fft_size, double rate, int fps, bool try_gpu);
void spectrum_destroy(spectrum_t *s);
void spectrum_feed(spectrum_t *s, const int16_t *iq, int n, spec_frame_fn cb, void *ctx);
bool spectrum_using_gpu(const spectrum_t *s);
/* Descripción legible del motor ("CPU" o "GPU: NVIDIA GeForce …"). */
const char *spectrum_backend(const spectrum_t *s);

#endif
