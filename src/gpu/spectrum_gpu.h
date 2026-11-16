/*
 * spectrum_gpu.h — Interfaz C del motor de espectro en GPU (CUDA).
 *
 * Si el programa se compila sin CUDA (make CUDA=0 o sin nvcc), se enlaza
 * spectrum_gpu_stub.c, cuyas funciones devuelven "no disponible" y el
 * programa usa automáticamente la FFT de CPU.
 */
#ifndef BLADERF_LAB_SPECTRUM_GPU_H
#define BLADERF_LAB_SPECTRUM_GPU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gpu_spec gpu_spec_t;

/* Crea el motor para FFTs de tamaño n, en lotes de hasta max_batch.
 * window: n coeficientes de ventana (se copian a la GPU).
 * Devuelve NULL y rellena err si no hay GPU utilizable. */
gpu_spec_t *gpu_spec_create(int n, int max_batch, const float *window, char *err, size_t errlen);
/* Búfer de memoria "pinned" (bloqueada) de max_batch·n muestras IQ int16 donde se
 * junta el SIGUIENTE lote (copiar desde aquí a la GPU es más rápido que desde
 * memoria normal). Hay dos ranuras: devuelve NULL si ambas tienen un lote en
 * vuelo sin recoger (el llamador debe descartar esa trama, no esperar). */
int16_t *gpu_spec_host_buffer(gpu_spec_t *g);
/* Lanza de forma ASÍNCRONA 'batch' FFTs desde el búfer anterior y vuelve ya.
 * Devuelve 0, o −1 si no hay ranura libre. */
int gpu_spec_submit(gpu_spec_t *g, int batch);
/* Si el lote más antiguo ya terminó, deja en power[n] la suma de |X[k]|² (sin
 * normalizar, sin fftshift) y devuelve cuántas FFT suma (> 0); 0 si aún no hay
 * resultado (no espera); −1 si falló. */
int gpu_spec_poll(gpu_spec_t *g, float *power);
/* Versión síncrona: submit + espera (pruebas). Devuelve 0 si todo fue bien. */
int gpu_spec_run(gpu_spec_t *g, int batch, float *power);
const char *gpu_spec_device_name(gpu_spec_t *g);
void gpu_spec_destroy(gpu_spec_t *g);

#ifdef __cplusplus
}
#endif

#endif
