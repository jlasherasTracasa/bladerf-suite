/*
 * filter.h — Diseño y aplicación de filtros digitales.
 *
 * Bloques que ofrece este módulo (todos propios):
 *
 *   Diseño de filtros FIR por ventana de Kaiser
 *       Un FIR paso bajo ideal tiene respuesta h[n] = 2fc·sinc(2fc·n). Se
 *       trunca a N coeficientes y se multiplica por una ventana de Kaiser,
 *       cuyo parámetro β controla el compromiso entre atenuación y anchura de
 *       la banda de transición. Las fórmulas empíricas de Kaiser dan N y β a
 *       partir de la atenuación deseada A (dB) y la transición Δf:
 *           β = 0.1102·(A − 8.7)                       (A > 50 dB)
 *           N ≈ (A − 8) / (2.285 · 2π·Δf)              (Δf normalizada a fs)
 *
 *   firdec_t   — FIR diezmador: datos complejos, coeficientes reales. Solo
 *                calcula una de cada D salidas (las demás se descartarían),
 *                así el coste es proporcional a la tasa de SALIDA.
 *   cfir_t     — FIR con coeficientes complejos (filtro paso banda asimétrico,
 *                necesario para BLU/SSB donde solo se quiere un lado).
 *   resamp_t   — Remuestreador fraccionario polifásico para pasar de una tasa
 *                cualquiera (p. ej. 52 083 Hz) a 48 000 Hz exactos de audio.
 *   nco_t      — Oscilador numérico (NCO) para desplazar frecuencias.
 *
 * Técnica de la "línea de retardo duplicada": cada filtro guarda su historia
 * en un búfer de 2·N posiciones y escribe cada muestra dos veces (en i y en
 * i+N). Así las N últimas muestras están siempre contiguas en memoria y el
 * producto escalar es un bucle simple que el compilador vectoriza (SIMD).
 */
#ifndef BLADERF_LAB_FILTER_H
#define BLADERF_LAB_FILTER_H

#include "../common.h"

/* ---- Diseño ------------------------------------------------------------ */

/* Nº de coeficientes (impar) para una transición Δf/fs y atenuación A dB. */
int fir_kaiser_ntaps(double trans_norm, double atten_db, int max_taps);
double kaiser_beta(double atten_db);
/* Paso bajo con corte fc/fs = cutoff_norm. Ganancia en continua = 1. */
void fir_design_lowpass(float *h, int ntaps, double cutoff_norm, double beta);

/* ---- FIR diezmador (complejo × real) ----------------------------------- */
typedef struct {
    int ntaps;
    int decim;
    int phase;     /* cuenta de muestras de entrada desde la última salida */
    int pos;       /* posición de escritura en la línea de retardo */
    float *taps;
    cf32 *hist;    /* 2·ntaps muestras (línea de retardo duplicada) */
} firdec_t;

/* Diseña el filtro para diezmar por 'decim' protegiendo la banda ±pass_hz
 * cuando la tasa de entrada es fs_in. */
int firdec_init(firdec_t *f, int decim, double fs_in, double pass_hz, int max_taps);
void firdec_free(firdec_t *f);
/* Procesa n muestras; escribe hasta n/decim+1 salidas. Devuelve cuántas. */
int firdec_process(firdec_t *f, const cf32 *in, int n, cf32 *out);

/* ---- FIR de coeficientes complejos (sin diezmado) ---------------------- */
typedef struct {
    int ntaps;
    int pos;
    cf32 *taps;
    cf32 *hist;
} cfir_t;

/* Paso banda complejo que deja pasar [f_lo, f_hi] Hz (pueden ser negativas)
 * a la tasa fs, con transición trans_hz. */
int cfir_init_bandpass(cfir_t *f, double fs, double f_lo, double f_hi, double trans_hz, int max_taps);
void cfir_free(cfir_t *f);
void cfir_process(cfir_t *f, const cf32 *in, int n, cf32 *out);

/* ---- Remuestreador fraccionario polifásico (real) ---------------------- */
typedef struct {
    double step;   /* avance en muestras de entrada por cada salida = fin/fout */
    double frac;   /* posición fraccionaria de la próxima salida */
    int L;         /* coeficientes por fase */
    int P;         /* nº de fases (resolución temporal 1/P muestra) */
    float *bank;   /* P × L coeficientes */
    float *hist;   /* 2·L */
    int pos;
} resamp_t;

/* cutoff_hz: frecuencia de corte del filtro anti-aliasing incorporado. */
int resamp_init(resamp_t *r, double fs_in, double fs_out, double cutoff_hz, double trans_hz);
void resamp_free(resamp_t *r);
/* Devuelve las muestras escritas en out (cabe con holgura n·fout/fin + 2). */
int resamp_process(resamp_t *r, const float *in, int n, float *out);

/* ---- Remuestreador fraccionario polifásico (complejo) ------------------ */
/* Mismo diseño que resamp_t, pero la línea de retardo guarda muestras IQ y
 * los coeficientes (reales) se aplican a I y a Q por igual. Se usa en el DDC
 * (ddc.h) para llegar a la tasa EXACTA que pide cada decodificador: la
 * relación fin/fout se acumula en coma flotante de 64 bits (campo frac), así
 * que el error de tasa a largo plazo es nulo (< 1e-12 relativo). */
typedef struct {
    double step;   /* avance en muestras de entrada por salida = fin/fout */
    double frac;   /* posición fraccionaria de la próxima salida */
    int L;         /* coeficientes por fase */
    int P;         /* nº de fases */
    float *bank;   /* P × L */
    cf32 *hist;    /* 2·L */
    int pos;
} cresamp_t;

int cresamp_init(cresamp_t *r, double fs_in, double fs_out, double cutoff_hz, double trans_hz);
void cresamp_free(cresamp_t *r);
/* Devuelve las salidas escritas (caben n·fout/fin + 2). */
int cresamp_process(cresamp_t *r, const cf32 *in, int n, cf32 *out);

/* ---- Oscilador numérico ------------------------------------------------ */
/* Se implementa como un "rotor": el fasor actual se multiplica en cada
 * muestra por e^{jω}. Es más rápido que llamar a sin/cos y, renormalizando la
 * amplitud cada cierto tiempo, no deriva. */
typedef struct {
    cf32 phasor;
    cf32 step;
    int count;
} nco_t;

void nco_init(nco_t *o, double freq_hz, double fs);
/* out[i] = in[i] · e^{jωi}  (desplaza el espectro +freq_hz) */
void nco_mix(nco_t *o, const cf32 *in, int n, cf32 *out);

#endif
