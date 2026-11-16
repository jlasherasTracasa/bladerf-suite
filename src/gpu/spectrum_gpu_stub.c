/*
 * spectrum_gpu_stub.c — Sustituto sin CUDA del motor de espectro en GPU.
 *
 * Se compila en lugar de spectrum_gpu.cu cuando no hay compilador CUDA
 * (o con make CUDA=0). Todas las funciones indican "no disponible" y el
 * módulo de espectro recurre a la FFT de CPU.
 */
#include "spectrum_gpu.h"

#include <stdio.h>

gpu_spec_t *gpu_spec_create(int n, int max_batch, const float *window, char *err, size_t errlen)
{
    (void)n;
    (void)max_batch;
    (void)window;
    if (err)
        snprintf(err, errlen, "compilado sin soporte CUDA");
    return NULL;
}

int16_t *gpu_spec_host_buffer(gpu_spec_t *g) { (void)g; return NULL; }
int gpu_spec_submit(gpu_spec_t *g, int batch) { (void)g; (void)batch; return -1; }
int gpu_spec_poll(gpu_spec_t *g, float *power) { (void)g; (void)power; return -1; }
int gpu_spec_run(gpu_spec_t *g, int batch, float *power) { (void)g; (void)batch; (void)power; return -1; }
const char *gpu_spec_device_name(gpu_spec_t *g) { (void)g; return "ninguna"; }
void gpu_spec_destroy(gpu_spec_t *g) { (void)g; }
