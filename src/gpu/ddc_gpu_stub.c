/*
 * ddc_gpu_stub.c — Sustituto sin CUDA del DDC en GPU. ddc_gpu_create()
 * devuelve NULL y el host sigue con el DDC de CPU. Ver ddc_gpu.h.
 */
#include "ddc_gpu.h"

#include <stdio.h>

ddc_gpu_t *ddc_gpu_create(int max_block, char *err, size_t errlen)
{
    (void)max_block;
    if (err)
        snprintf(err, errlen, "compilado sin soporte CUDA");
    return NULL;
}
void ddc_gpu_destroy(ddc_gpu_t *g) { (void)g; }
const char *ddc_gpu_device_name(ddc_gpu_t *g) { (void)g; return "ninguna"; }
int ddc_gpu_add(ddc_gpu_t *g, const ddc_plan_t *p, int64_t c, int64_t f) { (void)g; (void)p; (void)c; (void)f; return -1; }
void ddc_gpu_remove(ddc_gpu_t *g, int id) { (void)g; (void)id; }
int ddc_gpu_retune(ddc_gpu_t *g, int id, int64_t c, int64_t f) { (void)g; (void)id; (void)c; (void)f; return -1; }
int ddc_gpu_ntaps(ddc_gpu_t *g, int id) { (void)g; (void)id; return -1; }
int ddc_gpu_set_lead(ddc_gpu_t *g, int id, long lead) { (void)g; (void)id; (void)lead; return -1; }
void ddc_gpu_set_dc_removal(ddc_gpu_t *g, bool on) { (void)g; (void)on; }
int ddc_gpu_process(ddc_gpu_t *g, const int16_t *iq, int n, double fs, int64_t c, ddc_gpu_out_fn fn, void *ctx)
{
    (void)g; (void)iq; (void)n; (void)fs; (void)c; (void)fn; (void)ctx;
    return -1;
}
double ddc_gpu_last_ms(ddc_gpu_t *g) { (void)g; return 0; }
double ddc_gpu_busy_s(ddc_gpu_t *g) { (void)g; return 0; }
