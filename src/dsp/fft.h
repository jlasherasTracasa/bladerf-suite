/*
 * fft.h — Transformada rápida de Fourier (FFT) compleja, radix-2, propia.
 *
 * La FFT calcula la DFT  X[k] = Σ_{n=0}^{N-1} x[n]·e^{-j2πkn/N}  en
 * O(N·log2 N) operaciones en lugar de O(N²). Se usa el algoritmo de
 * Cooley-Tukey iterativo con decimación en el tiempo:
 *
 *   1. Permutación por inversión de bits (bit-reversal) de la entrada.
 *   2. log2(N) etapas de "mariposas": en la etapa con bloques de tamaño m,
 *      cada par (a, b) separado m/2 posiciones se combina como
 *          a' = a + W·b,   b' = a − W·b,   con W = e^{-j2πj/m}.
 *
 * Los factores W ("twiddles") y la tabla de inversión de bits se calculan una
 * sola vez al crear el plan, así cada ejecución solo hace sumas y productos.
 *
 * N debe ser potencia de dos (64 … 65536).
 */
#ifndef BLADERF_LAB_FFT_H
#define BLADERF_LAB_FFT_H

#include "../common.h"

typedef struct {
    int n;        /* tamaño de la transformada */
    int log2n;
    cf32 *tw;     /* twiddles W_N^k = e^{-j2πk/N}, k = 0..N/2-1 */
    int *rev;     /* rev[i] = i con sus log2n bits invertidos */
} fft_plan_t;

fft_plan_t *fft_plan_create(int n);
void fft_plan_destroy(fft_plan_t *p);
/* FFT directa, en el sitio (sobrescribe data). */
void fft_exec(const fft_plan_t *p, cf32 *data);

/* true si n es potencia de dos y está en el rango admitido. */
bool fft_size_valid(int n);

/* Ventana de Blackman-Harris de 4 términos: lóbulos laterales a −92 dB, muy
 * adecuada para ver señales débiles junto a otras fuertes en el espectro. */
void window_blackman_harris(float *w, int n);

#endif
