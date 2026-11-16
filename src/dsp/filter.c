/*
 * filter.c — Implementación de los filtros. Ver filter.h para la teoría.
 */
#include "filter.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/*  Diseño por ventana de Kaiser                                           */
/* ======================================================================= */

/* Función de Bessel modificada de primera especie y orden cero, I0(x),
 * evaluada con su serie de potencias:  I0(x) = Σ ((x/2)^k / k!)².
 * Converge rápido para los valores de β que usamos (< 15). */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, half = x / 2.0;
    for (int k = 1; k < 64; k++) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

double kaiser_beta(double a)
{
    if (a > 50.0)
        return 0.1102 * (a - 8.7);
    if (a >= 21.0)
        return 0.5842 * pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    return 0.0;
}

int fir_kaiser_ntaps(double trans_norm, double atten_db, int max_taps)
{
    if (trans_norm <= 0)
        return max_taps | 1;
    int n = (int)ceil((atten_db - 8.0) / (2.285 * 2.0 * M_PI * trans_norm)) + 1;
    n = CLAMP(n, 5, max_taps);
    return n | 1; /* impar: retardo de grupo entero y fase lineal simétrica */
}

void fir_design_lowpass(float *h, int ntaps, double fc, double beta)
{
    const double m = (ntaps - 1) / 2.0;
    const double i0b = bessel_i0(beta);
    double sum = 0.0;
    for (int i = 0; i < ntaps; i++) {
        double t = i - m;
        /* sinc ideal: sin(2π·fc·t)/(π·t), con su límite 2fc en t = 0 */
        double ideal = (t == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * t) / (M_PI * t);
        double r = t / m; /* −1 … 1 a lo largo de la ventana */
        double w = (m > 0) ? bessel_i0(beta * sqrt(fmax(0.0, 1.0 - r * r))) / i0b : 1.0;
        h[i] = (float)(ideal * w);
        sum += h[i];
    }
    /* Normalización: ganancia exactamente 1 en continua (0 Hz). */
    for (int i = 0; i < ntaps; i++)
        h[i] = (float)(h[i] / sum);
}

/* ======================================================================= */
/*  FIR diezmador                                                          */
/* ======================================================================= */

int firdec_init(firdec_t *f, int decim, double fs_in, double pass_hz, int max_taps)
{
    memset(f, 0, sizeof *f);
    f->decim = decim;
    const double fs_out = fs_in / decim;
    /* Lo que tras diezmar cae en ±pass_hz procede de [fs_out − pass_hz, …].
     * Esa es la banda de rechazo; la de paso termina en pass_hz. */
    double stop = fs_out - pass_hz;
    if (stop <= pass_hz)
        stop = pass_hz * 1.1; /* caso degenerado: transición mínima */
    const double trans = (stop - pass_hz) / fs_in;
    const double atten = 70.0;
    f->ntaps = fir_kaiser_ntaps(trans, atten, max_taps);
    f->taps = malloc(sizeof(float) * (size_t)f->ntaps);
    f->hist = calloc((size_t)f->ntaps * 2, sizeof(cf32));
    if (!f->taps || !f->hist)
        return -1;
    fir_design_lowpass(f->taps, f->ntaps, (pass_hz + stop) / 2.0 / fs_in, kaiser_beta(atten));
    return 0;
}

void firdec_free(firdec_t *f)
{
    free(f->taps);
    free(f->hist);
    memset(f, 0, sizeof *f);
}

int firdec_process(firdec_t *f, const cf32 *in, int n, cf32 *out)
{
    const int nt = f->ntaps;
    const float *restrict h = f->taps;
    int nout = 0;
    for (int i = 0; i < n; i++) {
        f->hist[f->pos] = in[i];
        f->hist[f->pos + nt] = in[i];
        if (++f->pos == nt)
            f->pos = 0;
        if (++f->phase < f->decim)
            continue;
        f->phase = 0;
        /* Ventana contigua de las nt últimas muestras: hist[pos .. pos+nt-1] */
        const cf32 *restrict x = &f->hist[f->pos];
        float re = 0.f, im = 0.f;
        for (int k = 0; k < nt; k++) {
            re += h[k] * x[k].re;
            im += h[k] * x[k].im;
        }
        out[nout].re = re;
        out[nout].im = im;
        nout++;
    }
    return nout;
}

/* ======================================================================= */
/*  FIR complejo (paso banda asimétrico)                                   */
/* ======================================================================= */

int cfir_init_bandpass(cfir_t *f, double fs, double f_lo, double f_hi, double trans_hz, int max_taps)
{
    memset(f, 0, sizeof *f);
    const double atten = 65.0;
    f->ntaps = fir_kaiser_ntaps(trans_hz / fs, atten, max_taps);
    float *proto = malloc(sizeof(float) * (size_t)f->ntaps);
    f->taps = malloc(sizeof(cf32) * (size_t)f->ntaps);
    f->hist = calloc((size_t)f->ntaps * 2, sizeof(cf32));
    if (!proto || !f->taps || !f->hist) {
        free(proto);
        return -1;
    }
    /* Prototipo paso bajo de anchura (f_hi − f_lo)/2, desplazado a la
     * frecuencia central fc multiplicando por e^{j2π·fc·n/fs}. */
    const double half_bw = (f_hi - f_lo) / 2.0;
    const double fc = (f_hi + f_lo) / 2.0;
    fir_design_lowpass(proto, f->ntaps, (half_bw + trans_hz / 2.0) / fs, kaiser_beta(atten));
    const double m = (f->ntaps - 1) / 2.0;
    for (int i = 0; i < f->ntaps; i++) {
        /* El búfer se recorre de la muestra más antigua a la más nueva, así
         * que el coeficiente i multiplica a x[n − (ntaps−1−i)]. */
        double t = (f->ntaps - 1 - i) - m;
        double a = 2.0 * M_PI * fc * t / fs;
        f->taps[i].re = (float)(proto[i] * cos(a));
        f->taps[i].im = (float)(proto[i] * sin(a));
    }
    free(proto);
    return 0;
}

void cfir_free(cfir_t *f)
{
    free(f->taps);
    free(f->hist);
    memset(f, 0, sizeof *f);
}

void cfir_process(cfir_t *f, const cf32 *in, int n, cf32 *out)
{
    const int nt = f->ntaps;
    const cf32 *restrict h = f->taps;
    for (int i = 0; i < n; i++) {
        f->hist[f->pos] = in[i];
        f->hist[f->pos + nt] = in[i];
        if (++f->pos == nt)
            f->pos = 0;
        const cf32 *restrict x = &f->hist[f->pos];
        float re = 0.f, im = 0.f;
        for (int k = 0; k < nt; k++) {
            re += h[k].re * x[k].re - h[k].im * x[k].im;
            im += h[k].re * x[k].im + h[k].im * x[k].re;
        }
        out[i].re = re;
        out[i].im = im;
    }
}

/* ======================================================================= */
/*  Remuestreador polifásico                                               */
/* ======================================================================= */
/*
 * Idea: para obtener la señal en un instante intermedio τ entre muestras,
 * se evalúa  y(τ) = Σ x[k]·h(τ − k)  con h un paso bajo continuo (sinc con
 * ventana). En vez de evaluar h al vuelo, se precalcula en P "fases":
 * bank[p][j] = h(L/2 − 1 − j + p/P). La salida usa la fase más próxima a la
 * fracción de τ, con error temporal ≤ 1/(2P) muestras (despreciable con P=256).
 */
int resamp_init(resamp_t *r, double fs_in, double fs_out, double cutoff_hz, double trans_hz)
{
    memset(r, 0, sizeof *r);
    r->step = fs_in / fs_out;
    r->P = 256;
    const double atten = 70.0;
    /* El corte nunca puede superar la mitad de la menor de las dos tasas. */
    double nyq = fmin(fs_in, fs_out) / 2.0;
    if (cutoff_hz + trans_hz / 2.0 > nyq)
        cutoff_hz = nyq - trans_hz / 2.0;
    if (cutoff_hz < 100.0)
        cutoff_hz = 100.0;
    int L = fir_kaiser_ntaps(trans_hz / fs_in, atten, 511);
    L = (L + 1) & ~1; /* par, para que el centro caiga entre muestras */
    r->L = L;
    r->bank = malloc(sizeof(float) * (size_t)(r->P * L));
    r->hist = calloc((size_t)L * 2, sizeof(float));
    if (!r->bank || !r->hist)
        return -1;
    const double fc = (cutoff_hz + trans_hz / 2.0) / fs_in;
    const double beta = kaiser_beta(atten);
    const double i0b = bessel_i0(beta);
    for (int p = 0; p < r->P; p++) {
        double sum = 0;
        float *row = &r->bank[p * L];
        for (int j = 0; j < L; j++) {
            double t = (L / 2.0 - 1.0 - j) + (double)p / r->P;
            double ideal = fabs(t) < 1e-9 ? 2.0 * fc : sin(2.0 * M_PI * fc * t) / (M_PI * t);
            double rr = t / (L / 2.0);
            double w = fabs(rr) < 1.0 ? bessel_i0(beta * sqrt(1.0 - rr * rr)) / i0b : 0.0;
            row[j] = (float)(ideal * w);
            sum += row[j];
        }
        for (int j = 0; j < L; j++) /* cada fase con ganancia unidad en DC */
            row[j] = (float)(row[j] / sum);
    }
    return 0;
}

void resamp_free(resamp_t *r)
{
    free(r->bank);
    free(r->hist);
    memset(r, 0, sizeof *r);
}

int resamp_process(resamp_t *r, const float *in, int n, float *out)
{
    const int L = r->L;
    int nout = 0;
    for (int i = 0; i < n; i++) {
        r->hist[r->pos] = in[i];
        r->hist[r->pos + L] = in[i];
        if (++r->pos == L)
            r->pos = 0;
        const float *restrict x = &r->hist[r->pos];
        /* Emitir todas las salidas cuyo instante cae antes de la siguiente
         * muestra de entrada (puede ser 0, 1 o varias si subimos la tasa). */
        while (r->frac < 1.0) {
            int p = (int)(r->frac * r->P);
            if (p >= r->P)
                p = r->P - 1;
            const float *restrict h = &r->bank[p * L];
            float acc = 0.f;
            for (int k = 0; k < L; k++)
                acc += h[k] * x[k];
            out[nout++] = acc;
            r->frac += r->step;
        }
        r->frac -= 1.0;
    }
    return nout;
}

/* ======================================================================= */
/*  Remuestreador polifásico complejo                                      */
/* ======================================================================= */
/* Reutiliza el diseño del banco de fases de resamp_init() (una instancia
 * temporal de resamp_t aporta el banco y L/P/step) y solo cambia la línea de
 * retardo a muestras complejas. Así ambos remuestreadores son idénticos. */
int cresamp_init(cresamp_t *r, double fs_in, double fs_out, double cutoff_hz, double trans_hz)
{
    resamp_t t;
    memset(r, 0, sizeof *r);
    if (resamp_init(&t, fs_in, fs_out, cutoff_hz, trans_hz) < 0) {
        resamp_free(&t);
        return -1;
    }
    r->step = t.step;
    r->frac = 0.0;
    r->L = t.L;
    r->P = t.P;
    r->bank = t.bank; /* se traspasa la propiedad del banco */
    r->hist = calloc((size_t)r->L * 2, sizeof(cf32));
    free(t.hist);
    if (!r->hist) {
        free(r->bank);
        memset(r, 0, sizeof *r);
        return -1;
    }
    return 0;
}

void cresamp_free(cresamp_t *r)
{
    free(r->bank);
    free(r->hist);
    memset(r, 0, sizeof *r);
}

int cresamp_process(cresamp_t *r, const cf32 *in, int n, cf32 *out)
{
    const int L = r->L;
    int nout = 0;
    for (int i = 0; i < n; i++) {
        r->hist[r->pos] = in[i];
        r->hist[r->pos + L] = in[i];
        if (++r->pos == L)
            r->pos = 0;
        const cf32 *restrict x = &r->hist[r->pos];
        while (r->frac < 1.0) {
            int p = (int)(r->frac * r->P);
            if (p >= r->P)
                p = r->P - 1;
            const float *restrict h = &r->bank[p * L];
            float re = 0.f, im = 0.f;
            for (int k = 0; k < L; k++) {
                re += h[k] * x[k].re;
                im += h[k] * x[k].im;
            }
            out[nout].re = re;
            out[nout].im = im;
            nout++;
            r->frac += r->step;
        }
        r->frac -= 1.0;
    }
    return nout;
}

/* ======================================================================= */
/*  NCO                                                                    */
/* ======================================================================= */

void nco_init(nco_t *o, double freq_hz, double fs)
{
    double w = 2.0 * M_PI * freq_hz / fs;
    o->phasor.re = 1.f;
    o->phasor.im = 0.f;
    o->step.re = (float)cos(w);
    o->step.im = (float)sin(w);
    o->count = 0;
}

void nco_mix(nco_t *o, const cf32 *in, int n, cf32 *out)
{
    cf32 ph = o->phasor;
    const cf32 st = o->step;
    for (int i = 0; i < n; i++) {
        const float xr = in[i].re, xi = in[i].im;
        out[i].re = xr * ph.re - xi * ph.im;
        out[i].im = xr * ph.im + xi * ph.re;
        const float pr = ph.re * st.re - ph.im * st.im;
        ph.im = ph.re * st.im + ph.im * st.re;
        ph.re = pr;
    }
    /* Renormaliza |fasor| = 1 una vez por bloque: el error de redondeo del
     * producto repetido haría crecer o menguar la amplitud lentamente. */
    float mag = sqrtf(ph.re * ph.re + ph.im * ph.im);
    if (mag > 0.f) {
        ph.re /= mag;
        ph.im /= mag;
    }
    o->phasor = ph;
}
