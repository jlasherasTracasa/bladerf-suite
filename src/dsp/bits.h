/*
 * bits.h — Utilidades de bits para decodificadores y moduladores.
 *
 * Dos representaciones conviven en el proyecto:
 *   - "array de bits": un bit por byte (0 o 1), cómodo tras el slicer.
 *   - "empaquetado": 8 bits por byte. MSB-first = el primer bit transmitido va
 *     en el bit 7 del byte 0 (Mode S, AIS tras HDLC invertido, POCSAG...);
 *     LSB-first = el primer bit va en el bit 0 (HDLC/AX.25, ACARS...).
 *
 * Numeración "de libro" (getbits de adsb.c): bits_get(m, a, b) toma los bits
 * a..b INCLUSIVE, numerados desde 1 y MSB-first, para poder comparar el código
 * con las tablas del estándar.
 *
 * NRZI (Non-Return-to-Zero Inverted): el dato se codifica en las TRANSICIONES.
 *   HDLC/AX.25/AIS: un 0 = transición, un 1 = sin cambio (one_changes = 0).
 *   Otros (USB, algunos sistemas): un 1 = transición (one_changes = 1).
 *
 * Desaleatorizadores LFSR (registro de desplazamiento con realimentación).
 * Un polinomio 1 + Σ x^k (k en 'taps') se describe con una máscara donde el
 * bit (k-1) vale 1 si aparece x^k. El registro guarda las últimas 'order'
 * muestras de la secuencia: el bit (k-1) del estado es la muestra retrasada k.
 *
 *   Aditivo (síncrono):  p[n] = XOR_k p[n-k];  out[n] = in[n] ^ p[n].
 *       La secuencia p es independiente del dato (PRBS). Aleatorizar y
 *       desaleatorizar son la MISMA operación y hay que sincronizar el estado
 *       inicial. Ej.: WiFi 802.11a/g x^7+x^4+1 (máscara 0x48, estado inicial
 *       0x7F), DVB 1+x^14+x^15 (0x2000|0x4000 = 0x6000, init 100101010000000b),
 *       dispersión de energía DAB x^9+x^5+1 (0x110, init 0x1FF).
 *   Multiplicativo (autosincronizante): el registro contiene la salida del
 *       aleatorizador:  scr: out[n] = in[n] ^ XOR_k out[n-k]
 *                       desc: out[n] = in[n] ^ XOR_k in[n-k]
 *       Se resincroniza solo tras 'order' bits (G3RUH 9600: 1+x^12+x^17,
 *       máscara 0x10800; 802.11b: x^7+x^4+1).
 */
#ifndef BLADERF_LAB_BITS_H
#define BLADERF_LAB_BITS_H

#include "../common.h"

/* ---- Empaquetado ------------------------------------------------------- */
/* bits[0..n-1] (0/1) -> ceil(n/8) bytes; los huecos finales se rellenan con 0. */
void bits_pack_msb(const uint8_t *bits, int n, uint8_t *out);
void bits_pack_lsb(const uint8_t *bits, int n, uint8_t *out);
/* Inverso: n bits de 'in' -> bits[0..n-1]. */
void bits_unpack_msb(const uint8_t *in, int n, uint8_t *bits);
void bits_unpack_lsb(const uint8_t *in, int n, uint8_t *bits);

/* ---- Acceso a campos (numeración de libro, desde 1, MSB-first) ---------- */
uint32_t bits_get(const uint8_t *m, int first, int last);           /* ≤ 32 bits */
void bits_set(uint8_t *m, int first, int last, uint32_t value);
void bits_flip(uint8_t *m, int index0);                              /* índice desde 0 */

/* ---- Inversión y conteo ------------------------------------------------- */
uint8_t  bits_reverse8(uint8_t v);
uint32_t bits_reverse(uint32_t v, int nbits);   /* invierte los nbits bajos (1..32) */
int      bits_popcount32(uint32_t v);
int      bits_popcount64(uint64_t v);
int      bits_popcount_buf(const uint8_t *buf, int nbytes);
int      bits_parity32(uint32_t v);             /* 1 si hay nº impar de unos */
/* Distancia de Hamming entre dos buffers de nbytes bytes. */
int      bits_hamming_dist(const uint8_t *a, const uint8_t *b, int nbytes);

/* Busca 'pattern' (pat_len ≤ 64 bits, MSB = primer bit) en un array de bits a
 * partir de 'start', admitiendo hasta max_err bits distintos. Devuelve el índice
 * del primer bit del patrón o −1. */
int bits_find_sync(const uint8_t *bits, int n, uint64_t pattern, int pat_len, int max_err, int start);

/* ---- NRZI ---------------------------------------------------------------- */
/* 'prev' = nivel previo a in[0]. Devuelve el último nivel (para encadenar). */
int nrzi_decode(const uint8_t *in, int n, uint8_t *out, int prev, int one_changes);
int nrzi_encode(const uint8_t *in, int n, uint8_t *out, int prev, int one_changes);

/* ---- LFSR aleatorizador / desaleatorizador ------------------------------ */
typedef enum { LFSR_ADDITIVE = 0, LFSR_MULTIPLICATIVE = 1 } lfsr_mode_t;

typedef struct {
    int order;         /* grado del polinomio (1..32) */
    uint32_t taps;     /* bit (k-1) = 1 si x^k está en el polinomio */
    uint32_t state;    /* bit (k-1) = muestra retrasada k */
    lfsr_mode_t mode;
} lfsr_t;

void lfsr_init(lfsr_t *l, int order, uint32_t taps, uint32_t init_state, lfsr_mode_t mode);
/* Presets: WiFi 802.11 (x^7+x^4+1, estado inicial 'seed' 1..127), DVB-T/S
 * (1+x^14+x^15, etapas 1..15 = 100101010000000b, estado 0x0A9), DAB (x^9+x^5+1, init 0x1FF),
 * G3RUH (1+x^12+x^17 multiplicativo). */
void lfsr_init_wifi(lfsr_t *l, uint32_t seed);
void lfsr_init_dvb(lfsr_t *l);
void lfsr_init_dab(lfsr_t *l);
void lfsr_init_g3ruh(lfsr_t *l);
/* Siguiente bit de la PRBS (solo modo aditivo; avanza el estado). */
int lfsr_prbs_bit(lfsr_t *l);
/* Procesa un bit. descramble=1 solo importa en modo multiplicativo. */
int lfsr_bit(lfsr_t *l, int in, int descramble);
/* Procesa n bits (un bit por byte). in y out pueden ser el mismo buffer. */
void lfsr_process(lfsr_t *l, const uint8_t *in, int n, uint8_t *out, int descramble);
/* Procesa bytes con los bits en orden MSB-first (o LSB-first si msb_first=0). */
void lfsr_process_bytes(lfsr_t *l, const uint8_t *in, int nbytes, uint8_t *out, int descramble, int msb_first);

#endif
