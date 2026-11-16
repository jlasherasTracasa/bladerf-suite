/*
 * nr_corr.h — Correlación masiva por FFT (el "núcleo" de la búsqueda de SSB).
 *
 * Interfaz pensada para sustituirse por una versión en GPU (agente C1) sin tocar
 * el resto del decodificador: dada UNA señal x (n muestras complejas) y NT plantillas
 * (cada una de longitud tlen[t], energía unidad), devuelve para cada t y cada
 * desplazamiento i = 0..nout−1 (nout = n − max(tlen) + 1) la correlación normalizada
 *
 *     out[t·nout + i] = | Σ_k x[i+k]·conj(p_t[k]) |² / Σ_k |x[i+k]|²      ∈ [0, 1]
 *
 * (cociente de Cauchy-Schwarz: 1 = x es idéntica a la plantilla salvo escala). Al denominador se
 * le suma un suelo ε = 1e-4·(potencia media de x)·max(tlen) para que las señales sin ruido (ficheros
 * sintéticos) no den correlaciones espurias en los huecos de energía casi nula.
 * Método: solape-guardado con FFT de tamaño nfft; la FFT de cada bloque de x se
 * comparte entre las NT plantillas (E38 de docs/INVESTIGACION.md): coste ≈ (1+NT)
 * FFT de nfft por cada nfft − maxlen + 1 muestras.
 */
#ifndef BLADERF_LAB_NR_CORR_H
#define BLADERF_LAB_NR_CORR_H

#include "../common.h"

typedef struct nr_corr nr_corr_t;

/* nfft: potencia de dos ≥ 2·max(tlen). tmpl[t] apunta a tlen[t] muestras. NULL si falla. */
nr_corr_t *nr_corr_create(int nfft, int nt, const cf32 *const *tmpl, const int *tlen);
void nr_corr_destroy(nr_corr_t *c);
/* Escribe NT·nout floats en 'out' (el llamador reserva). Devuelve nout o −1. */
int nr_corr_run(nr_corr_t *c, const cf32 *x, int n, float *out);

/* true si esta instancia correlaciona en GPU (src/gpu/nr_corr_gpu.*). BLADERF_NR_GPU=0 fuerza la CPU. */
bool nr_corr_using_gpu(const nr_corr_t *c);

#endif
