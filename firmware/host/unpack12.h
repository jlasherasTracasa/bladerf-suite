/* unpack12.h — Desempaquetado en el host de los formatos «estrechos» de RX (F11).
 * Ver unpack12.c para la teoría y la disposición de bits. NO está enlazado al binario aún. */
#ifndef UNPACK12_H
#define UNPACK12_H
#include <stddef.h>
#include <stdint.h>

/* SC16_Q11_PACKED (3 bytes por muestra IQ) → SC16_Q11 (I,Q int16, ±2048). 'nsamples' debe ser
 * múltiplo de 4 (cada 4 muestras = 12 bytes = 3 palabras de 32 bit). Lee 3·nsamples bytes. */
void unpack_sc12_to_sc16(const uint8_t *in, int16_t *out, size_t nsamples);

/* SC8_Q7 con ganancia digital 2^shift de la FPGA (registro GAIN8) → SC16_Q11 con la ganancia
 * deshecha (redondeo al más próximo). 'shift' 0..3. Lee 2·nsamples bytes. */
void unpack_sc8_to_sc16(const uint8_t *in, int16_t *out, size_t nsamples, unsigned shift);
#endif
