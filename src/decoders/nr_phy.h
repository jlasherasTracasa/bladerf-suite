/*
 * nr_phy.h — Secuencias de sincronía de 5G NR (3GPP TS 38.211 §7.4.2) y rejilla GSCN
 * (TS 38.104 §5.4.3), compartidas por el decodificador (nr.c) y el modulador (mod_nr.c).
 *
 * Bloque SS/PBCH (SSB): 4 símbolos OFDM × 240 subportadoras (20 RB).
 *   símbolo 0: PSS en las subportadoras 56..182 (127 BPSK, m-secuencia);
 *   símbolo 1: PBCH en 0..239;
 *   símbolo 2: SSS en 56..182 y PBCH en 0..47 y 192..239;
 *   símbolo 3: PBCH en 0..239.
 * La subportadora 120 (RE 0 del RB 10) está en SS_REF, la frecuencia del GSCN: la
 * subportadora k cae en f = SS_REF + (k − 120)·SCS. La PSS/SSS ocupan por tanto
 * f ∈ [−64, +62]·SCS alrededor de SS_REF.
 */
#ifndef BLADERF_LAB_NR_PHY_H
#define BLADERF_LAB_NR_PHY_H

#include "../common.h"

#define NR_SSB_SC 240
#define NR_PSS_K0 56      /* primera subportadora de la PSS/SSS dentro del SSB */
#define NR_SSB_REF 120    /* subportadora de SS_REF */

/* d_PSS(n) = 1 − 2x((n + 43·N_ID2) mod 127), x(i+7) = x(i+4) ⊕ x(i), x(6..0) = 1110110. */
void nr_pss_seq(int n_id2, int8_t d[127]);
/* d_SSS(n) = [1 − 2x0((n+m0) mod 127)]·[1 − 2x1((n+m1) mod 127)];
 * m0 = 15⌊N_ID1/112⌋ + 5·N_ID2, m1 = N_ID1 mod 112. */
void nr_sss_seq(int n_id1, int n_id2, int8_t d[127]);

/* Frecuencias de la rejilla de sincronía global (GSCN) dentro de [lo, hi] Hz, en
 * orden ascendente. Devuelve cuántas escribió (≤ max). */
int nr_gscn_candidates(double lo_hz, double hi_hz, int64_t *out, int max);

/* ---------------------------------------------------------------------- */
/*  PBCH (MIB): DMRS, polar y MIB                                          */
/* ---------------------------------------------------------------------- */
/* Campos del MIB (38.331 §6.2.2) más los 8 bits que añade la capa física (38.212 §7.1.1). */
typedef struct {
    int sfn;                    /* 0..1023 (6 bits del MIB + 4 de la capa física) */
    int hrf;                    /* semitrama: 0 = primera, 1 = segunda */
    int ssb_idx;                /* i_SSB (se deduce del DMRS; aquí solo para el modulador) */
    int scs_common;             /* 0 = 15 kHz (FR1), 1 = 30 kHz */
    int k_ssb;                  /* 0..31: ssb-SubcarrierOffset (4 bits del MIB + MSB de la capa física) */
    int dmrs_pos;               /* 2 ó 3: dmrs-TypeA-Position */
    int coreset0, ss0;          /* pdcch-ConfigSIB1: controlResourceSetZero, searchSpaceZero */
    bool barred;                /* cellBarred = barred */
    bool intra_freq_not_allowed;
} nr_mib_t;

#define NR_PBCH_RE 432          /* RE de datos del PBCH en un SSB (E = 864 bits) */

/* Secuencia DMRS del PBCH (38.211 §7.4.1.4.1) para ī_SSB: 144 QPSK, c_init = 2^11(ī+1)(⌊N_ID/4⌋+1) + 2^6(ī+1) + N_ID mod 4. */
void nr_dmrs_seq(int pci, int ibar, cf32 r[144]);
/* Codifica el MIB (CRC-24C, entrelazado, polar N = 512, adaptación E = 864, aleatorización con v) en 432 QPSK. */
void nr_pbch_encode(const nr_mib_t *m, int pci, int v, cf32 sym[NR_PBCH_RE]);
/* Decodifica 864 valores blandos (positivo = bit 0, antes de desaleatorizar). SCL con lista L = 8 y CRC-24C. */
bool nr_pbch_decode(const float *soft, int pci, int v, nr_mib_t *m);
/* Posición (símbolo 1..3, subportadora) del RE n-ésimo de datos del PBCH y del DMRS m-ésimo. */
void nr_pbch_re_pos(int pci, int n, int *l, int *k);
void nr_dmrs_re_pos(int pci, int m, int *l, int *k);

#endif
