/*
 * test_nr_gpu.c — La correlación de PSS de NR en GPU (src/gpu/nr_corr_gpu.cu) da lo
 * mismo que la de CPU (src/decoders/nr_corr.c) y el decodificador completo emite los
 * mismos eventos por ambas rutas. Sin GPU (CUDA=0 o sin dispositivo) las
 * comparaciones se omiten y el test pasa igualmente.
 *
 * También imprime el coste de CPU del correlador y del decodificador con y sin GPU a
 * 30,72 y 61,44 Msps (informativo: no se exige ninguna cifra).
 */
#include "test_util.h"

#include "decoders/nr_corr.h"
#include "tx/mod.h"

extern const decoder_desc_t DEC_NR;
extern const modulator_desc_t MOD_NR;

static double cpu_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}
static double wall_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

#define NT 6
#define NFFT 8192

/* Plantillas de energía unidad: ruido gaussiano (lo que importa es la aritmética, no la PSS). */
static void make_templates(cf32 *tm[NT], int tl[NT], uint32_t *seed)
{
    for (int t = 0; t < NT; t++) {
        tl[t] = t < 3 ? 256 : 512;
        tm[t] = malloc(sizeof(cf32) * (size_t)tl[t]);
        double e = 0;
        for (int k = 0; k < tl[t]; k++) {
            tm[t][k].re = (float)tu_gauss(seed);
            tm[t][k].im = (float)tu_gauss(seed);
            e += (double)tm[t][k].re * tm[t][k].re + (double)tm[t][k].im * tm[t][k].im;
        }
        float s = (float)(1.0 / sqrt(e));
        for (int k = 0; k < tl[t]; k++) {
            tm[t][k].re *= s;
            tm[t][k].im *= s;
        }
    }
}

static void compare_case(const char *name, int n, uint32_t seed)
{
    cf32 *tm[NT];
    int tl[NT];
    make_templates(tm, tl, &seed);
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    for (int i = 0; i < n; i++) {
        x[i].re = (float)(0.1 * tu_gauss(&seed));
        x[i].im = (float)(0.1 * tu_gauss(&seed));
    }
    /* Plantillas incrustadas (amplitudes y posiciones distintas, una cruzando un borde de bloque) */
    int pos[NT] = {1000, 8100, n / 2, n - 700, 3 * (NFFT - 511) + 5, 20};
    for (int t = 0; t < NT; t++)
        for (int k = 0; k < tl[t] && pos[t] + k < n; k++) {
            x[pos[t] + k].re += 1.5f * tm[t][k].re * (1 + t);
            x[pos[t] + k].im += 1.5f * tm[t][k].im * (1 + t);
        }
    /* Tramo de energía casi nula: el suelo ε evita correlaciones espurias */
    for (int i = n / 4; i < n / 4 + 3000; i++)
        x[i].re = x[i].im = 1e-6f;

    const cf32 *tp[NT];
    for (int t = 0; t < NT; t++)
        tp[t] = tm[t];
    setenv("BLADERF_NR_GPU", "0", 1);
    nr_corr_t *cc = nr_corr_create(NFFT, NT, tp, tl);
    unsetenv("BLADERF_NR_GPU");
    nr_corr_t *cg = nr_corr_create(NFFT, NT, tp, tl);
    CHECK(cc != NULL && cg != NULL);
    CHECK(!nr_corr_using_gpu(cc));
    if (!cg || !cc)
        return;
    if (!nr_corr_using_gpu(cg)) {
        printf("  [%s] sin GPU: comparación omitida\n", name);
        goto out;
    }
    size_t no = (size_t)NT * (size_t)n;
    float *oc = malloc(sizeof(float) * no), *og = malloc(sizeof(float) * no);
    int nc = nr_corr_run(cc, x, n, oc), ng = nr_corr_run(cg, x, n, og);
    CHECK_EQ(nc, ng);
    CHECK_EQ(nc, n - 512 + 1);
    double maxd = 0;
    int peak_ok = 1;
    for (int t = 0; t < NT; t++) {
        const float *a = oc + (size_t)t * nc, *b = og + (size_t)t * nc;
        int pa = 0, pb = 0;
        for (int i = 0; i < nc; i++) {
            maxd = fmax(maxd, fabs((double)a[i] - b[i]));
            if (a[i] > a[pa])
                pa = i;
            if (b[i] > b[pb])
                pb = i;
        }
        if (pa != pb)
            peak_ok = 0;
    }
    printf("  [%s] n=%d nout=%d  máx|CPU−GPU| = %.2e, picos %s\n", name, n, nc, maxd, peak_ok ? "iguales" : "DISTINTOS");
    CHECK(maxd < 2e-3);
    CHECK(peak_ok);
    free(oc);
    free(og);
out:
    nr_corr_destroy(cc);
    nr_corr_destroy(cg);
    for (int t = 0; t < NT; t++)
        free(tm[t]);
    free(x);
}

/* Coste del correlador a la longitud de una ventana de 25 ms a 7,68 Msps. */
static void bench_corr(void)
{
    uint32_t seed = 5;
    cf32 *tm[NT];
    int tl[NT];
    make_templates(tm, tl, &seed);
    const cf32 *tp[NT];
    for (int t = 0; t < NT; t++)
        tp[t] = tm[t];
    int n = 192000;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    float *o = malloc(sizeof(float) * (size_t)NT * (size_t)n);
    for (int i = 0; i < n; i++) {
        x[i].re = (float)tu_gauss(&seed);
        x[i].im = (float)tu_gauss(&seed);
    }
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0)
            setenv("BLADERF_NR_GPU", "0", 1);
        else
            unsetenv("BLADERF_NR_GPU");
        nr_corr_t *c = nr_corr_create(NFFT, NT, tp, tl);
        if (!c || (pass && !nr_corr_using_gpu(c))) {
            nr_corr_destroy(c);
            break;
        }
        nr_corr_run(c, x, n, o); /* calentamiento (reserva de memorias) */
        int reps = pass ? 100 : 20;
        double c0 = cpu_s(), w0 = wall_s();
        for (int r = 0; r < reps; r++)
            nr_corr_run(c, x, n, o);
        printf("  correlador %s: %.2f ms de pared, %.2f ms de CPU por ventana (n=%d)\n", pass ? "GPU" : "CPU",
               1e3 * (wall_s() - w0) / reps, 1e3 * (cpu_s() - c0) / reps, n);
        nr_corr_destroy(c);
    }
    unsetenv("BLADERF_NR_GPU");
    for (int t = 0; t < NT; t++)
        free(tm[t]);
    free(x);
    free(o);
}

/* Decodificador completo con ambas rutas: mismos eventos y coste de CPU. */
static void run_dec(double rate, int64_t center, int64_t gscn, int pci)
{
    iqbuf_t b = {0};
    char err[128] = "", params[160];
    snprintf(params, sizeof params, "{\"pci\":%d,\"scs\":30,\"rate\":%.0f,\"periods\":3}", pci, rate);
    if (MOD_NR.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        CHECK(0);
        return;
    }
    uint32_t seed = 11;
    chan_cfo(b.s, b.n, rate, (double)(gscn - center));
    chan_awgn(b.s, b.n, 10, &seed);
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)b.n);
    cf32_to_q11(b.s, b.n, 0.5, q);
    evs_t ev[2] = {{0}, {0}};
    double cpu[2], wall[2];
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0)
            setenv("BLADERF_NR_GPU", "0", 1);
        else
            unsetenv("BLADERF_NR_GPU");
        double c0 = cpu_s(), w0 = wall_s();
        run_wide(&DEC_NR, q, b.n, rate, center, "{\"period\":0}", &ev[pass]);
        cpu[pass] = cpu_s() - c0;
        wall[pass] = wall_s() - w0;
    }
    char key[64];
    snprintf(key, sizeof key, "\"pci\":%d,", pci);
    CHECK(ev_has(&ev[0], key));
    CHECK(ev_has(&ev[1], key));
    CHECK_EQ(evs_count(&ev[0]), evs_count(&ev[1]));
    CHECK(ev[0].data && ev[1].data && strcmp(ev[0].data, ev[1].data) == 0 ? 1 : (ev[0].data == NULL && ev[1].data == NULL));
    printf("  decodificador %.2f Msps (%d muestras, %d eventos): CPU sin GPU %.3f s (pared %.3f), con GPU %.3f s (pared %.3f)\n",
           rate / 1e6, b.n, evs_count(&ev[0]), cpu[0], wall[0], cpu[1], wall[1]);
    evs_free(&ev[0]);
    evs_free(&ev[1]);
    iqbuf_free(&b);
    free(q);
}

int main(void)
{
    compare_case("192000 muestras", 192000, 1);
    compare_case("n no múltiplo del paso", 50003, 2);
    compare_case("n pequeño (1 bloque)", 6000, 3);
    bench_corr();
    const int64_t C = 3500000000LL, G0 = 3000000000LL + 347LL * 1440000;
    run_dec(30720000, C, G0, 321);
    run_dec(61440000, C, G0 + 2 * 1440000, 77);
    return TEST_SUMMARY();
}
