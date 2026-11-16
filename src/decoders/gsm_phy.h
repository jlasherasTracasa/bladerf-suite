/*
 * gsm_phy.h — Estructura de ráfagas y codificación de canal de GSM (3GPP TS 45.001,
 * 45.002, 45.003) común al decodificador (gsm.c) y al modulador (mod_gsm.c).
 *
 * Numerología: velocidad de símbolo 270 833,33 Bd (= 13 MHz/48). Una trama TDMA son
 * 8 ranuras de 156,25 símbolos = 1250 símbolos = 4,615 ms; a 4 muestras/símbolo
 * (1 083 333 Hz) la ranura mide exactamente 625 muestras y la trama 5000. El
 * multitrama de control es de 51 tramas (FN mod 51); en la ranura 0 de la portadora
 * BCCH: FCCH en FN mod 51 = 0, 10, 20, 30, 40; SCH en 1, 11, 21, 31, 41; BCCH en 2–5
 * (los canales de aviso/acceso, CCCH, están en 6–9 y 12–19…: este código NO los toca).
 *
 * Bits de ráfaga: los bits d_k van a la modulación como b_k = 1 − 2·d_k. La
 * codificación diferencial de GSM (α_k = 1 − 2(d_k ⊕ d_{k−1})) hace que los símbolos de
 * Laurent a_k = j^k·b_k sean BPSK de d_k directamente (ver gsm.c).
 *
 * Posiciones en una ráfaga normal (148 bits): cola 0–2, datos 3–59, bit de robo 60,
 * entrenamiento 61–86, bit de robo 87, datos 88–144, cola 145–147. SCH: cola 0–2,
 * datos codificados 3–41, secuencia de entrenamiento ampliada (64) 42–105, datos
 * codificados 106–144, cola 145–147.
 */
#ifndef BLADERF_LAB_GSM_PHY_H
#define BLADERF_LAB_GSM_PHY_H

#include "../common.h"

#define GSM_RATE 1083333.3333
#define GSM_FB 270833.3333      /* velocidad de símbolo */
#define GSM_SPS 4
#define GSM_BURST_SYMS 148
#define GSM_SLOT 625            /* muestras por ranura a 4 sps */
#define GSM_FRAME 5000

extern const uint8_t GSM_TSC[8][26];
extern const uint8_t GSM_SCH_TRAIN[64];

/* ---- SCH (45.003 §4.7) ---- */
/* BSIC (6 bits: NCC·8 + BCC) y FN → 78 bits codificados (c(0..38) | c(39..77)). */
void gsm_sch_encode(int bsic, uint32_t fn, uint8_t bits[78]);
/* soft[78] en convención de fec.h (positivo = bit 1). Devuelve 0 si la paridad
 * cuadra y rellena bsic y fn (FN completo reconstruido de T1, T2, T3'), −1 si no. */
int gsm_sch_decode(const float *soft, int *bsic, uint32_t *fn);

/* ---- BCCH (45.003 §4.1) ---- */
/* 23 octetos → 456 bits codificados (Fire(40) + convolucional 1/2), SIN entrelazar. */
void gsm_bcch_encode(const uint8_t l3[23], uint8_t bits[456]);
int gsm_bcch_decode(const float *soft, uint8_t l3[23]);     /* 0 si el código Fire cuadra */
/* Entrelazado de bloque sobre 4 ráfagas: posición de la ráfaga B (0..3) y bit j (0..113)
 * que ocupa el bit k (0..455). */
static inline void gsm_interleave_pos(int k, int *b, int *j)
{
    *b = k & 3;
    *j = 2 * ((49 * k) % 57) + ((k & 7) >> 2);
}

/* ---- Mensajes del nivel 3 (44.018 §9.1) ---- */
typedef struct {
    int type;               /* tipo de mensaje RR (0x1B = SI3, 0x1C = SI4, …) */
    int mcc, mnc, mnc_digits, lac, ci;   /* ci = −1 si el mensaje no lo lleva */
    bool lai_ok;
} gsm_si_t;
int gsm_si_parse(const uint8_t l3[23], gsm_si_t *si);
/* SI3 (type 0x1B) o SI4 (0x1C) con un LAI dado; rellena el resto con valores típicos. */
void gsm_si_build(int type, int mcc, int mnc, int mnc_digits, int lac, int ci, uint8_t l3[23]);

#endif
