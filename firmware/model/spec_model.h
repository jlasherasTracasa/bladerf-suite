/*
 * spec_model.h — Modelo de referencia, exacto al bit, del motor de espectro
 * de la FPGA "lab" (hdl/fpga/platforms/bladerf-micro/vhdl/lab/spec_*.vhd).
 *
 * El motor es: ventana Blackman-Harris Q15 -> FFT SDF radix-2^2 de coma fija
 * (N = 1024..8192) -> |X|^2 -> acumulación exacta de K tramas -> palabra de
 * 32 bits {E[5:0], M[25:0]} (valor = M * 2^E).
 *
 * Todo el redondeo y truncado coincide con el HDL; ver el comentario de
 * cabecera de spec_model.c para las fórmulas exactas.
 */
#ifndef SPEC_MODEL_H
#define SPEC_MODEL_H

#include <stdint.h>

#define SPEC_NMIN_LOG2 10
#define SPEC_NMAX_LOG2 13
#define SPEC_HDR_WORDS 8
#define SPEC_PAD_WORDS 2048
#define SPEC_MAGIC0 0x4C415031u /* "LAP1" */
#define SPEC_MAGIC1 0xB3BEAFCEu /* ~MAGIC0 */
#define SPEC_DATA_BITS 25       /* ancho máximo de los datos internos */

/* Ventana Q15 de N puntos (i = 0..N-1): round(32768 * BH(i/(N-1))). */
void spec_window_q15(int log2n, uint16_t *w);

/* Una trama: iq = N pares (I,Q) de 12 bits con signo (±2048). Salida en el
 * ORDEN DE SALIDA DE LA FFT (bit-invertido): re[j], im[j] con 25 bits con signo.
 * Devuelve 0, o -1 si algún valor se sale de rango (no debe ocurrir). */
int spec_fft_frame(int log2n, const int16_t *iq, int32_t *re, int32_t *im);

void spec_twiddle(int64_t m, int64_t l, int32_t *wr, int32_t *wi);

/* |X|^2 exacto de una salida de la FFT. */
uint64_t spec_power(int32_t re, int32_t im);

/* Palabra de 32 bits de una potencia acumulada. */
uint32_t spec_pack_power(uint64_t acc);
double spec_unpack_power(uint32_t w);

/* Bin (orden natural, 0..N-1) que corresponde a la posición j de salida. */
unsigned spec_bitrev(int log2n, unsigned j);

/* Procesa K tramas consecutivas (K*N muestras IQ en iq) y escribe la trama de
 * salida completa de palabras: cabecera (SPEC_HDR_WORDS) + N bins en orden
 * fftshift + relleno si pad. Devuelve el número de palabras escritas. La
 * marca de tiempo de la cabecera se escribe como ts (64 bit). */
int spec_group(int log2n, int k, const int16_t *iq, uint32_t group_counter,
               uint64_t ts, uint32_t flags, int pad, uint32_t *out);

#endif
