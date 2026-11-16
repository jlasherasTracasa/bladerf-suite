/*
 * dvbt_outer.h — Código externo de DVB-T en el receptor: sincronismo de paquete,
 * desentrelazado convolucional de Forney (I=12, M=17), Reed-Solomon RS(204,188,t=8) y
 * desaleatorizado (PRBS 1+x^14+x^15). Entrada: flujo de bytes que sale del Viterbi (con
 * un desfase de bits desconocido); salida: paquetes TS de 188 B alineados a 0x47.
 *
 * Teoría del sincronismo. El entrelazado de Forney deja intacto el byte de sincronía
 * (rama 0, sin retardo), de modo que en la salida del Viterbi aparece un 0x47 (o 0xB8 cada
 * ocho paquetes) cada 204 bytes. Se busca el desfase (0…203 bytes × 0…7 bits) en el que
 * más posiciones periódicas valen 0x47/0xB8; la probabilidad de que el azar dé 5 de 6 es
 * ≈ 1e-8. Una vez alineado, el desentrelazador (rama j con retardo (11−j)·17 posiciones)
 * añade 11 paquetes de latencia: los 11 primeros se descartan. El 0xB8 corregido por RS
 * marca el primer paquete de cada grupo de 8 para la PRBS.
 */
#ifndef BLADERF_LAB_DVBT_OUTER_H
#define BLADERF_LAB_DVBT_OUTER_H

#include <stdint.h>
#include <stddef.h>

typedef void (*dvbt_outer_sink_fn)(void *ctx, const uint8_t *pkts, int npkts);

typedef struct dvbt_outer dvbt_outer_t;

typedef struct {
    unsigned long rs_ok, rs_corrected, rs_failed;
    unsigned long bit_errors;     /* bits corregidos por el RS (BER antes del RS = bit_errors / bits) */
    unsigned long bits;           /* bits de paquetes recuperables evaluados */
    unsigned long out_pkts;
    int synced;
} dvbt_outer_stats_t;

dvbt_outer_t *dvbt_outer_create(void);
void dvbt_outer_destroy(dvbt_outer_t *o);
void dvbt_outer_reset(dvbt_outer_t *o);
/* Procesa nbytes del flujo del Viterbi. Los paquetes uncorrectables salen con TEI = 1. */
void dvbt_outer_push(dvbt_outer_t *o, const uint8_t *bytes, int nbytes, dvbt_outer_sink_fn sink, void *ctx);
void dvbt_outer_stats(const dvbt_outer_t *o, dvbt_outer_stats_t *st);
/* Porcentaje de paquetes fallidos entre los últimos 64 (0…100), 100 si no hay sincronismo. */
int dvbt_outer_fail_pct(const dvbt_outer_t *o);

#endif
