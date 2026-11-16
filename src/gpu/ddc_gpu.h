/*
 * ddc_gpu.h — DDC por lotes en GPU: N canales de frecuencia arbitraria sobre
 * UN mismo bloque IQ (Q11 int16) que se copia una sola vez a la GPU.
 *
 * Problema: el host crea una instancia de decodificador por canal estrecho y
 * cada una hacía su propio DDC en CPU (NCO + FIR diezmadores sobre los 20–61
 * Msps). Con 16 instancias eso son 16 pasadas completas sobre la entrada.
 *
 * Idea (análoga al canalizador de B. Merry, arXiv:2303.09886, pero para
 * canales NO alineados a una rejilla): el coste de un DDC se concentra en el
 * primer FIR diezmador, que trabaja a la tasa de entrada. Aquí ese paso se
 * hace en GPU con UN SOLO filtro polifásico por canal:
 *
 *     y[k] = e^{-jθ·e_k} · Σ_j c[j]·x[e_k − j],     e_k = (k+1)·D − 1
 *     c[j] = h[j]·e^{+jθ·j} / 2048,                 θ = 2π(f − fc)/fs
 *
 * Es la misma operación que "NCO → FIR real h → diezmar por D" (reordenando
 * la suma: x[m]·e^{-jθm} = e^{-jθe}·x[m]·e^{+jθ(e−m)}), pero:
 *   - solo se calculan las salidas (1 de cada D): coste por muestra de entrada
 *     ≈ ntaps/D complejas×enteras (~13 con 80 dB de rechazo), no ntaps;
 *   - no hay NCO por muestra: los coeficientes complejos c[j] se precalculan
 *     en doble precisión en la CPU y la fase de salida usa un acumulador
 *     entero de 64 bits (sin deriva por larga que sea la captura);
 *   - cada WARP calcula una salida: los 32 hilos recorren los coeficientes de
 *     32 en 32 (lecturas coalescidas) y reducen con __shfl_xor.
 * La salida queda a la tasa intermedia fs/D ∈ [fs_out, ~1,5·fs_out); el ajuste
 * fino (filtro de canal + remuestreo exacto a fs_out) sigue en CPU con las
 * piezas de dsp/filter.c (ddc_fine_t), idéntico al final de ddc.c.
 *
 * Quita la continua como el motor (paso alto de 1 polo): la media de cada
 * bloque, suavizada, se resta de las muestras antes de filtrar.
 *
 * Sin CUDA se enlaza ddc_gpu_stub.c: ddc_gpu_create() devuelve NULL.
 * Las funciones de "plan" y "etapa fina" son C puro y existen siempre.
 */
#ifndef BLADERF_LAB_DDC_GPU_H
#define BLADERF_LAB_DDC_GPU_H

#include "../common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Por debajo de este diezmado no compensa lanzar nada a la GPU. */
#define DDC_GPU_MIN_DECIM 16
#define DDC_GPU_MAX_TAPS 262143

/* ---------------- Plan de un canal (C puro, ddc_plan.c) ---------------- */
typedef struct {
    double fs_in, fs_out, bw;  /* bw ya acotada a 0,9·fs_out, como ddc.c */
    int decim;                 /* D (solo factores 2,3,5,7, como ddc.c) */
    double fs_mid;             /* fs_in / D */
    int ntaps;                 /* impar */
    float *h;                  /* ntaps reales, ganancia 1 en continua */
} ddc_plan_t;

/* 0 si es realizable en GPU; <0 si el diezmado sale menor que
 * DDC_GPU_MIN_DECIM, la tasa es incoherente o falta memoria. */
int ddc_plan_design(ddc_plan_t *p, double fs_in, double fs_out, double bw_hz);
void ddc_plan_free(ddc_plan_t *p);
/* Retardo de grupo (en muestras de ENTRADA) del diezmado de ddc.c: espejo de
 * su diseño en etapas. Sirve para alinear en el tiempo ambas rutas en pruebas
 * (ver ddc_gpu_set_lead). Devuelve <0 si ddc_create fallaría. */
long ddc_plan_cpu_delay(double fs_in, double fs_out, double bw_hz);

/* ---------- Etapa fina en CPU: filtro de canal + remuestreo ---------- */
typedef struct ddc_fine ddc_fine_t;
ddc_fine_t *ddc_fine_create(double fs_mid, double fs_out, double bw_hz);
void ddc_fine_destroy(ddc_fine_t *f);
size_t ddc_fine_max_out(const ddc_fine_t *f, int n);
int ddc_fine_process(ddc_fine_t *f, const cf32 *in, int n, cf32 *out, int out_cap);

/* ---------------------- Banco de canales en GPU ---------------------- */
typedef struct ddc_gpu ddc_gpu_t;

/* max_block: nº máximo de muestras por llamada a process. NULL (y err) si no
 * hay GPU o se compiló sin CUDA. */
ddc_gpu_t *ddc_gpu_create(int max_block, char *err, size_t errlen);
void ddc_gpu_destroy(ddc_gpu_t *g);
const char *ddc_gpu_device_name(ddc_gpu_t *g);
/* Añade un canal (copia los coeficientes del plan). Devuelve su id ≥ 0 o −1. */
int ddc_gpu_add(ddc_gpu_t *g, const ddc_plan_t *plan, int64_t center_hz, int64_t freq_hz);
void ddc_gpu_remove(ddc_gpu_t *g, int id);
/* Cambia la frecuencia conservando la fase del oscilador (sin clics). */
int ddc_gpu_retune(ddc_gpu_t *g, int id, int64_t center_hz, int64_t freq_hz);
/* Nº de coeficientes del canal (para calcular 'lead'). */
int ddc_gpu_ntaps(ddc_gpu_t *g, int id);
/* Adelanto (≤ 0) de la ventana de cada salida respecto a la posición normal
 * e_k = (k+1)·D−1: con lead = (ntaps−1)/2 − ddc_plan_cpu_delay() la salida k
 * coincide EN EL TIEMPO con la de ddc.c (solo para pruebas; en producción 0). */
int ddc_gpu_set_lead(ddc_gpu_t *g, int id, long lead);
void ddc_gpu_set_dc_removal(ddc_gpu_t *g, bool on);

/* y: n salidas de ese canal a fs_mid. t_first: segundos desde el inicio del
 * bloque hasta la muestra de entrada que centra la primera salida. */
typedef void (*ddc_gpu_out_fn)(void *ctx, int chan, const cf32 *y, int n, double t_first);
/* Procesa un bloque de n ≤ max_block muestras Q11 (I,Q,I,Q…). Los canales
 * cuya (fs, centro) no coincida con la del bloque se saltan. Si fs cambia se
 * reinicia el flujo. Bloqueante: solo para hilos propios (no el hilo DSP).
 * Devuelve el nº total de salidas o −1. */
int ddc_gpu_process(ddc_gpu_t *g, const int16_t *iq, int n, double fs, int64_t center_hz, ddc_gpu_out_fn fn,
                    void *ctx);
/* Tiempo de GPU (ms) del último bloque y total acumulado (s) de núcleo. */
double ddc_gpu_last_ms(ddc_gpu_t *g);
double ddc_gpu_busy_s(ddc_gpu_t *g);

#ifdef __cplusplus
}
#endif
#endif
