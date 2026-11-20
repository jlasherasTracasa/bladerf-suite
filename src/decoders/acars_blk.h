/*
 * acars_blk.h — Bloque ACARS (ARINC 618): análisis, construcción y BCS.
 *
 * Lo comparten el decodificador de ACARS (VHF, MSK sobre AM), el modulador y
 * el decodificador de VDL2 (ACARS sobre AVLC, "AOA").
 *
 * Formato de un bloque en el aire (cada carácter = 7 bits + paridad IMPAR en el
 * bit 7; el primer bit transmitido es el menos significativo):
 *
 *   pre-key (≥16 unos) | '+' '*' | SYN SYN | SOH | Modo(1) | Matrícula(7) |
 *   TAK(1) | Etiqueta(2) | Id. de bloque(1) | STX | texto… | ETX o ETB |
 *   BCS(2) | DEL
 *
 *   Modo      '2' = categoría A habitual.
 *   Matrícula 7 caracteres, rellenos con '.' por la izquierda (p. ej. ".EC-MXY").
 *   TAK       acuse del bloque anterior (letra/dígito) o NAK (0x15).
 *   Etiqueta  2 caracteres; la etiqueta "Q0" aparece como '_' + DEL (0x7F).
 *   Bloque    enlace descendente (avión→tierra): dígito '0'..'9'; ascendente: letra.
 *   Texto     en bajada: nº de mensaje (4: 'M'+2 dígitos+letra), vuelo (6) y cuerpo.
 *   ETX/ETB   fin de mensaje / fin de bloque intermedio de un mensaje largo.
 *   BCS       CRC-16 (polinomio x^16+x^12+x^5+1, registro inicial 0, bits LSB
 *             primero = "CRC-16/KERMIT") calculado sobre Modo…ETX/ETB, enviado con
 *             el byte bajo primero. Alimentar al CRC los dos bytes del BCS deja
 *             el registro a 0. (Sin norma a mano: ver docs/protocolos/acars.md.)
 */
#ifndef BLADERF_LAB_ACARS_BLK_H
#define BLADERF_LAB_ACARS_BLK_H

#include "../common.h"

#define ACARS_SOH 0x01
#define ACARS_STX 0x02
#define ACARS_ETX 0x03
#define ACARS_SYN 0x16
#define ACARS_ETB 0x17
#define ACARS_NAK 0x15
#define ACARS_DEL 0x7F

typedef struct {
    char mode;
    char reg[8];        /* sin los '.' de relleno */
    char tak;           /* 0x15 = NAK */
    char label[3];
    char bid;
    bool downlink;      /* bid es un dígito */
    bool etb;           /* bloque intermedio */
    char msgno[5];      /* solo en bajada ("" si no) */
    char flight[7];
    char text[260];     /* cuerpo (sin nº de mensaje ni vuelo) */
    int text_len;
} acars_blk_t;

/* c[0..n-1] = Modo … ETX/ETB (sin SOH ni BCS), YA sin bit de paridad.
 * Devuelve 0 o −1 si la estructura no es válida. */
int acars_blk_parse(const uint8_t *c, int n, acars_blk_t *b);

/* BCS (CRC-16/KERMIT) de n bytes tal como van por el aire (con paridad). */
uint16_t acars_bcs(const uint8_t *c, int n);

/* Construye Modo … ETX/ETB (sin paridad) a partir de b; devuelve su longitud.
 * Para bajada compone 'msgno'+'flight'+'text' como texto. */
int acars_blk_build(const acars_blk_t *b, uint8_t *out, int cap);

/* Paridad impar de 7 bits en el bit 7. */
uint8_t acars_add_parity(uint8_t c);

#endif
