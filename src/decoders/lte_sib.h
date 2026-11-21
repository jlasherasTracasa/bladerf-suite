/*
 * lte_sib.h — Cadena de capa física LTE necesaria para leer SIB1 (el bloque de
 * información de sistema que anuncia operador, banda y área de seguimiento),
 * compartida por el decodificador (lte.c) y el modulador (mod_lte.c).
 *
 * Teoría resumida. SIB1 viaja en el PDSCH de la subtrama 5 de las tramas pares
 * (SFN mod 2 = 0; 3GPP TS 36.331 §5.2.1.2), repetida cada 20 ms. Para llegar a
 * él hay que recorrer, en la misma subtrama, el camino que sigue un móvil:
 *
 *  1. PCFICH (TS 36.211 §6.7): dice cuántos símbolos OFDM ocupa el control (CFI =
 *     1..3). 32 bits = palabra de código de la tabla 36.212 5.3.4-1, aleatorizada
 *     con c_init = (⌊ns/2⌋+1)(2N_ID+1)2^9 + N_ID, QPSK, 4 REG repartidos por todo el
 *     ancho a partir de N_ID mod 2N_RB.
 *  2. PDCCH (36.211 §6.8, 36.212 §5.3.3): los DCI se reparten en CCE (36 símbolos
 *     QPSK = 9 REG de 4 RE cada uno). Los REG libres de PCFICH y PHICH se
 *     entrelazan (matriz de 32 columnas con la permutación de §6.8.5) y se desplazan
 *     cíclicamente N_ID posiciones. Cada DCI = carga útil + CRC-16 enmascarado con el
 *     RNTI (SI-RNTI = 0xFFFF para SIB1), convolucional tail-biting 1/3 y adaptación
 *     de velocidad hasta 72·L bits (L = 4 u 8 CCE en el espacio común, 36.213
 *     §9.1.1). El receptor no sabe dónde está: decodifica a ciegas los 4 + 2
 *     candidatos y se queda con el que cumple CRC ⊕ máscara = 0xFFFF.
 *  3. DCI 1A (36.212 §5.3.3.1.3): dice qué bloques de recurso (RIV, §7.1.6.3), MCS
 *     (= I_TBS, QPSK), RV (2 bits) y, con SI-RNTI, N_PRB^1A = 2 ó 3 (bit bajo de
 *     TPC) para la tabla de tamaños de bloque de 36.213 §7.1.7.2.1.
 *  4. PDSCH (36.211 §6.3, §6.4): bits aleatorizados con c_init = n_RNTI·2^14 +
 *     ⌊ns/2⌋·2^9 + N_ID (n_RNTI = 0xFFFF), QPSK, diversidad de transmisión (SFBC,
 *     Alamouti) con 2 puertos, sobre los RE que no son control, CRS ni PSS/SSS.
 *  5. DL-SCH (36.212 §5.1): CRC-24A + turbocódigo (dos RSC de 8 estados g0 = 013,
 *     g1 = 015 y entrelazador QPP) + adaptación de velocidad con búfer circular y RV.
 *     El decodificador es max-log-MAP con 8 iteraciones y parada anticipada por CRC.
 *  6. SIB1 (36.331 §6.2.2) está codificado en ASN.1 UPER (X.691): PLMN → MCC/MNC,
 *     trackingAreaCode, cellIdentity, banda, ventana de SI y lista de planificación.
 *
 * Convención de soft: valor > 0 ⇒ bit 0 (ya desaleatorizado). Los bits son un
 * entero 0/1 por byte. No se soportan CP extendido, TDD ni 4 puertos (aviso en
 * docs/protocolos/lte.md).
 */
#ifndef BLADERF_LAB_LTE_SIB_H
#define BLADERF_LAB_LTE_SIB_H

#include "../common.h"

#define LTE_SI_RNTI 0xFFFF

/* ---------------------------------------------------------------------- */
/*  CRC y DL-SCH                                                           */
/* ---------------------------------------------------------------------- */
/* gCRC24A = D^24+D^23+D^18+D^17+D^14+D^11+D^10+D^7+D^6+D^5+D^4+D^3+D+1 (36.212 §5.1.1). */
uint32_t lte_crc24a(const uint8_t *bits, int n);

/* TBS de DCI 1A con QPSK: I_TBS = 0..9 y N_PRB = 1..110 (36.213 tabla 7.1.7.2.1-1). 0 si no válido. */
int lte_tbs_qpsk_get(int itbs, int nprb);
/* TBS de DCI 1C (tabla 7.1.7.2.3-1). */
int lte_tbs_1c_get(int mcs);

/* Codifica un bloque de transporte de 'tbs' bits (CRC-24A + turbo + adaptación de
 * velocidad) y escribe E bits (sin aleatorizar). −1 si tbs+24 no es un tamaño QPP o supera 6144. */
int lte_dlsch_encode(const uint8_t *tb, int tbs, int E, int rv, uint8_t *out);
/* Decodifica E valores blandos (positivo = bit 0). true si el CRC-24A cuadra; tb[tbs] = bits. */
bool lte_dlsch_decode(const float *soft, int E, int tbs, int rv, int iters, uint8_t *tb);

/* ---------------------------------------------------------------------- */
/*  Distribución del canal de control (PCFICH / PHICH / PDCCH)             */
/* ---------------------------------------------------------------------- */
typedef struct {
    int l;          /* símbolo OFDM dentro de la subtrama (0..3) */
    int k0;         /* primera subportadora del REG (alineada a 6 u 4) */
    int k[4];       /* subportadoras (0 = la más baja del ancho completo) */
} lte_reg_t;

typedef struct {
    int pci, nrb, ports, phich_res;
    int nregs;
    lte_reg_t *regs;
    int pcfich[4];          /* índices de REG del PCFICH */
    int *pdcch[3];          /* por CFI-1: lista ordenada de REG tras entrelazado y desplazamiento */
    int npdcch[3];          /* REG útiles (múltiplo de 9) */
} lte_ctrl_t;

/* Símbolos de control para un CFI: CFI (N_RB > 10) o CFI+1 (N_RB ≤ 10), 36.211 §6.7. */
int lte_ctrl_symbols(int nrb, int cfi);
int lte_ctrl_init(lte_ctrl_t *c, int pci, int nrb, int ports, int phich_res);
void lte_ctrl_free(lte_ctrl_t *c);
/* PHICH ocupa REG que el PDCCH no usa: nº de grupos = ⌈Ng·N_RB/8⌉ (36.211 §6.9). */
int lte_phich_groups(int nrb, int phich_res);

/* Palabra de código del PCFICH (32 bits) para CFI 1..3 (36.212 tabla 5.3.4-1). */
void lte_pcfich_cw(int cfi, uint8_t bits[32]);
/* c_init de la aleatorización del PCFICH y del PDCCH en la subtrama sf (0..9). */
uint32_t lte_pcfich_cinit(int pci, int sf);
uint32_t lte_pdcch_cinit(int pci, int sf);
uint32_t lte_pdsch_cinit(int pci, int sf, int rnti);

/* ---------------------------------------------------------------------- */
/*  DCI                                                                    */
/* ---------------------------------------------------------------------- */
int lte_riv_bits(int nrb);                      /* ⌈log2(N(N+1)/2)⌉ */
int lte_dci1a_size(int nrb);                    /* FDD, con relleno al tamaño del formato 0 */
int lte_dci1c_size(int nrb);
/* Resultado de interpretar un DCI 1A localizado con SI-RNTI. */
typedef struct {
    bool valid, distributed, is1c, gap2;
    int mcs, rv, harq, nprb1a;    /* nprb1a = 2 ó 3 */
    int riv;                      /* campo de asignación sin interpretar */
    int rb_start, nprb;           /* asignación localizada (RIV ya decodificado) */
} lte_dci1a_t;
int lte_dci1a_pack(const lte_dci1a_t *d, int nrb, uint8_t *bits);       /* devuelve el nº de bits */
int lte_dci1a_unpack(const uint8_t *bits, int nbits, int nrb, lte_dci1a_t *d);
/* DCI 1C (36.212 §5.3.3.1.4): [gap (N_RB ≥ 50)] RIV(N_VRB^gap1/N_step) MCS(5). */
int lte_dci1c_pack(const lte_dci1a_t *d, int nrb, uint8_t *bits);
int lte_dci1c_unpack(const uint8_t *bits, int nbits, int nrb, lte_dci1a_t *d);
/* Asignación de tipo 2 (RIV) → PRB ocupados en cada ranura (36.211 §6.2.3.2, 36.213 §7.1.6.3). Con
 * 'distributed' los VRB se entrelazan (matriz de 4 columnas) y la segunda ranura salta N_VRB/2 PRB.
 * step = 1 (DCI 1A) o N_RB^step (DCI 1C: 2 si N_RB < 50, si no 4). Devuelve el nº de PRB o −1. */
int lte_type2_prb(int nrb, bool distributed, bool gap2, int step, int riv, uint8_t mask[2][110]);
/* N_VRB^DL con VRB distribuidos (36.211 §6.2.3.2): 2·min(N_gap, N_RB−N_gap) con gap 1. */
int lte_type2_nvrb(int nrb, bool gap2);
/* RIV (36.213 §7.1.6.3) */
int lte_riv_encode(int nrb, int start, int len);
bool lte_riv_decode(int nrb, int riv, int *start, int *len);

/* DCI → E bits (72·L) con CRC-16 enmascarado con 'rnti' (36.212 §5.3.3.2 – §5.3.3.4). */
int lte_dci_encode(const uint8_t *dci, int a, uint16_t rnti, int E, uint8_t *out);
/* E soft (positivo = bit 0, desaleatorizado) → a bits. Devuelve CRC ⊕ bits de máscara (= RNTI) o −1. */
int lte_dci_decode(const float *soft, int E, int a, uint8_t *dci);

/* ---------------------------------------------------------------------- */
/*  RE del PDSCH                                                           */
/* ---------------------------------------------------------------------- */
/* Lista de RE (símbolo 0..13 de la subtrama y subcarrera) de una asignación localizada
 * de la subtrama 5 FDD, en el orden de mapeo (primero k, luego l; 36.211 §6.3.5):
 * fuera de los símbolos de control, de las CRS de los 'ports' puertos y de los RE de PSS/SSS
 * (6 RB centrales de los símbolos 5 y 6). Devuelve el nº de RE (≤ cap). */
int lte_pdsch_res(int pci, int nrb, int ports, int cfi, int rb_start, int nprb, bool sf5_sync, int *ls, int *ks,
                  int cap);
/* Igual con máscaras de PRB por ranura (asignación distribuida). */
int lte_pdsch_res_mask(int pci, int nrb, int ports, int cfi, const uint8_t mask[2][110], bool sf5_sync, int *ls,
                       int *ks, int cap);

/* CRS completas: r(m') de la ranura ns y el símbolo l para 2·nrb pilotos (36.211 §6.10.1.1). */
void lte_crs_full(int pci, int ns, int l, int nrb, bool ext, cf32 *r);
/* Subcarrera de la primera CRS del puerto en el símbolo l de la ranura (v + N_ID mod 6). */
int lte_crs_k0(int pci, int port, int l);

/* ---------------------------------------------------------------------- */
/*  SIB1 (36.331 §6.2.2), ASN.1 UPER                                       */
/* ---------------------------------------------------------------------- */
typedef struct {
    int mcc[3], mnc[3], mnc_len;
    bool reserved;                  /* cellReservedForOperatorUse */
} lte_plmn_t;

typedef struct {
    int nsi;
    int period_idx;                 /* si-Periodicity: 0..6 = 8,16,32,64,128,256,512 tramas */
    int nsib, sib[16];              /* tipos (3 = SIB3, …) */
} lte_sched_t;

typedef struct {
    int nplmn;
    lte_plmn_t plmn[6];
    int tac;
    uint32_t cell_id;               /* 28 bits */
    bool barred, intra_freq_not_allowed, csg_ind;
    int q_rxlevmin;                 /* INTEGER (−70..−22), en pasos de 2 dB */
    bool has_pmax;
    int p_max;                      /* −30..33 dBm */
    int band;                       /* freqBandIndicator 1..64 */
    int nsched;
    lte_sched_t sched[8];
    int si_window_idx;              /* 0..6 = 1,2,5,10,15,20,40 ms */
    int value_tag;
} lte_sib1_t;

/* Escribe el mensaje BCCH-DL-SCH (CHOICE c1 → SIB1) en bits; devuelve el nº de bits. */
int lte_sib1_encode(const lte_sib1_t *s, uint8_t *bits, int cap);
/* Interpreta los bits; −1 si no es un SIB1 coherente (rangos, MCC/MNC no decimales…). */
int lte_sib1_decode(const uint8_t *bits, int nbits, lte_sib1_t *s);
int lte_si_window_ms(int idx);
int lte_si_period_rf(int idx);

#endif
