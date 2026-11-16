/*
 * ts.h — Demultiplexor de MPEG-2 Transport Stream (ISO/IEC 13818-1, §2.4.3).
 *
 * Un TS es una sucesión de paquetes de 188 bytes:
 *
 *   [0]      0x47            byte de sincronía
 *   [1..2]   TEI(1) PUSI(1) prioridad(1) PID(13)
 *   [3]      scrambling(2) adaptation_field_control(2) continuity_counter(4)
 *   [4..]    campo de adaptación (opcional) y carga útil (opcional)
 *
 *  - PUSI (payload_unit_start_indicator): la carga empieza una sección PSI
 *    (tras el pointer_field) o un paquete PES.
 *  - continuity_counter: módulo 16, sube 1 en cada paquete CON carga del mismo PID.
 *    Un salto indica pérdida de paquetes (aquí, restos de errores no corregidos
 *    por Reed-Solomon): el reensamblador de la capa superior descarta lo que
 *    tenía a medias para no entregar secciones o imágenes corruptas.
 *  - El campo de adaptación puede llevar la PCR (Program Clock Reference): 33 bits
 *    base a 90 kHz + 9 bits extensión a 27 MHz, PCR = base·300 + ext.
 *
 * ts_demux_feed() acepta bytes arbitrarios (no tienen por qué estar alineados):
 * busca el 0x47 y exige que se repita a 188, 376 y 564 bytes para darlo por válido,
 * y pierde la sincronía tras 3 paquetes seguidos sin 0x47.
 */
#ifndef BLADERF_LAB_TS_H
#define BLADERF_LAB_TS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TS_PKT 188
#define TS_PID_PAT 0x0000
#define TS_PID_NIT 0x0010
#define TS_PID_SDT 0x0011
#define TS_PID_NULL 0x1FFF

typedef struct {
    int pid;
    bool pusi;
    bool tei;            /* transport_error_indicator: el RS no pudo corregirlo */
    bool scrambled;      /* transport_scrambling_control != 0 (contenido cifrado: se ignora) */
    int cc;              /* continuity_counter */
    bool cc_err;         /* salto de continuidad en este PID (el paquete es válido) */
    bool dup;            /* paquete duplicado (permitido una vez): no debe procesarse */
    bool has_pcr;
    int64_t pcr27;       /* PCR en ticks de 27 MHz (solo si has_pcr) */
    bool discont;        /* discontinuity_indicator del campo de adaptación */
    const uint8_t *payload; /* NULL si no hay carga útil */
    int plen;
} ts_pkt_t;

/* Analiza un paquete de 188 bytes. Devuelve 0 o −1 si no empieza por 0x47.
 * No toca el estado de continuidad (cc_err/dup quedan en false). */
int ts_parse(const uint8_t *p, ts_pkt_t *o);

typedef void (*ts_pkt_fn)(void *ctx, const ts_pkt_t *pkt);
typedef struct ts_demux ts_demux_t;

ts_demux_t *ts_demux_create(ts_pkt_fn fn, void *ctx);
void ts_demux_destroy(ts_demux_t *d);
void ts_demux_reset(ts_demux_t *d);
/* Bytes del flujo en cualquier alineación. Llama a fn por cada paquete válido. */
void ts_demux_feed(ts_demux_t *d, const uint8_t *data, size_t len);

typedef struct {
    unsigned long packets, tei, cc_errors, sync_losses, null_pkts;
} ts_stats_t;
void ts_demux_stats(const ts_demux_t *d, ts_stats_t *st);

/* PCR en segundos sobre ticks de 27 MHz */
static inline double ts_pcr_sec(int64_t pcr27) { return (double)pcr27 / 27e6; }

#endif
