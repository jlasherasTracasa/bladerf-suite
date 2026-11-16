/*
 * crc.h — CRC genérico (modelo de Rocksoft / catálogo de Greg Cook, "reveng").
 *
 * Un CRC de 'width' bits es el resto de dividir el mensaje (como polinomio
 * sobre GF(2)) por un polinomio generador G(x) de grado 'width':
 *
 *     CRC = ( M(x)·x^width  mod  G(x) )    con las variantes del modelo:
 *
 *   poly    G(x) sin el término x^width (p. ej. CCITT = 0x1021)
 *   init    valor inicial del registro
 *   refin   1 = cada byte entra por el bit MENOS significativo primero
 *   refout  1 = se invierte el registro antes de aplicar xorout
 *   xorout  máscara XOR final
 *   check   CRC de la cadena ASCII "123456789" (para autopruebas)
 *
 * Implementación: tabla de 256 entradas (un byte por paso). En los modelos
 * refin (la mayoría de los de enlace de datos: HDLC, Kermit, CRC-32) el
 * registro se mantiene "reflejado" y se desplaza a la derecha; en los demás se
 * desplaza a la izquierda. Las anchuras 1..32 están soportadas.
 *
 * Uso típico (AX.25/HDLC, trama recibida con FCS al final, LSB primero):
 *     crc_t c; crc_init(&c, crc_preset("CRC-16/X-25"));
 *     uint32_t fcs = crc_compute(&c, data, n);
 * Comprobación "por residuo": alimentar datos + CRC (en el orden de la trama)
 * deja el registro en una constante ("residue") que no depende del mensaje
 * (X-25: registro 0xF0B8 antes de xorout).
 *
 * Bits sueltos (tramas cuya longitud no es múltiplo de 8: GSM, LTE, DMR):
 * crc_update_bit() procesa un bit en orden de TRANSMISIÓN. Con refin=1 ese
 * orden es LSB primero dentro de cada byte.
 */
#ifndef BLADERF_LAB_CRC_H
#define BLADERF_LAB_CRC_H

#include "../common.h"

typedef struct {
    const char *name;     /* nombre del catálogo */
    const char *alias;    /* nombre alternativo o NULL */
    int width;
    uint32_t poly, init;
    bool refin, refout;
    uint32_t xorout;
    uint32_t check;       /* CRC de "123456789" */
} crc_params_t;

typedef struct {
    crc_params_t p;
    uint32_t mask;
    uint32_t rpoly;       /* poly reflejado (modelos refin) */
    uint32_t table[256];
} crc_t;

/* Inicializa (construye la tabla). Devuelve 0 o −1 si width fuera de 1..32. */
int crc_init(crc_t *c, const crc_params_t *p);

/* Presets con nombre (catálogo reveng + Mode S). Se busca por nombre o alias,
 * sin distinguir mayúsculas: "CRC-16/X-25", "HDLC", "CRC-16/CCITT-FALSE",
 * "CRC-16/KERMIT", "CRC-16/GSM", "CRC-24/MODES", "CRC-24/LTE-A", "CRC-24/LTE-B",
 * "CRC-32", "CRC-8"... Devuelve NULL si no existe. */
const crc_params_t *crc_preset(const char *name);
const crc_params_t *crc_preset_at(int index);   /* NULL al acabar: para listar */

/* Uso incremental. El registro 'reg' es opaco (dominio reflejado si refin). */
uint32_t crc_start(const crc_t *c);
uint32_t crc_update(const crc_t *c, uint32_t reg, const uint8_t *data, size_t n);
uint32_t crc_update_bit(const crc_t *c, uint32_t reg, int bit);
uint32_t crc_finish(const crc_t *c, uint32_t reg);   /* aplica refout y xorout */

/* Todo de una vez, sobre bytes. */
uint32_t crc_compute(const crc_t *c, const uint8_t *data, size_t n);
/* Sobre un array de bits (uno por byte) en orden de transmisión. */
uint32_t crc_compute_bits(const crc_t *c, const uint8_t *bits, size_t nbits);

/* Conveniencia para Mode S: CRC-24 (poly 0xFFF409) de nbits_data/8 bytes. */
uint32_t crc24_modes(const uint8_t *msg, int nbytes);

#endif
