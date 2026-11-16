/*
 * dmr_fec.h — Códigos de canal de DMR (ETSI TS 102 361-1 V2.5.1, anexo B),
 * compartidos por el decodificador y el modulador.
 *
 * Todas las matrices generadoras se han transcrito de la norma (tablas B.11 a
 * B.18); NO son las automáticas de fec.c. En todas la palabra es [I | P]: los
 * bits de datos van primero (MSB = primer bit transmitido).
 *
 *   Golay (20,8,7)        B.3.1  tabla B.11   tipo de ranura (CC 4 b + tipo 4 b + 12 b)
 *   QR (16,7,6)           B.3.2  tabla B.12   EMB (CC 4, PI 1, LCSS 2, + 9 b)
 *   Hamming (13,9,3)      B.3.4  tabla B.14   columnas del BPTC(196,96)
 *   Hamming (15,11,3)     B.3.4  tabla B.15   filas del BPTC(196,96)
 *   Hamming (16,11,4)     B.3.4  tabla B.16   filas del LC embebido
 *   Hamming (7,4,3)       B.3.5  tabla B.17   TACT del CACH
 *   Reed-Solomon (12,9)   B.3.6  g(x) = (x+α)(x+α²)(x+α³) = x³+0E x²+38 x+40, GF(2^8) con 0x11D
 *   CRC-CCITT             B.3.8  FH = (x^16·M mod G) + I, G = x^16+x^12+x^5+1, registro inicial 0
 *   BPTC (196,96)         B.1.1  matriz 13×15, entrelazado (i·181) mod 196 (ec. B.1, tabla B.3)
 *   CACH                  B.4.1  figura B.9
 *   LC embebido           B.2.1  figura B.3 (Hamming 16,11 por filas + paridad por columnas)
 */
#ifndef BLADERF_LAB_DMR_FEC_H
#define BLADERF_LAB_DMR_FEC_H

#include "../common.h"

/* Inicializa las tablas (idempotente, seguro entre hilos). */
void dmr_fec_init(void);

/* Tipo de ranura: 20 bits (CC en los 4 altos del byte de datos). Devuelve nº de
 * bits corregidos (0..3) o −1. */
uint32_t dmr_slottype_encode(int cc, int dt);
int dmr_slottype_decode(uint32_t w20, int *cc, int *dt);

/* EMB: 16 bits. */
uint32_t dmr_emb_encode(int cc, int pi, int lcss);
int dmr_emb_decode(uint32_t w16, int *cc, int *pi, int *lcss);

/* TACT: 7 bits = AT TC LCSS(2) + 3 de paridad. Devuelve 0 si es válido (corrige 1 bit). */
uint32_t dmr_tact_encode(int at, int tc, int lcss);
int dmr_tact_decode(uint32_t w7, int *at, int *tc, int *lcss);

/* CACH de 24 bits (en orden temporal, bits[0] primero) a partir de TACT y de los
 * 17 bits de carga útil P(16..0) (payload[0] = P(16)). */
void dmr_cach_build(uint32_t tact7, const uint8_t *payload17, uint8_t *bits24);
void dmr_cach_split(const uint8_t *bits24, uint32_t *tact7, uint8_t *payload17);

/* BPTC(196,96): info = 96 bits I(95)..I(0) (info[0] = I(95)); out = 196 bits en
 * orden temporal. El decodificador corrige por filas/columnas (2 pasadas) y
 * devuelve el nº de bits corregidos, o −1 si quedan síndromes no nulos. */
void dmr_bptc_encode(const uint8_t *info96, uint8_t *out196);
int dmr_bptc_decode(const uint8_t *in196, uint8_t *info96);

/* RS(12,9): 3 bytes de paridad (p2,p1,p0) de 9 bytes. dmr_rs129_check corrige
 * hasta 1 byte; block = 12 bytes (datos + paridad sin máscara). Devuelve nº de
 * símbolos corregidos o −1. */
void dmr_rs129_parity(const uint8_t *d9, uint8_t *p3);
int dmr_rs129_check(uint8_t *block12);

/* CRC-CCITT de la norma: devuelve el campo FH (ya invertido) de n bytes. */
uint16_t dmr_crc_ccitt(const uint8_t *bytes, int n);

/* LC embebido: 9 bytes de LC → 4 fragmentos de 32 bits (burst B..E). El
 * decodificador devuelve el nº de bits corregidos o −1 (fallo de Hamming, de
 * paridad de columna o de suma de comprobación). */
void dmr_emblc_encode(const uint8_t *lc9, uint32_t *frag4);
int dmr_emblc_decode(const uint32_t *frag4, uint8_t *lc9);

/* Distancia mínima del código transcrito (0 Golay 20,8; 1 QR 16,7; 2 H 13,9; 3 H 15,11;
 * 4 H 16,11; 5 H 7,4): permite comprobar la transcripción de las matrices. */
int dmr_fec_dmin(int code);

#endif
