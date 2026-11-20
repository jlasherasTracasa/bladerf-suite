/*
 * bench_fpga_spectrum.c — Banco de CPU del intérprete de tramas del espectro FPGA.
 *
 * Reproduce el flujo de la placa (30,72 Msps, N=4096, K=375 → 20 imágenes/s, cada
 * trama de 6144 palabras = 4096 + cabecera + relleno → ~123 kpalabras/s) y mide el
 * tiempo de CPU del camino parser → fpga_lab_frame_to_dbfs → empaquetado tipo 1,
 * que es lo que el hilo DSP hace por trama (on_spectrum añade detector y S-meter,
 * ver engine.c; aquí solo se mide lo que vive en fpga_lab.c).
 * Se alimenta en bloques de una trama (6144 palabras = lo que lee el motor en modo espectro,
 * engine.c) y a mitad del flujo se simula un cambio de K en caliente: una trama truncada que el
 * intérprete debe resincronizar (cuenta un hueco) sin perder las siguientes.
 *
 *   gcc -std=c11 -D_GNU_SOURCE -O2 -Isrc tests/bench_fpga_spectrum.c src/sdr/fpga_lab.c \
 *       src/dsp/detector.c src/dsp/fft.c src/util/log.c -lm -lpthread -o build/f1b/bench_fpga && build/f1b/bench_fpga
 */
#include "common.h"
#include "sdr/fpga_lab.h"
#include "dsp/detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N 4096
#define TOTAL 6144
#define FPS 20
#define SECONDS 60

static uint32_t make_power(uint64_t acc)
{
    unsigned e = 0;
    while ((acc >> e) > 0x3FFFFFFu) e++;
    return (uint32_t)((acc >> e) & 0x3FFFFFFu) | (e << 26);
}

static double cpu_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static long g_frames;
static detector_t *g_det;
static int g_with_det;
static float g_db[FPGA_SPEC_NMAX];
static uint8_t g_msg[32 + FPGA_SPEC_NMAX];

static void on_frame(void *ctx, const fpga_lab_frame_t *f)
{
    (void)ctx;
    g_frames++;
    fpga_lab_frame_to_dbfs(f, g_db);
    if (g_with_det) {
        /* igual que on_spectrum: relleno del DC y detector (promedio lineal por trama) */
        const int c = f->n / 2;
        const float fill = 0.5f * (g_db[c - 3] + g_db[c + 3]);
        for (int k = -2; k <= 2; k++)
            g_db[c + k] = fill;
        detector_feed(g_det, g_db, f->n, (float)FPS);
    }
    spectrum_msg_pack(g_msg, g_db, f->n, SPEC_SRC_FPGA, 100e6, 30.72e6, -145.f, 0.6f);
}

int main(int argc, char **argv)
{
    (void)argv;
    g_with_det = argc > 1; /* cualquier argumento: añade el detector de on_spectrum */
    if (g_with_det)
        g_det = detector_create(N);
    static uint32_t fr[TOTAL];
    fr[0] = FPGA_SPEC_MAGIC0;
    fr[1] = FPGA_SPEC_MAGIC1;
    fr[2] = (uint32_t)N | (375u << 16);
    fr[6] = 8u | (1u << 8);
    fr[7] = TOTAL;
    for (int i = 0; i < N; i++)
        fr[8 + i] = make_power(100000000ull + (uint64_t)(i * 7919 % 100000));
    for (int i = 8 + N; i < TOTAL; i++)
        fr[i] = 0xDEADBEEFu;

    const int nframes = FPS * SECONDS;
    /* cambio de K en caliente a mitad: la trama 'cut' se corta tras 1000 palabras */
    const int cut = nframes / 2;
    const size_t total_words = (size_t)nframes * TOTAL;
    uint32_t *stream = malloc(total_words * 4);
    size_t w = 0;
    int expected = 0;
    for (int i = 0; i < nframes; i++) {
        const size_t len = i == cut ? 1000 : TOTAL;
        memcpy(stream + w, fr, len * 4);
        stream[w + 3] = (uint32_t)i;
        if (i > cut)
            stream[w + 2] = (uint32_t)N | (188u << 16); /* K nuevo */
        w += len;
        if (i != cut)
            expected++;
    }
    const size_t total_used = w;
    /* Como llega de sync_rx: pares int16 I,Q (palabra = I | Q<<16) en bloques de 4096 */
    fpga_lab_parser_t *p = fpga_lab_parser_create(on_frame, NULL);
    const double t0 = cpu_s();
    for (size_t i = 0; i < total_used; i += TOTAL) {
        size_t m = total_used - i < TOTAL ? total_used - i : TOTAL;
        fpga_lab_parser_feed_iq(p, (const int16_t *)(stream + i), m);
    }
    const double dt = cpu_s() - t0;
    uint64_t fr_n, sk, gp;
    fpga_lab_parser_stats(p, &fr_n, &sk, &gp);
    printf("huecos %llu (2 esperados por el cambio de K: trama cortada + salto de grupo), palabras descartadas %llu\n", (unsigned long long)gp,
           (unsigned long long)sk);
    printf("tramas %ld (%d esperadas), CPU %.3f s por %d s de flujo = %.3f %% de un núcleo, %.1f us/trama\n",
           g_frames, expected, dt, SECONDS, 100.0 * dt / SECONDS, 1e6 * dt / nframes);
    fpga_lab_parser_destroy(p);
    free(stream);
    (void)total_words;
    return g_frames == expected && gp == 2 ? 0 : 1;
}
