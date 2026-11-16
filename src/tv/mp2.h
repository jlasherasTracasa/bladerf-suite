/*
 * mp2.h — Decodificador propio de audio MPEG-1/2 Layer II (ISO/IEC 11172-3 y
 * 13818-3), el audio de la TDT: 48 kHz, 192 kbit/s estéreo típicamente.
 *
 * Estructura de una trama de Layer II (1152 muestras por canal, 24 ms a 48 kHz):
 *
 *   cabecera 32 bits (sincronía 0xFFF, versión, capa, CRC, tasa, frecuencia,
 *            relleno, modo, extensión de modo…)  [CRC 16 bits si protection_bit=0]
 *   asignación de bits  nbal(sb) bits por subbanda y canal: índice → clase de
 *            cuantificador (3, 5, 7, 9, 15… 65535 niveles). Las tablas dependen de
 *            frecuencia y tasa por canal (B.2a–d) y, en MPEG-2 de baja frecuencia,
 *            de la tabla B.1.
 *   scfsi    2 bits por subbanda/canal activo: cuántos de los 3 factores de escala
 *            se envían (se reutilizan los de las partes anteriores)
 *   factores de escala  6 bits: sf = 2^(1 − índice/3) (pasos de 2 dB)
 *   muestras 12 grupos × 3 muestras por subbanda: cada terna se codifica en un
 *            solo código si el cuantificador es de 3, 5 o 9 niveles («grouping»)
 *
 * Reconstrucción: valor = (2c − (n−1))/n · factor_de_escala, con c el código y n
 * el número de niveles (equivale a C·(s''+D) de la norma). Después, el banco de
 * síntesis polifásico de 32 subbandas: por cada instante, V = N·S con
 * N[i][k] = cos((16+i)(2k+1)π/64), ventana de 512 coeficientes D[i] y suma de 16
 * términos por muestra de salida (ISO 11172-3 Fig. A.2).
 *
 * El joint stereo con intensidad (subbandas ≥ «bound» comparten asignación y
 * códigos, cada canal con su factor de escala) está implementado.
 */
#ifndef BLADERF_LAB_MP2_H
#define BLADERF_LAB_MP2_H

#include <stddef.h>
#include <stdint.h>

typedef struct mp2 mp2_t;

/* Recibe una trama decodificada: pcm = nsamp·ch muestras int16 intercaladas. */
typedef void (*mp2_frame_fn)(void *ctx, const int16_t *pcm, int nsamp, int ch, int rate);

mp2_t *mp2_create(void);
void mp2_destroy(mp2_t *d);
void mp2_reset(mp2_t *d);
/* Alimenta bytes del flujo elemental (cualquier troceado): busca sincronía, valida
 * cabeceras y decodifica cada trama completa. Devuelve las tramas decodificadas. */
int mp2_decode(mp2_t *d, const uint8_t *data, size_t len, mp2_frame_fn fn, void *ctx);
unsigned long mp2_bad_frames(const mp2_t *d);

/* Decodifica una trama completa que empieza en data (para pruebas). Devuelve los
 * bytes de la trama, o −1 si la cabecera no vale. pcm: 1152·2 int16. */
int mp2_decode_one(mp2_t *d, const uint8_t *data, size_t len, int16_t *pcm, int *ch, int *rate);

#endif
