/*
 * pulse.h — Diseño de pulsos de conformación y FIR con coeficientes propios.
 *
 * Todos los pulsos se generan muestreados a 'sps' muestras por símbolo, con
 * 'span' símbolos de duración total (span·sps+1 coeficientes, simétricos y
 * centrados: el retardo de grupo es span·sps/2 muestras). El tiempo t está
 * en periodos de símbolo T (T = 1):
 *
 *   Coseno alzado (RC), roll-off β — cumple el criterio de Nyquist (cero ISI):
 *       h(t) = sinc(t) · cos(πβt) / (1 − (2βt)²),      sinc(t) = sin(πt)/(πt)
 *       (límite en |t| = 1/(2β):  h = (π/4)·sinc(1/(2β)))
 *   Raíz de coseno alzado (RRC): su "raíz cuadrada" en frecuencia; emisor y
 *   receptor llevan cada uno un RRC y el conjunto es un RC:
 *       h(t) = [ sin(πt(1−β)) + 4βt·cos(πt(1+β)) ] / [ πt·(1 − (4βt)²) ]
 *       (t = 0: 1 − β + 4β/π;  |t| = 1/(4β): (β/√2)[(1+2/π)sin(π/4β) + (1−2/π)cos(π/4β)])
 *   Gaussiano (GFSK/GMSK), producto ancho de banda·tiempo BT:
 *       h(t) = exp(−t²/(2σ²)),   σ = √(ln 2) / (2π·BT)       (en unidades de T)
 *       su frecuencia a −3 dB es BT/T. Bluetooth BT=0.5, GSM BT=0.3, AIS 0.4.
 *   GMSK: pulso de frecuencia = gaussiano ⊛ rectángulo de 1 símbolo, de área 1;
 *       un símbolo ±1 gira la fase ±π/2 (índice h = 0.5) si se multiplica por π/2·área.
 *
 * Normalizaciones (pulse_norm_t): ENERGY Σh² = 1 (RRC emparejado: ganancia de
 * ruido unidad), DC Σh = 1 (ganancia 1 en continua; para interpolar o filtrar
 * con ganancia unidad) y PEAK máx(h) = 1.
 *
 * FIR genérico: línea de retardo duplicada (como filter.h), coeficientes
 * arbitrarios, diezmado opcional. fir_f_t para datos reales (float); fir_cf_t
 * para datos complejos (cf32) con coeficientes reales. Convención de diezmado:
 * se emite una salida por cada 'decim' entradas, la primera tras la entrada
 * nº decim (índice decim−1).
 *
 * Conformación TX: pulse_shape_*() sobreinterpola los símbolos por sps (ceros
 * entre símbolos) y filtra, en forma polifásica (sin multiplicar por ceros).
 */
#ifndef BLADERF_LAB_PULSE_H
#define BLADERF_LAB_PULSE_H

#include "../common.h"

typedef enum { PULSE_NORM_ENERGY = 0, PULSE_NORM_DC = 1, PULSE_NORM_PEAK = 2 } pulse_norm_t;

int pulse_ntaps(int sps, int span);                 /* span·sps + 1 */
/* Cada función escribe pulse_ntaps(sps, span) coeficientes y devuelve ese nº
 * (o −1 si los parámetros no son válidos). */
int pulse_rrc(float *h, double beta, int sps, int span, pulse_norm_t norm);
int pulse_rc(float *h, double beta, int sps, int span, pulse_norm_t norm);
int pulse_gaussian(float *h, double bt, int sps, int span, pulse_norm_t norm);
int pulse_rect(float *h, int sps);                  /* sps unos normalizados a Σ=1 */
/* Pulso de frecuencia GMSK: devuelve span·sps+sps coeficientes con Σh = 1. */
int pulse_gmsk(float *h, double bt, int sps, int span);

/* ---- FIR real ---------------------------------------------------------- */
typedef struct {
    int ntaps, decim, phase, pos;
    float *rtaps;     /* coeficientes en orden inverso (el 0 multiplica la más antigua) */
    float *hist;      /* 2·ntaps */
} fir_f_t;

int fir_f_init(fir_f_t *f, const float *taps, int ntaps, int decim);   /* 0 / −1 */
void fir_f_free(fir_f_t *f);
void fir_f_reset(fir_f_t *f);
/* Procesa n muestras; escribe hasta n/decim+1 salidas. Devuelve cuántas. */
int fir_f_process(fir_f_t *f, const float *in, int n, float *out);

/* ---- FIR complejo × real ---------------------------------------------- */
typedef struct {
    int ntaps, decim, phase, pos;
    float *rtaps;
    cf32 *hist;
} fir_cf_t;

int fir_cf_init(fir_cf_t *f, const float *taps, int ntaps, int decim);
void fir_cf_free(fir_cf_t *f);
void fir_cf_reset(fir_cf_t *f);
int fir_cf_process(fir_cf_t *f, const cf32 *in, int n, cf32 *out);

/* ---- Conformación TX ---------------------------------------------------- */
/* out recibe nsym·sps + ntaps − 1 muestras (convolución completa). Devuelve ese nº. */
int pulse_shape_f(const float *sym, int nsym, const float *h, int ntaps, int sps, float *out);
int pulse_shape_cf(const cf32 *sym, int nsym, const float *h, int ntaps, int sps, cf32 *out);

#endif
