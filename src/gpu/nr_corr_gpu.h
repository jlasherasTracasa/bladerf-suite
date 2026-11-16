/*
 * nr_corr_gpu.h — Correlación normalizada por FFT (solape-guardado) en GPU.
 *
 * Mismo contrato que nr_corr.h (decoders/): dada una señal x y NT plantillas de
 * energía unidad, calcula para cada t e i
 *
 *     out[t·nout + i] = | Σ_k x[i+k]·conj(p_t[k]) |² / (Σ_k |x[i+k]|² + ε)
 *
 * con nout = n − max(tlen) + 1 y ε = 1e-4·(potencia media de x)·max(tlen) + 1e-20.
 *
 * Diferencia de implementación respecto a la CPU: en vez de procesar los bloques
 * de solape-guardado uno tras otro, TODOS los bloques de la ventana (≈ 25 por
 * posición GSCN a 7,68 Msps) y las NT plantillas forman un único lote de FFT:
 *   1. FFT de B bloques de x            (B·nfft puntos)
 *   2. producto X·conj(T_t) para t < NT (B·NT·nfft puntos)
 *   3. FFT de B·NT bloques  (IFFT por conjugación: solo hace falta |·|²)
 *   4. |·|² / energía de ventana (suma deslizante en doble precisión)
 * FFT Stockham radix-2 propia (sin cuFFT), como spectrum_gpu.cu.
 *
 * Sin CUDA se enlaza nr_corr_gpu_stub.c y nr_corr_gpu_create() devuelve NULL.
 */
#ifndef BLADERF_LAB_NR_CORR_GPU_H
#define BLADERF_LAB_NR_CORR_GPU_H

#include "../common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_corr_gpu nr_corr_gpu_t;

/* nfft: potencia de dos ≥ 2·max(tlen); nt ≤ 8. NULL si no hay GPU o falla. */
nr_corr_gpu_t *nr_corr_gpu_create(int nfft, int nt, const cf32 *const *tmpl, const int *tlen);
void nr_corr_gpu_destroy(nr_corr_gpu_t *c);
/* Igual que nr_corr_run. Devuelve nout, o −1 (el llamador puede recurrir a la CPU). */
int nr_corr_gpu_run(nr_corr_gpu_t *c, const cf32 *x, int n, float *out);
/* ms de GPU+copias de la última llamada. */
double nr_corr_gpu_last_ms(const nr_corr_gpu_t *c);

#ifdef __cplusplus
}
#endif
#endif
