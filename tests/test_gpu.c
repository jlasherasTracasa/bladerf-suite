/*
 * test_gpu.c — Pruebas de los módulos CUDA de C1: DDC por lotes y canalizador
 * PFB. Sin GPU (o compilado con CUDA=0) las pruebas de GPU se omiten y solo se
 * comprueba la parte de C puro (plan y etapa fina), de modo que el test pasa
 * en ambas compilaciones.
 */
#include "test_util.h"

#include "dsp/ddc.h"
#include "dsp/spectrum.h"
#include <unistd.h>
#include "gpu/ddc_gpu.h"
#include "gpu/pfb_gpu.h"

/* ---- Señal de prueba: tonos + ruido blanco, cuantizada a Q11 ------------ */
typedef struct {
    double hz, amp;
} tone_t;

static int16_t *make_iq(long n, double fs, double center, const tone_t *tn, int nt, double noise_rms, uint32_t seed)
{
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
    uint32_t s = seed;
    for (long i = 0; i < n; i++) {
        double re = 0, im = 0;
        for (int t = 0; t < nt; t++) {
            double a = 2.0 * M_PI * fmod((tn[t].hz - center) / fs * (double)i, 1.0);
            re += tn[t].amp * cos(a);
            im += tn[t].amp * sin(a);
        }
        re += noise_rms * tu_gauss(&s);
        im += noise_rms * tu_gauss(&s);
        q[2 * i] = (int16_t)lrint(fmax(-2048, fmin(2047, re * 2048.0)));
        q[2 * i + 1] = (int16_t)lrint(fmax(-2048, fmin(2047, im * 2048.0)));
    }
    return q;
}

/* ---- Recogida de salidas de la GPU por canal ---------------------------- */
#define MAXC 8
typedef struct {
    cf32 *y[MAXC];
    long n[MAXC], cap[MAXC];
} gout_t;

static void gcollect(void *ctx, int ch, const cf32 *y, int n, double t)
{
    (void)t;
    gout_t *g = ctx;
    if (g->n[ch] + n > g->cap[ch]) {
        g->cap[ch] = (g->n[ch] + n) * 2;
        g->y[ch] = realloc(g->y[ch], sizeof(cf32) * (size_t)g->cap[ch]);
    }
    memcpy(g->y[ch] + g->n[ch], y, sizeof(cf32) * (size_t)n);
    g->n[ch] += n;
}

/* SNR (dB) de la diferencia a[i]-b[i] respecto a la potencia de b, saltando
 * 'skip' muestras iniciales. */
static double diff_snr_db(const cf32 *a, const cf32 *b, long n, long skip)
{
    double pd = 0, pb = 0;
    for (long i = skip; i < n; i++) {
        double dr = (double)a[i].re - b[i].re, di = (double)a[i].im - b[i].im;
        pd += dr * dr + di * di;
        pb += (double)b[i].re * b[i].re + (double)b[i].im * b[i].im;
    }
    return 10.0 * log10((pb + 1e-30) / (pd + 1e-30));
}

/* Lleva el canal a 0 Hz con un oscilador en DOBLE precisión (fase continua si
 * hay cambio de frecuencia en 'split'). La ruta CPU de ddc.c usa un rotor en
 * float: su paso e^{-jω} redondeado a 24 bits tiene un error de frecuencia de
 * ~0,05 Hz que acumula varias décimas de radián en 4 M muestras (irrelevante
 * para decodificar, pero ensucia una comparación a 60 dB). Con la entrada ya
 * mezclada, ddc_t corre con freq = centro (paso exacto 1+0j) y solo se
 * comparan los filtros, que es lo que se quiere verificar. */
static cf32 *premix(const int16_t *q, long n, double fs, double dfreq, double dfreq2, long split)
{
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    for (long i = 0; i < n; i++) {
        double turns = i < split ? dfreq / fs * (double)i
                                 : dfreq / fs * (double)split + dfreq2 / fs * (double)(i - split);
        double a = -2.0 * M_PI * fmod(turns, 1.0);
        double c = cos(a), s = sin(a), re = q[2 * i] / 2048.0, im = q[2 * i + 1] / 2048.0;
        x[i].re = (float)(re * c - im * s);
        x[i].im = (float)(re * s + im * c);
    }
    return x;
}

/* ======================================================================= */
/*  DDC en GPU frente al DDC de CPU                                         */
/* ======================================================================= */
typedef struct {
    int64_t freq;
    double fs_out, bw;
} chcfg_t;

static void test_ddc_vs_cpu(void)
{
    printf("DDC GPU frente a ddc.c (tonos + ruido)\n");
    const double fs = 10.24e6;
    const int64_t center = 100000000;
    const long N = 4 * 1024 * 1024; /* 0,41 s */
    const chcfg_t cfg[] = {
        {100000000 + 350000, 48000, 12500},
        {100000000 - 1200000, 48000, 16000},
        {100000000 + 2500000, 25000, 10000},
        {100000000, 96000, 40000}, /* sobre el centro: lleva la continua */
        {100000000 + 4100000, 50000, 20000},
    };
    const int nc = (int)ARRAY_LEN(cfg);

    /* Dos tonos dentro de cada canal, uno fuera de banda y ruido a −35 dBFS */
    tone_t tn[16];
    int nt = 0;
    for (int c = 0; c < nc; c++) {
        tn[nt++] = (tone_t){(double)cfg[c].freq + 1000.0, 0.12};
        tn[nt++] = (tone_t){(double)cfg[c].freq - 2300.0, 0.08};
    }
    tn[nt++] = (tone_t){(double)center + 3300000.0, 0.15};
    int16_t *q = make_iq(N, fs, (double)center, tn, nt, 0.014, 1234);

    char err[200] = "";
    ddc_gpu_t *g = ddc_gpu_create(1 << 20, err, sizeof err);
    if (!g) {
        printf("  (sin GPU: %s: se omite)\n", err);
        free(q);
        return;
    }
    printf("  GPU: %s\n", ddc_gpu_device_name(g));
    ddc_gpu_set_dc_removal(g, false); /* la ruta CPU de referencia no quita continua */

    ddc_plan_t plan[MAXC];
    int id[MAXC];
    ddc_fine_t *fine[MAXC];
    for (int c = 0; c < nc; c++) {
        CHECK(ddc_plan_design(&plan[c], fs, cfg[c].fs_out, cfg[c].bw) == 0);
        id[c] = ddc_gpu_add(g, &plan[c], center, cfg[c].freq);
        CHECK(id[c] >= 0);
        long t_cpu = ddc_plan_cpu_delay(fs, cfg[c].fs_out, cfg[c].bw);
        long lead = (ddc_gpu_ntaps(g, id[c]) - 1) / 2 - t_cpu;
        CHECK(lead <= 0);
        ddc_gpu_set_lead(g, id[c], lead);
        fine[c] = ddc_fine_create(plan[c].fs_mid, cfg[c].fs_out, cfg[c].bw);
        CHECK(fine[c] != NULL);
    }

    /* Bloques de tamaño irregular: comprueba la continuidad entre bloques */
    gout_t go = {{0}, {0}, {0}};
    const int sizes[] = {100003, 262144, 77777, 500000, 1048576};
    long off = 0;
    int bi = 0;
    while (off < N) {
        /* MIN es una macro que evalúa sus argumentos dos veces: el índice se avanza fuera de ella */
        const long want = sizes[bi++ % 5];
        int n = (int)MIN(want, N - off);
        CHECK(ddc_gpu_process(g, q + 2 * off, n, fs, center, gcollect, &go) >= 0);
        off += n;
    }

    /* Referencia de CPU: ddc_t sobre los mismos enteros normalizados */
    for (int c = 0; c < nc; c++) {
        cf32 *xin = premix(q, N, fs, (double)(cfg[c].freq - center), 0, N);
        ddc_t *d = ddc_create(fs, center, center, cfg[c].fs_out, cfg[c].bw);
        CHECK(d != NULL);
        size_t cap = ddc_max_out(d, (int)N);
        cf32 *ref = malloc(sizeof(cf32) * cap);
        int nref = ddc_process(d, xin, (int)N, ref, (int)cap);
        /* Etapa fina de CPU sobre las salidas de la GPU (a fs/D) */
        size_t fcap = ddc_fine_max_out(fine[c], (int)go.n[id[c]]);
        cf32 *got = malloc(sizeof(cf32) * fcap);
        int ngot = ddc_fine_process(fine[c], go.y[id[c]], (int)go.n[id[c]], got, (int)fcap);
        long m = MIN(nref, ngot);
        double snr = diff_snr_db(got, ref, m, 8);
        printf("  canal %d  f=%+.0f kHz  fs_out=%.0f  D=%d  taps=%d  salidas=%ld/%d  SNR(dif)=%.1f dB\n", c,
               (double)(cfg[c].freq - center) / 1e3, cfg[c].fs_out, plan[c].decim, plan[c].ntaps, (long)ngot, nref,
               snr);
        CHECK(m > 1000);
        CHECK(ngot >= nref - 2 && ngot <= nref + 2 + (int)(plan[c].ntaps / plan[c].decim * 30)); /* la GPU emite antes */
        CHECK(snr > 60.0);
        free(ref);
        free(got);
        free(xin);
        ddc_destroy(d);
    }

    /* Retune: continuidad de fase y mismo resultado que ddc_retune tras asentarse */
    {
        const int64_t f0 = cfg[0].freq, f1 = f0 + 4000;
        const long N2 = 2 * 1024 * 1024, split = 1048576;
        ddc_gpu_t *g2 = ddc_gpu_create(1 << 20, err, sizeof err);
        ddc_plan_t p;
        CHECK(ddc_plan_design(&p, fs, 48000, 12500) == 0);
        ddc_gpu_set_dc_removal(g2, false);
        int c0 = ddc_gpu_add(g2, &p, center, f0);
        long lead = (p.ntaps - 1) / 2 - ddc_plan_cpu_delay(fs, 48000, 12500);
        ddc_gpu_set_lead(g2, c0, lead);
        gout_t r = {{0}, {0}, {0}};
        ddc_gpu_process(g2, q, (int)split, fs, center, gcollect, &r);
        ddc_gpu_retune(g2, c0, center, f1);
        ddc_gpu_process(g2, q + 2 * split, (int)(N2 - split), fs, center, gcollect, &r);
        cf32 *xin = premix(q, N2, fs, (double)(f0 - center), (double)(f1 - center), split);
        ddc_t *d = ddc_create(fs, center, center, 48000, 12500);
        cf32 *ref = malloc(sizeof(cf32) * ddc_max_out(d, (int)N2));
        int n1 = ddc_process(d, xin, (int)split, ref, (int)ddc_max_out(d, (int)N2));
        int n2 = ddc_process(d, xin + split, (int)(N2 - split), ref + n1, (int)ddc_max_out(d, (int)N2) - n1);
        free(xin);
        ddc_fine_t *ff = ddc_fine_create(p.fs_mid, 48000, 12500);
        cf32 *got = malloc(sizeof(cf32) * ddc_fine_max_out(ff, (int)r.n[c0]));
        int ng = ddc_fine_process(ff, r.y[c0], (int)r.n[c0], got, (int)ddc_fine_max_out(ff, (int)r.n[c0]));
        long k_ret = (long)((double)split / fs * 48000.0);
        long skip = k_ret + 600; /* ≫ longitud de los filtros (≈ 50 ms a 48 ksps) */
        long m = MIN(n1 + n2, ng);
        double snr = diff_snr_db(got, ref, m, skip);
        printf("  retune a +4 kHz: SNR(dif) tras asentarse = %.1f dB\n", snr);
        CHECK(snr > 60.0);
        free(ref);
        free(got);
        free(r.y[c0]);
        ddc_fine_destroy(ff);
        ddc_destroy(d);
        ddc_plan_free(&p);
        ddc_gpu_destroy(g2);
    }

    /* Con quita-continua: un offset de continua grande no debe colarse en el canal central */
    {
        ddc_gpu_t *g3 = ddc_gpu_create(1 << 20, err, sizeof err);
        ddc_plan_t p;
        CHECK(ddc_plan_design(&p, fs, 96000, 40000) == 0);
        int c0 = ddc_gpu_add(g3, &p, center, center);
        int16_t *z = malloc(sizeof(int16_t) * 2 * (size_t)(1 << 20));
        uint32_t s = 7;
        for (long i = 0; i < (1 << 20); i++) {
            z[2 * i] = (int16_t)(200 + lrint(1.5 * tu_gauss(&s)));
            z[2 * i + 1] = (int16_t)(-150 + lrint(1.5 * tu_gauss(&s)));
        }
        gout_t r = {{0}, {0}, {0}};
        for (int b = 0; b < 3; b++)
            ddc_gpu_process(g3, z, 1 << 20, fs, center, gcollect, &r);
        double pw = 0;
        long cnt = 0;
        for (long i = r.n[c0] / 2; i < r.n[c0]; i++, cnt++)
            pw += r.y[c0][i].re * r.y[c0][i].re + r.y[c0][i].im * r.y[c0][i].im;
        printf("  continua (200,-150)/2048 = −17 dBFS: potencia en canal central = %.1f dBFS\n", 10 * log10(pw / cnt + 1e-30));
        CHECK(10 * log10(pw / cnt + 1e-30) < -70.0);
        free(z);
        free(r.y[c0]);
        ddc_plan_free(&p);
        ddc_gpu_destroy(g3);
    }

    for (int c = 0; c < nc; c++) {
        free(go.y[id[c]]);
        ddc_fine_destroy(fine[c]);
        ddc_plan_free(&plan[c]);
    }
    free(q);
    ddc_gpu_destroy(g);
}


/* ======================================================================= */
/*  Canalizador PFB                                                         */
/* ======================================================================= */
/* Potencia media (|y|²) del canal k sobre las tramas [t0, nf) */
static double chan_power(const cf32 *y, int M, int k, int t0, int nf)
{
    double p = 0;
    for (int t = t0; t < nf; t++)
        p += (double)y[(size_t)t * M + k].re * y[(size_t)t * M + k].re +
             (double)y[(size_t)t * M + k].im * y[(size_t)t * M + k].im;
    return p / (nf - t0);
}

static void test_pfb(void)
{
    printf("Canalizador PFB (M=64, ov=2, P=8)\n");
    const int M = 64, OV = 2, P = 8;
    char err[200] = "";
    pfb_gpu_t *g = pfb_gpu_create(M, OV, P, 1 << 16, err, sizeof err);
    if (!g) {
        printf("  (sin GPU: %s: se omite)\n", err);
        return;
    }
    const int D = pfb_gpu_decim(g);
    CHECK(D == M / OV);
    const double fs = 6.4e6;                 /* Δ = fs/M = 100 kHz por canal */
    const long N = 40000 * 4;
    const int j = 10, jn = M - 5;            /* canal positivo y canal de frecuencia negativa */
    tone_t tn[2] = {{j * fs / M, 0.4}, {-5 * fs / M, 0.3}};
    int16_t *q = make_iq(N, fs, 0.0, tn, 2, 0.0, 1);

    /* En trozos irregulares: comprueba el resto que no completa trama */
    int maxf = pfb_gpu_max_frames(g);
    cf32 *y = malloc(sizeof(cf32) * (size_t)((N / D + 8) * M));
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)maxf * M);
    long nf = 0, off = 0;
    int chunk[] = {1000, 4099, 777, 20000, 3};
    int ci = 0;
    while (off < N) {
        const long want = chunk[ci++ % 5]; /* MIN evalúa dos veces sus argumentos */
        int n = (int)MIN(want, N - off);
        int f = pfb_gpu_process(g, q + 2 * off, n, tmp);
        CHECK(f >= 0);
        memcpy(y + (size_t)nf * M, tmp, sizeof(cf32) * (size_t)f * M);
        nf += f;
        off += n;
    }
    CHECK(nf == N / D);
    const int t0 = 3 * P; /* tras llenarse el filtro */
    double pj = chan_power(y, M, j, t0, (int)nf), pn = chan_power(y, M, jn, t0, (int)nf);
    printf("  tono 0,4 en canal %d: |y|² = %.4f (esperado 0,16); canal %d: %.4f (esperado 0,09)\n", j, pj, jn, pn);
    CHECK_NEAR(pj, 0.16, 0.16 * 0.02);
    CHECK_NEAR(pn, 0.09, 0.09 * 0.02);
    double worst = -300, adj = -300;
    for (int k = 0; k < M; k++) {
        double d = k > j ? k - j : j - k, d2 = (k - jn) > 0 ? k - jn : jn - k;
        if (k == j || k == jn)
            continue;
        double pk = chan_power(y, M, k, t0, (int)nf);
        double rel1 = 10 * log10(pk / pj + 1e-30), rel2 = 10 * log10(pk / pn + 1e-30);
        if (d == 1 || d2 == 1)
            adj = fmax(adj, d == 1 ? rel1 : rel2);
        else if (d >= 2 && d2 >= 2)
            worst = fmax(worst, fmax(d >= 2 ? rel1 : -300, d2 >= 2 ? rel2 : -300));
    }
    /* El máximo de 'worst' mezcla el tono del otro canal (a ≥ 2 de uno pero no del otro): se
     * mide por separado respecto del tono propio de cada uno. */
    printf("  rechazo a >= 2 canales: %.1f dB; canal adyacente: %.1f dB (diseño: -6 dB en su centro)\n", worst, adj);
    CHECK(worst < -60.0);
    CHECK(adj < -3.0 && adj > -9.0);

    /* Frente a la definición directa (doble precisión), todos los canales de 4 tramas */
    {
        const float *h = pfb_gpu_proto(g);
        const int L = pfb_gpu_ntaps(g);
        double pd = 0, ps = 0;
        for (int t = 40; t < 44; t++) {
            long e = (long)(t + 1) * D - 1;
            for (int k = 0; k < M; k++) {
                double sr = 0, si = 0;
                for (int n = 0; n < L; n++) {
                    long i = e - n;
                    if (i < 0)
                        break;
                    double a = 2.0 * M_PI * (double)((long)k * n % M) / M;
                    double xr = q[2 * i] / 2048.0, xi = q[2 * i + 1] / 2048.0;
                    sr += h[n] * (xr * cos(a) - xi * sin(a));
                    si += h[n] * (xr * sin(a) + xi * cos(a));
                }
                double b = -2.0 * M_PI * (double)((long)k * (e % M) % M) / M;
                double rr = sr * cos(b) - si * sin(b), ri = sr * sin(b) + si * cos(b);
                cf32 v = y[(size_t)t * M + k];
                pd += (rr - v.re) * (rr - v.re) + (ri - v.im) * (ri - v.im);
                ps += rr * rr + ri * ri;
            }
        }
        printf("  GPU frente a la definición directa: SNR(dif) = %.1f dB\n", 10 * log10(ps / (pd + 1e-30)));
        CHECK(10 * log10(ps / (pd + 1e-30)) > 80.0);
    }
    free(y);
    free(tmp);
    free(q);
    pfb_gpu_destroy(g);

    /* Rendimiento: M=1024, ov=2, P=8, bloques de 1 Mi muestras */
    {
        const int BM = 1024, BN = 1 << 20, NB = 40;
        pfb_gpu_t *b = pfb_gpu_create(BM, 2, 8, BN, err, sizeof err);
        CHECK(b != NULL);
        if (!b)
            return;
        int16_t *z = malloc(sizeof(int16_t) * 2 * (size_t)BN);
        uint32_t s = 99;
        for (long i = 0; i < 2L * BN; i++)
            z[i] = (int16_t)lrint(300.0 * tu_gauss(&s));
        cf32 *o = malloc(sizeof(cf32) * (size_t)pfb_gpu_max_frames(b) * BM);
        pfb_gpu_process(b, z, BN, NULL); /* calentamiento */
        double t0s = now_monotonic();
        for (int i = 0; i < NB; i++)
            pfb_gpu_process(b, z, BN, NULL);
        double dt = now_monotonic() - t0s;
        double msps = (double)NB * BN / dt / 1e6;
        t0s = now_monotonic();
        for (int i = 0; i < NB / 2; i++)
            pfb_gpu_process(b, z, BN, o);
        double dt2 = now_monotonic() - t0s;
        double msps2 = (double)(NB / 2) * BN / dt2 / 1e6;
        printf("  rendimiento M=1024 ov=2 P=8: %.0f Msps solo calculo; %.0f Msps con bajada de las tramas a la CPU\n",
               msps, msps2);
        CHECK(msps > 61.44);
        free(z);
        free(o);
        pfb_gpu_destroy(b);
    }
}


/* Rendimiento del banco: 16 canales a 20 y a 61,44 Msps */
static void bench_ddc(double fs, int nch)
{
    char err[200] = "";
    const int BN = 1 << 20;
    ddc_gpu_t *g = ddc_gpu_create(BN, err, sizeof err);
    if (!g)
        return;
    int16_t *z = malloc(sizeof(int16_t) * 2 * (size_t)BN);
    uint32_t s = 5;
    for (long i = 0; i < 2L * BN; i++)
        z[i] = (int16_t)lrint(300.0 * tu_gauss(&s));
    ddc_plan_t p;
    CHECK(ddc_plan_design(&p, fs, 48000, 12500) == 0);
    for (int c = 0; c < nch; c++)
        CHECK(ddc_gpu_add(g, &p, 100000000, 100000000 + (int64_t)(-0.4 * fs + c * 0.8 * fs / nch)) >= 0);
    ddc_gpu_process(g, z, BN, fs, 100000000, NULL, NULL); /* calentamiento */
    const int NB = 30;
    double t0 = now_monotonic();
    for (int i = 0; i < NB; i++)
        ddc_gpu_process(g, z, BN, fs, 100000000, NULL, NULL);
    double dt = now_monotonic() - t0;
    double msps = (double)NB * BN / dt / 1e6;
    printf("  %d canales, D=%d, %d coef/canal: %.0f Msps sostenidos (%.2f ms/bloque de 1 Mi; GPU al %.1f %% a %.2f Msps)\n",
           nch, p.decim, p.ntaps, msps, dt / NB * 1e3, 100.0 * fs / (msps * 1e6), fs / 1e6);
    CHECK(msps > fs / 1e6);
    free(z);
    ddc_plan_free(&p);
    ddc_gpu_destroy(g);
}

/* ---- Espectro: la ruta GPU asíncrona da las mismas tramas que la de CPU ---- */
typedef struct {
    float db[64][1024];
    int n;
} frames_t;
static void on_frame(void *ctx, const float *db, int n, int navg)
{
    (void)navg;
    frames_t *f = ctx;
    if (f->n < 64 && n == 1024)
        memcpy(f->db[f->n++], db, sizeof(float) * 1024);
}

static void test_spectrum_async(void)
{
    /* 2,4 Msps, 20 fps, FFT 1024: la CPU promedia 117 FFT (tope de la trama), igual que la GPU. */
    const double fs = 2.4e6;
    const int blk = 50000, nblk = 40;
    tone_t tn[2] = {{300e3, 0.2}, {-700e3, 0.05}};
    int16_t *q = make_iq((long)blk * nblk, fs, 0, tn, 2, 0.02, 3);
    frames_t *fc = calloc(1, sizeof *fc), *fg = calloc(1, sizeof *fg);
    spectrum_t *sc = spectrum_create(1024, fs, 20, false), *sg = spectrum_create(1024, fs, 20, true);
    CHECK(sc && sg);
    if (!sc || !sg)
        return;
    for (int b = 0; b < nblk; b++) {
        spectrum_feed(sc, q + 2L * b * blk, blk, on_frame, fc);
        spectrum_feed(sg, q + 2L * b * blk, blk, on_frame, fg);
        if (spectrum_using_gpu(sg))
            usleep(2000); /* la GPU es asíncrona: deja que acabe el lote */
    }
    if (!spectrum_using_gpu(sg)) {
        printf("  espectro: sin GPU, comparación omitida\n");
    } else {
        usleep(50000);
        spectrum_feed(sg, q, 1, on_frame, fg); /* recoge el último lote */
        int m = fc->n < fg->n ? fc->n : fg->n;
        double maxd = 0;
        for (int f = 0; f < m; f++)
            for (int k = 0; k < 1024; k++)
                maxd = fmax(maxd, fabs((double)fc->db[f][k] - fg->db[f][k]));
        printf("  espectro: %d tramas CPU, %d GPU, máx|dif| = %.4f dB\n", fc->n, fg->n, maxd);
        CHECK(fg->n >= fc->n - 1 && fg->n >= 10);
        CHECK(maxd < 0.1);
    }
    spectrum_destroy(sc);
    spectrum_destroy(sg);
    free(fc);
    free(fg);
    free(q);
}

int main(void)
{
    test_spectrum_async();
    test_ddc_vs_cpu();
    printf("Rendimiento del DDC en GPU\n");
    bench_ddc(20e6, 16);
    bench_ddc(61.44e6, 16);
    bench_ddc(61.44e6, 40);
    test_pfb();
    return TEST_SUMMARY();
}
