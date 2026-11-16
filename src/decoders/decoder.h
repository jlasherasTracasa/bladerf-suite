/*
 * decoder.h — Contrato de los decodificadores de protocolos.
 *
 * Un decodificador convierte muestras IQ en mensajes. Hay dos clases:
 *
 *   DEC_NARROW  Recibe un canal ya aislado: el host le pone delante un DDC
 *               (dsp/ddc.h) que baja la frecuencia del canal a 0 Hz y entrega
 *               muestras complejas normalizadas (±1,0 = fondo de escala) a la
 *               tasa EXACTA 'rate' del descriptor. AIS, POCSAG, ACARS…
 *   DEC_WIDE    Recibe las muestras crudas del hardware (Q11: I,Q,I,Q… con
 *               ±2048 = fondo de escala) a su tasa original. Se usa cuando el
 *               protocolo ocupa toda la banda o exige una tasa concreta:
 *               ADS-B (2 Msps), DAB (2,048 Msps), WiFi (20 Msps)…
 *
 * Cada instancia se ejecuta en SU PROPIO hilo (decoders/host.c): process() y
 * process_raw() se llaman siempre desde el mismo hilo y no necesitan cerrojos.
 *
 * Eventos: por cada mensaje válido (CRC/FEC correctos) el decodificador llama
 * a emit() con un objeto JSON completo que incluye "text": un resumen de una
 * línea para la interfaz. El host lo envuelve en {"type":"dec",...}. Lo que no
 * supera la comprobación de integridad NO se emite.
 *
 * Estado: los que mantienen una tabla (aviones, barcos, celdas…) la exponen
 * con state_json() (~1 Hz) y caducan las entradas viejas.
 */
#ifndef BLADERF_LAB_DECODER_H
#define BLADERF_LAB_DECODER_H

#include "../common.h"
#include "../util/strbuf.h"

typedef enum { DEC_NARROW = 0, DEC_WIDE = 1 } dec_kind_t;

/* json: objeto JSON completo y válido. Debe incluir "text": resumen de una
 * línea para la interfaz. Resto de campos: propios del protocolo. */
typedef void (*dec_emit_fn)(void *ctx, const char *json, size_t len);

typedef struct {
    double rate;        /* tasa de las muestras que recibirá process/process_raw */
    int64_t freq_hz;    /* NARROW: frecuencia del canal; WIDE: = center_hz */
    int64_t center_hz;  /* frecuencia central del hardware */
    bool loc_valid;
    double lat, lon;    /* posición del receptor */
    const char *opts;   /* objeto JSON de opciones propias, o NULL */
} dec_cfg_t;

typedef struct {
    const char *id;        /* "ais": minúsculas, único, = id del modulador */
    const char *name;      /* "AIS" */
    const char *group;     /* "Marítimo", "Aviación"… (agrupa en la interfaz) */
    const char *desc;      /* una frase */
    dec_kind_t kind;
    double rate;           /* NARROW: tasa compleja EXACTA que entrega el DDC */
    int bw_hz;             /* NARROW: anchura del filtro de canal del DDC */
    uint32_t wide_rate;    /* WIDE: tasa exacta exigida al hardware (0 = libre) */
    uint32_t wide_min_rate;/* WIDE: tasa mínima si wide_rate == 0 */
    int64_t freqs[4];      /* frecuencias típicas (0 = fin de lista) */
    void *(*create)(const dec_cfg_t *cfg, char *err, size_t errlen);
    void (*destroy)(void *st);
    /* NARROW: muestras complejas normalizadas (±1,0 = fondo de escala) a 'rate'. */
    void (*process)(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx);
    /* WIDE: muestras Q11 crudas (I,Q,I,Q…; ±2048) a la tasa del hardware. */
    void (*process_raw)(void *st, const int16_t *iq, int n, double now, dec_emit_fn emit, void *ctx);
    void (*state_json)(void *st, sb_t *sb, double now);              /* opcional, ~1 Hz */
    void (*set_location)(void *st, bool valid, double lat, double lon); /* opcional */
} decoder_desc_t;

extern const decoder_desc_t *const DECODERS[];   /* registry.c, acaba en NULL */
const decoder_desc_t *decoder_find(const char *id);

#endif
