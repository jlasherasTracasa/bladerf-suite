/*
 * spectrum.c — Máquina de estados de las tramas de espectro. Ver spectrum.h.
 *
 * Cada trama abarca L = fs/fps muestras. De ellas se toman las primeras K·N
 * (N = tamaño FFT) para promediar y el resto se descarta. K se elige así:
 *   - GPU: todas las que quepan (K = L/N, hasta GPU_MAX_BATCH y GPU_MAX_SAMPLES).
 *   - CPU: como mucho las necesarias para procesar ~4 M muestras/s, que en
 *          un núcleo moderno es una fracción pequeña de su capacidad.
 */
#include "spectrum.h"

#include "../gpu/spectrum_gpu.h"
#include "fft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CPU_BUDGET_SPS 4.0e6   /* muestras/s que la CPU dedica a FFT */
#define GPU_MAX_BATCH 4096     /* FFTs por lote en la GPU */
/* Tope de muestras por lote: acota la memoria de la GPU (≈ 40 B por muestra: dos ranuras
 * pinned/device de entrada y dos búferes float2 de trabajo). 8 Mi muestras = 6,1 tramas a
 * 20 fps y 122,88 Msps: no recorta nada en el uso normal; con fps ≤ 14 a esa tasa se
 * promedia solo una parte de la trama (más que de sobra para alisar el ruido). */
#define GPU_MAX_SAMPLES (1L << 23)

struct spectrum {
    int n;               /* tamaño de la FFT */
    double rate;
    long frame_len;      /* L: muestras por trama */
    long frame_pos;      /* muestras consumidas de la trama actual */
    int k_target;        /* FFTs a promediar por trama */
    int k_done;
    float *window;
    double norm_db;      /* corrección para que fondo de escala = 0 dBFS */
    float *acc;          /* potencia acumulada por bin (sin fftshift) */
    float *db;           /* salida en dBFS (con fftshift) */
    /* CPU */
    fft_plan_t *plan;
    cf32 *work;
    int work_fill;
    /* GPU */
    gpu_spec_t *gpu;
    int16_t *gpu_host;   /* búfer "pinned" donde se juntan las muestras del lote */
    long gpu_fill;       /* muestras complejas copiadas al lote */
    bool gpu_drop;       /* trama en curso sin ranura libre en la GPU: se descarta */
    char backend[160];
};

spectrum_t *spectrum_create(int n, double rate, int fps, bool try_gpu)
{
    if (!fft_size_valid(n) || rate <= 0 || fps <= 0)
        return NULL;
    spectrum_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->n = n;
    s->rate = rate;
    s->frame_len = (long)(rate / fps);
    if (s->frame_len < n)
        s->frame_len = n;
    s->window = malloc(sizeof(float) * (size_t)n);
    s->acc = calloc((size_t)n, sizeof(float));
    s->db = malloc(sizeof(float) * (size_t)n);
    window_blackman_harris(s->window, n);
    double wsum = 0;
    for (int i = 0; i < n; i++)
        wsum += s->window[i];
    /* |X| de un tono complejo de amplitud 1 = Σw (las muestras ya llegan normalizadas: en la CPU se
     * multiplican por w/2048 al rellenar la FFT y en la GPU k_load hace lo mismo). Por tanto solo
     * falta dividir por Σw; el 2048 NO se vuelve a aplicar aquí (estaba duplicado: −66,2 dB). */
    s->norm_db = -20.0 * log10(wsum);

    long kmax_frame = s->frame_len / n;
    if (try_gpu) {
        int batch = (int)CLAMP(kmax_frame, 1, MIN((long)GPU_MAX_BATCH, MAX(1L, GPU_MAX_SAMPLES / n)));
        char err[128] = "";
        s->gpu = gpu_spec_create(n, batch, s->window, err, sizeof err);
        if (s->gpu) {
            s->k_target = batch;
            snprintf(s->backend, sizeof s->backend, "GPU (%s)", gpu_spec_device_name(s->gpu));
        } else if (err[0]) {
            LOG_W("Espectro: GPU no disponible (%s); se usa la CPU", err);
        }
    }
    if (!s->gpu) {
        s->plan = fft_plan_create(n);
        s->work = malloc(sizeof(cf32) * (size_t)n);
        double per_frame_budget = CPU_BUDGET_SPS / fps;
        s->k_target = (int)CLAMP(MIN((double)kmax_frame, per_frame_budget / n), 1, 1 << 20);
        snprintf(s->backend, sizeof s->backend, "CPU");
    }
    return s;
}

void spectrum_destroy(spectrum_t *s)
{
    if (!s)
        return;
    if (s->gpu)
        gpu_spec_destroy(s->gpu);
    fft_plan_destroy(s->plan);
    free(s->work);
    free(s->window);
    free(s->acc);
    free(s->db);
    free(s);
}

bool spectrum_using_gpu(const spectrum_t *s) { return s->gpu != NULL; }
const char *spectrum_backend(const spectrum_t *s) { return s->backend; }

/* Convierte la potencia acumulada a dBFS, reordena (fftshift) y entrega. */
static void emit(spectrum_t *s, spec_frame_fn cb, void *ctx)
{
    const int n = s->n, half = n / 2;
    const double k = s->k_done > 0 ? s->k_done : 1;
    for (int i = 0; i < n; i++) {
        float p = (float)(s->acc[(i + half) & (n - 1)] / k);
        s->db[i] = (float)(power_to_db(p) + s->norm_db);
    }
    cb(ctx, s->db, n, s->k_done);
    memset(s->acc, 0, sizeof(float) * (size_t)n);
    s->k_done = 0;
}

static void cpu_fft_block(spectrum_t *s)
{
    fft_exec(s->plan, s->work);
    for (int i = 0; i < s->n; i++)
        s->acc[i] += s->work[i].re * s->work[i].re + s->work[i].im * s->work[i].im;
    s->k_done++;
}

/* Recoge de la GPU los lotes terminados y emite sus tramas (nunca espera). */
static void gpu_poll(spectrum_t *s, spec_frame_fn cb, void *ctx)
{
    int k;
    while ((k = gpu_spec_poll(s->gpu, s->acc)) > 0) {
        s->k_done = k;
        emit(s, cb, ctx);
    }
}

void spectrum_feed(spectrum_t *s, const int16_t *iq, int count, spec_frame_fn cb, void *ctx)
{
    const long need = (long)s->k_target * s->n; /* muestras útiles por trama */
    int i = 0;
    if (s->gpu)
        gpu_poll(s, cb, ctx);
    while (i < count) {
        if (s->frame_pos < need) {
            /* Fase de recogida */
            if (s->gpu && s->frame_pos == 0 && s->gpu_fill == 0) {
                /* Principio de trama: ¿hay ranura libre? Si las dos están ocupadas (la GPU va
                 * por detrás) se descarta esta trama entera en vez de bloquear el hilo DSP. */
                s->gpu_host = gpu_spec_host_buffer(s->gpu);
                s->gpu_drop = s->gpu_host == NULL;
            }
            int take = (int)MIN((long)(count - i), need - s->frame_pos);
            if (s->gpu) {
                if (!s->gpu_drop) {
                    memcpy(s->gpu_host + 2 * s->gpu_fill, iq + 2 * i, sizeof(int16_t) * 2 * (size_t)take);
                    s->gpu_fill += take;
                }
            } else {
                for (int k = 0; k < take; k++) {
                    const int j = s->work_fill;
                    const float w = s->window[j] * (1.0f / 2048.0f);
                    s->work[j].re = iq[2 * (i + k)] * w;
                    s->work[j].im = iq[2 * (i + k) + 1] * w;
                    if (++s->work_fill == s->n) {
                        s->work_fill = 0;
                        cpu_fft_block(s);
                    }
                }
            }
            i += take;
            s->frame_pos += take;
            if (s->frame_pos == need) {
                if (s->gpu) {
                    /* Lote completo: se lanza asíncrono; el resultado se emite en un feed posterior. */
                    if (!s->gpu_drop)
                        gpu_spec_submit(s->gpu, s->k_target);
                    s->gpu_fill = 0;
                } else {
                    emit(s, cb, ctx);
                }
            }
        } else {
            /* Fase de descarte hasta completar la duración de la trama */
            long skip = MIN((long)(count - i), s->frame_len - s->frame_pos);
            i += (int)skip;
            s->frame_pos += skip;
        }
        if (s->frame_pos >= s->frame_len && s->frame_pos >= need)
            s->frame_pos = 0;
    }
    if (s->gpu)
        gpu_poll(s, cb, ctx);
}
