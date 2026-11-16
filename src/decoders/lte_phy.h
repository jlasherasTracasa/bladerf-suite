/*
 * lte_phy.h — Secuencias y codificación de capa física LTE (3GPP TS 36.211 /
 * 36.212) compartidas por el decodificador (lte.c) y el modulador (mod_lte.c).
 *
 * Solo se necesita lo que cabe en la banda de 1,92 Msps (FFT de 128, SCS de
 * 15 kHz): los 6 RB centrales (72 subcarreras), donde viven PSS, SSS, PBCH y las
 * CRS centrales. Convenciones:
 *
 *  - "ventana" j = 0..71: índice de las 72 subcarreras centrales (PBCH/CRS). La
 *    subcarrera DC no existe: j < 36 ↔ frecuencia f = j−36 (bins −36..−1) y
 *    j ≥ 36 ↔ f = j−35 (bins +1..+36), en unidades de 15 kHz (36.211 §6.12).
 *  - PSS/SSS (62 elementos n = 0..61) ocupan j = n+5 (36.211 §6.11).
 *  - Las CRS del centro usan siempre los índices m' = 104..115 de la secuencia
 *    r(m'), sea cual sea el ancho del canal (m' = m + N_RBmax − N_RB y la
 *    ventana empieza en m = N_RB − 6).
 */
#ifndef BLADERF_LAB_LTE_PHY_H
#define BLADERF_LAB_LTE_PHY_H

#include "../common.h"

#define LTE_N 128           /* FFT a 1,92 Msps */
#define LTE_FS 1920000.0
#define LTE_SLOT 960        /* muestras por ranura (0,5 ms) */
#define LTE_FRAME 19200
#define LTE_HALF 9600       /* separación de las PSS: 5 ms */

/* Rejillas de hipótesis: separación PSS–SSS y tipo de CP. */
typedef enum { LTE_FDD_N = 0, LTE_FDD_E = 1, LTE_TDD_N = 2, LTE_TDD_E = 3 } lte_grid_t;

static inline bool lte_grid_ext(int g) { return g == LTE_FDD_E || g == LTE_TDD_E; }
static inline bool lte_grid_tdd(int g) { return g >= LTE_TDD_N; }

int lte_nsym(bool ext);                     /* símbolos por ranura: 7 / 6 */
int lte_cp_len(bool ext, int l);            /* muestras de CP del símbolo l de la ranura */
int lte_sym_start(bool ext, int l);         /* inicio (con CP) del símbolo l dentro de la ranura */
/* Inicio de la parte útil de la PSS dentro de la trama (FDD: ranura 0 / TDD: subtrama 1) */
int lte_pss_pos(int grid);
/* Distancia (muestras) del inicio útil de la SSS al de la PSS que le sigue. */
int lte_sss_delta(int grid);

int lte_win_to_f(int j);                    /* frecuencia (−36..36, sin 0) de la ventana j */
int lte_f_to_bin(int f);                    /* bin de la FFT de 128 */

/* Secuencia Gold de 31 bits (36.211 §7.2) con inicialización c_init; n bits. */
void lte_gold(uint32_t cinit, int n, uint8_t *out);

/* PSS: 62 símbolos ZC con la raíz de N_ID2 (25, 29, 34). */
int lte_pss_root(int n_id2);
void lte_pss_seq(int n_id2, cf32 d[62]);
/* SSS: 62 valores ±1; half = 0 (subtrama 0) o 1 (subtrama 5). */
void lte_sss_seq(int n_id1, int n_id2, int half, int8_t d[62]);

/* CRS (36.211 §6.10.1): 12 valores del centro (m' = 104..115) para la ranura ns
 * (0..19) y el símbolo l. */
void lte_crs_seq(int pci, int ns, int l, bool ext, cf32 r[12]);
/* Posición j (0..71) del primer RE de CRS del puerto p (0..3) en el símbolo l; los
 * siguientes están cada 6. Devuelve −1 si el puerto no tiene CRS en ese símbolo. */
int lte_crs_j0(int pci, int port, int ns, int l, bool ext);

/* PBCH: REs de los símbolos l = 0..3 de la ranura 1 de la subtrama 0, en el orden de
 * mapeo (k y luego l), excluyendo los reservados a CRS de 4 puertos. Devuelve nº. */
int lte_pbch_res(int pci, bool ext, int *lsym, int *j);

/* MIB de 24 bits (un bit por byte) */
typedef struct {
    int bw_rb;          /* 6, 15, 25, 50, 75, 100 */
    int phich_ext;      /* 0 normal, 1 extendida */
    int phich_res;      /* 0: 1/6, 1: 1/2, 2: 1, 3: 2 */
    int sfn8;           /* 8 MSB del SFN */
} lte_mib_t;
void lte_mib_pack(const lte_mib_t *m, uint8_t a[24]);
int lte_mib_unpack(const uint8_t a[24], lte_mib_t *m);     /* −1 si el ancho no es válido */

/* CRC-16 de la MIB con la máscara de puertos (1, 2 o 4). */
uint16_t lte_crc16(const uint8_t *bits, int n);

/* Codifica la MIB y devuelve los E = 4·e_seg bits aleatorizados del PBCH
 * (e_seg = 480 con CP normal, 432 extendido). */
int lte_pbch_encode(const lte_mib_t *m, int ports, int pci, bool ext, uint8_t *bits);
/* Decodifica una ráfaga (un cuarto de 4 tramas): soft[0..e_seg-1] con signo
 * "positivo = bit 0" ANTES de desaleatorizar; seg = nº de trama mod 4 supuesto.
 * Devuelve el nº de puertos (1, 2 o 4) si el CRC cuadra, o 0. */
int lte_pbch_decode(const float *soft, int seg, int pci, bool ext, lte_mib_t *m);

#endif
