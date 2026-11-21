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
/* 23 octetos → 456 bits codificados (Fire(40) + convolucional 1/2), SIN entrelazar.
 * Orden de los bits de cada octeto: el bit 1 (LSB) del octeto va PRIMERO (45.003 §4.1.1: d(8i+j) =
 * bit j+1 del octeto i+1; contrastado con la grabación real, donde el MSB primero daba 0x60 en vez
 * del discriminador de protocolo 0x06). */
void gsm_bcch_encode(const uint8_t l3[23], uint8_t bits[456]);
int gsm_bcch_decode(const float *soft, uint8_t l3[23]);     /* 0 si el código Fire cuadra */
/* Entrelazado de bloque sobre 4 ráfagas: posición de la ráfaga B (0..3) y bit j (0..113)
 * que ocupa el bit k (0..455). */
static inline void gsm_interleave_pos(int k, int *b, int *j)
{
    *b = k & 3;
    *j = 2 * ((49 * k) % 57) + ((k & 7) >> 2);
}

/* ---- Mensajes del nivel 3: System Information (44.018 §9.1.31–9.1.36, IE de §10.5.2) ---- */
#define GSM_SI1 0x19      /* §9.1.31: Cell Channel Description + RACH */
#define GSM_SI2 0x1A      /* §9.1.32: BA list (vecinas) + NCC permitidos + RACH */
#define GSM_SI2BIS 0x02   /* §9.1.33 */
#define GSM_SI2TER 0x03   /* §9.1.34 */
#define GSM_SI3 0x1B      /* §9.1.35: Cell Identity, LAI, CCCH, opciones, selección, RACH */
#define GSM_SI4 0x1C      /* §9.1.36: LAI, selección, RACH */

typedef struct {
    int type;               /* tipo de mensaje RR (0x1B = SI3, 0x1C = SI4, …) */
    int mcc, mnc, mnc_digits, lac, ci;   /* ci = −1 si el mensaje no lo lleva */
    bool lai_ok;
    /* Cell Selection Parameters (10.5.2.4), SI3/SI4 */
    bool sel_ok;
    int hyst_db, txpwr_max_cch, rxlev_min_dbm, acs, neci;
    /* Control Channel Description (10.5.2.11), SI3 */
    bool cc_ok;
    int att, bs_ag_blks_res, ccch_conf, bs_pa_mfrms, t3212_decih;
    /* Cell Options BCCH (10.5.2.3), SI3 */
    bool opt_ok;
    int pwrc, dtx, radio_link_timeout;
    /* RACH Control Parameters (10.5.2.29) */
    bool rach_ok;
    int max_retrans, tx_integer, cell_bar, reest_allowed;
    unsigned ac_barred;     /* bit n = clase de acceso n restringida (C0..C15) */
    /* Lista de ARFCN: Cell Channel Description (SI1) o BA/extended (SI2, 2bis, 2ter) */
    bool arfcn_ok;          /* false: formato distinto del «bit map 0» (no decodificado) */
    int arfcn_fmt;          /* octeto 1 de la lista (bits de formato) */
    int ba_ind;             /* −1 si no aplica */
    int n_arfcn;
    uint16_t arfcn[124];
    int ncc_permitted;      /* SI2: máscara de 8 bits; −1 si no aplica */
} gsm_si_t;
/* Devuelve −1 si el mensaje no es RR/está corrupto; 0 si lo ha interpretado (o no es SI). */
int gsm_si_parse(const uint8_t l3[23], gsm_si_t *si);
/* SI1/2/2bis/2ter/3/4 con un LAI dado y valores típicos (los de GSM_SI_DEF_*), relleno 0x2B. */
void gsm_si_build(int type, int mcc, int mnc, int mnc_digits, int lac, int ci, uint8_t l3[23]);
/* SI1/SI2/SI2bis/SI2ter con una lista de ARFCN (formato bit map 0: 1..124). */
void gsm_si_build_arfcn(int type, const int *arfcn, int n, uint8_t l3[23]);
/* ARFCN a partir de la frecuencia de bajada (45.005 §2): GSM 850, P/E/R-GSM 900, DCS 1800, PCS
 * 1900; −1 si no cae en ninguna banda. */
int gsm_arfcn_from_hz(double hz);

/* Valores típicos que escribe gsm_si_build (los comprueba el test). */
#define GSM_SI_DEF_HYST_DB 4
#define GSM_SI_DEF_TXPWR 5
#define GSM_SI_DEF_RXLEV_MIN_DBM (-104)
#define GSM_SI_DEF_T3212 10

#endif
