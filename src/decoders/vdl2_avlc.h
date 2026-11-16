/*
 * vdl2_avlc.h — Estructura de una trama AVLC (ICAO Doc 9776 / ETSI EN 301 841-1)
 * y su construcción/análisis. Compartido por el decodificador y el modulador.
 *
 *   dirección destino (4 octetos) | dirección origen (4) | control (1) |
 *   información (0..n) | FCS (2, CRC-16/X-25, byte bajo primero)
 *
 * Dirección: 4 octetos con el bit 0 de cada uno como extensión (0 = sigue otro,
 * 1 = último) y 7 bits útiles → 28 bits: val = Σ (oct[i] >> 1) << 7i.
 *   bits 0..2   tipo de dirección (1 aeronave, 4 estación de tierra, 7 difusión…)
 *   bit 3       indicador (en la dirección ORIGEN: C/R, 0 = orden, 1 = respuesta)
 *   bits 4..27  dirección ICAO de 24 bits
 * Control, como HDLC con módulo 8 (bit 0 es el menos significativo):
 *   I:  N(R) bits 7..5 | P bit 4 | N(S) bits 3..1 | 0
 *   S:  N(R) 7..5 | P/F 4 | tipo bits 3..2 (RR 0, RNR 1, REJ 2, SREJ 3) | 01
 *   U:  tipo en los bits 7..5 y 3..2 con P/F en 4: UI 0x03, XID 0xAF, TEST 0xE3,
 *       SABM 0x2F, DISC 0x43, UA 0x63, DM 0x0F, FRMR 0x87 (sin el bit P/F).
 * ACARS sobre AVLC (AOA): el campo de información empieza por 0x01 y sigue el
 * bloque ACARS (Modo, matrícula…ETX/ETB [+BCS]).
 */
#ifndef BLADERF_LAB_VDL2_AVLC_H
#define BLADERF_LAB_VDL2_AVLC_H

#include "../common.h"

typedef struct {
    uint32_t icao;
    int type;
    int flag;     /* bit 3 */
} avlc_addr_t;

typedef struct {
    avlc_addr_t dst, src;
    uint8_t ctrl;
    char ftype[8];          /* "I", "RR", "UI", "XID"… */
    int ns, nr, poll;
    const uint8_t *info;    /* apunta dentro de la trama */
    int info_len;
} avlc_t;

/* Devuelve 0 o −1 si la trama es demasiado corta o las extensiones no cuadran. */
int avlc_parse(const uint8_t *f, int n, avlc_t *a);
void avlc_addr_put(uint8_t *o, uint32_t icao, int type, int flag);
const char *avlc_type_name(int type);

#endif
