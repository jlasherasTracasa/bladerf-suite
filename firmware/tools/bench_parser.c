/*
 * bench_parser.c — Microbenchmark del lado host del flujo de espectro (sin hardware).
 * Genera tramas sintéticas con el formato de la FPGA (cabecera + N potencias + relleno) y mide
 *   a) el intérprete (fpga_lab_parser_feed_iq) solo,
 *   b) el intérprete + el análisis "completo" que hacía lab_probe por trama (dBFS de todos los bins,
 *      qsort para la mediana y malloc),
 *   c) el intérprete + análisis de 1 de cada M tramas.
 * y lo expresa en palabras/s sostenibles por un solo hilo. Uso: bench_parser N K [fs_msps]
 * Compilación: gcc -O2 -Isrc firmware/tools/bench_parser.c src/sdr/fpga_lab.c -lm -o build/f1/bench_parser
 */
#include "sdr/fpga_lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static long g_frames;
static int g_every = 1;
static float g_sink;

static void on_full(void *ctx, const fpga_lab_frame_t *f)
{
    (void)ctx;
    if (++g_frames % g_every) return;
    float *db = malloc(sizeof(float) * (size_t)f->n), *tmp = malloc(sizeof(float) * (size_t)f->n);
    fpga_lab_frame_to_dbfs(f, db);
    memcpy(tmp, db, sizeof(float) * (size_t)f->n);
    qsort(tmp, (size_t)f->n, sizeof(float), cmp_float);
    g_sink += tmp[f->n / 2] + db[0];
    free(db);
    free(tmp);
}

static void on_count(void *ctx, const fpga_lab_frame_t *f) { (void)ctx; (void)f; g_frames++; }

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1024, k = argc > 2 ? atoi(argv[2]) : 2;
    double fs = (argc > 3 ? atof(argv[3]) : 30.72) * 1e6;
    int total = FPGA_SPEC_HDR_WORDS + n;
    total = (total + 2047) / 2048 * 2048;
    int nframes = 4000000 / total + 1;
    uint32_t *w = calloc((size_t)total * (size_t)nframes, 4);
    for (int f = 0; f < nframes; f++) {
        uint32_t *p = w + (size_t)f * total;
        p[0] = FPGA_SPEC_MAGIC0; p[1] = FPGA_SPEC_MAGIC1; p[2] = ((uint32_t)k << 16) | (uint32_t)n; p[3] = (uint32_t)f;
        p[6] = 0x0108; p[7] = (uint32_t)total;
        for (int i = 0; i < n; i++) p[8 + i] = (uint32_t)((20u << 26) | (1000u + (uint32_t)(i * 7919 % 100000)));
    }
    const size_t nw = (size_t)total * (size_t)nframes;
    const double need = fs / ((double)k * n) * total; /* palabras/s que genera la FPGA */
    printf("N=%d K=%d fs=%.2f Msps: la FPGA genera %.2f Mpalabras/s (%.0f tramas/s, %d palabras/trama)\n", n, k, fs / 1e6, need / 1e6, fs / (k * n), total);
    struct { const char *name; fpga_lab_frame_fn cb; int every; } cases[] = {
        {"intérprete solo", on_count, 1}, {"intérprete + análisis de cada trama (antes)", on_full, 1},
        {"intérprete + análisis de 1 de cada 50", on_full, 50}, {"intérprete + análisis de 1 de cada 1000", on_full, 1000}};
    for (size_t c = 0; c < ARRAY_LEN(cases); c++) {
        g_frames = 0; g_every = cases[c].every;
        fpga_lab_parser_t *p = fpga_lab_parser_create(cases[c].cb, NULL);
        double t0 = now_s();
        for (size_t o = 0; o < nw; o += 16384) fpga_lab_parser_feed_words(p, w + o, MIN((size_t)16384, nw - o));
        double dt = now_s() - t0;
        double rate = (double)nw / dt;
        printf("  %-45s %7.1f Mpalabras/s  -> %s (necesita %.2f)\n", cases[c].name, rate / 1e6, rate >= need ? "SOSTIENE" : "NO sostiene", need / 1e6);
        fpga_lab_parser_destroy(p);
    }
    printf("(%g)\n", (double)g_sink);
    return 0;
}
