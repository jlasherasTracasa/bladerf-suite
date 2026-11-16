/*
 * ddc_plan.c — Parte de C puro del DDC en GPU: diseño del filtro de un canal
 * y etapa fina en CPU. Se compila siempre (con y sin CUDA). Ver ddc_gpu.h.
 */
#include "ddc_gpu.h"

#include "../dsp/filter.h"

#include <stdlib.h>
#include <string.h>

/* Mismas reglas que dsp/ddc.c (copiadas a propósito: ddc.c es de otro
 * agente y sus funciones son estáticas). */
static int smooth_decim(int m)
{
    for (; m > 1; m--) {
        int r = m;
        for (int p = 2; p <= 7; p++)
            while (r % p == 0)
                r /= p;
        if (r == 1)
            return m;
    }
    return 1;
}

static int split_stages(int m, int *out, int cap)
{
    static const int cand[] = {8, 7, 6, 5, 4, 3, 2};
    int n = 0;
    while (m > 1 && n < cap) {
        int d = 0;
        for (size_t i = 0; i < ARRAY_LEN(cand); i++)
            if (m % cand[i] == 0) {
                d = cand[i];
                break;
            }
        if (!d)
            return -1;
        out[n++] = d;
        m /= d;
    }
    return m == 1 ? n : -1;
}

/* ======================================================================= */
/*  Plan                                                                   */
/* ======================================================================= */
int ddc_plan_design(ddc_plan_t *p, double fs_in, double fs_out, double bw_hz)
{
    memset(p, 0, sizeof *p);
    if (fs_in <= 0 || fs_out <= 0 || fs_out > fs_in || bw_hz <= 0)
        return -1;
    int d = smooth_decim((int)floor(fs_in / fs_out + 1e-9));
    if (d < DDC_GPU_MIN_DECIM)
        return -1;
    p->fs_in = fs_in;
    p->fs_out = fs_out;
    p->bw = MIN(bw_hz, 0.9 * fs_out);
    p->decim = d;
    p->fs_mid = fs_in / d;

    /* Un solo FIR para ÷D. Banda de paso: ±bw/2 más un cuarto del margen libre
     * (como ddc.c); la de rechazo empieza en fs_mid − paso, de modo que lo que
     * aliasea sobre ±bw/2 viene de más allá y queda ≥ 80 dB abajo (ddc.c usa
     * 70; aquí se pide más porque el filtro polifásico cuesta poco). */
    const double atten = 80.0;
    double pass = p->bw / 2.0 + 0.25 * (p->fs_mid - p->bw);
    double stop = p->fs_mid - pass;
    if (stop <= pass)
        stop = pass * 1.1;
    p->ntaps = fir_kaiser_ntaps((stop - pass) / fs_in, atten, DDC_GPU_MAX_TAPS);
    p->h = malloc(sizeof(float) * (size_t)p->ntaps);
    if (!p->h)
        return -1;
    fir_design_lowpass(p->h, p->ntaps, (pass + stop) / 2.0 / fs_in, kaiser_beta(atten));
    return 0;
}

void ddc_plan_free(ddc_plan_t *p)
{
    free(p->h);
    memset(p, 0, sizeof *p);
}

long ddc_plan_cpu_delay(double fs_in, double fs_out, double bw_hz)
{
    if (fs_in <= 0 || fs_out <= 0 || fs_out > fs_in || bw_hz <= 0)
        return -1;
    double bw = MIN(bw_hz, 0.9 * fs_out);
    int dec[8];
    int ns = split_stages(smooth_decim((int)floor(fs_in / fs_out + 1e-9)), dec, 8);
    if (ns < 0)
        return -1;
    double fs = fs_in;
    long prod = 1, total = 0;
    for (int i = 0; i < ns; i++) {
        double r = fs / dec[i];
        double pass = bw / 2.0 + 0.25 * (r - bw);
        firdec_t f;
        if (firdec_init(&f, dec[i], fs, pass, 801) < 0)
            return -1;
        total += (long)((f.ntaps - 1) / 2) * prod; /* retardo de la etapa en muestras de entrada */
        firdec_free(&f);
        prod *= dec[i];
        fs = r;
    }
    return total;
}

/* ======================================================================= */
/*  Etapa fina (idéntica a la parte final de ddc.c)                         */
/* ======================================================================= */
#define FINE_CHUNK 4096

struct ddc_fine {
    double fs_mid, fs_out;
    cfir_t chan;
    bool have_chan;
    cresamp_t rs;
    bool have_rs;
    cf32 *a, *b;
};

ddc_fine_t *ddc_fine_create(double fs_mid, double fs_out, double bw_hz)
{
    if (fs_mid <= 0 || fs_out <= 0 || fs_out > fs_mid * (1 + 1e-9) || bw_hz <= 0)
        return NULL;
    ddc_fine_t *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->fs_mid = fs_mid;
    f->fs_out = fs_out;
    double bw = MIN(bw_hz, 0.9 * fs_out);
    double trans = MAX(0.2 * bw, 0.02 * fs_mid);
    if (bw / 2.0 + trans < fs_mid / 2.0) {
        if (cfir_init_bandpass(&f->chan, fs_mid, -bw / 2.0, bw / 2.0, trans, 1023) < 0)
            goto fail;
        f->have_chan = true;
    }
    if (fabs(fs_mid - fs_out) > 1e-6) {
        double rtrans = MAX((fs_out - bw) / 2.0, 0.05 * fs_out);
        if (cresamp_init(&f->rs, fs_mid, fs_out, bw / 2.0, rtrans) < 0)
            goto fail;
        f->have_rs = true;
    }
    f->a = malloc(sizeof(cf32) * (FINE_CHUNK + 16));
    f->b = malloc(sizeof(cf32) * (FINE_CHUNK + 16));
    if (!f->a || !f->b)
        goto fail;
    return f;
fail:
    ddc_fine_destroy(f);
    return NULL;
}

void ddc_fine_destroy(ddc_fine_t *f)
{
    if (!f)
        return;
    if (f->have_chan)
        cfir_free(&f->chan);
    if (f->have_rs)
        cresamp_free(&f->rs);
    free(f->a);
    free(f->b);
    free(f);
}

size_t ddc_fine_max_out(const ddc_fine_t *f, int n)
{
    size_t chunks = (size_t)n / FINE_CHUNK + 1;
    return (size_t)((double)n * f->fs_out / f->fs_mid) + 4 * chunks + 8;
}

int ddc_fine_process(ddc_fine_t *f, const cf32 *in, int n, cf32 *out, int out_cap)
{
    int total = 0;
    for (int off = 0; off < n; off += FINE_CHUNK) {
        int cn = MIN(FINE_CHUNK, n - off);
        const cf32 *src = in + off;
        if (f->have_chan) {
            cfir_process(&f->chan, src, cn, f->a);
            src = f->a;
        }
        if (f->have_rs) {
            int m = cresamp_process(&f->rs, src, cn, f->b); /* ≤ cn·fout/fin + 2 ≤ cn + 2 */
            int w = MIN(m, out_cap - total);
            if (w > 0)
                memcpy(out + total, f->b, sizeof(cf32) * (size_t)w);
            total += MAX(w, 0);
        } else {
            int w = MIN(cn, out_cap - total);
            if (w > 0)
                memcpy(out + total, src, sizeof(cf32) * (size_t)w);
            total += MAX(w, 0);
        }
    }
    return total;
}
