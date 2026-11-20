/*
 * ddc.c — Conversor digital descendente. Ver ddc.h.
 */
#include "ddc.h"

#include "filter.h"

#include <stdlib.h>
#include <string.h>

#define DDC_CHUNK 8192
#define DDC_MAX_STAGES 8

struct ddc {
    double fs_in, fs_out, bw;
    int64_t center, freq;
    nco_t nco;
    int nstages;
    int decim[DDC_MAX_STAGES];
    firdec_t stage[DDC_MAX_STAGES];
    cfir_t chan;
    bool have_chan;
    cresamp_t rs;
    bool have_rs;
    double fs_mid;      /* tasa tras el diezmado entero */
    cf32 *a, *b;        /* búferes de trabajo de DDC_CHUNK (+margen) muestras */
};

/* Mayor M' ≤ M cuyos factores primos sean 2, 3, 5 o 7 (así se puede dividir en
 * etapas de ≤ 8). Con M ≤ 1 no hay diezmado entero. */
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

/* Reparte M en etapas de ≤ 8, probando los divisores grandes primero. */
static int split_stages(int m, int *out)
{
    static const int cand[] = {8, 7, 6, 5, 4, 3, 2};
    int n = 0;
    while (m > 1 && n < DDC_MAX_STAGES) {
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

static void set_nco_step(ddc_t *d)
{
    /* e^{-jω0 n}: se desplaza el canal (freq − center) hacia 0 Hz */
    double w = -2.0 * M_PI * (double)(d->freq - d->center) / d->fs_in;
    d->nco.step.re = (float)cos(w);
    d->nco.step.im = (float)sin(w);
}

ddc_t *ddc_create(double fs_in, int64_t center_hz, int64_t freq_hz, double fs_out, double bw_hz)
{
    if (fs_in <= 0 || fs_out <= 0 || fs_out > fs_in || bw_hz <= 0)
        return NULL;
    ddc_t *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->fs_in = fs_in;
    d->fs_out = fs_out;
    d->center = center_hz;
    d->freq = freq_hz;
    d->bw = MIN(bw_hz, 0.9 * fs_out);
    d->nco.phasor.re = 1.f;
    set_nco_step(d);

    int m = smooth_decim((int)floor(fs_in / fs_out + 1e-9));
    d->nstages = split_stages(m, d->decim);
    if (d->nstages < 0) {
        free(d);
        return NULL;
    }
    double fs = fs_in;
    for (int i = 0; i < d->nstages; i++) {
        double r = fs / d->decim[i];
        /* Banda de paso: ±bw/2 más un cuarto del margen libre; la banda de
         * rechazo empieza en r − paso, así lo que aliasea sobre ±bw/2 viene
         * de más allá y queda atenuado ≥ 70 dB (ver ddc.h). */
        double pass = d->bw / 2.0 + 0.25 * (r - d->bw);
        if (firdec_init(&d->stage[i], d->decim[i], fs, pass, 801) < 0)
            goto fail;
        fs = r;
    }
    d->fs_mid = fs;
    /* Filtro de canal: transición el 20 % del ancho (mín. 2 % de la tasa) */
    double trans = MAX(0.2 * d->bw, 0.02 * fs);
    if (d->bw / 2.0 + trans < fs / 2.0) {
        if (cfir_init_bandpass(&d->chan, fs, -d->bw / 2.0, d->bw / 2.0, trans, 1023) < 0)
            goto fail;
        d->have_chan = true;
    }
    if (fabs(fs - fs_out) > 1e-6) {
        double rtrans = MAX((fs_out - d->bw) / 2.0, 0.05 * fs_out);
        if (cresamp_init(&d->rs, fs, fs_out, d->bw / 2.0, rtrans) < 0)
            goto fail;
        d->have_rs = true;
    }
    d->a = malloc(sizeof(cf32) * (DDC_CHUNK + 16));
    d->b = malloc(sizeof(cf32) * (DDC_CHUNK + 16));
    if (!d->a || !d->b)
        goto fail;
    return d;
fail:
    ddc_destroy(d);
    return NULL;
}

double ddc_delay_in(const ddc_t *d)
{
    double fs = d->fs_in, delay = 0;
    for (int i = 0; i < d->nstages; i++) {
        delay += 0.5 * (d->stage[i].ntaps - 1) * (d->fs_in / fs);
        fs /= d->decim[i];
    }
    if (d->have_chan)
        delay += 0.5 * (d->chan.ntaps - 1) * (d->fs_in / fs);
    if (d->have_rs)
        delay += 0.5 * (d->rs.L - 1) * (d->fs_in / fs);
    return delay;
}

void ddc_destroy(ddc_t *d)
{
    if (!d)
        return;
    for (int i = 0; i < d->nstages; i++)
        firdec_free(&d->stage[i]);
    if (d->have_chan)
        cfir_free(&d->chan);
    if (d->have_rs)
        cresamp_free(&d->rs);
    free(d->a);
    free(d->b);
    free(d);
}

void ddc_retune(ddc_t *d, int64_t center_hz, int64_t freq_hz)
{
    d->center = center_hz;
    d->freq = freq_hz;
    set_nco_step(d); /* la fase actual del fasor se conserva: sin clics */
}

size_t ddc_max_out(const ddc_t *d, int n)
{
    size_t chunks = (size_t)n / DDC_CHUNK + 1;
    return (size_t)((double)n * d->fs_out / d->fs_in) + 8 * chunks + 8;
}

int ddc_process(ddc_t *d, const cf32 *in, int n, cf32 *out, int out_cap)
{
    int total = 0;
    for (int off = 0; off < n; off += DDC_CHUNK) {
        int cn = MIN(DDC_CHUNK, n - off);
        nco_mix(&d->nco, in + off, cn, d->a);
        cf32 *src = d->a, *dst = d->b;
        int cnt = cn;
        for (int i = 0; i < d->nstages; i++) {
            cnt = firdec_process(&d->stage[i], src, cnt, dst);
            cf32 *t = src;
            src = dst;
            dst = t;
        }
        if (d->have_chan) {
            cfir_process(&d->chan, src, cnt, dst);
            cf32 *t = src;
            src = dst;
            dst = t;
        }
        if (d->have_rs) {
            /* La salida del remuestreador cabe en 'out' si el llamador usó
             * ddc_max_out(); si no, se descarta el exceso (nunca se desborda). */
            cf32 tmp[DDC_CHUNK / 2 + 64];
            int m = 0;
            for (int s = 0; s < cnt; s += DDC_CHUNK / 4) {
                int c = MIN(DDC_CHUNK / 4, cnt - s);
                m = cresamp_process(&d->rs, src + s, c, tmp);
                int room = out_cap - total;
                int w = MIN(m, room);
                if (w > 0)
                    memcpy(out + total, tmp, sizeof(cf32) * (size_t)w);
                total += MAX(w, 0);
            }
        } else {
            int w = MIN(cnt, out_cap - total);
            if (w > 0)
                memcpy(out + total, src, sizeof(cf32) * (size_t)w);
            total += MAX(w, 0);
        }
    }
    return total;
}
