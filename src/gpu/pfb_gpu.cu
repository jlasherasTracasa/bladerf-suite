/*
 * pfb_gpu.cu — Canalizador polifásico WOLA en GPU. Teoría en pfb_gpu.h.
 *
 * Un solo núcleo por bloque de tramas: cada bloque CUDA procesa UNA trama
 *   1. pliegue:  z[m] = Σ_p h[m+pM]·x[e−m−pM]  (lecturas coalescidas: hilos
 *                consecutivos leen muestras consecutivas) → memoria compartida
 *   2. FFT Stockham radix-2 de M puntos ENTERA en memoria compartida (ping-pong
 *      entre dos matrices, una barrera por etapa; mismo esquema que
 *      spectrum_gpu.cu pero sin pasar por memoria global entre etapas)
 *   3. reordena (k → −k), aplica e^{−j2πk(e mod M)/M} y escribe la trama.
 * Merry hace lo mismo con la FIR y la FFT en núcleos separados para grandes M;
 * para M ≤ 4096 la trama cabe en compartida y un núcleo basta.
 */
#include "pfb_gpu.h"

#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pfb_gpu {
    int M, ov, P, D, L, max_samples, max_frames, keep;
    int log2m, threads;
    float *h;                 /* L, host */
    float *d_h;
    float2 *d_tw;             /* e^{−j2πk/M}, k < M/2 */
    short2 *d_x[2];           /* [keep historia | max_samples] */
    int cur;
    short2 *h_in;             /* pinned, max_samples */
    float2 *d_out;
    long long S;              /* muestras consumidas */
    long long Fe;             /* tramas emitidas */
    cudaStream_t st;
    cudaEvent_t e0, e1;
    double last_ms;
    char name[256];
};

__global__ void k_pfb(const short2 *__restrict__ x, const float *__restrict__ h, const float2 *__restrict__ tw_g,
                      float2 *__restrict__ out, int M, int log2m, int P, int D, long long e_rel0, long long e_abs0)
{
    extern __shared__ float2 sm[];
    float2 *a = sm, *b = sm + M, *tw = sm + 2 * M;
    const int f = blockIdx.x;
    const long long e_rel = e_rel0 + (long long)f * D;     /* fin de ventana en d_x */
    const long long e_abs = e_abs0 + (long long)f * D;     /* ... en el flujo */

    for (int k = threadIdx.x; k < M / 2; k += blockDim.x)
        tw[k] = tw_g[k];
    /* 1. pliegue */
    for (int m = threadIdx.x; m < M; m += blockDim.x) {
        float re = 0.f, im = 0.f;
        for (int p = 0; p < P; p++) {
            short2 s = __ldg(&x[e_rel - m - (long long)p * M]);
            float c = __ldg(&h[m + p * M]);
            re += c * s.x;
            im += c * s.y;
        }
        a[m] = make_float2(re * (1.0f / 2048.0f), im * (1.0f / 2048.0f));
    }
    __syncthreads();
    /* 2. FFT Stockham en compartida */
    const int half = M >> 1;
    for (int s = 0; s < log2m; s++) {
        const int ns = 1 << s;
        const int stride = half >> s; /* M/(2·Ns) */
        for (int j = threadIdx.x; j < half; j += blockDim.x) {
            const int k = j & (ns - 1);
            const float2 w = tw[k * stride];
            const float2 u = a[j], c = a[j + half];
            const float2 v = make_float2(c.x * w.x - c.y * w.y, c.x * w.y + c.y * w.x);
            const int d = ((j - k) << 1) + k;
            b[d] = make_float2(u.x + v.x, u.y + v.y);
            b[d + ns] = make_float2(u.x - v.x, u.y - v.y);
        }
        __syncthreads();
        float2 *t = a;
        a = b;
        b = t;
    }
    /* 3. canal k ← X[(−k) mod M] · e^{−j2π·k·(e mod M)/M} */
    const int em = (int)(e_abs & (M - 1));
    float2 *o = out + (size_t)f * M;
    for (int k = threadIdx.x; k < M; k += blockDim.x) {
        float2 v = a[(M - k) & (M - 1)];
        int r = (int)(((long long)k * em) & (M - 1)); /* k·em mod M */
        float2 w = r < half ? tw[r] : make_float2(-tw[r - half].x, -tw[r - half].y);
        o[k] = make_float2(v.x * w.x - v.y * w.y, v.x * w.y + v.y * w.x);
    }
}

#define CK(call)                                                                                   \
    do {                                                                                           \
        cudaError_t e_ = (call);                                                                   \
        if (e_ != cudaSuccess) {                                                                   \
            if (err)                                                                               \
                snprintf(err, errlen, "CUDA: %s", cudaGetErrorString(e_));                         \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

pfb_gpu_t *pfb_gpu_create(int M, int ov, int P, int max_samples, char *err, size_t errlen)
{
    if (M < 8 || M > 4096 || (M & (M - 1)) || (ov != 1 && ov != 2 && ov != 4) || P < 4 || max_samples < M) {
        if (err)
            snprintf(err, errlen, "parámetros de PFB no válidos");
        return NULL;
    }
    pfb_gpu_t *g = (pfb_gpu_t *)calloc(1, sizeof *g);
    if (!g)
        return NULL;
    int ndev = 0;
    cudaDeviceProp prop;
    float2 *tw = NULL;
    if (cudaGetDeviceCount(&ndev) != cudaSuccess || ndev < 1) {
        if (err)
            snprintf(err, errlen, "no hay GPU CUDA");
        free(g);
        return NULL;
    }
    CK(cudaSetDevice(0));
    CK(cudaGetDeviceProperties(&prop, 0));
    snprintf(g->name, sizeof g->name, "%s", prop.name);
    g->M = M;
    g->ov = ov;
    g->P = P;
    g->D = M / ov;
    g->L = P * M;
    g->keep = g->L;
    g->max_samples = max_samples;
    g->max_frames = max_samples / g->D + 2;
    g->log2m = 0;
    while ((1 << g->log2m) < M)
        g->log2m++;
    g->threads = MIN(M / 2, 512);
    g->h = (float *)malloc(sizeof(float) * (size_t)g->L);
    tw = (float2 *)malloc(sizeof(float2) * (size_t)(M / 2));
    if (!g->h || !tw) {
        free(tw);
        pfb_gpu_destroy(g);
        return NULL;
    }
    pfb_design_proto(g->h, M, P);
    for (int k = 0; k < M / 2; k++) {
        tw[k].x = (float)cos(2.0 * M_PI * k / M);
        tw[k].y = (float)-sin(2.0 * M_PI * k / M);
    }
    {
        size_t smem = sizeof(float2) * ((size_t)2 * M + M / 2);
        CK(cudaFuncSetAttribute(k_pfb, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
    }
    CK(cudaStreamCreateWithFlags(&g->st, cudaStreamNonBlocking));
    CK(cudaEventCreate(&g->e0));
    CK(cudaEventCreate(&g->e1));
    CK(cudaMalloc(&g->d_h, sizeof(float) * (size_t)g->L));
    CK(cudaMemcpy(g->d_h, g->h, sizeof(float) * (size_t)g->L, cudaMemcpyHostToDevice));
    CK(cudaMalloc(&g->d_tw, sizeof(float2) * (size_t)(M / 2)));
    CK(cudaMemcpy(g->d_tw, tw, sizeof(float2) * (size_t)(M / 2), cudaMemcpyHostToDevice));
    for (int i = 0; i < 2; i++) {
        size_t sz = sizeof(short2) * ((size_t)g->keep + (size_t)max_samples);
        CK(cudaMalloc(&g->d_x[i], sz));
        CK(cudaMemset(g->d_x[i], 0, sz));
    }
    CK(cudaMallocHost(&g->h_in, sizeof(short2) * (size_t)max_samples));
    CK(cudaMalloc(&g->d_out, sizeof(float2) * (size_t)g->max_frames * (size_t)M));
    free(tw);
    return g;
fail:
    free(tw);
    pfb_gpu_destroy(g);
    return NULL;
}
#undef CK

void pfb_gpu_destroy(pfb_gpu_t *g)
{
    if (!g)
        return;
    if (g->st)
        cudaStreamSynchronize(g->st);
    for (int i = 0; i < 2; i++)
        cudaFree(g->d_x[i]);
    cudaFree(g->d_h);
    cudaFree(g->d_tw);
    cudaFree(g->d_out);
    cudaFreeHost(g->h_in);
    if (g->e0)
        cudaEventDestroy(g->e0);
    if (g->e1)
        cudaEventDestroy(g->e1);
    if (g->st)
        cudaStreamDestroy(g->st);
    free(g->h);
    free(g);
}

const char *pfb_gpu_device_name(pfb_gpu_t *g) { return g ? g->name : "ninguna"; }
int pfb_gpu_channels(const pfb_gpu_t *g) { return g->M; }
int pfb_gpu_decim(const pfb_gpu_t *g) { return g->D; }
int pfb_gpu_ntaps(const pfb_gpu_t *g) { return g->L; }
const float *pfb_gpu_proto(const pfb_gpu_t *g) { return g->h; }
int pfb_gpu_max_frames(const pfb_gpu_t *g) { return g->max_frames; }
double pfb_gpu_last_ms(const pfb_gpu_t *g) { return g->last_ms; }

int pfb_gpu_process(pfb_gpu_t *g, const int16_t *iq, int n, cf32 *out)
{
    if (n < 0 || n > g->max_samples)
        return -1;
    const int D = g->D, M = g->M;
    const long long S_new = g->S + n;
    const long long F = S_new / D - g->Fe;
    short2 *x = g->d_x[g->cur];
    memcpy(g->h_in, iq, sizeof(short2) * (size_t)n);
    cudaEventRecord(g->e0, g->st);
    cudaMemcpyAsync(x + g->keep, g->h_in, sizeof(short2) * (size_t)n, cudaMemcpyHostToDevice, g->st);
    if (F > 0) {
        /* Primera trama pendiente: e = (Fe+1)·D − 1 (índice de flujo); en d_x el
         * flujo s está en la posición s − S + keep. */
        const long long e_abs0 = (g->Fe + 1) * D - 1;
        const long long e_rel0 = e_abs0 - g->S + g->keep;
        size_t smem = sizeof(float2) * ((size_t)2 * M + M / 2);
        k_pfb<<<(unsigned)F, g->threads, smem, g->st>>>(x, g->d_h, g->d_tw, g->d_out, M, g->log2m, g->P, D, e_rel0,
                                                     e_abs0);
        if (out)
            cudaMemcpyAsync(out, g->d_out, sizeof(float2) * (size_t)F * (size_t)M, cudaMemcpyDeviceToHost, g->st);
    }
    /* historia: las 'keep' últimas muestras de [keep | n] */
    cudaMemcpyAsync(g->d_x[g->cur ^ 1], x + n, sizeof(short2) * (size_t)g->keep, cudaMemcpyDeviceToDevice, g->st);
    cudaEventRecord(g->e1, g->st);
    cudaEventSynchronize(g->e1);
    float ms = 0.f;
    cudaEventElapsedTime(&ms, g->e0, g->e1);
    g->last_ms = ms;
    g->cur ^= 1;
    g->S = S_new;
    g->Fe += F;
    return cudaGetLastError() == cudaSuccess ? (int)F : -1;
}
