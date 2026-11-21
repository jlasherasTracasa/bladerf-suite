/*
 * wifi_phy.h — Constantes y tablas de la capa física IEEE 802.11 que comparten
 * el decodificador (wifi.c) y el modulador (tx/mod_wifi.c).
 *
 * 802.11a/g OFDM (IEEE 802.11-2016, cláusula 17), 20 MHz, FFT de 64 puntos:
 *   - Subcarriles −26…+26 (el 0 es nulo). Pilotos en ±7 y ±21; los otros 48 son
 *     de datos, numerados en orden creciente de frecuencia (§17.3.5.10).
 *   - Preámbulo: 10 símbolos cortos (STF, 8 µs, periodo 16 muestras) + GI2 de
 *     32 muestras + 2 símbolos largos (LTF, 64 muestras cada uno) = 16 µs.
 *   - Campo SIGNAL (24 bits: RATE 4, reservado 1, LENGTH 12 LSB-primero,
 *     paridad par, cola 6) en un símbolo BPSK 1/2; después los símbolos de
 *     datos con prefijo cíclico de 16 muestras (símbolo = 80 muestras = 4 µs).
 *   - Código convolucional K=7, g0=133₈ (salida A), g1=171₈ (salida B). Las
 *     tasas 2/3 y 3/4 se obtienen por punzonado (§17.3.5.6, Fig. 17-9);
 *     2/3 = patrón A0 B0 A1 · (se pierde B1) y 3/4 = A0 B0 A1 · · B2 (se
 *     pierden B1 y A2). Tabla de tasas: Tabla 17-4 (6, 9, 12, 18, 24, 36, 48, 54).
 *   - Constelaciones Gray (§17.3.5.8, Tabla 17-9..17-11): BPSK, QPSK (1/√2),
 *     16-QAM (1/√10: 00→−3 01→−1 11→+1 10→+3) y 64-QAM (1/√42:
 *     000→−7 001→−5 011→−3 010→−1 110→+1 111→+3 101→+5 100→+7); el primer
 *     bit de cada grupo va al eje I y el siguiente grupo al Q.
 *   - Aleatorizador x⁷+x⁴+1 (lfsr_init_wifi); la secuencia de polaridad de los
 *     pilotos es la misma con estado inicial 1111111 (0→+1, 1→−1), periodo 127.
 *
 * 802.11b DSSS (cláusula 16): chips a 11 Mchip/s, Barker de 11 chips, DBPSK a
 * 1 Mb/s, aleatorizador autosincronizado G(z)=z⁻⁷+z⁻⁴+1, preámbulo largo:
 * SYNC = 128 unos aleatorizados, SFD = 0xF3A0 (transmitido LSB primero:
 * 0000 0101 1100 1111), cabecera PLCP de 48 bits (SIGNAL, SERVICE, LENGTH,
 * CRC-16 CCITT con unos iniciales y complementado, transmitido MSB primero).
 */
#ifndef BLADERF_LAB_WIFI_PHY_H
#define BLADERF_LAB_WIFI_PHY_H

#include "../common.h"
#include "../dsp/fec.h"

#define WIFI_FS 20000000.0

typedef struct {
    int mbps;
    int code;   /* RATE R1..R4: R1 es el bit 3 (6 Mb/s = 1101) */
    int nbpsc;  /* bits por subportadora: 1 BPSK, 2 QPSK, 4 16-QAM, 6 64-QAM */
    int ncbps;  /* bits codificados por símbolo */
    int ndbps;  /* bits de datos por símbolo */
    int r34;    /* 1 si la tasa del código es 3/4 */
    int r23;    /* 1 si la tasa del código es 2/3 (solo 48 Mb/s) */
} wifi_rate_t;

const wifi_rate_t *wifi_rate_by_code(int code);
const wifi_rate_t *wifi_rate_by_mbps(int mbps);

/* Número de subportadora (−26…26) del dato i = 0…47. */
int wifi_data_carrier(int i);
static inline int wifi_bin(int k) { return (k + 64) & 63; }
static const int WIFI_PILOT_K[4] = {-21, -7, 7, 21};
static const int WIFI_PILOT_V[4] = {1, 1, 1, -1};
int wifi_pilot_polarity(int n);       /* ±1, n módulo 127 */

void wifi_stf_bins(cf32 out[64]);     /* S(k) en 64 bins de FFT */
void wifi_ltf_bins(float out[64]);    /* L(k) (reales ±1) */

/* Primera y segunda permutación del entrelazador: j_of_k[k] = posición. */
void wifi_interleave_table(const wifi_rate_t *r, int *j_of_k);

conv_code_t wifi_conv_code(void);
const conv_punct_t *wifi_punct(const wifi_rate_t *r);   /* NULL para 1/2 */
int wifi_coded_bits(int ninfo, const conv_punct_t *p);  /* nº de bits tras punzonar */

/* ---- DSSS ---------------------------------------------------------------- */
extern const int8_t WIFI_BARKER[11];
#define WIFI_SFD_BITS 0x05CFu   /* SFD en orden de transmisión: primer bit = MSB de 16 */
/* CRC-16 de la cabecera PLCP sobre n bits en orden de transmisión. */
uint16_t wifi_plcp_crc16(const uint8_t *bits, int n);

#endif
