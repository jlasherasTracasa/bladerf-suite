/*
 * cal.h — Calibración del transmisor por bucle interno (IQ y fuga de LO).
 *
 * Modelo (en banda base, normalizado por la ganancia compleja μ del TX, que se
 * confunde con la del canal de bucle y no importa):
 *
 *        y = x + q·x* + c
 *
 *   q  desequilibrio de ganancia/fase. Con yI = xI, yQ = g·(xQ·cosφ + xI·sinφ)
 *      resulta μ = (1 + g·e^{jφ})/2, ν = (1 − g·e^{−jφ})/2 y q = ν/μ. La imagen
 *      de un tono sale a −f con amplitud |q| respecto a la portadora:
 *      IRR = −20·log10|q|.
 *   c  fuga del oscilador local (offset de continua en banda base).
 *
 * Precorrección (inversa exacta del modelo, válida si |q| < 1): para que la
 * salida sea z se transmite
 *        w = z − c ,   x = (w − q·w*) / (1 − |q|²)
 * pues  x + q·x* = w.
 *
 * Medida (idea de Hsu & Sheen y de US 8290458: tono desplazado para separar la
 * imagen de la fuga de LO, ver docs/investigacion/INVESTIGACION.md E40): se transmite una
 * ráfaga [silencio][preámbulo PN][tono +fs/16][tono −fs/16][silencio] y se
 * captura por el receptor. El preámbulo da el retardo entero d del bucle.
 * Proyectando cada tramo sobre ±fs/16 (ciclos enteros → sin fugas espectrales):
 *     P+ = H·e^{−jθ}   (portadora del tono +)   i+ = H·q·e^{+jθ}  (imagen)
 *     P− = H·e^{+jθ}   (portadora del tono −)   i− = H·q·e^{−jθ}
 * con θ = ω·τ el retardo fraccionario residual. Combinando los dos tonos el
 * retardo se cancela:
 *     θ = arg(P−/P+)/2,  H = (P+e^{jθ} + P−e^{−jθ})/2,
 *     q = (i+e^{−jθ} + i−e^{jθ}) / (2H),   c = media(silencio final)/H.
 * (θ se toma en (−π/2, π/2]: vale porque el retardo entero ya está quitado.)
 *
 * LIMITACIÓN: en el bucle BLADERF_LB_RFIC_BIST de la bladeRF 2.0 la
 * realimentación es digital dentro del AD9361 y NO atraviesa la etapa analógica
 * del TX (mezclador, LO): ahí el desequilibrio medido es ~0. La calibración
 * solo tiene sentido con un bucle que incluya el RF (o con una medida externa
 * del espectro de salida); el algoritmo es el mismo.
 */
#ifndef BLADERF_LAB_CAL_H
#define BLADERF_LAB_CAL_H

#include "mod.h"

typedef struct {
    double q_re, q_im;   /* q = ν/μ */
    double dc_re, dc_im; /* c (misma escala que x, |x| ≤ 1) */
} cal_t;

typedef struct {
    double irr_db;    /* rechazo de imagen medido, −20·log10|q| */
    double lo_dbc;    /* fuga de LO respecto a la amplitud del tono */
    double delay;     /* retardo total del bucle en muestras */
    double gain_db;   /* |H|, ganancia del bucle */
    double q_abs;
} cal_meas_t;

#define CAL_CAPTURE_LEN 65536 /* muestras a capturar */
#define CAL_PRE_ZEROS 8192    /* ceros antes de la ráfaga en el flujo de TX */
#define CAL_POST_ZEROS 16384  /* ceros después */

/* Ráfaga de calibración a la tasa 'rate' (tono a rate/16), amplitud amp. */
int cal_burst_make(iqbuf_t *b, double rate, double amp);
/* Longitud de la ráfaga en muestras. */
int cal_burst_len(void);
/* Estima q y c de la captura rx (n muestras). La captura debe contener la
 * ráfaga entera y haberse tomado con los ceros previos del flujo de TX; amp es
 * la amplitud usada en cal_burst_make. 0 si va bien, <0 con el motivo. */
int cal_estimate(const cf32 *rx, int n, double amp, cal_t *out, cal_meas_t *m, char *err, size_t errlen);

void cal_identity(cal_t *c);
bool cal_is_identity(const cal_t *c);
/* Precorrige x en el sitio; si el pico supera 0,95 reescala todo para no recortar. */
void cal_apply(const cal_t *c, cf32 *x, int n);
/* Rechazo de imagen (dB) de un tono a f_norm·fs en x: |P(+f)| / |P(−f)|. */
double cal_tone_irr_db(const cf32 *x, int n, double f_norm);

#endif
