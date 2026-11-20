/*
 * flex.h — Estructura de trama FLEX común al decodificador y al modulador.
 * VER docs/protocolos/flex.md: la norma de Motorola no es de dominio público y
 * varias constantes (marcadas "SIN CONFIRMAR") salen de descripciones
 * públicas recordadas, no de la especificación.
 *
 * Trama (1,875 s; 128 tramas por ciclo de 4 min, 15 ciclos por hora):
 *   Sync 1  : 32 bits "dotting" 1010… + palabra A de 32 bits = código de 16
 *             bits seguido de su complemento. A fija la velocidad:
 *                0x870C → 1600 bit/s, 2 niveles   (1 fase:  A)
 *                0xB068 → 3200 bit/s, 2 niveles   (2 fases: A,B; 3200 Bd)
 *                0xDEA0 → 3200 bit/s, 4 niveles   (2 fases: A,B; 1600 Bd)
 *                0x4C7C → 6400 bit/s, 4 niveles   (4 fases: A..D; 3200 Bd)
 *             [SIN CONFIRMAR: la asignación código→modo y el relleno BS1.]
 *   FIW     : palabra de información de trama (32 bits, siempre 1600 Bd 2
 *             niveles). Datos de 21 bits: bits 0-3 suma de comprobación
 *             (suma de los 5 nibbles + bit 20 ≡ 0xF mod 16), 4-7 ciclo (0-14),
 *             8-14 nº de trama (0-127).
 *   Sync 2  : dotting a la velocidad de datos (longitud real SIN CONFIRMAR: el
 *             decodificador la busca por validez BCH).
 *   Datos   : 11 bloques × 8 palabras × 32 bits por fase, con entrelazado
 *             (el bit j de la palabra w de un bloque va en la posición j·8+w).
 * Palabra: 21 bits de datos + 10 de BCH(31,21) + 1 de paridad par; el primer
 * bit transmitido es el LSB del campo de datos.
 */
#ifndef BLADERF_LAB_FLEX_H
#define BLADERF_LAB_FLEX_H

#include "../dsp/bits.h"
#include "../dsp/fec.h"

#define FLEX_BLOCKS 11
#define FLEX_WORDS_PER_PHASE (FLEX_BLOCKS * 8)
#define FLEX_BS1 0xAAAAAAAAu
#define FLEX_SYNC_B 0xA6C6u /* segunda mitad de la palabra A, fija (validado con grabaciones reales) */
#define FLEX_SYNC2_MAX 96 /* símbolos de búsqueda de Sync 2 */

typedef struct {
    uint16_t code;
    int baud;      /* baudios de la parte de datos */
    int levels;    /* 2 o 4 */
    int phases;
    const char *name;
} flex_mode_t;

static const flex_mode_t FLEX_MODES[4] = {
    {0x870C, 1600, 2, 1, "1600/2"},
    {0xB068, 3200, 2, 2, "3200/2"},
    {0xDEA0, 1600, 4, 2, "3200/4"},
    {0x4C7C, 3200, 4, 4, "6400/4"},
};

static inline uint32_t flex_nib_sum(uint32_t d21)
{
    return ((d21 & 0xF) + ((d21 >> 4) & 0xF) + ((d21 >> 8) & 0xF) + ((d21 >> 12) & 0xF) + ((d21 >> 16) & 0xF) +
            ((d21 >> 20) & 1)) & 0xF;
}

/* Datos de 21 bits → 32 bits en orden temporal (bits[0] se transmite primero). */
static inline void flex_word_bits(const bch_t *b, uint32_t d21, uint8_t *bits)
{
    uint32_t c31 = bch_encode_u32(b, bits_reverse(d21, 21), 21);
    for (int j = 0; j < 31; j++)
        bits[j] = (uint8_t)((c31 >> (30 - j)) & 1);
    bits[31] = (uint8_t)bits_parity32(c31);
}

/* Inverso: corrige hasta 2 errores. Devuelve nº de errores o −1; d21 de salida. */
static inline int flex_word_fix(const bch_t *b, const uint8_t *bits, uint32_t *d21)
{
    uint32_t c31 = 0;
    for (int j = 0; j < 31; j++)
        c31 = (c31 << 1) | bits[j];
    int r = bch_decode_u32(b, &c31, 31);
    if (r < 0)
        return -1;
    int e = r;
    if (bits_parity32(c31) != bits[31]) {
        if (r >= 2)
            return -1;
        e++;
    }
    *d21 = bits_reverse(c31 >> 10, 21);
    return e;
}

#endif
