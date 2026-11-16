/*
 * vdl2_frame.h — Capa de tramado de VDL Modo 2 (compartida por decodificador y
 * modulador). Describe lo que viaja DENTRO de una ráfaga una vez demodulados los
 * símbolos D8PSK:
 *
 *   ráfaga = rampa (símbolos nulos) | palabra única (16 símb. = 48 bits) |
 *            cabecera (25 bits) | datos protegidos y aleatorizados
 *
 *   Palabra única (UW): 000 010 011 110 000 001 101 110 001 100 011 111 101 111 100 010
 *   (16 tripletes de bits, cada uno es el INCREMENTO de fase de un símbolo).
 *   Cabecera: 3 bits reservados (000) + longitud de 17 bits (LSB primero; nº de
 *   bits del flujo AVLC, sin la paridad RS ni el relleno) + 5 bits de paridad de
 *   un código (25,20) de un solo error. Aquí se usa g(x) = x^5 + x^2 + 1 (ver .md).
 *   Datos: AVLC (bits) → octetos (MSB primero) → bloques RS(255,249) de 249 bytes
 *   (el último más corto, con 0/2/4/6 bytes de paridad según su longitud) →
 *   entrelazado por columnas → bits (MSB primero) → aleatorizador x^15+x^14+1.
 *   Los bits se agrupan de 3 en 3 (flujo continuo desde la UW) y se codifican
 *   diferencialmente: incremento de fase = k·π/4 con k = Gray→binario del triplete.
 *
 * Los tamaños de bloque y el orden de entrelazado salen de la descripción
 * publicada del modem (Kim et al., J. KICS 34(11), 2009) y NO se han podido
 * contrastar con una captura real: ver docs/decoders/vdl2.md.
 */
#ifndef BLADERF_LAB_VDL2_FRAME_H
#define BLADERF_LAB_VDL2_FRAME_H

#include "../common.h"

#define VDL2_RATE 105000.0
#define VDL2_BAUD 10500.0
#define VDL2_SPS 10
#define VDL2_UW_SYMS 16
#define VDL2_HDR_BITS 25
#define VDL2_BLOCK_DATA 249
#define VDL2_MAX_LEN 131071

extern const uint8_t VDL2_UW_BITS[48];          /* un bit por byte */
extern const uint8_t VDL2_GRAY_TO_INC[8];       /* triplete (b1b2b3 como número) → incremento 0..7 */
extern const uint8_t VDL2_INC_TO_GRAY[8];

/* Cabecera. Rellena 25 bits (reservados, longitud, paridad). */
void vdl2_hdr_encode(uint32_t len_bits, uint8_t bits[VDL2_HDR_BITS]);
/* Devuelve 0 (paridad correcta), 1 (corregido 1 bit) o −1 (síndrome no corregible;
 * len se rellena igualmente con lo recibido). */
int vdl2_hdr_decode(const uint8_t bits[VDL2_HDR_BITS], uint32_t *len_bits);

/* Aleatorizador aditivo (la misma función aleatoriza y desaleatoriza). */
void vdl2_scramble(uint8_t *bits, int nbits);

/* Reparto en bloques RS: devuelve el nº de bytes codificados (datos + paridad). */
int vdl2_encoded_bytes(int data_bytes);
/* Codifica (RS + entrelazado). out: vdl2_encoded_bytes() bytes. */
void vdl2_fec_encode(const uint8_t *data, int data_bytes, uint8_t *out);
/* Desentrelaza y corrige. data: data_bytes. Devuelve nº total de símbolos RS
 * corregidos o −1 si algún bloque no se pudo corregir. */
int vdl2_fec_decode(const uint8_t *enc, int data_bytes, uint8_t *data);

/* HDLC (bit a bit, LSB primero dentro de cada octeto, con relleno de ceros):
 * flag + frame(n bytes, ya con FCS) + flag. bits: un bit por byte; devuelve nº. */
int vdl2_hdlc_encode(const uint8_t *frame, int n, uint8_t *bits, int cap);
/* Recorre el flujo y llama a cb por cada trama con FCS (CRC-16/X-25) correcto. */
typedef void (*vdl2_frame_cb)(void *ctx, const uint8_t *frame, int n);
int vdl2_hdlc_decode(const uint8_t *bits, int nbits, vdl2_frame_cb cb, void *ctx);
/* Añade el FCS (2 bytes) a frame[0..n-1]. Devuelve n+2. */
int vdl2_add_fcs(uint8_t *frame, int n);

#endif
