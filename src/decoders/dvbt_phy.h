/*
 * dvbt_phy.h — Tablas de la capa física DVB-T (ETSI EN 300 744) compartidas por
 * el decodificador (dvbt.c) y el modulador (tx/mod_dvbt.c).
 *
 * Parámetros OFDM (canal de 8 MHz, T = 7/64 µs → fs = 64/7 MHz = 9,142857 MHz):
 *   modo 2k: N = 2048, K = 1705 portadoras activas (k = 0…1704), Δf = 4464 Hz
 *   modo 8k: N = 8192, K = 6817 portadoras activas (k = 0…6816), Δf = 1116 Hz
 *   intervalo de guarda Δ/T_U ∈ {1/32, 1/16, 1/8, 1/4}; símbolo = N + N·Δ/T_U.
 * La portadora k ocupa el bin (k − (K−1)/2) mod N de la FFT (la portadora central
 * está en la frecuencia de RF nominal).
 *
 * Trama: 68 símbolos; superTrama: 4 tramas. Referencias dentro de cada símbolo l:
 *   - pilotos continuos (tabla 7 de la norma): posiciones fijas; el conjunto de
 *     8k es el de 2k repetido cada 1704 portadoras (4 copias, 177 pilotos);
 *   - pilotos dispersos: k mod 12 = 3·(l mod 4); modulados como los continuos con
 *     amplitud 4/3;
 *   - portadoras TPS (tabla 9): 17 en 2k, 68 en 8k (mismo patrón +m·1704);
 *     DBPSK diferencial entre símbolos consecutivos, amplitud 1;
 *   - los pilotos y la referencia TPS del símbolo l = 0 usan la secuencia pseudo
 *     aleatoria w_k de x^11 + x^2 + 1 con registro inicial a unos (un bit por
 *     portadora, empezando en k = 0): valor = 2(1/2 − w_k), ×4/3 en los pilotos.
 *
 * Palabra TPS (68 bits s0…s67; s0 es el bit de inicialización):
 *   s1–s16 sincronía (tramas pares 0011010111101110, impares su complemento),
 *   s17–s22 indicador de longitud (010111 = 23 sin cell id, 011111 = 31 con él),
 *   s23–s24 nº de trama, s25–s26 constelación, s27–s29 jerarquía,
 *   s30–s32 tasa HP, s33–s35 tasa LP, s36–s37 guarda, s38–s39 modo,
 *   s40–s47 cell id (un byte por trama), s48–s53 reservados (0),
 *   s54–s67 paridad BCH(67,53) t = 2, g(x)=x^14+x^9+x^8+x^6+x^5+x^4+x^2+x+1.
 */
#ifndef BLADERF_LAB_DVBT_PHY_H
#define BLADERF_LAB_DVBT_PHY_H

#include "../common.h"

#define DVBT_FS (64e6 / 7.0)

static inline int dvbt_ncar(int n) { return n == 2048 ? 1705 : 6817; }
/* Nº de pilotos continuos (45 / 177) y de portadoras TPS (17 / 68); rellenan out. */
int dvbt_cp_list(int n, int *out);
int dvbt_tps_list(int n, int *out);
/* w_k de la PRBS (0/1), k = 0…6816. */
int dvbt_prbs(int k);
static inline bool dvbt_is_scattered(int k, int l) { return k % 12 == 3 * (l % 4); }

typedef struct {
    int frame;          /* s23-s24 (0…3) */
    int constellation;  /* 0 QPSK, 1 16-QAM, 2 64-QAM */
    int hierarchy;      /* s27-s29: bits 0-1 α (0 ninguna, 1, 2, 4), bit 2 entrelazado en profundidad */
    int hp, lp;         /* 0 1/2, 1 2/3, 2 3/4, 3 5/6, 4 7/8 */
    int guard;          /* 0 1/32, 1 1/16, 2 1/8, 3 1/4 */
    int mode;           /* 0 2k, 1 8k */
    int length;         /* 23 o 31 */
    int cell_byte;      /* s40-s47 */
    int bch_alt;        /* 1 si el BCH solo cuadró con el orden de paridad invertido */
} dvbt_tps_t;

extern const uint8_t DVBT_SYNC_EVEN[16], DVBT_SYNC_ODD[16];
void dvbt_tps_build(const dvbt_tps_t *t, uint8_t s[68]);
/* Comprueba/corrige s1…s67 (t=2). Devuelve nº de bits corregidos o −1. *alt = 1 si
 * hizo falta invertir el orden de la paridad. Modifica s. */
int dvbt_tps_check(uint8_t s[68], int *alt);
void dvbt_tps_parse(const uint8_t s[68], dvbt_tps_t *t);

const char *dvbt_rate_name(int r);
const char *dvbt_guard_name(int g);
const char *dvbt_const_name(int c);
int dvbt_guard_div(int g);   /* 32, 16, 8, 4 */

#endif
