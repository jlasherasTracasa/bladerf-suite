/*
 * pulse.c — Pulsos de conformación y FIR genéricos. Ver pulse.h.
 */
#include "pulse.h"

#include <stdlib.h>
#include <string.h>

int pulse_ntaps(int sps, int span) { return span * sps + 1; }

static double sinc(double x) { return x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

static void normalize(float *h, int n, pulse_norm_t norm)
{
    double s = 0, e = 0, pk = 0;
    for (int i = 0; i < n; i++) {
        s += h[i];
        e += (double)h[i] * h[i];
        pk = fmax(pk, fabs(h[i]));
    }
    double g = norm == PULSE_NORM_DC ? (s != 0 ? 1.0 / s : 1.0)
             : norm == PULSE_NORM_PEAK ? (pk > 0 ? 1.0 / pk : 1.0)
             : (e > 0 ? 1.0 / sqrt(e) : 1.0);
    for (int i = 0; i < n; i++)
        h[i] = (float)(h[i] * g);
}

int pulse_rrc(float *h, double beta, int sps, int span, pulse_norm_t norm)
{
    if (sps < 1 || span < 1 || beta < 0 || beta > 1)
        return -1;
    int n = pulse_ntaps(sps, span);
    for (int i = 0; i < n; i++) {
        double t = (i - (n - 1) / 2.0) / sps, v;
        if (fabs(t) < 1e-9)
            v = 1.0 - beta + 4.0 * beta / M_PI;
        else if (beta > 0 && fabs(fabs(4.0 * beta * t) - 1.0) < 1e-9)
            v = beta / sqrt(2.0) * ((1 + 2 / M_PI) * sin(M_PI / (4 * beta)) + (1 - 2 / M_PI) * cos(M_PI / (4 * beta)));
        else
            v = (sin(M_PI * t * (1 - beta)) + 4 * beta * t * cos(M_PI * t * (1 + beta))) /
                (M_PI * t * (1 - (4 * beta * t) * (4 * beta * t)));
        h[i] = (float)v;
    }
    normalize(h, n, norm);
    return n;
}

int pulse_rc(float *h, double beta, int sps, int span, pulse_norm_t norm)
{
    if (sps < 1 || span < 1 || beta < 0 || beta > 1)
        return -1;
    int n = pulse_ntaps(sps, span);
    for (int i = 0; i < n; i++) {
        double t = (i - (n - 1) / 2.0) / sps, v;
        if (beta > 0 && fabs(fabs(2.0 * beta * t) - 1.0) < 1e-9)
            v = M_PI / 4.0 * sinc(1.0 / (2.0 * beta));
        else
            v = sinc(t) * cos(M_PI * beta * t) / (1.0 - (2 * beta * t) * (2 * beta * t));
        h[i] = (float)v;
    }
    normalize(h, n, norm);
    return n;
}

int pulse_gaussian(float *h, double bt, int sps, int span, pulse_norm_t norm)
{
    if (sps < 1 || span < 1 || bt <= 0)
        return -1;
    int n = pulse_ntaps(sps, span);
    double sigma = sqrt(log(2.0)) / (2.0 * M_PI * bt);
    for (int i = 0; i < n; i++) {
        double t = (i - (n - 1) / 2.0) / sps;
        h[i] = (float)exp(-t * t / (2.0 * sigma * sigma));
    }
    normalize(h, n, norm);
    return n;
}

int pulse_rect(float *h, int sps)
{
    if (sps < 1)
        return -1;
    for (int i = 0; i < sps; i++)
        h[i] = 1.0f / (float)sps;
    return sps;
}

int pulse_gmsk(float *h, double bt, int sps, int span)
{
    if (sps < 1 || span < 1 || bt <= 0)
        return -1;
    int ng = pulse_ntaps(sps, span), n = ng + sps - 1;
    float *g = malloc((size_t)ng * sizeof(float));
    if (!g)
        return -1;
    pulse_gaussian(g, bt, sps, span, PULSE_NORM_DC);
    for (int i = 0; i < n; i++) {
        double a = 0;
        for (int k = 0; k < sps; k++)
            if (i - k >= 0 && i - k < ng)
                a += g[i - k];
        h[i] = (float)a;
    }
    free(g);
    normalize(h, n, PULSE_NORM_DC);
    return n;
}

/* ======================================================================= */
/*  FIR                                                                    */
/* ======================================================================= */
int fir_f_init(fir_f_t *f, const float *taps, int ntaps, int decim)
{
    memset(f, 0, sizeof *f);
    if (ntaps < 1 || decim < 1)
        return -1;
    f->ntaps = ntaps;
    f->decim = decim;
    f->rtaps = malloc((size_t)ntaps * sizeof(float));
    f->hist = calloc((size_t)ntaps * 2, sizeof(float));
    if (!f->rtaps || !f->hist) {
        fir_f_free(f);
        return -1;
    }
    for (int i = 0; i < ntaps; i++)
        f->rtaps[i] = taps[ntaps - 1 - i];
    return 0;
}

void fir_f_free(fir_f_t *f)
{
    free(f->rtaps);
    free(f->hist);
    memset(f, 0, sizeof *f);
}

void fir_f_reset(fir_f_t *f)
{
    memset(f->hist, 0, (size_t)f->ntaps * 2 * sizeof(float));
    f->pos = f->phase = 0;
}

int fir_f_process(fir_f_t *f, const float *in, int n, float *out)
{
    const int N = f->ntaps;
    int w = 0;
    for (int i = 0; i < n; i++) {
        f->hist[f->pos] = f->hist[f->pos + N] = in[i];
        f->pos = f->pos + 1 == N ? 0 : f->pos + 1;
        if (++f->phase == f->decim) {
            f->phase = 0;
            /* las N últimas muestras, de más antigua a más reciente, empiezan en hist[pos] */
            const float *x = f->hist + f->pos, *h = f->rtaps;
            float acc = 0;
            for (int k = 0; k < N; k++)
                acc += h[k] * x[k];
            out[w++] = acc;
        }
    }
    return w;
}

int fir_cf_init(fir_cf_t *f, const float *taps, int ntaps, int decim)
{
    memset(f, 0, sizeof *f);
    if (ntaps < 1 || decim < 1)
        return -1;
    f->ntaps = ntaps;
    f->decim = decim;
    f->rtaps = malloc((size_t)ntaps * sizeof(float));
    f->hist = calloc((size_t)ntaps * 2, sizeof(cf32));
    if (!f->rtaps || !f->hist) {
        fir_cf_free(f);
        return -1;
    }
    for (int i = 0; i < ntaps; i++)
        f->rtaps[i] = taps[ntaps - 1 - i];
    return 0;
}

void fir_cf_free(fir_cf_t *f)
{
    free(f->rtaps);
    free(f->hist);
    memset(f, 0, sizeof *f);
}

void fir_cf_reset(fir_cf_t *f)
{
    memset(f->hist, 0, (size_t)f->ntaps * 2 * sizeof(cf32));
    f->pos = f->phase = 0;
}

int fir_cf_process(fir_cf_t *f, const cf32 *in, int n, cf32 *out)
{
    const int N = f->ntaps;
    int w = 0;
    for (int i = 0; i < n; i++) {
        f->hist[f->pos] = f->hist[f->pos + N] = in[i];
        f->pos = f->pos + 1 == N ? 0 : f->pos + 1;
        if (++f->phase == f->decim) {
            f->phase = 0;
            const cf32 *x = f->hist + f->pos;
            const float *h = f->rtaps;
            float re = 0, im = 0;
            for (int k = 0; k < N; k++) {
                re += h[k] * x[k].re;
                im += h[k] * x[k].im;
            }
            out[w].re = re;
            out[w].im = im;
            w++;
        }
    }
    return w;
}

/* ======================================================================= */
/*  Conformación                                                           */
/* ======================================================================= */
int pulse_shape_f(const float *sym, int nsym, const float *h, int ntaps, int sps, float *out)
{
    int n = nsym * sps + ntaps - 1;
    if (nsym < 1 || sps < 1)
        return -1;
    memset(out, 0, (size_t)n * sizeof(float));
    for (int k = 0; k < nsym; k++) {
        float s = sym[k];
        if (s == 0.0f)
            continue;
        float *o = out + (size_t)k * sps;
        for (int i = 0; i < ntaps; i++)
            o[i] += s * h[i];
    }
    return n;
}

int pulse_shape_cf(const cf32 *sym, int nsym, const float *h, int ntaps, int sps, cf32 *out)
{
    int n = nsym * sps + ntaps - 1;
    if (nsym < 1 || sps < 1)
        return -1;
    memset(out, 0, (size_t)n * sizeof(cf32));
    for (int k = 0; k < nsym; k++) {
        cf32 s = sym[k];
        cf32 *o = out + (size_t)k * sps;
        for (int i = 0; i < ntaps; i++) {
            o[i].re += s.re * h[i];
            o[i].im += s.im * h[i];
        }
    }
    return n;
}
