/*
 * pfb_gpu.h — Canalizador polifásico (PFB) en GPU: M canales uniformes con
 * sobremuestreo ov (2× por omisión), como base de un escáner de banda completa.
 *
 * Referencia: B. Merry, "Efficient channelization on a Graphics Processing
 * Unit", arXiv:2303.09886 (FIR polifásico + FFT por lotes en un solo núcleo de
 * GPU, a tasas de varios Gsps; el límite real es el PCIe). Aquí se sigue la
 * misma estructura "ponderar-solapar-sumar" (WOLA) con FFT propia (sin cuFFT):
 *
 *   para cada trama t (cada D = M/ov muestras), con e = fin de la ventana:
 *     z[m]   = Σ_{p<P} h[m + p·M] · x[e − m − p·M]        m = 0…M−1   (pliegue)
 *     X      = FFT_M(z)
 *     y_k[t] = X[(−k) mod M] · e^{−j2π·k·(e mod M)/M}                  (canal k)
 *
 *   h es un paso bajo de P·M coeficientes (Kaiser, 80 dB), ganancia 1 en
 *   continua. Un tono en el centro del canal k sale con su amplitud original.
 *   El factor e^{−j2πk·e/M} cuenta el desplazamiento absoluto de la trama:
 *   sin él la fase de cada canal saltaría al pasar de una trama a la siguiente
 *   cuando D no es múltiplo de M (para ov = 2 sería un signo (−1)^{k·t}).
 *
 * Canal k está centrado en k·fs/M (k ≥ M/2 → frecuencias negativas). Tasa de
 * salida por canal: ov·fs/M. El filtro tiene corte a ±fs/M (−6 dB en el centro
 * del canal vecino), banda de paso ≈ ±0,69·fs/M y de rechazo desde ≈ 1,31·fs/M:
 * los canales a ≥ 2 posiciones quedan > 60 dB abajo y el diezmado no aliasea
 * sobre la banda de paso (la ventana ov = 2 hace que las transiciones de
 * canales contiguos se solapen en vez de dejar huecos).
 *
 * Disposición de la salida: out[t·M + k] (trama a trama; cada trama es una
 * "línea" de espectro en tiempo-frecuencia).
 *
 * Sin CUDA se enlaza pfb_gpu_stub.c y pfb_gpu_create() devuelve NULL.
 */
#ifndef BLADERF_LAB_PFB_GPU_H
#define BLADERF_LAB_PFB_GPU_H

#include "../common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pfb_gpu pfb_gpu_t;

/* M: potencia de 2 en [8, 4096]; ov ∈ {1, 2, 4} (D = M/ov); P: coeficientes por
 * fase (≥ 4, típico 8); max_samples: máximo de muestras por llamada. */
pfb_gpu_t *pfb_gpu_create(int M, int ov, int P, int max_samples, char *err, size_t errlen);
void pfb_gpu_destroy(pfb_gpu_t *g);
const char *pfb_gpu_device_name(pfb_gpu_t *g);
int pfb_gpu_channels(const pfb_gpu_t *g);
int pfb_gpu_decim(const pfb_gpu_t *g); /* D */
int pfb_gpu_ntaps(const pfb_gpu_t *g); /* P·M */
const float *pfb_gpu_proto(const pfb_gpu_t *g);
/* Nº máximo de tramas que puede devolver una llamada (tamaño mínimo de out). */
int pfb_gpu_max_frames(const pfb_gpu_t *g);

/* Procesa n muestras Q11 (I,Q,I,Q…); las que no completen una trama se
 * guardan para la llamada siguiente. out recibe frames·M muestras (puede ser
 * NULL para medir solo el cálculo). Devuelve las tramas (≥ 0) o −1. */
int pfb_gpu_process(pfb_gpu_t *g, const int16_t *iq, int n, cf32 *out);
/* ms de GPU de la última llamada (copias incluidas). */
double pfb_gpu_last_ms(const pfb_gpu_t *g);

/* Diseño del prototipo, en C puro: ntaps = P·M, ganancia 1 en continua. */
void pfb_design_proto(float *h, int M, int P);

#ifdef __cplusplus
}
#endif
#endif
