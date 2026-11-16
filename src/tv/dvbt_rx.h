/*
 * dvbt_rx.h — Receptor DVB-T (ETSI EN 300 744) completo hasta paquetes TS de 188 B.
 *
 * Entrada: IQ complejo a 64/7 MHz = 9 142 857,14 Hz (canal de 8 MHz, T = 7/64 µs), con el
 * canal centrado en 0 Hz (la portadora central de la señal en banda base ≈ 0). Salida:
 * paquetes TS alineados a 0x47 (los no corregibles por el RS salen con TEI = 1).
 *
 * Cadena (ver dvbt_rx.c): sincronía de símbolo y CFO fraccional por prefijo cíclico
 * (van de Beek) → CFO entero y fase de pilotos dispersos con los pilotos continuos →
 * perfil de retardos para colocar la ventana FFT → seguimiento de fase común, pendiente
 * de fase (temporización) y deriva de reloj con los pilotos continuos → estimación de
 * canal con pilotos dispersos (4 símbolos, rejilla cada 3 portadoras) e interpolación
 * FIR en frecuencia → palabra TPS (DBPSK diferencial acumulado entre tramas + BCH) →
 * demapeo blando max-log 4/16/64-QAM (jerárquica: solo HP) → desentrelazado de símbolo
 * y de bit → Viterbi → Forney → RS → desaleatorizado.
 */
#ifndef BLADERF_LAB_DVBT_RX_H
#define BLADERF_LAB_DVBT_RX_H

#include "../common.h"

typedef void (*ts_sink_fn)(void *ctx, const uint8_t *pkts, int npkts); /* npkts × 188 B, alineados a 0x47 */

typedef struct {
    bool lock;                 /* sincronía OFDM + TPS + RS estables */
    int mode_k, guard_den;     /* 2/8, 4/8/16/32 */
    int qam, cr_num, cr_den;   /* 4/16/64, 1/2…7/8 (HP) */
    int cell_id;               /* −1 si no se conoce */
    float cfo_hz, mer_db, snr_db;
    double ber_pre_viterbi, ber_pre_rs;
    unsigned long rs_ok, rs_corrected, rs_failed;
    double ts_bitrate;         /* b/s útiles */
} dvbt_rx_stats_t;

typedef struct dvbt_rx dvbt_rx_t;

dvbt_rx_t *dvbt_rx_create(double rate_in, char *err, size_t errlen);   /* rate_in = 64e6/7 */
void dvbt_rx_destroy(dvbt_rx_t *r);
void dvbt_rx_reset(dvbt_rx_t *r);
void dvbt_rx_process(dvbt_rx_t *r, const cf32 *iq, int n, ts_sink_fn sink, void *ctx);
void dvbt_rx_get_stats(dvbt_rx_t *r, dvbt_rx_stats_t *st);

/* Extras (no forman parte de la API de §8c): estado interno y TPS completo. */
typedef struct {
    int state;                 /* 0 búsqueda, 1 lote de adquisición, 2 seguimiento+TPS, 3 datos */
    bool tps_ok;               /* palabra TPS decodificada */
    int hierarchy;             /* 0 ninguna, 1, 2, 4 (α) */
    int rate_lp_num, rate_lp_den;
    int frame;                 /* nº de trama del superframe (último TPS) */
    double cp_rho;             /* correlación del prefijo cíclico en la última adquisición */
    double pilot_q;            /* calidad de pilotos continuos (0…1) */
    double symbols;            /* símbolos OFDM procesados */
    double snr_pilot_db;       /* SNR de los pilotos continuos (relativa a la potencia de datos) */
    int tps_bch_fixed;         /* bits corregidos por el BCH en la última palabra */
    double t_sym_s;            /* duración del símbolo */
} dvbt_rx_info_t;
void dvbt_rx_get_info(dvbt_rx_t *r, dvbt_rx_info_t *info);

/* Variables de depuración: fuerza el tipo de modo/guarda en la adquisición (0 = auto). */
void dvbt_rx_force_params(dvbt_rx_t *r, int mode_k, int guard_den);

#endif
