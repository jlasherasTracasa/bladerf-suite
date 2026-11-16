/*
 * pfb_gpu_stub.c — Sustituto sin CUDA del canalizador PFB. Pfb_gpu_create()
 * devuelve NULL. Ver pfb_gpu.h.
 */
#include "pfb_gpu.h"

#include <stdio.h>

pfb_gpu_t *pfb_gpu_create(int M, int ov, int P, int max_samples, char *err, size_t errlen)
{
    (void)M; (void)ov; (void)P; (void)max_samples;
    if (err)
        snprintf(err, errlen, "compilado sin soporte CUDA");
    return NULL;
}
void pfb_gpu_destroy(pfb_gpu_t *g) { (void)g; }
const char *pfb_gpu_device_name(pfb_gpu_t *g) { (void)g; return "ninguna"; }
int pfb_gpu_channels(const pfb_gpu_t *g) { (void)g; return 0; }
int pfb_gpu_decim(const pfb_gpu_t *g) { (void)g; return 0; }
int pfb_gpu_ntaps(const pfb_gpu_t *g) { (void)g; return 0; }
const float *pfb_gpu_proto(const pfb_gpu_t *g) { (void)g; return NULL; }
int pfb_gpu_max_frames(const pfb_gpu_t *g) { (void)g; return 0; }
int pfb_gpu_process(pfb_gpu_t *g, const int16_t *iq, int n, cf32 *out) { (void)g; (void)iq; (void)n; (void)out; return -1; }
double pfb_gpu_last_ms(const pfb_gpu_t *g) { (void)g; return 0; }
