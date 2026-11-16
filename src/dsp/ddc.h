/*
 * ddc.h — Conversor digital descendente (DDC, "digital down converter").
 *
 * Toma la banda ancha capturada por el bladeRF (p. ej. 2,4 Msps centrados en
 * 100 MHz) y entrega UN canal estrecho (p. ej. 48 ksps centrados en la emisora
 * de 100,3 MHz) con la tasa compleja EXACTA que el decodificador necesita.
 *
 * Cadena:
 *
 *   x[n] ──► NCO ──► firdec ÷D1 ──► firdec ÷D2 … ──► cfir ──► cresamp ──► y[m]
 *            e^{-jωn}   (≤ ÷8 cada etapa)             canal     fraccionario
 *
 *  1. Mezcla: multiplicar por e^{-j2π(f−fc)n/fs} lleva la frecuencia f del
 *     canal a 0 Hz (traslación del espectro: x·e^{-jω0 n} ⇒ X(ω+ω0)).
 *  2. Diezmado en etapas. Diezmar de golpe por M = fs/fs_out exigiría un FIR
 *     larguísimo; en etapas de ≤ 8 cada filtro es corto porque su banda de
 *     transición es ancha (se permite que aliasee lo que luego se descarta).
 *     La descomposición usa factores 2,3,5,7 (M' = mayor número ≤ M con esa
 *     propiedad); la tasa intermedia queda en [fs_out, ~1,5·fs_out].
 *  3. Filtro de canal complejo (cfir_t) de ±bw/2: fija la anchura real.
 *  4. Remuestreador polifásico complejo hasta fs_out exacto (cresamp_t).
 *
 * El cambio de frecuencia (ddc_retune) solo modifica el paso del NCO y conserva
 * su fase actual, por lo que no hay saltos de fase (clics) en la salida.
 *
 * Los filtros procesan por bloques internos de DDC_CHUNK muestras: la memoria
 * de trabajo es pequeña y cabe en caché aunque la entrada sea de 61 Msps.
 */
#ifndef BLADERF_LAB_DDC_H
#define BLADERF_LAB_DDC_H

#include "../common.h"

typedef struct ddc ddc_t;

/* fs_in: tasa de entrada; center_hz: frecuencia central de la captura;
 * freq_hz: frecuencia del canal; fs_out: tasa de salida; bw_hz: anchura del
 * canal. Devuelve NULL si los parámetros no son realizables (fs_out > fs_in,
 * canal fuera de banda…). */
ddc_t *ddc_create(double fs_in, int64_t center_hz, int64_t freq_hz, double fs_out, double bw_hz);
void ddc_destroy(ddc_t *d);
void ddc_retune(ddc_t *d, int64_t center_hz, int64_t freq_hz);
/* Procesa n muestras de entrada y escribe como mucho out_cap salidas. */
int ddc_process(ddc_t *d, const cf32 *in, int n, cf32 *out, int out_cap);
/* Cota superior del nº de salidas que puede producir ddc_process(n). */
size_t ddc_max_out(const ddc_t *d, int n);

#endif
