/*
 * mod.h — Contrato de los moduladores.
 *
 * Cada protocolo decodificable tiene su modulador gemelo: construye la señal
 * de banda base que el decodificador espera recibir. Sirven para tres cosas:
 *   1. Probar decodificadores sin radio (modular → canal con ruido → decodificar).
 *   2. Generar ficheros SigMF de prueba (bladerf-lab --gen, o la orden "tx"
 *      en modo "file").
 *   3. Emitir de verdad (modos loopback/air de tx.c), SOLO si la política de
 *      AGENTS.md §6 lo permite (air_ok).
 *
 * La salida es banda base compleja centrada en 0 Hz, con |x| ≤ 0,9, a la tasa
 * out->rate que fija el propio modulador (normalmente la tasa nativa del
 * decodificador: así el test enchufa la señal directamente).
 */
#ifndef BLADERF_LAB_MOD_H
#define BLADERF_LAB_MOD_H

#include "../common.h"

typedef struct {
    cf32 *s;
    int n, cap;
    double rate;
} iqbuf_t;

/* Garantiza espacio para 'extra' muestras más. Termina el programa solo si no
 * hay memoria (caso patológico); el resto de la API no falla. */
void iqbuf_reserve(iqbuf_t *b, int extra);
void iqbuf_push(iqbuf_t *b, cf32 v);
void iqbuf_free(iqbuf_t *b);

typedef struct {
    const char *id;     /* = id del decodificador */
    const char *name;
    bool air_ok;        /* true SOLO si puede radiarse legalmente (§6) */
    /* params: objeto JSON (NULL → mensaje de ejemplo). Escribe banda base
     * compleja centrada en 0 Hz, |x| ≤ 0,9, y fija out->rate. Devuelve 0 o <0
     * con el texto en err. */
    int (*modulate)(const char *params, iqbuf_t *out, char *err, size_t errlen);
} modulator_desc_t;

extern const modulator_desc_t *const MODULATORS[];  /* tx/modreg.c, acaba en NULL */
const modulator_desc_t *modulator_find(const char *id);

#endif
