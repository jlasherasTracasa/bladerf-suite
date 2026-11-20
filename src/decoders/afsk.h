/*
 * afsk.h — Demodulador AFSK/FFSK no coherente por correladores (APRS, DSC).
 *
 * Teoría. La señal de audio (salida de un discriminador FM) conmuta entre dos
 * tonos f1 ("1") y f0 ("0") con fase continua a 'baud' baudios. Para cada tono
 * se correla la entrada con e^{-j2πf n/fs} sobre una ventana deslizante de
 * exactamente un símbolo (N = fs/baud muestras):
 *
 *     c_k[n] = Σ_{i=0}^{N-1} x[n-i]·e^{-j2π f_k (n-i)/fs}      e_k = |c_k|²
 *
 * La energía e_k es máxima cuando el tono k ocupa la ventana. Se decide con
 *     d[n] = e_1[n] − g·e_0[n]       (d > 0 → bit 1)
 * donde g compensa el desequilibrio de amplitud entre tonos (preénfasis de
 * 6 dB/oct de los transmisores FM: el tono agudo llega más fuerte). Varios
 * demoduladores con distinto g corren en paralelo y el CRC elige (idea de
 * Dire Wolf, WB2OSZ).
 *
 * Reloj: PLL digital de un símbolo. La fase p ∈ [−½, ½) avanza baud/fs por
 * muestra; al cruzar +½ se toma un bit (centro del símbolo) y cada transición
 * de d acerca p a 0 (p ← p·inertia), de modo que las transiciones caen en p=0
 * y el muestreo a medio símbolo. Tolera ±1000 ppm sin más.
 */
#ifndef BLADERF_LAB_AFSK_H
#define BLADERF_LAB_AFSK_H

#include "../common.h"

typedef struct afsk afsk_t;
typedef void (*afsk_bit_fn)(void *ctx, int bit);

/* inertia: 0..1; cuanto mayor, más lento corrige (0,75 típico). */
afsk_t *afsk_create(double fs, double baud, double f1, double f0, double gain0, double inertia);
void afsk_destroy(afsk_t *a);
void afsk_process(afsk_t *a, const float *x, int n, afsk_bit_fn fn, void *ctx);
/* Índice (desde la creación) de la muestra que se está procesando; válido dentro del callback
 * de afsk_process: la muestra del bit que se entrega. Sirve para dar la posición de los mensajes. */
long afsk_index(const afsk_t *a);

#endif
