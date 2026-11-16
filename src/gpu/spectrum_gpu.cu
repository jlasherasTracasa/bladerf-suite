/*
 * spectrum_gpu.cu — Espectro promediado en GPU con una FFT Stockham propia.
 *
 * No se usa cuFFT: la FFT está escrita aquí desde cero.
 *
 * Por qué Stockham y no Cooley-Tukey "en el sitio": la variante de Stockham
 * ("autosort") va alternando entre dos búferes (ping-pong) y en cada etapa
 * escribe los resultados ya en su posición final, sin la permutación por
 * inversión de bits. En una GPU eso significa accesos a memoria regulares y
 * un kernel idéntico para todas las etapas. Formulación radix-2 basada en:
 *   N. K. Govindaraju et al., "High Performance Discrete Fourier Transforms
 *   on Graphics Processors", SC'08.
 *
 * Etapa con subtransformadas de tamaño Ns (1, 2, 4, …, N/2), para cada
 * j ∈ [0, N/2):
 *     a = in[j],  b = in[j + N/2] · W,   W = e^{−j2π(j mod Ns)/(2Ns)}
 *     d = (j / Ns)·2Ns + (j mod Ns)
 *     out[d] = a + b,   out[d + Ns] = a − b
 * Tras log2(N) etapas, out contiene X[k] en orden natural.
 *
 * Flujo por lote de K FFTs (todas en paralelo):
 *   1. memcpy asíncrono de las muestras int16 (memoria pinned → GPU)
 *   2. k_load:   int16 → float, × ventana / 2048
 *   3. k_stage:  log2(N) lanzamientos del kernel de mariposas
 *   4. k_power:  Σ_k |X_k[bin]|² sobre las K FFTs (un hilo por bin)
 *   5. memcpy de N floats de vuelta a la CPU
 *
 * Asíncrono con dos "ranuras" (C2): cada ranura tiene su búfer pinned de entrada,
 * su copia en la GPU y su resultado. Mientras la GPU procesa la ranura A, el hilo
 * DSP ya está rellenando la B (gpu_spec_host_buffer / gpu_spec_submit) y recoge los
 * resultados con gpu_spec_poll (cudaEventQuery: nunca espera). Con una sola ranura el
 * hilo DSP se quedaba esperando (≈ 7 ms por trama a 122,88 Msps) y además la GPU
 * estaba parada mientras la CPU copiaba. Los búferes de trabajo de la FFT (d_a, d_b)
 * se comparten: un único flujo ejecuta los lotes en orden.
 */
#include "spectrum_gpu.h"

#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSLOT 2

typedef struct {
    int16_t *h_iq;      /* pinned, max_batch·n·2 */
    int16_t *d_iq;
    float *d_pow;
    float *h_pow;       /* pinned, n */
    cudaEvent_t done;
    int batch;          /* FFTs del lote en vuelo */
} slot_t;

struct gpu_spec {
    int n, log2n, max_batch;
    slot_t slot[NSLOT];
    int fill, drain, inflight;  /* ranura que se rellena / la más antigua en vuelo / nº en vuelo */
    float2 *d_a, *d_b;  /* ping-pong, max_batch·n */
    float2 *d_tw;       /* twiddles e^{−j2πk/N}, k < N/2 */
    float *d_win;
    cudaStream_t stream;
    char name[256];
};

/* ---------------------------------------------------------------- kernels */

__global__ void k_load(const short2 *__restrict__ iq, const float *__restrict__ win, float2 *__restrict__ out,
                       int n, int total)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= total)
        return;
    short2 s = iq[i];
    float w = win[i & (n - 1)] * (1.0f / 2048.0f);
    out[i] = make_float2(s.x * w, s.y * w);
}

/* Una etapa radix-2 de Stockham para 'batch' FFTs a la vez.
 * Hilo t → FFT b = t / (n/2), mariposa j = t mod (n/2). */
__global__ void k_stage(const float2 *__restrict__ in, float2 *__restrict__ out, const float2 *__restrict__ tw,
                        int n, int ns, int tw_stride, int total)
{
    int t = blockIdx.x * blockDim.x + threadIdx.x;
    if (t >= total)
        return;
    const int half = n >> 1;
    const int b = t / half;
    const int j = t - b * half;
    const float2 *x = in + (size_t)b * n;
    float2 *y = out + (size_t)b * n;

    const int k = j & (ns - 1);          /* j mod Ns (Ns es potencia de 2) */
    const float2 w = tw[k * tw_stride];  /* e^{−j2πk/(2Ns)} = W_N^{k·N/(2Ns)} */
    float2 a = x[j];
    float2 c = x[j + half];
    float2 bw = make_float2(c.x * w.x - c.y * w.y, c.x * w.y + c.y * w.x);
    const int d = (j - k) * 2 + k;       /* (j/Ns)·2Ns + k */
    y[d] = make_float2(a.x + bw.x, a.y + bw.y);
    y[d + ns] = make_float2(a.x - bw.x, a.y - bw.y);
}

/* Suma de potencias de todas las FFTs del lote, bin a bin. */
__global__ void k_power(const float2 *__restrict__ X, float *__restrict__ pw, int n, int batch)
{
    int bin = blockIdx.x * blockDim.x + threadIdx.x;
    if (bin >= n)
        return;
    float acc = 0.f;
    for (int b = 0; b < batch; b++) {
        float2 v = X[(size_t)b * n + bin];
        acc += v.x * v.x + v.y * v.y;
    }
    pw[bin] = acc;
}

/* ------------------------------------------------------------- API host */

#define CK(call)                                                                         \
    do {                                                                                 \
        cudaError_t e_ = (call);                                                         \
        if (e_ != cudaSuccess) {                                                         \
            if (err)                                                                     \
                snprintf(err, errlen, "%s: %s", #call, cudaGetErrorString(e_));          \
            gpu_spec_destroy(g);                                                         \
            return NULL;                                                                 \
        }                                                                                \
    } while (0)

extern "C" gpu_spec_t *gpu_spec_create(int n, int max_batch, const float *window, char *err, size_t errlen)
{
    int ndev = 0;
    if (cudaGetDeviceCount(&ndev) != cudaSuccess || ndev == 0) {
        if (err)
            snprintf(err, errlen, "no se encontró ninguna GPU CUDA");
        return NULL;
    }
    gpu_spec_t *g = (gpu_spec_t *)calloc(1, sizeof *g);
    g->n = n;
    g->max_batch = max_batch;
    while ((1 << g->log2n) < n)
        g->log2n++;

    cudaDeviceProp prop;
    CK(cudaGetDeviceProperties(&prop, 0));
    snprintf(g->name, sizeof g->name, "%s", prop.name);

    const size_t nsamp = (size_t)max_batch * n;
    CK(cudaStreamCreate(&g->stream));
    for (int i = 0; i < NSLOT; i++) {
        slot_t *sl = &g->slot[i];
        CK(cudaMallocHost((void **)&sl->h_iq, nsamp * 2 * sizeof(int16_t)));
        CK(cudaMalloc((void **)&sl->d_iq, nsamp * 2 * sizeof(int16_t)));
        CK(cudaMalloc((void **)&sl->d_pow, (size_t)n * sizeof(float)));
        CK(cudaMallocHost((void **)&sl->h_pow, (size_t)n * sizeof(float)));
        /* sin sincronización por espera activa: solo se consulta (poll) o se duerme (wait) */
        CK(cudaEventCreateWithFlags(&sl->done, cudaEventDisableTiming | cudaEventBlockingSync));
    }
    CK(cudaMalloc((void **)&g->d_a, nsamp * sizeof(float2)));
    CK(cudaMalloc((void **)&g->d_b, nsamp * sizeof(float2)));
    CK(cudaMalloc((void **)&g->d_tw, (size_t)(n / 2) * sizeof(float2)));
    CK(cudaMalloc((void **)&g->d_win, (size_t)n * sizeof(float)));

    /* Twiddles en doble precisión en la CPU y copiados una vez. */
    float2 *tw = (float2 *)malloc((size_t)(n / 2) * sizeof(float2));
    for (int k = 0; k < n / 2; k++) {
        double a = -2.0 * 3.14159265358979323846 * k / n;
        tw[k] = make_float2((float)cos(a), (float)sin(a));
    }
    CK(cudaMemcpy(g->d_tw, tw, (size_t)(n / 2) * sizeof(float2), cudaMemcpyHostToDevice));
    free(tw);
    CK(cudaMemcpy(g->d_win, window, (size_t)n * sizeof(float), cudaMemcpyHostToDevice));
    return g;
}

extern "C" int16_t *gpu_spec_host_buffer(gpu_spec_t *g)
{
    /* Las dos ranuras en vuelo: no hay dónde copiar (el llamador descarta la trama). */
    return g->inflight >= NSLOT ? NULL : g->slot[g->fill].h_iq;
}
extern "C" const char *gpu_spec_device_name(gpu_spec_t *g) { return g->name; }

/* Lanza el lote 'batch' (ya copiado en gpu_spec_host_buffer) y vuelve de inmediato. */
extern "C" int gpu_spec_submit(gpu_spec_t *g, int batch)
{
    if (batch < 1 || batch > g->max_batch || g->inflight >= NSLOT)
        return -1;
    slot_t *sl = &g->slot[g->fill];
    const int n = g->n;
    const int total = batch * n;
    const int TPB = 256;

    cudaMemcpyAsync(sl->d_iq, sl->h_iq, (size_t)total * 2 * sizeof(int16_t), cudaMemcpyHostToDevice, g->stream);
    k_load<<<(total + TPB - 1) / TPB, TPB, 0, g->stream>>>((const short2 *)sl->d_iq, g->d_win, g->d_a, n, total);

    float2 *in = g->d_a, *out = g->d_b;
    const int bfly = batch * (n / 2);
    for (int ns = 1; ns < n; ns <<= 1) {
        /* W para k/(2Ns) en la tabla de N puntos: índice k·N/(2Ns) */
        int stride = n / (2 * ns);
        k_stage<<<(bfly + TPB - 1) / TPB, TPB, 0, g->stream>>>(in, out, g->d_tw, n, ns, stride, bfly);
        float2 *t = in;
        in = out;
        out = t;
    }
    k_power<<<(n + TPB - 1) / TPB, TPB, 0, g->stream>>>(in, sl->d_pow, n, batch);
    cudaMemcpyAsync(sl->h_pow, sl->d_pow, (size_t)n * sizeof(float), cudaMemcpyDeviceToHost, g->stream);
    if (cudaEventRecord(sl->done, g->stream) != cudaSuccess)
        return -1;
    sl->batch = batch;
    g->fill = (g->fill + 1) % NSLOT;
    g->inflight++;
    return 0;
}

/* Recoge el resultado más antiguo si ya terminó (wait = true: duerme hasta que termine).
 * Devuelve el nº de FFTs promediadas (> 0), 0 si aún no hay nada, −1 si falló. */
static int collect(gpu_spec_t *g, float *power, bool wait)
{
    if (g->inflight == 0)
        return 0;
    slot_t *sl = &g->slot[g->drain];
    cudaError_t e = wait ? cudaEventSynchronize(sl->done) : cudaEventQuery(sl->done);
    if (e == cudaErrorNotReady)
        return 0;
    g->drain = (g->drain + 1) % NSLOT;
    g->inflight--;
    if (e != cudaSuccess)
        return -1;
    memcpy(power, sl->h_pow, (size_t)g->n * sizeof(float));
    return sl->batch;
}
extern "C" int gpu_spec_poll(gpu_spec_t *g, float *power) { return collect(g, power, false); }

/* Versión síncrona (compatibilidad y pruebas): submit + espera. */
extern "C" int gpu_spec_run(gpu_spec_t *g, int batch, float *power)
{
    while (g->inflight > 0) /* vacía lo pendiente: aquí no se espera a nadie más */
        if (collect(g, power, true) < 0)
            return -1;
    if (gpu_spec_submit(g, batch) < 0)
        return -1;
    return collect(g, power, true) > 0 ? 0 : -1;
}

extern "C" void gpu_spec_destroy(gpu_spec_t *g)
{
    if (!g)
        return;
    if (g->stream)
        cudaStreamSynchronize(g->stream);
    for (int i = 0; i < NSLOT; i++) {
        slot_t *sl = &g->slot[i];
        if (sl->h_iq) cudaFreeHost(sl->h_iq);
        if (sl->d_iq) cudaFree(sl->d_iq);
        if (sl->d_pow) cudaFree(sl->d_pow);
        if (sl->h_pow) cudaFreeHost(sl->h_pow);
        if (sl->done) cudaEventDestroy(sl->done);
    }
    if (g->d_a) cudaFree(g->d_a);
    if (g->d_b) cudaFree(g->d_b);
    if (g->d_tw) cudaFree(g->d_tw);
    if (g->d_win) cudaFree(g->d_win);
    if (g->stream) cudaStreamDestroy(g->stream);
    free(g);
}
