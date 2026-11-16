/*
 * nr_corr_gpu.cu — Correlación normalizada de NR por FFT en GPU. Ver nr_corr_gpu.h.
 *
 * FFT: Stockham radix-2 con ping-pong en memoria global (misma formulación que
 * spectrum_gpu.cu; Govindaraju et al., SC'08). Con nfft = 8192, B ≈ 25 bloques y
 * NT = 6 plantillas el lote tiene ≈ 1,2 M puntos: ocupa la GPU de sobra aunque
 * cada FFT sea pequeña, que es justo lo que la CPU hace en serie.
 *
 * Correlación (teorema de la correlación): para un bloque X = FFT(x_b) y la
 * plantilla T = FFT(p), Σ_k x[m+k]·conj(p[k]) = IFFT(X·conj(T))[m] para m < nfft−tlen+1
 * (solape-guardado: los primeros nfft−maxlen+1 valores no están contaminados por el
 * enrollado circular). Como solo interesa |·|², la IFFT se hace con la FFT directa
 * sobre el conjugado: FFT(conj(Z)) = N·conj(IFFT(Z)) y |conj(·)| = |·|.
 *
 * Energía de ventana: E_i = Σ_{k<tlen} |x[i+k]|². Se calcula por trozos con suma
 * deslizante en doble precisión (cada hilo parte de la suma directa de su trozo, así
 * no hay deriva acumulada de float ni suma de prefijos global).
 */
#include "nr_corr_gpu.h"

#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXNT 8
#define TPB 256
#define ECHUNK 512 /* salidas por hilo en el cálculo de energía */

struct nr_corr_gpu {
    int nfft, log2n, nt, maxlen, tlen[MAXNT];
    float2 *d_tw;         /* e^{−j2πk/N}, k < N/2 */
    float2 *d_tf;         /* nt·nfft: FFT de las plantillas */
    int tl_h[MAXNT];
    int *d_tl;            /* longitudes en la GPU */
    /* capacidades que crecen con n */
    int cap_n, cap_b;
    float2 *d_x;          /* cap_n */
    float2 *d_a, *d_b;    /* cap_b·nt·nfft (ping-pong) */
    float *d_out;         /* nt·cap_nout */
    double *d_tot;
    float2 *h_x;          /* pinned, cap_n */
    float *h_out;         /* pinned, nt·cap_n */
    cudaStream_t st;
    cudaEvent_t e0, e1;
    float last_ms;
};

/* ---------------------------------------------------------------- kernels */

/* Etapa de Stockham para 'batch' FFTs de tamaño n (ver spectrum_gpu.cu). */
__global__ void k_stage(const float2 *__restrict__ in, float2 *__restrict__ out, const float2 *__restrict__ tw, int n,
                        int ns, int tw_stride, long total)
{
    long t = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= total)
        return;
    const int half = n >> 1;
    const long b = t / half;
    const int j = (int)(t - b * half);
    const float2 *x = in + b * n;
    float2 *y = out + b * n;
    const int k = j & (ns - 1);
    const float2 w = tw[k * tw_stride];
    float2 a = x[j], c = x[j + half];
    float2 bw = make_float2(c.x * w.x - c.y * w.y, c.x * w.y + c.y * w.x);
    const int d = (j - k) * 2 + k;
    y[d] = make_float2(a.x + bw.x, a.y + bw.y);
    y[d + ns] = make_float2(a.x - bw.x, a.y - bw.y);
}

/* Bloque b de solape-guardado: x[b·step … b·step+nfft), con ceros pasado el final. */
__global__ void k_pack(const float2 *__restrict__ x, float2 *__restrict__ blk, int n, int nfft, int step, long total)
{
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total)
        return;
    long b = i / nfft;
    long k = i - b * nfft;
    long s = b * step + k;
    blk[i] = s < n ? x[s] : make_float2(0.f, 0.f);
}

/* acc[(b·nt+t)·nfft + k] = conj(X_b[k]·conj(T_t[k])) */
__global__ void k_mul(const float2 *__restrict__ X, const float2 *__restrict__ T, float2 *__restrict__ acc, int nfft,
                      int nt, long total)
{
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total)
        return;
    int k = (int)(i & (nfft - 1));
    long bt = i / nfft;
    int t = (int)(bt % nt);
    long b = bt / nt;
    float2 xv = X[b * nfft + k], tv = T[(size_t)t * nfft + k];
    float re = xv.x * tv.x + xv.y * tv.y;
    float im = xv.y * tv.x - xv.x * tv.y;
    acc[i] = make_float2(re, -im);
}

/* Energía total de x (doble precisión) para el suelo ε. */
__global__ void k_total(const float2 *__restrict__ x, int n, double *tot)
{
    double s = 0;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x)
        s += (double)x[i].x * x[i].x + (double)x[i].y * x[i].y;
    for (int o = 16; o; o >>= 1)
        s += __shfl_xor_sync(0xffffffffu, s, o);
    __shared__ double sh[TPB / 32];
    if ((threadIdx.x & 31) == 0)
        sh[threadIdx.x >> 5] = s;
    __syncthreads();
    if (threadIdx.x == 0) {
        double r = 0;
        for (int w = 0; w < TPB / 32; w++)
            r += sh[w];
        atomicAdd(tot, r);
    }
}

/* Denominador E_i + ε, por trozos de ECHUNK salidas con suma deslizante. Hilo = (t, trozo).
 * Se escribe en 'out' (se divide después en k_out). */
__global__ void k_energy(const float2 *__restrict__ x, const int *__restrict__ tl, const double *__restrict__ tot,
                         float *__restrict__ out, int n, int nout, int nt, int maxlen)
{
    int nch = (nout + ECHUNK - 1) / ECHUNK;
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= nch * nt)
        return;
    int t = id / nch, ch = id - t * nch;
    int len = tl[t];
    int i0 = ch * ECHUNK, i1 = min(i0 + ECHUNK, nout);
    const double eps = 1e-4 * (*tot / (double)n) * maxlen + 1e-20;
    double e = 0;
    for (int k = 0; k < len; k++) {
        float2 v = x[i0 + k];
        e += (double)v.x * v.x + (double)v.y * v.y;
    }
    float *o = out + (size_t)t * nout;
    for (int i = i0; i < i1; i++) {
        o[i] = (float)(e + eps);
        if (i + 1 < i1) {
            float2 a = x[i + len], b = x[i];
            e += ((double)a.x * a.x + (double)a.y * a.y) - ((double)b.x * b.x + (double)b.y * b.y);
        }
    }
}

/* out[t·nout + b·step + m] = |Z|²/N² / out[...]   (Z = FFT del producto conjugado) */
__global__ void k_out(const float2 *__restrict__ Z, float *__restrict__ out, int nfft, int step, int nout, int nt,
                      float inv, long total)
{
    long i = (long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total) /* total = B·nt·step */
        return;
    int m = (int)(i % step);
    long bt = i / step;
    int t = (int)(bt % nt);
    long b = bt / nt;
    long pos = b * step + m;
    if (pos >= nout)
        return;
    float2 z = Z[(b * nt + t) * nfft + m];
    float re = z.x * inv, im = z.y * inv;
    float *o = out + (size_t)t * nout + pos;
    *o = (re * re + im * im) / *o;
}

/* ------------------------------------------------------------- API host */

static inline unsigned blocks_for(long total) { return (unsigned)((total + TPB - 1) / TPB); }

/* FFT directa de 'batch' bloques; devuelve el búfer con el resultado. */
static float2 *fft_batch(nr_corr_gpu_t *g, float2 *a, float2 *b, long batch)
{
    float2 *in = a, *out = b;
    const long bfly = batch * (g->nfft / 2);
    for (int ns = 1; ns < g->nfft; ns <<= 1) {
        k_stage<<<blocks_for(bfly), TPB, 0, g->st>>>(in, out, g->d_tw, g->nfft, ns, g->nfft / (2 * ns), bfly);
        float2 *t = in;
        in = out;
        out = t;
    }
    return in;
}

static int ensure(nr_corr_gpu_t *g, int n)
{
    const int step = g->nfft - (g->maxlen - 1);
    const int nout = n - g->maxlen + 1;
    const int B = (nout + step - 1) / step;
    if (n <= g->cap_n && B <= g->cap_b)
        return 0;
    cudaStreamSynchronize(g->st);
    cudaFree(g->d_x); cudaFree(g->d_a); cudaFree(g->d_b); cudaFree(g->d_out);
    cudaFreeHost(g->h_x); cudaFreeHost(g->h_out);
    g->d_x = g->d_a = g->d_b = NULL; g->d_out = NULL; g->h_x = NULL; g->h_out = NULL;
    g->cap_n = g->cap_b = 0;
    const int cn = n + n / 4, cb = B + B / 4 + 1;
    if (cudaMalloc((void **)&g->d_x, sizeof(float2) * (size_t)cn) != cudaSuccess ||
        cudaMalloc((void **)&g->d_a, sizeof(float2) * (size_t)cb * g->nt * g->nfft) != cudaSuccess ||
        cudaMalloc((void **)&g->d_b, sizeof(float2) * (size_t)cb * g->nt * g->nfft) != cudaSuccess ||
        cudaMalloc((void **)&g->d_out, sizeof(float) * (size_t)g->nt * cn) != cudaSuccess ||
        cudaMallocHost((void **)&g->h_x, sizeof(float2) * (size_t)cn) != cudaSuccess ||
        cudaMallocHost((void **)&g->h_out, sizeof(float) * (size_t)g->nt * cn) != cudaSuccess)
        return -1;
    g->cap_n = cn;
    g->cap_b = cb;
    return 0;
}

extern "C" void nr_corr_gpu_destroy(nr_corr_gpu_t *g)
{
    if (!g)
        return;
    cudaFree(g->d_tw); cudaFree(g->d_tf); cudaFree(g->d_tl); cudaFree(g->d_x); cudaFree(g->d_a);
    cudaFree(g->d_b); cudaFree(g->d_out); cudaFree(g->d_tot);
    cudaFreeHost(g->h_x); cudaFreeHost(g->h_out);
    if (g->e0) cudaEventDestroy(g->e0);
    if (g->e1) cudaEventDestroy(g->e1);
    if (g->st) cudaStreamDestroy(g->st);
    free(g);
}

extern "C" nr_corr_gpu_t *nr_corr_gpu_create(int nfft, int nt, const cf32 *const *tmpl, const int *tlen)
{
    int ndev = 0;
    if (nt < 1 || nt > MAXNT || nfft < 16 || (nfft & (nfft - 1)) || cudaGetDeviceCount(&ndev) != cudaSuccess || ndev == 0)
        return NULL;
    nr_corr_gpu_t *g = (nr_corr_gpu_t *)calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->nfft = nfft;
    g->nt = nt;
    while ((1 << g->log2n) < nfft)
        g->log2n++;
    for (int t = 0; t < nt; t++) {
        if (tlen[t] * 2 > nfft) {
            free(g);
            return NULL;
        }
        g->tlen[t] = tlen[t];
        g->maxlen = tlen[t] > g->maxlen ? tlen[t] : g->maxlen;
    }
    float2 *tw = (float2 *)malloc(sizeof(float2) * (size_t)(nfft / 2));
    float2 *tp = (float2 *)calloc((size_t)nt * nfft, sizeof(float2));
    for (int k = 0; k < nfft / 2; k++) {
        double a = -2.0 * M_PI * k / nfft;
        tw[k] = make_float2((float)cos(a), (float)sin(a));
    }
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < tlen[t]; k++)
            tp[(size_t)t * nfft + k] = make_float2(tmpl[t][k].re, tmpl[t][k].im);
    float2 *tmp_b = NULL;
    bool ok = cudaStreamCreateWithFlags(&g->st, cudaStreamNonBlocking) == cudaSuccess &&
              /* bloqueo por interrupción: la CPU duerme mientras la GPU trabaja (no gira) */
              cudaEventCreateWithFlags(&g->e0, cudaEventBlockingSync) == cudaSuccess &&
              cudaEventCreateWithFlags(&g->e1, cudaEventBlockingSync) == cudaSuccess &&
              cudaMalloc((void **)&g->d_tw, sizeof(float2) * (size_t)(nfft / 2)) == cudaSuccess &&
              cudaMalloc((void **)&g->d_tf, sizeof(float2) * (size_t)nt * nfft) == cudaSuccess &&
              cudaMalloc((void **)&tmp_b, sizeof(float2) * (size_t)nt * nfft) == cudaSuccess &&
              cudaMalloc((void **)&g->d_tl, sizeof(int) * MAXNT) == cudaSuccess &&
              cudaMalloc((void **)&g->d_tot, sizeof(double)) == cudaSuccess &&
              cudaMemcpyAsync(g->d_tw, tw, sizeof(float2) * (size_t)(nfft / 2), cudaMemcpyHostToDevice, g->st) == cudaSuccess &&
              cudaMemcpyAsync(g->d_tl, tlen, sizeof(int) * (size_t)nt, cudaMemcpyHostToDevice, g->st) == cudaSuccess &&
              cudaMemcpyAsync(g->d_tf, tp, sizeof(float2) * (size_t)nt * nfft, cudaMemcpyHostToDevice, g->st) == cudaSuccess &&
              /* en el MISMO flujo que los núcleos (cudaMemcpy síncrono no se ordena con un flujo no bloqueante) */
              cudaStreamSynchronize(g->st) == cudaSuccess;
    free(tw);
    free(tp);
    if (ok) {
        /* FFT de las plantillas (una vez); el resultado queda en d_tf o en tmp_b según la paridad. */
        float2 *r = fft_batch(g, g->d_tf, tmp_b, nt);
        if (r != g->d_tf)
            ok = cudaMemcpyAsync(g->d_tf, r, sizeof(float2) * (size_t)nt * nfft, cudaMemcpyDeviceToDevice, g->st) ==
                 cudaSuccess;
        ok = ok && cudaStreamSynchronize(g->st) == cudaSuccess;
    }
    cudaFree(tmp_b);
    if (!ok) {
        nr_corr_gpu_destroy(g);
        return NULL;
    }
    return g;
}

extern "C" double nr_corr_gpu_last_ms(const nr_corr_gpu_t *g) { return g ? g->last_ms : 0; }

extern "C" int nr_corr_gpu_run(nr_corr_gpu_t *g, const cf32 *x, int n, float *out)
{
    const int nout = n - g->maxlen + 1;
    if (nout < 1)
        return -1;
    if (ensure(g, n) < 0)
        return -1;
    const int nfft = g->nfft, nt = g->nt, step = nfft - (g->maxlen - 1);
    const int B = (nout + step - 1) / step;

    memcpy(g->h_x, x, sizeof(float2) * (size_t)n);
    cudaEventRecord(g->e0, g->st);
    cudaMemcpyAsync(g->d_x, g->h_x, sizeof(float2) * (size_t)n, cudaMemcpyHostToDevice, g->st);
    cudaMemsetAsync(g->d_tot, 0, sizeof(double), g->st);
    k_total<<<64, TPB, 0, g->st>>>(g->d_x, n, g->d_tot);
    const int nch = (nout + ECHUNK - 1) / ECHUNK;
    k_energy<<<blocks_for((long)nch * nt), TPB, 0, g->st>>>(g->d_x, g->d_tl, g->d_tot, g->d_out, n, nout, nt, g->maxlen);

    /* 1. FFT de los B bloques de x */
    k_pack<<<blocks_for((long)B * nfft), TPB, 0, g->st>>>(g->d_x, g->d_a, n, nfft, step, (long)B * nfft);
    float2 *X = fft_batch(g, g->d_a, g->d_b, B);
    float2 *other = X == g->d_a ? g->d_b : g->d_a;
    /* 2. producto con cada plantilla → B·nt bloques; 3. FFT */
    const long tot = (long)B * nt * nfft;
    k_mul<<<blocks_for(tot), TPB, 0, g->st>>>(X, g->d_tf, other, nfft, nt, tot);
    float2 *Z = fft_batch(g, other, X, (long)B * nt);
    /* 4. módulo² / energía */
    const long to = (long)B * nt * step;
    k_out<<<blocks_for(to), TPB, 0, g->st>>>(Z, g->d_out, nfft, step, nout, nt, 1.0f / (float)nfft, to);
    cudaMemcpyAsync(g->h_out, g->d_out, sizeof(float) * (size_t)nt * nout, cudaMemcpyDeviceToHost, g->st);
    cudaEventRecord(g->e1, g->st);
    if (cudaEventSynchronize(g->e1) != cudaSuccess || cudaGetLastError() != cudaSuccess)
        return -1;
    cudaEventElapsedTime(&g->last_ms, g->e0, g->e1);
    memcpy(out, g->h_out, sizeof(float) * (size_t)nt * nout);
    return nout;
}
