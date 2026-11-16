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

#endif
