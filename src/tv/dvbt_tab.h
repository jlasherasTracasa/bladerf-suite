/*
 * dvbt_tab.h — Tablas y bloques de codificación de canal de DVB-T (ETSI EN 300 744)
 * compartidos por el receptor (dvbt_rx.c) y el modulador (tx/mod_dvbt.c).
 *
 * Cadena del transmisor (cap. 4 de la norma) y su inversa en el receptor:
 *
 *   TS 188 B ─► aleatorizador (1+x^14+x^15, 8 paquetes) ─► RS(204,188)
 *      ─► entrelazado convolucional de Forney (I=12, M=17) ─► convolucional K=7
 *      (171,133 octal, X antes que Y) + perforado ─► demux a v subflujos + entrelazado
 *      de bits (126 bits por bloque) ─► entrelazado de símbolo (H(q)) ─► constelación
 *      Gray 4/16/64-QAM (α = 1, 2, 4 si es jerárquica) ─► OFDM.
 *
 * El receptor y el modulador comparten UNA tabla por (modo, v, paridad de símbolo,
 * flujo): dst[p·v + i] = posición, dentro del flujo de bits codificados (el que sale
 * del perforado), del bit i de la palabra de la portadora de datos p (p = 0…ndata−1
 * en orden creciente de frecuencia). Así el desentrelazado completo de un símbolo OFDM
 * es un único escatter y en TX es el mismo gather.
 *
 * Convenciones: la palabra de una portadora es (y0 y1 … y_{v−1}); en el entero
 * "word" el bit i (LSB = 0) es y_i. Los bits y0,y2,y4 van por el eje I y los y1,y3,y5
 * por el Q (Gray; y0 = signo, 0 = positivo). Soft: positivo = más probable el bit 0.
 */
#ifndef BLADERF_LAB_DVBT_TAB_H
#define BLADERF_LAB_DVBT_TAB_H

#include "../common.h"

/* Configuración de capa física que afecta a la codificación. */
typedef struct {
    int n;          /* 2048 | 8192 */
    int v;          /* bits por portadora: 2, 4, 6 */
    int alpha;      /* 1, 2, 4 (solo jerárquica; 1 en el resto) */
    bool hier;      /* jerárquica: HP en y0,y1 y LP en el resto */
    int rate_hp;    /* 0..4: 1/2 2/3 3/4 5/6 7/8 */
    int rate_lp;
} dvbt_cfg_t;

static inline int dvbt_ndata(int n) { return n == 2048 ? 1512 : 6048; }

/* H(q) del entrelazado de símbolo (ndata entradas). */
const uint16_t *dvbt_symint(int n);

/* Tabla (p·v+i) → índice en el flujo; stream 0 = HP (o único), 1 = LP. Los bits que
 * no pertenecen al flujo valen 0xFFFFFFFF. Memoria estática compartida (no liberar).
 * parity = 0 para símbolos pares de la trama (l = 0, 2, …), 1 para impares. */
const uint32_t *dvbt_bitmap(int n, int v, bool hier, int parity, int stream);
/* Bits por símbolo OFDM de cada flujo: ndata·v (único) o ndata·2 (HP) / ndata·(v−2) (LP). */
int dvbt_stream_bits(int n, int v, bool hier, int stream);

/* Constelación. */
cf32 dvbt_qam_point(int v, int alpha, unsigned word);
/* Tabla de LLR por eje (max-log, sin escala): llr[a][idx] = d²(bit a=1) − d²(bit a=0)
 * con la posición x en unidades de la constelación normalizada (potencia media 1).
 * a = 0.. v/2−1 es el índice del bit en el eje (bit de la palabra 2a en I, 2a+1 en Q). */
#define DVBT_LLR_NT 1024
typedef struct {
    int nab;                    /* bits por eje: 1, 2, 3 */
    float lo, inv;              /* idx = (x − lo)·inv */
    float t[3][DVBT_LLR_NT];
} dvbt_llrtab_t;
const dvbt_llrtab_t *dvbt_llrtab(int v, int alpha);

/* Perforado (cap. 4.3.4.3): patrones X e Y sobre 'period' bits de entrada. */
typedef struct {
    int period;                 /* bits de entrada por periodo */
    int nout;                   /* bits transmitidos por periodo */
    uint8_t x[8], y[8];
} dvbt_punct_t;
const dvbt_punct_t *dvbt_punct(int rate);

/* Codificador convolucional K=7 (X = 171, Y = 133 octal), sin cola, de un flujo continuo.
 * bits[nin] (0/1) → out[] con el perforado aplicado; *state se conserva entre llamadas.
 * Devuelve nº de bits de salida (nin debe ser múltiplo del periodo). */
int dvbt_conv_encode(uint32_t *state, const uint8_t *bits, int nin, int rate, uint8_t *out);

/* Aleatorizador: 1503 bytes de la PRBS 1+x^14+x^15 (semilla 100101010000000) que se
 * aplican a los bytes 1..1503 de cada grupo de 8 paquetes. */
const uint8_t *dvbt_prbs1503(void);

#endif
