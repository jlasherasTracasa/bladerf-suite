/*
 * rds.h — Utilidades comunes del RDS (IEC 62106 / CENELEC EN 50067) compartidas
 * por el decodificador (rds.c), el modulador (tx/mod_rds.c), el decodificador de
 * DAB (tabla de caracteres EBU Latin) y los tests.
 *
 * Código de bloque (26,16)
 * ------------------------
 * Cada bloque RDS son 26 bits: 16 de datos m(x) y 10 de "palabra de control".
 * El código es cíclico acortado con el generador
 *
 *     g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1            (0x5B9)
 *
 * La palabra de control es  rem( m(x)·x^10 , g(x) )  XOR  d, donde d es la
 * "palabra de offset" del bloque (A, B, C, C', D). Los offsets sirven para que
 * el receptor sepa en qué bloque del grupo está (sincronía de bloque):
 *
 *     A = 0x0FC   B = 0x198   C = 0x168   C' = 0x350   D = 0x1B4  (10 bits)
 *
 * Como c(x) = m(x)·x^10 + rem(...) es múltiplo de g(x), el resto de dividir por
 * g(x) la palabra recibida de 26 bits es justo el offset (si no hay errores):
 * ese resto es el "síndrome". Un grupo son 4 bloques (A,B,C|C',D) = 104 bits,
 * y la tasa es 1187,5 bit/s (= 19 kHz / 16).
 *
 * Verificación: estas constantes (g, offsets) coinciden con las matrices de
 * paridad del anexo B de EN 50067 tal como las usa el decodificador abierto
 * "redsea" (síndromes 0x3D8, 0x3D4, 0x25C, 0x3CC, 0x258 con esa matriz H);
 * tests/test_rds.c lo comprueba de forma independiente.
 */
#ifndef BLADERF_LAB_RDS_H
#define BLADERF_LAB_RDS_H

#include "../common.h"

#define RDS_RATE 250000.0
#define RDS_BITRATE 1187.5

enum { RDS_OFF_A = 0, RDS_OFF_B = 1, RDS_OFF_C = 2, RDS_OFF_CP = 3, RDS_OFF_D = 4 };

/* Resto polinómico de v (nbits bits, MSB = coeficiente de mayor grado) entre g(x). */
uint32_t rds_rem(uint32_t v, int nbits);
/* Bloque de 26 bits (MSB primero) con los 16 bits de datos y el offset dado. */
uint32_t rds_block_encode(uint16_t data, int offset);
/* Devuelve el offset (RDS_OFF_*) cuyo síndrome coincide con el de w26, o −1. */
int rds_block_offset(uint32_t w26);
/* Offset esperado (palabra de 10 bits) de cada RDS_OFF_*. */
uint32_t rds_offset_word(int offset);

/* Nombre de PTY de la tabla europea (EN 50067 anexo F); "" si pty > 31. */
const char *rds_pty_name(int pty);

/* Carácter EBU Latin (TS 101 756 anexo C; DAB juego 0 y RDS anexo E) → UTF-8.
 * Escribe hasta 4 bytes + '\0' y devuelve su longitud. Con ascii=true los
 * códigos 0x20..0x7E se dejan como ASCII y los <0x20 pasan a espacio (RDS; lo que emiten casi todas las
 * emisoras de RDS); con ascii=false se aplica la tabla completa (DAB). */
int ebu_latin_to_utf8(uint8_t c, bool ascii, char out[5]);
/* Convierte n bytes y quita los espacios finales. dst ≥ 4n+1 bytes. */
void ebu_latin_string(const uint8_t *src, int n, bool ascii, char *dst);

/* Fecha juliana modificada (MJD) ↔ fecha civil (EN 50067 anexo G). */
void rds_mjd_to_date(int mjd, int *y, int *m, int *d);
int rds_date_to_mjd(int y, int m, int d);

#endif
