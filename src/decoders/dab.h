/*
 * dab.h — Constantes y bloques comunes de DAB modo I (ETSI EN 300 401) que
 * comparten el decodificador (dab.c), el modulador (tx/mod_dab.c) y los tests.
 *
 * Modo I (banda III, 1536 kHz): T = 1/2048000 s
 *   - Trama de transmisión: 196 608 T = 96 ms = símbolo nulo (2656 T) + 76
 *     símbolos OFDM de 2552 T (= guarda de 504 T + parte útil de 2048 T).
 *   - Símbolo 0 = símbolo de referencia de fase (PRS); símbolos 1..3 = FIC;
 *     4..75 = MSC (el audio; este proyecto no lo decodifica).
 *   - K = 1536 portadoras separadas 1 kHz, índices k = −768..−1, +1..+768 (la
 *     k = 0 no se usa). La portadora k cae en el bin k (k > 0) o 2048 + k (k < 0)
 *     de una FFT de 2048 puntos a 2,048 MS/s.
 *   - Modulación D-QPSK entre símbolos consecutivos (el PRS es la referencia).
 *   - FIC: 12 FIB (Fast Information Block) por trama; cada FIB son 256 bits
 *     = 30 bytes de FIGs + CRC-16; 3 FIB forman un bloque de 768 bits que se
 *     dispersa (PRBS x^9+x^5+1), se codifica convolucionalmente (K=7, tasa 1/4,
 *     0133,0171,0145,0133) y se punzona a 2304 bits (tasa ≈ 1/3: 21 bloques de
 *     128 bits con PI_16, 3 con PI_15 y los 24 bits de cola con PI_X).
 *     Los 4 bloques de 2304 bits (= 9216 bits) llenan exactamente los 3 símbolos
 *     de FIC (3 × 2 × 1536 bits).
 *
 * Origen de las tablas (no se pudo consultar la norma en papel): el desentrelazado
 * de frecuencia (Π(i) = 13·Π(i−1) + 511 mod 2048, conservando 256 ≤ Π ≤ 1792,
 * Π ≠ 1024), los vectores de punzonado PI_1..PI_24, los parámetros del símbolo de
 * fase (tablas h y k',i,n) y la tabla UEP se contrastaron con la implementación
 * abierta welle.io (backend/{freq-interleaver,protTables,phasetable,uep-protection}.cpp,
 * fic-handler.cpp), que las deriva de EN 300 401 §14.6, §11.1–11.3 y §14.3.2.
 * Ver docs/decoders/dab.md (riesgos).
 */
#ifndef BLADERF_LAB_DAB_H
#define BLADERF_LAB_DAB_H

#include "../common.h"
#include "../dsp/fec.h"

#define DAB_RATE 2048000
#define DAB_TU 2048
#define DAB_GUARD 504
#define DAB_SYM 2552
#define DAB_NULL 2656
#define DAB_NSYM 76
#define DAB_FRAME 196608
#define DAB_K 1536

/* Mapa del desentrelazador: n (0..1535) → índice de portadora k (−768..768, ≠ 0). */
const int16_t *dab_carrier_map(void);
/* Espectro del símbolo de referencia de fase (2048 bins; ceros fuera de las portadoras). */
void dab_prs_spectrum(cf32 *bins);
/* CRC-16 de un FIB (30 bytes de datos + 2 de CRC; CCITT, init 0xFFFF, complementado). */
bool dab_fib_crc_ok(const uint8_t fib[32]);
void dab_fib_set_crc(uint8_t fib[32]);
/* Codifica 3 FIB (3×32 bytes) → 2304 bits (uno por byte): dispersión + K=7 r1/4 + punzonado. */
void dab_fic_encode(const uint8_t fib[3][32], uint8_t out[2304]);
/* Decodifica 2304 símbolos blandos (+ = bit 1, 0 = borrado) → 3 FIB. Devuelve las
 * discrepancias del Viterbi (≈ errores de canal) o −1. v: viterbi_create(conv_dab_k7, 768). */
int dab_fic_decode(viterbi_t *v, const int8_t soft[2304], uint8_t fib[3][32]);
/* Tabla UEP (EN 300 401 tabla 8): índice 0..63 → tamaño (CU), nivel (1..5) y kbps. */
bool dab_uep_info(int index, int *size_cu, int *level, int *kbps);

#endif
