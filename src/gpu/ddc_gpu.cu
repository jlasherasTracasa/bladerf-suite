/*
 * ddc_gpu.cu — DDC por lotes en GPU (ver ddc_gpu.h para la teoría).
 *
 * Flujo por bloque de n muestras Q11:
 *   1. memcpy a un búfer pinned (una sola vez, común a todos los canales) y
 *      H2D asíncrono a d_x[H..H+n); delante va la "historia" (las H últimas
 *      muestras del bloque anterior, copiadas D2D), de modo que cualquier
 *      ventana de ≤ H coeficientes es contigua y los canales no necesitan
 *      estado propio en la GPU.
 *   2. k_mean: media de I y Q del bloque (quita-continua).
 *   3. k_ddc: rejilla (bloques de 8 warps) × (canales). Un warp = una salida.
 *   4. D2H de todas las salidas (compactadas por canal) y callbacks.
 *
 * El índice de flujo g0 (nº de muestras ya consumidas) hace que la fase de
 * salida e^{-jθ·e} sea función del índice ABSOLUTO e: un acumulador de 64 bits
 * (ph = e·inc + ph0 mod 2^64) la mantiene exacta durante horas.
 */
#include "ddc_gpu.h"

#include <cuda_runtime.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CH 64
#define HIST0 (1 << 17) /* historia inicial: cubre hasta 131 072 coeficientes */
#define WARPS 8

typedef unsigned long long u64;

struct cdesc {
    const float2 *taps;
    float2 *out;
    int nt, D, lead, nk;
    long long k0;
    u64 inc, ph0;
};

struct chan {
    bool used, started;
    ddc_plan_t plan;
    int64_t center, freq;
    long lead;
    long long next_k;
    u64 inc, ph0;
    float2 *d_taps;
    float2 *h_taps; /* copia pinned para la subida asíncrona */
};

struct ddc_gpu {
    int max_block;
    int H;                       /* longitud de la historia (muestras) */
    short2 *d_x[2];              /* [H historia | max_block datos] */
    int cur;
    long long g0;                /* muestras consumidas del flujo */
    double fs;                   /* tasa del flujo actual (0 = ninguno) */
    int64_t center;
    short2 *h_in;                /* pinned, max_block */
    double *d_sum;               /* 2 dobles */
    double dc_re, dc_im;         /* continua estimada (Q11) */
    bool dc_on, dc_init;
    cdesc *d_desc, *h_desc;      /* MAX_CH */
    float2 *d_out, *h_out;
    size_t out_cap;
    chan ch[MAX_CH];
    cudaStream_t st;
    cudaEvent_t e0, e1;
    double last_ms, busy_s;
    char name[128];
};

/* ---------------------------------------------------------------- kernels */

/* Suma de I y Q de n muestras (doble precisión: 1M muestras de ±2048 no
 * caben con exactitud en un float de 24 bits). */
__global__ void k_mean(const short2 *__restrict__ x, int n, double *__restrict__ sum)
{
    double sr = 0, si = 0;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += gridDim.x * blockDim.x) {
        short2 s = x[i];
        sr += s.x;
        si += s.y;
    }
    for (int o = 16; o > 0; o >>= 1) {
        sr += __shfl_down_sync(0xffffffffu, sr, o);
        si += __shfl_down_sync(0xffffffffu, si, o);
    }
    if ((threadIdx.x & 31) == 0) {
        atomicAdd(&sum[0], sr);
        atomicAdd(&sum[1], si);
    }
}

__global__ void k_ddc(const short2 *__restrict__ x, int H, long long g0, const cdesc *__restrict__ cd, float dcr,
                      float dci)
{
    const cdesc d = cd[blockIdx.y];
    const int w = threadIdx.x >> 5, lane = threadIdx.x & 31;
    const int k = blockIdx.x * WARPS + w;
    if (k >= d.nk)
        return;
    const long long e = (d.k0 + k + 1) * d.D - 1 + d.lead; /* índice de flujo del fin de ventana */
    const long long top = e - g0 + H;                      /* su posición en d_x */
    float ar = 0.f, ai = 0.f;
    for (int j = lane; j < d.nt; j += 32) {
        long long idx = top - j;
        if (idx < 0)
            break; /* antes del inicio del flujo: ceros */
        short2 s = __ldg(&x[idx]);
        float2 c = __ldg(&d.taps[j]);
        float xr = (float)s.x - dcr, xi = (float)s.y - dci;
        ar += c.x * xr - c.y * xi;
        ai += c.x * xi + c.y * xr;
    }
    for (int o = 16; o > 0; o >>= 1) {
        ar += __shfl_xor_sync(0xffffffffu, ar, o);
        ai += __shfl_xor_sync(0xffffffffu, ai, o);
    }
    if (lane == 0) {
        /* e^{-jθe} = e^{j2π·t}, con t = fase acumulada módulo 2^64 (signo = giro) */
        u64 ph = (u64)e * d.inc + d.ph0;
        float t = (float)(int)(unsigned)(ph >> 32) * (1.0f / 4294967296.0f);
        float s, c;
        sincospif(2.0f * t, &s, &c);
        d.out[k] = make_float2(ar * c - ai * s, ar * s + ai * c);
    }
}

/* ------------------------------------------------------------ utilidades */

#define CK(call)                                                                                   \
    do {                                                                                           \
        cudaError_t e_ = (call);                                                                   \
        if (e_ != cudaSuccess) {                                                                   \
            if (err)                                                                               \
                snprintf(err, errlen, "CUDA: %s", cudaGetErrorString(e_));                         \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

/* Incremento de fase por muestra (en vueltas·2^64) del giro e^{-jθ}: −(f−fc)/fs. */
static u64 phase_inc(int64_t center, int64_t freq, double fs)
{
    double r = -(double)(freq - center) / fs;
    r -= floor(r);
    double v = r * 18446744073709551616.0;
    if (v >= 18446744073709551616.0)
        return 0;
    return (u64)v;
}

/* Coeficientes complejos c[j] = h[j]·e^{+jθj}/2048, con θj reducido en doble. */
static void make_taps(const chan *c, float2 *out)
{
    const double q = (double)(c->freq - c->center) / c->plan.fs_in; /* vueltas por muestra */
    for (int j = 0; j < c->plan.ntaps; j++) {
        double ph = q * (double)j;
        ph -= floor(ph);
        double a = 2.0 * M_PI * ph;
        double v = (double)c->plan.h[j] * (1.0 / 2048.0);
        out[j] = make_float2((float)(v * cos(a)), (float)(v * sin(a)));
    }
}

ddc_gpu_t *ddc_gpu_create(int max_block, char *err, size_t errlen)
{
    ddc_gpu_t *g = (ddc_gpu_t *)calloc(1, sizeof *g);
    if (!g)
        return NULL;
    int ndev = 0;
    cudaDeviceProp prop;
    if (cudaGetDeviceCount(&ndev) != cudaSuccess || ndev < 1) {
        if (err)
            snprintf(err, errlen, "no hay GPU CUDA");
        free(g);
        return NULL;
    }
    CK(cudaSetDevice(0));
    CK(cudaGetDeviceProperties(&prop, 0));
    snprintf(g->name, sizeof g->name, "%s", prop.name);
    g->max_block = max_block;
    g->H = HIST0;
    g->dc_on = true;
    CK(cudaStreamCreateWithFlags(&g->st, cudaStreamNonBlocking));
    CK(cudaEventCreate(&g->e0));
    CK(cudaEventCreate(&g->e1));
    for (int i = 0; i < 2; i++) {
        CK(cudaMalloc(&g->d_x[i], sizeof(short2) * ((size_t)g->H + (size_t)max_block)));
        CK(cudaMemset(g->d_x[i], 0, sizeof(short2) * ((size_t)g->H + (size_t)max_block)));
    }
    CK(cudaMallocHost(&g->h_in, sizeof(short2) * (size_t)max_block));
    CK(cudaMalloc(&g->d_sum, 2 * sizeof(double)));
    CK(cudaMalloc(&g->d_desc, sizeof(cdesc) * MAX_CH));
    CK(cudaMallocHost(&g->h_desc, sizeof(cdesc) * MAX_CH));
    return g;
fail:
    ddc_gpu_destroy(g);
    return NULL;
}
#undef CK

void ddc_gpu_destroy(ddc_gpu_t *g)
{
    if (!g)
        return;
    cudaStreamSynchronize(g->st);
    for (int i = 0; i < MAX_CH; i++)
        if (g->ch[i].used)
            ddc_gpu_remove(g, i);
    for (int i = 0; i < 2; i++)
        cudaFree(g->d_x[i]);
    cudaFreeHost(g->h_in);
    cudaFree(g->d_sum);
    cudaFree(g->d_desc);
    cudaFreeHost(g->h_desc);
    cudaFree(g->d_out);
    cudaFreeHost(g->h_out);
    if (g->e0)
        cudaEventDestroy(g->e0);
    if (g->e1)
        cudaEventDestroy(g->e1);
    if (g->st)
        cudaStreamDestroy(g->st);
    free(g);
}

const char *ddc_gpu_device_name(ddc_gpu_t *g) { return g ? g->name : "ninguna"; }

/* Posición del flujo a la que se refiere un cambio de frecuencia: la próxima
 * muestra aún no consumida. */
static void apply_freq(ddc_gpu_t *g, chan *c, int64_t center, int64_t freq, bool keep_phase)
{
    u64 inc_new = phase_inc(center, freq, c->plan.fs_in);
    if (keep_phase) {
        /* Fase continua en g->g0: φ_vieja(g0) = g0·inc + ph0 = g0·inc' + ph0' */
        u64 e = (u64)g->g0;
        c->ph0 = e * c->inc + c->ph0 - e * inc_new;
    } else {
        c->ph0 = 0;
    }
    c->inc = inc_new;
    c->center = center;
    c->freq = freq;
}

int ddc_gpu_add(ddc_gpu_t *g, const ddc_plan_t *plan, int64_t center_hz, int64_t freq_hz)
{
    int id = -1;
    for (int i = 0; i < MAX_CH; i++)
        if (!g->ch[i].used) {
            id = i;
            break;
        }
    if (id < 0 || !plan || plan->ntaps < 1 || plan->ntaps > DDC_GPU_MAX_TAPS)
        return -1;
    chan *c = &g->ch[id];
    memset(c, 0, sizeof *c);
    c->plan = *plan;
    c->plan.h = (float *)malloc(sizeof(float) * (size_t)plan->ntaps);
    if (!c->plan.h)
        return -1;
    memcpy(c->plan.h, plan->h, sizeof(float) * (size_t)plan->ntaps);
    if (cudaMalloc(&c->d_taps, sizeof(float2) * (size_t)plan->ntaps) != cudaSuccess ||
        cudaMallocHost(&c->h_taps, sizeof(float2) * (size_t)plan->ntaps) != cudaSuccess) {
        cudaFree(c->d_taps);
        free(c->plan.h);
        memset(c, 0, sizeof *c);
        return -1;
    }
    /* Historia suficiente: si el filtro no cabe se reasigna (se pierde la historia). */
    if (plan->ntaps > g->H) {
        cudaStreamSynchronize(g->st);
        int nh = plan->ntaps * 2;
        for (int i = 0; i < 2; i++) {
            cudaFree(g->d_x[i]);
            g->d_x[i] = NULL;
            if (cudaMalloc(&g->d_x[i], sizeof(short2) * ((size_t)nh + (size_t)g->max_block)) != cudaSuccess) {
                cudaFree(c->d_taps);
                cudaFreeHost(c->h_taps);
                free(c->plan.h);
                memset(c, 0, sizeof *c);
                return -1;
            }
            cudaMemset(g->d_x[i], 0, sizeof(short2) * ((size_t)nh + (size_t)g->max_block));
        }
        g->H = nh;
    }
    c->used = true;
    c->center = center_hz;
    c->freq = freq_hz;
    c->inc = phase_inc(center_hz, freq_hz, plan->fs_in);
    c->ph0 = 0;
    make_taps(c, c->h_taps);
    cudaMemcpyAsync(c->d_taps, c->h_taps, sizeof(float2) * (size_t)plan->ntaps, cudaMemcpyHostToDevice, g->st);
    return id;
}

void ddc_gpu_remove(ddc_gpu_t *g, int id)
{
    if (id < 0 || id >= MAX_CH || !g->ch[id].used)
        return;
    chan *c = &g->ch[id];
    cudaStreamSynchronize(g->st);
    cudaFree(c->d_taps);
    cudaFreeHost(c->h_taps);
    free(c->plan.h);
    memset(c, 0, sizeof *c);
}

int ddc_gpu_retune(ddc_gpu_t *g, int id, int64_t center_hz, int64_t freq_hz)
{
    if (id < 0 || id >= MAX_CH || !g->ch[id].used)
        return -1;
    chan *c = &g->ch[id];
    cudaStreamSynchronize(g->st); /* h_taps puede estar en vuelo */
    apply_freq(g, c, center_hz, freq_hz, c->started);
    make_taps(c, c->h_taps);
    cudaMemcpyAsync(c->d_taps, c->h_taps, sizeof(float2) * (size_t)c->plan.ntaps, cudaMemcpyHostToDevice, g->st);
    return 0;
}

int ddc_gpu_ntaps(ddc_gpu_t *g, int id)
{
    return (id >= 0 && id < MAX_CH && g->ch[id].used) ? g->ch[id].plan.ntaps : -1;
}

int ddc_gpu_set_lead(ddc_gpu_t *g, int id, long lead)
{
    if (id < 0 || id >= MAX_CH || !g->ch[id].used || lead > 0)
        return -1;
    g->ch[id].lead = lead;
    return 0;
}

void ddc_gpu_set_dc_removal(ddc_gpu_t *g, bool on) { g->dc_on = on; }
double ddc_gpu_last_ms(ddc_gpu_t *g) { return g->last_ms; }
double ddc_gpu_busy_s(ddc_gpu_t *g) { return g->busy_s; }

/* ------------------------------------------------------------- proceso */
int ddc_gpu_process(ddc_gpu_t *g, const int16_t *iq, int n, double fs, int64_t center_hz, ddc_gpu_out_fn fn,
                    void *ctx)
{
    if (n <= 0 || n > g->max_block)
        return -1;
    if (fs != g->fs) {
        /* flujo nuevo: historia a cero y canales por empezar */
        g->fs = fs;
        g->g0 = 0;
        g->dc_init = false;
        for (int i = 0; i < MAX_CH; i++)
            g->ch[i].started = false;
        cudaMemsetAsync(g->d_x[g->cur], 0, sizeof(short2) * (size_t)g->H, g->st);
    }
    g->center = center_hz;

    /* 1. Subida del bloque (una sola copia para todos los canales) */
    short2 *x = g->d_x[g->cur];
    memcpy(g->h_in, iq, sizeof(short2) * (size_t)n);
    cudaEventRecord(g->e0, g->st);
    cudaMemcpyAsync(x + g->H, g->h_in, sizeof(short2) * (size_t)n, cudaMemcpyHostToDevice, g->st);

    /* 2. Continua del bloque */
    float dcr = 0.f, dci = 0.f;
    if (g->dc_on) {
        double s[2];
        cudaMemsetAsync(g->d_sum, 0, 2 * sizeof(double), g->st);
        int nb = MIN(64, (n + 255) / 256);
        k_mean<<<nb, 256, 0, g->st>>>(x + g->H, n, g->d_sum);
        cudaMemcpyAsync(s, g->d_sum, sizeof s, cudaMemcpyDeviceToHost, g->st);
        cudaStreamSynchronize(g->st);
        double mr = s[0] / n, mi = s[1] / n;
        /* Paso alto de 1 polo del motor (k = 2e-5 por muestra) aplicado por bloque */
        double a = g->dc_init ? 1.0 - pow(1.0 - 2e-5, (double)n) : 1.0;
        g->dc_re += a * (mr - g->dc_re);
        g->dc_im += a * (mi - g->dc_im);
        g->dc_init = true;
        dcr = (float)g->dc_re;
        dci = (float)g->dc_im;
    }

    /* 3. Descriptores de canal y tamaño de la salida */
    cdesc *hd = g->h_desc;
    int nch = 0, idmap[MAX_CH];
    size_t total = 0;
    int maxnk = 0;
    for (int i = 0; i < MAX_CH; i++) {
        chan *c = &g->ch[i];
        if (!c->used || c->plan.fs_in != fs || c->center != center_hz)
            continue;
        const int D = c->plan.decim;
        const long long lead = c->lead;
        if (!c->started) {
            /* Primera salida cuya ventana termina en este bloque o después */
            c->next_k = g->g0 == 0 ? 0 : (g->g0 - lead + 1 + D - 1) / D - 1;
            c->started = true;
        }
        long long last = (g->g0 + n - lead) / D; /* nº de salidas con e ≤ g0+n−1 */
        long long nk = last - c->next_k;
        if (nk <= 0)
            continue;
        hd[nch].taps = c->d_taps;
        hd[nch].out = NULL;
        hd[nch].nt = c->plan.ntaps;
        hd[nch].D = D;
        hd[nch].lead = (int)lead;
        hd[nch].nk = (int)nk;
        hd[nch].k0 = c->next_k;
        hd[nch].inc = c->inc;
        hd[nch].ph0 = c->ph0;
        idmap[nch] = i;
        total += (size_t)nk;
        maxnk = MAX(maxnk, (int)nk);
        nch++;
    }
    int rc = 0;
    if (nch > 0) {
        if (total > g->out_cap) {
            cudaStreamSynchronize(g->st);
            cudaFree(g->d_out);
            cudaFreeHost(g->h_out);
            g->d_out = NULL;
            g->h_out = NULL;
            size_t cap = total * 2 + 1024;
            if (cudaMalloc(&g->d_out, sizeof(float2) * cap) != cudaSuccess ||
                cudaMallocHost(&g->h_out, sizeof(float2) * cap) != cudaSuccess) {
                cudaFree(g->d_out);
                g->d_out = NULL;
                g->out_cap = 0;
                return -1;
            }
            g->out_cap = cap;
        }
        size_t off = 0;
        for (int i = 0; i < nch; i++) {
            hd[i].out = g->d_out + off;
            off += (size_t)hd[i].nk;
        }
        cudaMemcpyAsync(g->d_desc, hd, sizeof(cdesc) * (size_t)nch, cudaMemcpyHostToDevice, g->st);
        dim3 grid((unsigned)((maxnk + WARPS - 1) / WARPS), (unsigned)nch);
        k_ddc<<<grid, WARPS * 32, 0, g->st>>>(x, g->H, g->g0, g->d_desc, dcr, dci);
        cudaMemcpyAsync(g->h_out, g->d_out, sizeof(float2) * total, cudaMemcpyDeviceToHost, g->st);
    }
    /* 4. Historia para el próximo bloque: las H últimas muestras de [H|n] */
    short2 *nx = g->d_x[g->cur ^ 1];
    cudaMemcpyAsync(nx, x + n, sizeof(short2) * (size_t)g->H, cudaMemcpyDeviceToDevice, g->st);
    cudaEventRecord(g->e1, g->st);
    cudaEventSynchronize(g->e1);
    float ms = 0.f;
    cudaEventElapsedTime(&ms, g->e0, g->e1);
    g->last_ms = ms;
    g->busy_s += ms * 1e-3;
    if (cudaGetLastError() != cudaSuccess)
        rc = -1;

    if (nch > 0 && rc == 0) {
        size_t off = 0;
        for (int i = 0; i < nch; i++) {
            chan *c = &g->ch[idmap[i]];
            long long e0 = (hd[i].k0 + 1) * hd[i].D - 1 + hd[i].lead;
            double t = (double)(e0 - (c->plan.ntaps - 1) / 2 - g->g0) / fs;
            if (fn)
                fn(ctx, idmap[i], (const cf32 *)(g->h_out + off), hd[i].nk, t);
            c->next_k += hd[i].nk;
            off += (size_t)hd[i].nk;
        }
    }
    g->cur ^= 1;
    g->g0 += n;
    return rc == 0 ? (int)total : -1;
}
