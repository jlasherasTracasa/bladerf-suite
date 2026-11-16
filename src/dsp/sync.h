/*
 * sync.h — Sincronización: AGC, discriminador FM, PLL, Costas, FLL,
 * recuperación de reloj de símbolo (Gardner, Mueller-Müller) con interpolador
 * de Farrow cúbico, y detector de picos de correlación.
 *
 * Teoría resumida
 * ---------------
 * PLL de 2º orden. Un lazo con detector de fase de error e[n], filtro PI y NCO:
 *     freq += β·e;   phase += freq + α·e
 * Para un ancho de banda de ruido normalizado Bn·T (por muestra) y
 * amortiguamiento ζ (0.707 típico):  θ = Bn / (ζ + 1/(4ζ)),  d = 1 + 2ζθ + θ²,
 *     α = 4ζθ / d,     β = 4θ² / d.
 * Es el diseño de Rice "Digital Communications: A Discrete-Time Approach" (§8).
 * Fase en radianes, frecuencia en rad/muestra.
 *
 * Costas (M-PSK). Se desrota y = x·e^{−jφ} y el error de fase es:
 *   BPSK : e = sgn(Re y)·Im y / |y|          (ambigüedad de 180°)
 *   QPSK : e = [sgn(Re y)·Im y − sgn(Im y)·Re y] / |y|   (ambigüedad de 90°)
 *   8PSK : e = arg(y^8)/8                    (ambigüedad de 45°)
 * Rango de enganche ~ ±Bn. El resultado queda alineado salvo la ambigüedad:
 * usa codificación diferencial o una palabra de sincronía para resolverla.
 *
 * FLL (lazo de frecuencia). Engancha con offsets mucho mayores que un PLL y no
 * necesita fase:
 *   BANDEDGE: dos filtros paso banda centrados en los bordes de la banda de
 *     la señal con roll-off α: ±(1+α)/(2·sps) ciclos/muestra y anchura α/sps.
 *     e = (|u|² − |l|²)/(|u|² + |l|²) es ∝ al offset (la energía de un borde
 *     crece cuando el espectro se desplaza hacia él). Requiere señal conformada
 *     (RRC/RC) con α > 0 y sps ≥ 2. Es un FLL "grueso": precisión ~1 % de la tasa
 *     de símbolo (jitter por el patrón de datos); úsalo para llevar el offset
 *     dentro del alcance del Costas y baja loop_bw (~5e-4) para menos jitter.
 *   DISC: discriminador por autocorrelación de retardo 1: e = arg(y[n]^M·conj(y[n−1]^M))/M.
 *     Válido para portadora sin modular (M=1) o tras elevar a M una señal
 *     M-PSK (M=2 BPSK, 4 QPSK). Alcance ±fs/(2M).
 *   cfo_estimate_mpower(): versión por bloques (estimador de Kay) para tramas.
 *
 * Recuperación de reloj (symsync_t). Un bucle con tiempo continuo T (en
 * muestras de entrada): los símbolos se leen en T, T+ω, T+2ω… interpolando con
 * un Farrow cúbico (Lagrange de 4 puntos, mu ∈ [0,1) entre x[0] y x[1]):
 *     v3 = −x[−1]/6 + x[0]/2 − x[1]/2 + x[2]/6      v2 = x[−1]/2 − x[0] + x[1]/2
 *     v1 = −x[−1]/3 − x[0]/2 + x[1] − x[2]/6        v0 = x[0]
 *     y(mu) = ((v3·mu + v2)·mu + v1)·mu + v0
 *   Gardner: e = Re{ y(T−ω/2)·conj(y(T) − y(T−ω)) }   (no necesita decisiones,
 *     sirve en cualquier modulación; necesita sps ≥ 2).
 *   Mueller-Müller: e = Re{ conj(d[k−1])·y[k] − conj(d[k])·y[k−1] }, d = decisión
 *     del slicer; 1 muestra/símbolo, sps ≥ 2 en la entrada.
 * Con e > 0 (muestreo tardío): T_sig = T + ω − α·e,  ω −= β·e (limitado a
 * ±tol alrededor de sps). La entrada debe estar normalizada (amplitud ~1:
 * usa agc_t antes) y filtrada con el filtro adaptado (RRC) para Gardner/M&M.
 * El filtro adaptado debe aplicarse ANTES (a la tasa de entrada).
 *
 * Correlador de sincronía (corr_t). Métrica normalizada
 *     m[n] = |Σ x[n−L+1+i]·conj(ref[i])|² / (Σ|x|²·Σ|ref|²)   ∈ [0, 1]
 * (Cauchy-Schwarz: 1 = la ventana es la referencia salvo fase y escala). Un
 * pico es el máximo de m dentro de 'window' muestras desde que supera el umbral.
 */
#ifndef BLADERF_LAB_SYNC_H
#define BLADERF_LAB_SYNC_H

#include "../common.h"

/* ---- AGC ---------------------------------------------------------------- */
typedef struct {
    float gain, target, attack, decay, max_gain;
} agc_t;
/* target: amplitud de salida deseada; attack/decay: tasas por muestra (~1e-3..1e-1)
 * cuando la salida está por encima / por debajo del objetivo. */
void agc_init(agc_t *a, float target, float attack, float decay);
void agc_process(agc_t *a, const cf32 *in, int n, cf32 *out);
void agc_process_f(agc_t *a, const float *in, int n, float *out);

/* ---- Discriminador FM --------------------------------------------------- */
typedef struct { cf32 prev; float gain; } fmdisc_t;
/* salida = (Δfase por muestra)·fs/(2π·dev_hz): ±1 para ±dev_hz de desviación. */
void fmdisc_init(fmdisc_t *d, double fs, double dev_hz);
void fmdisc_process(fmdisc_t *d, const cf32 *in, int n, float *out);

/* ---- PLL de 2º orden ---------------------------------------------------- */
typedef struct {
    float phase, freq, alpha, beta, freq_min, freq_max;
} pll_t;
void pll_init(pll_t *p, double loop_bw, double damping);   /* loop_bw = Bn/fs */
void pll_set_limits(pll_t *p, double freq_min, double freq_max);   /* rad/muestra */
void pll_update(pll_t *p, float err);                     /* un paso del lazo */
cf32 pll_derotate(const pll_t *p, cf32 x);                /* x·e^{−jφ} */
/* Sigue una portadora (piloto, CW): out = x derotada; err = arg(out). */
void pll_track_carrier(pll_t *p, const cf32 *in, int n, cf32 *out);
static inline double pll_freq_hz(const pll_t *p, double fs) { return p->freq * fs / (2.0 * M_PI); }

/* ---- Costas ------------------------------------------------------------- */
typedef struct { pll_t pll; int order; } costas_t;
int costas_init(costas_t *c, int order, double loop_bw, double damping);   /* order 2, 4 u 8 */
void costas_process(costas_t *c, const cf32 *in, int n, cf32 *out);
static inline double costas_freq_hz(const costas_t *c, double fs) { return pll_freq_hz(&c->pll, fs); }

/* ---- FLL ---------------------------------------------------------------- */
typedef struct fll fll_t;
/* BANDEDGE: sps y roll-off de la señal; ntaps = 0 → automático. */
fll_t *fll_create_bandedge(double sps, double rolloff, int ntaps, double loop_bw);
/* DISC: potencia M (1 portadora, 2 BPSK, 4 QPSK). */
fll_t *fll_create_disc(int power, double loop_bw);
void fll_destroy(fll_t *f);
/* Desrota 'in' con la frecuencia estimada; out puede ser igual que in. */
void fll_process(fll_t *f, const cf32 *in, int n, cf32 *out);
double fll_freq_hz(const fll_t *f, double fs);
void fll_reset(fll_t *f);
/* Estimación por bloque del offset (Hz) con la fórmula de Kay sobre x^M. */
double cfo_estimate_mpower(const cf32 *x, int n, int power, double fs);

/* ---- Interpolador de Farrow cúbico ------------------------------------- */
/* x4 apunta a x[−1], x[0], x[1], x[2]; devuelve el valor en x[0] + mu. */
float farrow3_f(const float *x4, float mu);
cf32 farrow3_c(const cf32 *x4, float mu);

/* ---- Recuperación de reloj de símbolo ---------------------------------- */
typedef enum { TED_GARDNER = 0, TED_MM = 1 } ted_t;
typedef enum { SLICE_BPSK = 0, SLICE_QPSK = 1, SLICE_PAM4 = 2 } slicer_t;   /* solo M&M */
typedef struct symsync symsync_t;

/* sps: muestras por símbolo nominales (2..32); tol: variación relativa admitida
 * del periodo (p. ej. 0.005); loop_bw: Bn·T por SÍMBOLO (p. ej. 0.01). */
symsync_t *symsync_create(ted_t ted, slicer_t slicer, double sps, double tol, double loop_bw, double damping);
void symsync_destroy(symsync_t *s);
void symsync_reset(symsync_t *s);
/* PAM4: niveles ±a y ±3a (por defecto a = 1/3 → ±1/3, ±1). */
void symsync_set_pam4_level(symsync_t *s, float a);
/* Cota de salidas para n muestras de entrada. */
int symsync_max_out(const symsync_t *s, int n);
/* Devuelve nº de símbolos escritos (≤ out_cap). */
int symsync_process(symsync_t *s, const cf32 *in, int n, cf32 *out, int out_cap);
int symsync_process_f(symsync_t *s, const float *in, int n, float *out, int out_cap);
double symsync_period(const symsync_t *s);   /* periodo estimado (muestras/símbolo) */

/* ---- Detector de picos de correlación ---------------------------------- */
typedef struct {
    int64_t pos;     /* índice absoluto (desde la creación) de la ÚLTIMA muestra de la ventana */
    float metric;    /* 0..1 */
    cf32 corr;       /* correlación compleja: arg = fase de portadora, abs/√(E·E_ref) */
} corr_peak_t;
typedef struct corr corr_t;

corr_t *corr_create(const cf32 *ref, int len, float threshold, int window);
corr_t *corr_create_f(const float *ref, int len, float threshold, int window);
void corr_destroy(corr_t *c);
void corr_reset(corr_t *c);
/* Escribe hasta max_peaks picos; devuelve cuántos. */
int corr_process(corr_t *c, const cf32 *in, int n, corr_peak_t *peaks, int max_peaks);
int corr_process_f(corr_t *c, const float *in, int n, corr_peak_t *peaks, int max_peaks);

#endif
