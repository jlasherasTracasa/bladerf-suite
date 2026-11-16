/*
 * pocsag.h — Constantes de POCSAG (ITU-R M.584, "Radio-paging code No. 1")
 * compartidas por el decodificador y el modulador.
 *
 * Trama: preámbulo (≥ 576 bits alternos 1010…), y uno o más lotes. Cada lote
 * = palabra de sincronía de 32 bits + 16 palabras de 32 bits (8 tramas de 2).
 * Palabra = 21 bits de datos + 10 bits BCH(31,21) + 1 bit de paridad par.
 *   dirección: 0 | 18 bits (RIC>>3) | 2 bits de función | BCH | paridad
 *   mensaje:   1 | 20 bits de datos                       | BCH | paridad
 * Los 3 bits bajos del RIC (dirección de 21 bits) no se transmiten: son el
 * índice de la trama (0..7) en la que se coloca la palabra de dirección.
 */
#ifndef BLADERF_LAB_POCSAG_H
#define BLADERF_LAB_POCSAG_H

#include <stdint.h>

#define POCSAG_SYNC 0x7CD215D8u
#define POCSAG_IDLE 0x7A89C197u
#define POCSAG_PREAMBLE_BITS 576
#define POCSAG_DEV_HZ 4500.0 /* desviación nominal en canales de 12,5 kHz */

#endif
