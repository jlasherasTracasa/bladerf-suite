/*
 * hdlc.h — Desentramador / entramador HDLC común (AIS, APRS/AX.25).
 *
 * Teoría (ISO 13239, ITU-R M.1371 anexo 2 §3.2, AX.25 v2.2 §3):
 *   - Bandera de trama: 0x7E = 01111110 (seis unos seguidos). Se transmite el
 *     bit menos significativo primero.
 *   - Inserción de ceros (bit stuffing): tras cinco unos consecutivos del dato
 *     el emisor intercala un 0. El receptor lo elimina. Seis unos = bandera;
 *     siete o más = aborto de trama.
 *   - FCS: CRC-16/X-25 (CCITT reflejado, init 0xFFFF, xorout 0xFFFF) de todos
 *     los bits previos, transmitido con el bit menos significativo primero.
 *     Se calcula bit a bit, así que vale también para cargas cuya longitud
 *     no es múltiplo de 8 (AIS).
 *   - NRZI: el dato va en las transiciones (0 = cambia de nivel, 1 = igual).
 *     El receptor puede alimentarse con niveles de línea (hdlc_rx_push_level) o
 *     con bits ya decodificados (hdlc_rx_push_bit).
 *
 * El receptor guarda los bits de la trama en el orden de llegada (un bit por
 * byte). Al cerrarse una trama con FCS correcto devuelve el nº de bits de
 * carga (sin FCS) y los deja en h->bits[0..n).
 */
#ifndef BLADERF_LAB_HDLC_H
#define BLADERF_LAB_HDLC_H

#include "../common.h"

typedef struct {
    uint8_t *bits;      /* bits de la trama en curso (sin bandera, sin ceros insertados) */
    int cap, n;
    int ones;           /* unos consecutivos recibidos */
    int prev_level;     /* nivel NRZI previo */
    bool inframe;
    unsigned long frames_ok, frames_bad;   /* contadores: FCS correcto / incorrecto */
} hdlc_rx_t;

int hdlc_rx_init(hdlc_rx_t *h, int cap_bits);   /* 0 o -1 */
void hdlc_rx_free(hdlc_rx_t *h);
void hdlc_rx_reset(hdlc_rx_t *h);
/* Bit ya decodificado de NRZI. Devuelve nº de bits de carga si cierra una trama válida, o 0. */
int hdlc_rx_push_bit(hdlc_rx_t *h, int bit);
/* Nivel de línea (0/1) con decodificación NRZI integrada (HDLC: 0 = transición). */
int hdlc_rx_push_level(hdlc_rx_t *h, int level);
/* Empaqueta los 'nbits' bits de la carga en bytes, LSB primero (AX.25). */
void hdlc_rx_bytes(const hdlc_rx_t *h, int nbits, uint8_t *out);

/* Entramador: pre_flags banderas + carga (bits) + FCS con inserción de ceros +
 * post_flags banderas. Salida: bits ANTES de NRZI (aplicar nrzi_encode con
 * one_changes=0). Devuelve nº de bits escritos o −1 si no caben en cap. */
int hdlc_encode(const uint8_t *payload_bits, int nbits, int pre_flags, int post_flags, uint8_t *out, int cap);
/* Utilidad: bytes → bits LSB primero. */
int hdlc_bytes_to_bits(const uint8_t *bytes, int n, uint8_t *bits);

#endif
