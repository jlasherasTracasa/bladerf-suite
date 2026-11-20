/*
 * lora_phy.h — Capa física LoRa (CSS) común a los decodificadores lora y
 * meshtastic y a sus moduladores.
 *
 * ---------------------------------------------------------------------------
 * Modulación (Chirp Spread Spectrum). Con ancho de banda BW y factor de
 * ensanchado SF hay N = 2^SF "chips" por símbolo y el símbolo dura N/BW s. Un
 * símbolo es un chirp que barre la banda de −BW/2 a +BW/2 con frecuencia
 *     f(t) = BW·( ((s + t·BW) mod N)/N − 1/2 ),
 * es decir, el chirp base desplazado cíclicamente s chips: el valor s (SF bits)
 * va en el instante en que la frecuencia "salta" de +BW/2 a −BW/2.
 *
 * Demodulación (Tapparel et al., arXiv:2002.08208, §II): multiplicar la
 * ventana de N muestras (a fs = BW) por el chirp base conjugado ("dechirp")
 * convierte el chirp en un tono de frecuencia s·BW/N; la FFT de N puntos lo
 * concentra en el bin s. Una ventana desalineada τ muestras o con desvío de
 * portadora ε bins solo desplaza el bin a s+τ+ε: por eso un único preámbulo
 * basta para estimar ambos (ver abajo).
 *
 * Trama:  [preámbulo: n upchirps]  [2 símbolos de palabra de sincronía, de
 * valores 8·nibble_alto y 8·nibble_bajo]  [2,25 downchirps (SFD)]  [símbolos de
 * cabecera y carga].
 *
 * Codificación (orden de transmisión):
 *   carga → (+CRC16) → dewhitening con LFSR x^8+x^6+x^5+x^4+1 (semilla 0xFF,
 *   un byte de secuencia por byte de carga; nibble bajo con el nibble bajo) →
 *   Hamming 4/(4+CR) por nibble → entrelazado diagonal de bloques de
 *   PPM codewords en (4+CR) símbolos de PPM bits → Gray inverso → símbolo.
 *   PPM = SF (carga normal) o SF−2 (cabecera y modo LDRO); en el segundo caso
 *   el símbolo es múltiplo de 4 (los 2 bits bajos son 0) para ser robusto.
 *   La cabecera explícita son 5 nibbles (longitud ×2, CR·2+CRC, 5 bits de
 *   checksum) codificados SIEMPRE a 4/8 en el primer bloque de 8 símbolos.
 *
 * Las tablas (whitening, paridades Hamming, entrelazado, checksum de cabecera,
 * CRC) están contrastadas con las implementaciones de código abierto
 * gr-lora_sdr (EPFL), LoRa-SDR (Pothosware) y gr-lora (rpp0) leídas como
 * referencia de HECHOS; LoRa es propietario y NO se han verificado contra
 * tramas de un chip Semtech real: ver docs/protocolos/lora.md (limitaciones).
 *
 * Receptor (lora_rx_*): trabaja a 2·BW. Por cada SF activo, ventanas de N·2
 * muestras sin solape detectan el preámbulo (mismo bin de pico ≥ 4 veces),
 * se localiza el SFD con el chirp descendente, se resuelven CFO y STO con
 * ε = (q_up + q_down)/2, τ = (q_up − q_down)/2 y se afinan con ventanas ya
 * alineadas (mezcla del CFO, filtro/diezmado a BW, dechirp, FFT, interpolación
 * de Jacobsen). La deriva de reloj (SFO) se mide en el preámbulo (pendiente
 * del bin de pico por símbolo, idea de Tapparel & Burg arXiv:2502.08485 de
 * corregirla ya en el preámbulo) y después se sigue símbolo a símbolo con un
 * lazo PI sobre la parte fraccionaria del pico.
 */
#ifndef BLADERF_LAB_LORA_PHY_H
#define BLADERF_LAB_LORA_PHY_H

#include "../common.h"
#include "../tx/mod.h"

#define LORA_MAX_PAYLOAD 255

typedef struct {
    int sf, bw_hz;
    int cr;            /* 1..4  (4/5..4/8) */
    bool has_crc;      /* bit CRC de la cabecera */
    bool crc_ok;       /* CRC de carga verificado */
    bool ldro;
    int len;
    uint8_t data[LORA_MAX_PAYLOAD + 2];
    int sync;          /* palabra de sincronía recibida (p. ej. 0x34) */
    int fec_fixed;     /* nibbles corregidos por Hamming */
    double snr_db;     /* SNR en la banda BW */
    double rssi_dbfs;  /* potencia de la señal (dBFS) */
    double cfo_hz;
    double sfo_ppm;
    double t_end;      /* segundos de señal de entrada hasta el final de la trama */
} lora_frame_t;

typedef void (*lora_frame_fn)(void *ctx, const lora_frame_t *f);

typedef struct {
    double fs;         /* tasa de entrada: múltiplo entero de 2·bw_hz */
    int bw_hz;         /* 125000 o 250000 (también 62500..500000) */
    unsigned sf_mask;  /* bit sf activo (7..12) */
    bool sync_any;     /* aceptar cualquier palabra de sincronía */
    int sync[8];
    int nsync;
    int ldro;          /* −1 auto (T_símbolo > 16 ms), 0, 1 */
} lora_rx_cfg_t;

typedef struct lora_rx lora_rx_t;

lora_rx_t *lora_rx_create(const lora_rx_cfg_t *cfg, char *err, size_t errlen);
void lora_rx_destroy(lora_rx_t *rx);
void lora_rx_process(lora_rx_t *rx, const cf32 *in, int n, lora_frame_fn cb, void *ctx);
/* Contadores: detecciones de preámbulo con sincronía válida, cabeceras
 * inválidas, CRC erróneos, tramas correctas. */
void lora_rx_stats(const lora_rx_t *rx, unsigned long *sync_ok, unsigned long *hdr_bad, unsigned long *crc_bad,
                   unsigned long *frames);

/* ---- Bloques de la cadena (expuestos para tests y para el modulador) ---- */
uint16_t lora_crc16(const uint8_t *p, int n);   /* CRC de carga LoRa (ver .c) */
uint8_t lora_whitening_byte(int i);              /* secuencia de blanqueo */
bool lora_ldro_auto(int sf, int bw_hz);
int lora_frame_nsyms(int len, int sf, int cr, bool crc, bool ldro);
/* Codifica una trama (cabecera explícita) en valores de símbolo transmitidos. */
int lora_encode_symbols(const uint8_t *pl, int len, int sf, int cr, bool crc, bool ldro, uint16_t *syms, int cap);
/* Añade a o (a la tasa fs) preámbulo + sync + SFD + símbolos. Fija o->rate. */
void lora_modulate_frame(iqbuf_t *o, double fs, int bw_hz, int sf, int sync, int npreamble, const uint16_t *syms,
                         int nsym, float amp);
/* Daños de tiempo muerto: silencio de n muestras. */
void lora_push_silence(iqbuf_t *o, int n);

#endif
