/*
 * nr_corr_gpu_stub.c — Sustituto sin CUDA de la correlación de NR en GPU.
 * nr_corr_gpu_create() devuelve NULL y nr_corr.c usa la ruta de CPU.
 */
#include "nr_corr_gpu.h"

nr_corr_gpu_t *nr_corr_gpu_create(int nfft, int nt, const cf32 *const *tmpl, const int *tlen)
{
    (void)nfft; (void)nt; (void)tmpl; (void)tlen;
    return NULL;
}
void nr_corr_gpu_destroy(nr_corr_gpu_t *c) { (void)c; }
int nr_corr_gpu_run(nr_corr_gpu_t *c, const cf32 *x, int n, float *out)
{
    (void)c; (void)x; (void)n; (void)out;
    return -1;
}
double nr_corr_gpu_last_ms(const nr_corr_gpu_t *c) { (void)c; return 0; }
