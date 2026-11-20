/*
 * flex.h — Estructura de trama FLEX común al decodificador y al modulador.
 *
 * FLEX es de Motorola y su especificación no es pública. Lo que sigue sale de
 * fuentes públicas y de grabaciones reales (ver docs/protocolos/flex.md):
 *   [T] S. McCulley, "Design and Implementation of a Practical FLEX Paging
 *       Decoder", tesis de máster, Virginia Tech, 1997 (cap. 2: modulación,
 *       trama, entrelazado, palabra de código, fases).
 *   [M] multimon-ng, demod_flex.c (tabla de palabras de sincronía, reparto de
 *       bits entre fases, vectores). Solo como referencia contrastada con
 *       grabaciones; no se ha copiado código.
 *
 * Trama (1,875 s; 128 tramas por ciclo de 4 min, 15 ciclos por hora):
 *   Sync 1 (siempre 1600 Bd, 2 niveles, 112 bits) [T §2.3.1: "dos palabras de
 *             sincronía de 32 bits separadas por 16 bits de puntos"; M]:
 *             BS1 32 bits 1010… · [A:16 · 0xA6C6] · puntos 0xAAAA · [~A:16 · ~0xA6C6].
 *             La palabra A (16 bits) fija el modo de Sync 2 y de los datos:
 *                0x870C → 1600 Bd, 2 niveles = 1600 bit/s (fase A)
 *                0xB068 → 1600 Bd, 4 niveles = 3200 bit/s (fases A y B)
 *                0x7B18 → 3200 Bd, 2 niveles = 3200 bit/s (fases A y C)
 *                0xDEA0 → 3200 Bd, 4 niveles = 6400 bit/s (fases A, B, C, D)
 *                0x4C7C → igual que 0xDEA0 (variante que lista [M])
 *             POLARIDAD: las palabras de sincronía llevan sus bits invertidos
 *             respecto a los datos (en la grabación P2000, la sincronía sale
 *             "al revés" y el FIW sale tal cual; igual que en [M]). Se llama bit
 *             "canónico" al de la sincronía; el bit de datos es su complemento.
 *   FIW     : palabra de información de trama (32 bits, 1600 Bd 2 niveles).
 *             Datos de 21 bits: bits 0-3 suma de comprobación (suma de los 5
 *             nibbles + bit 20 ≡ 0xF mod 16), 4-7 ciclo (0-14), 8-14 nº de
 *             trama (0-127).
 *   Sync 2  : 25 ms de puntos a la velocidad de símbolo de los datos (40
 *             símbolos a 1600 Bd, 80 a 3200 Bd) [T §2.3.1, M].
 *   Datos   : 1760 ms = 11 bloques de 160 ms. Por fase, un bloque son 8
 *             palabras de 32 bits entrelazadas: el bit j de la palabra w va en
 *             la posición 8j + w [T fig. 2-5].
 *
 * Reparto de los bits de una fase en símbolos [T §2.2, §2.3.2 y M]:
 *   2 niveles : un bit por símbolo; frecuencia alta = 1 [T fig. 2-1] (la
 *               sincronía, con la convención contraria, fija la polaridad).
 *   4 niveles : Gray, de −4,8 kHz a +4,8 kHz: 00, 01, 11, 10. El primer bit del
 *               símbolo es el SIGNO (fase "par" A/C) y el segundo el nivel
 *               INTERNO (1 si |f| = 1,6 kHz; fase "impar" B/D).
 *   3200 Bd   : los símbolos alternan entre el par (A,B) y el par (C,D); a 3200
 *               bit/s con 2 niveles solo viajan A y C.
 * Palabra: 21 bits de datos + 10 de BCH(31,21) + 1 de paridad par; el primer
 * bit transmitido es el LSB del campo de datos.
 */
#ifndef BLADERF_LAB_FLEX_H
#define BLADERF_LAB_FLEX_H

#include "../dsp/bits.h"
#include "../dsp/fec.h"

#define FLEX_BLOCKS 11
#define FLEX_WORDS_PER_PHASE (FLEX_BLOCKS * 8)
#define FLEX_BITS_PER_PHASE (FLEX_BLOCKS * 256)
#define FLEX_BS1 0xAAAAAAAAu
#define FLEX_SYNC_B 0xA6C6u /* tras el código A (validado con grabaciones reales) */
#define FLEX_SYNC_C 0xAAAAu /* y a continuación 16 bits de puntos, ~A y ~0xA6C6 */
#define FLEX_SYNC2_SLACK 24 /* margen (símbolos) de búsqueda de Sync 2 a cada lado */

typedef struct {
    uint16_t code;
    int bps;       /* bit/s totales */
    int baud;      /* símbolos/s de Sync 2 y datos: 1600 o 3200 */
    int levels;    /* 2 o 4 */
    int phases;    /* nº de fases transmitidas */
    int8_t ph[4];  /* índices de fase (0 = A … 3 = D) */
    const char *name;
} flex_mode_t;

#define FLEX_NMODES 5       /* con el alias 0x4C7C */
#define FLEX_NMODES_TX 4    /* los que genera el modulador */

static const flex_mode_t FLEX_MODES[FLEX_NMODES] = {
    {0x870C, 1600, 1600, 2, 1, {0, -1, -1, -1}, "1600/2"},
    {0x7B18, 3200, 3200, 2, 2, {0, 2, -1, -1}, "3200/2"},
    {0xB068, 3200, 1600, 4, 2, {0, 1, -1, -1}, "3200/4"},
    {0xDEA0, 6400, 3200, 4, 4, {0, 1, 2, 3}, "6400/4"},
    {0x4C7C, 6400, 3200, 4, 4, {0, 1, 2, 3}, "6400/4"},
};

/* Símbolos de Sync 2 y de datos de un modo. */
static inline int flex_sync2_syms(const flex_mode_t *m) { return m->baud * 25 / 1000; }
static inline int flex_data_syms(const flex_mode_t *m) { return m->baud * 1760 / 1000; }

/* Final de Sync 2 (8 símbolos, nivel en el dominio canónico: > 0 = bit 1 de la sincronía),
 * leído en grabaciones reales de 6400/4; el 5º símbolo es de nivel interno. */
static const int8_t FLEX_SYNC2_TAIL[8] = {3, 3, 3, -3, 1, -3, -3, -3};

/* Índice de símbolo de datos que lleva el bit c de la fase de letra L (0 = A…). */
static inline int flex_sym_index(const flex_mode_t *m, int L, int c)
{
    return m->baud == 3200 ? 2 * c + (L >> 1) : c;
}

/* Nivel ±3 / ±1 (en unidades de 1600 Hz) del símbolo con bits (a, b): a = bit de
 * signo (fases A, C), b = bit interno (B, D). En 2 niveles solo cuenta a. */
static inline int flex_level_of(int levels, int a, int b)
{
    if (levels == 2)
        return a ? 3 : -3;
    return a ? (b ? 1 : 3) : (b ? -1 : -3);
}

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

/* Direcciones. Palabra corta: 0x8001 ≤ w ≤ 0x1E0000 → capcode = w − 0x8000 (1…1 933 312).
 * Larga (2 palabras en el campo de direcciones y 2 en el de vectores): primera palabra a1 ∈ [1, 0x8000]
 * y segunda a2 ≥ 0x1DFFFF (el máximo, 4 297 068 542, da a2 = 0x1DFFFF); capcode = a1 + ((a2 ^ 0x1FFFFF) << 15) + 0x1F9000, hasta 4 297 068 542.
 * La fórmula es la de multimon-ng y da los mismos capcodes que él en las grabaciones reales de 6400
 * bit/s (p. ej. a1 = 0x4F5D, a2 = 0x1FFF60 → 7 298 909). Los rangos exactos de a1/a2 no están
 * confirmados fuera de lo observado (a2 entre 0x1FFF00 y 0x1FFFFF). */
static inline bool flex_addr_is_short(uint32_t w) { return w >= 0x8001u && w <= 0x1E0000u; }
static inline bool flex_addr_is_long1(uint32_t w) { return w >= 1u && w <= 0x8000u; }
static inline bool flex_addr_is_long2(uint32_t w) { return w >= 0x1DFFFFu && w <= 0x1FFFFFu; }
static inline int64_t flex_long_capcode(uint32_t a1, uint32_t a2)
{
    return (int64_t)a1 + ((int64_t)(a2 ^ 0x1FFFFFu) << 15) + 0x1F9000;
}

#endif
