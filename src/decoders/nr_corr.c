/*
 * nr_corr.c — Correlación normalizada por FFT con solape-guardado. Ver nr_corr.h.
 */
#include "nr_corr.h"

#include "../dsp/fft.h"
#include "../gpu/nr_corr_gpu.h"

#include <stdlib.h>
#include <string.h>

struct nr_corr {
    int nfft, nt, maxlen;
    int tlen[8];
    fft_plan_t *plan;
    cf32 *tf[8];        /* FFT de cada plantilla */
    cf32 *blk, *acc;
    double *ene;
    int ene_cap;
    nr_corr_gpu_t *gpu; /* NULL = ruta de CPU (sin GPU, BLADERF_NR_GPU=0 o fallo) */
};

nr_corr_t *nr_corr_create(int nfft, int nt, const cf32 *const *tmpl, const int *tlen)
{
    if (nt < 1 || nt > 8 || !fft_size_valid(nfft))
        return NULL;
    nr_corr_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->nfft = nfft;
    c->nt = nt;
    c->plan = fft_plan_create(nfft);
    c->blk = malloc(sizeof(cf32) * (size_t)nfft);
    c->acc = malloc(sizeof(cf32) * (size_t)nfft);
    if (!c->plan || !c->blk || !c->acc)
        goto fail;
    for (int t = 0; t < nt; t++) {
        c->tlen[t] = tlen[t];
        if (tlen[t] * 2 > nfft)
            goto fail;
        c->maxlen = tlen[t] > c->maxlen ? tlen[t] : c->maxlen;
        c->tf[t] = calloc((size_t)nfft, sizeof(cf32));
        if (!c->tf[t])
            goto fail;
        memcpy(c->tf[t], tmpl[t], sizeof(cf32) * (size_t)tlen[t]);
        fft_exec(c->plan, c->tf[t]);
    }
    /* GPU opcional: mismo resultado que la CPU (tests/test_nr_gpu.c). BLADERF_NR_GPU=0 la desactiva. */
    const char *env = getenv("BLADERF_NR_GPU");
    if (!env || env[0] != '0')
        c->gpu = nr_corr_gpu_create(nfft, nt, tmpl, tlen);
    return c;
fail:
    nr_corr_destroy(c);
    return NULL;
}

bool nr_corr_using_gpu(const nr_corr_t *c) { return c && c->gpu; }

void nr_corr_destroy(nr_corr_t *c)
{
    if (!c)
        return;
    for (int t = 0; t < 8; t++)
        free(c->tf[t]);
    free(c->blk);
    free(c->acc);
    free(c->ene);
    nr_corr_gpu_destroy(c->gpu);
    fft_plan_destroy(c->plan);
    free(c);
}

int nr_corr_run(nr_corr_t *c, const cf32 *x, int n, float *out)
{
    int nout = n - c->maxlen + 1;
    if (nout < 1)
        return -1;
    if (c->gpu) {
        int r = nr_corr_gpu_run(c->gpu, x, n, out);
        if (r == nout)
            return r;
        nr_corr_gpu_destroy(c->gpu); /* fallo de la GPU: se sigue en CPU */
        c->gpu = NULL;
    }
    if (c->ene_cap < n + 1) {
        free(c->ene);
        c->ene = malloc(sizeof(double) * (size_t)(n + 1));
        c->ene_cap = c->ene ? n + 1 : 0;
        if (!c->ene)
            return -1;
    }
    c->ene[0] = 0;
    for (int i = 0; i < n; i++)
        c->ene[i + 1] = c->ene[i] + (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
    /* Suelo de normalización: una señal sin ruido (fichero sintético) deja tramos de energía
     * casi nula donde el rizado del filtro de canal, minúsculo, correlaría "perfectamente".
     * Se suma ε = 1e-4 × (energía media por muestra) × (longitud máxima) al denominador. */
    const double eps = 1e-4 * (c->ene[n] / (double)n) * c->maxlen + 1e-20;
    const int nfft = c->nfft, step = nfft - (c->maxlen - 1);
    const float inv = 1.0f / (float)nfft;
    for (int b = 0; b < nout; b += step) {
        int len = n - b < nfft ? n - b : nfft;
        memcpy(c->blk, x + b, sizeof(cf32) * (size_t)len);
        if (len < nfft)
            memset(c->blk + len, 0, sizeof(cf32) * (size_t)(nfft - len));
        fft_exec(c->plan, c->blk);
        int mmax = step - 1 < nout - 1 - b ? step - 1 : nout - 1 - b;
        for (int t = 0; t < c->nt; t++) {
            const cf32 *tf = c->tf[t];
            for (int k = 0; k < nfft; k++) {
                /* conj(FFT(conj(X·conj(P)))) : IFFT por conjugación */
                float re = c->blk[k].re * tf[k].re + c->blk[k].im * tf[k].im;
                float im = c->blk[k].im * tf[k].re - c->blk[k].re * tf[k].im;
                c->acc[k].re = re;
                c->acc[k].im = -im;
            }
            fft_exec(c->plan, c->acc);
            float *o = out + (size_t)t * nout + b;
            int tl = c->tlen[t];
            for (int m = 0; m <= mmax; m++) {
                double e = c->ene[b + m + tl] - c->ene[b + m] + eps;
                float re = c->acc[m].re * inv, im = c->acc[m].im * inv;
                o[m] = (float)((re * re + im * im) / e);
            }
        }
    }
    return nout;
}
