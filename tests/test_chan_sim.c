/*
 * test_chan_sim.c — Comprueba el emulador de canal tests/chan_sim.h contra la teoría.
 */
#include "chan_sim.h"
#include "test_util.h"

/* IRR de un tono a f_norm: |P(+f)|/|P(−f)| en dB (misma idea que cal_tone_irr_db, sin depender de tx/) */
static double cal_tone_irr_db_local(const cf32 *x, int n, double f)
{
    double pr = 0, pi_ = 0, mr = 0, mi = 0;
    for (int k = 0; k < n; k++) {
        double c = cos(2 * M_PI * f * k), s = sin(2 * M_PI * f * k);
        pr += x[k].re * c + x[k].im * s;
        pi_ += x[k].im * c - x[k].re * s;
        mr += x[k].re * c - x[k].im * s;
        mi += x[k].im * c + x[k].re * s;
    }
    return 10 * log10((pr * pr + pi_ * pi_) / (mr * mr + mi * mi));
}

static double besj0(double x) /* serie de potencias de J0 (|x| ≲ 8) */
{
    double term = 1, sum = 1;
    for (int k = 1; k < 40; k++) {
        term *= -(x * x / 4.0) / ((double)k * k);
        sum += term;
    }
    return sum;
}

int main(void)
{
    cs_rng_t r;
    cs_seed(&r, 7);

    /* Gaussiana: media 0, varianza 1 */
    double m = 0, v = 0;
    int N = 200000;
    for (int i = 0; i < N; i++) {
        double g = cs_gauss(&r);
        m += g;
        v += g * g;
    }
    CHECK_NEAR(m / N, 0.0, 0.02);
    CHECK_NEAR(v / N, 1.0, 0.03);

    /* Rayleigh: E|h|² = 1, P(|h|² < 0.1) = 1 − e^{−0.1}; autocorrelación J0(2π·fd·τ).
     * Promedio sobre varias realizaciones (cada una tiene pocas fases aleatorias). */
    double fs = 10000, fd = 50;
    double pw = 0, low = 0, ac = 0, ac0 = 0;
    long cnt = 0;
    int lag = 12; /* τ = 1,2 ms → 2π·fd·τ = 0,377 */
    for (int rep = 0; rep < 200; rep++) {
        cs_fader_t f;
        cs_fader_init(&f, fd, 0, 0, &r);
        for (int i = 0; i < 4000; i += 3) {
            cf32 h = cs_fader_at(&f, i / fs), h2 = cs_fader_at(&f, (i + lag) / fs);
            double p = (double)h.re * h.re + (double)h.im * h.im;
            pw += p;
            low += p < 0.1;
            ac += h.re * h2.re + h.im * h2.im;
            ac0 += p;
            cnt++;
        }
    }
    CHECK_NEAR(pw / cnt, 1.0, 0.06);
    CHECK_NEAR(low / cnt, 1.0 - exp(-0.1), 0.02);
    CHECK_NEAR(ac / ac0, besj0(2 * M_PI * fd * lag / fs), 0.05);
    CHECK_NEAR(besj0(2.0), 0.2239, 1e-3); /* la propia J0 de la prueba */

    /* Rice K = 10 dB: potencia 1 y varianza de la potencia mucho menor que Rayleigh */
    double s1 = 0, s2 = 0, rs1 = 0, rs2 = 0;
    cnt = 0;
    for (int rep = 0; rep < 100; rep++) {
        cs_fader_t f, g;
        cs_fader_init(&f, fd, 10, 1, &r);
        cs_fader_init(&g, fd, 0, 0, &r);
        for (int i = 0; i < 4000; i += 5) {
            cf32 h = cs_fader_at(&f, i / fs), q = cs_fader_at(&g, i / fs);
            double p = (double)h.re * h.re + (double)h.im * h.im, pq = (double)q.re * q.re + (double)q.im * q.im;
            s1 += p;
            s2 += p * p;
            rs1 += pq;
            rs2 += pq * pq;
            cnt++;
        }
    }
    double var_rice = s2 / cnt - (s1 / cnt) * (s1 / cnt), var_ray = rs2 / cnt - (rs1 / cnt) * (rs1 / cnt);
    CHECK_NEAR(s1 / cnt, 1.0, 0.08);
    CHECK(var_rice < 0.5 * var_ray);

    /* Multitrayecto estático: un impulso reaparece en cada retardo con su potencia relativa */
    {
        int n = 256;
        cf32 *x = calloc(n, sizeof(cf32)), *y = calloc(n, sizeof(cf32));
        x[50].re = 1;
        cs_multipath_t mp = {.ntaps = 2, .delay_s = {0, 20e-6}, .power_db = {0, -6.0206}, .fd_hz = 0};
        cs_multipath(x, n, 1e6, &mp, &r, y);
        double p0 = y[50].re * y[50].re + y[50].im * y[50].im, p1 = y[70].re * y[70].re + y[70].im * y[70].im;
        CHECK_NEAR(p1 / p0, 0.25, 0.01);
        CHECK_NEAR(p0 + p1, 1.0, 0.02); /* potencia total normalizada */
        double other = 0;
        for (int i = 0; i < n; i++)
            if (i != 50 && i != 70)
                other += y[i].re * y[i].re + y[i].im * y[i].im;
        CHECK(other < 1e-6);
        /* retardo fraccionario: 20,5 muestras reparte la energía entre 70 y 71 */
        mp.delay_s[1] = 20.5e-6;
        cs_multipath(x, n, 1e6, &mp, &r, y);
        double a = y[70].re * y[70].re + y[70].im * y[70].im, b = y[71].re * y[71].re + y[71].im * y[71].im;
        CHECK(a > 0.3 * p0 * 0.25 && b > 0.3 * p0 * 0.25);
        free(x);
        free(y);
    }

    /* Ruido de fase: varianza del incremento = 2π·β/fs */
    {
        int n = 100000;
        cf32 *x = malloc(sizeof(cf32) * n);
        for (int i = 0; i < n; i++)
            x[i] = (cf32){1, 0};
        double bw = 100, fs2 = 1e5;
        cs_phase_noise(x, n, fs2, bw, &r);
        double vs = 0, prev = 0;
        for (int i = 0; i < n; i++) {
            double ph = atan2(x[i].im, x[i].re), d = ph - prev;
            d -= 2 * M_PI * round(d / (2 * M_PI));
            if (i)
                vs += d * d;
            prev = ph;
        }
        CHECK_NEAR(vs / (n - 1) / (2 * M_PI * bw / fs2), 1.0, 0.05);
        free(x);
    }

    /* Desequilibrio IQ: rechazo de imagen = |μ/ν|² (teoría) */
    {
        int n = 20000;
        cf32 *x = malloc(sizeof(cf32) * n);
        for (int i = 0; i < n; i++) {
            double a = 2 * M_PI * 0.05 * i;
            x[i] = (cf32){(float)(0.5 * cos(a)), (float)(0.5 * sin(a))};
        }
        double g = 1.1, ph = 5.0, mr, mi, nr, ni;
        cs_iq_munu(g, ph * M_PI / 180, &mr, &mi, &nr, &ni);
        double irr_th = 20 * log10(hypot(mr, mi) / hypot(nr, ni));
        cs_iq_imbalance(x, n, g, ph, 0.01, -0.02);
        CHECK_NEAR(cal_tone_irr_db_local(x, n, 0.05), irr_th, 0.2);
        free(x);
    }

    /* Deriva de reloj: un tono a f0 pasa a f0/(1+ppm·1e−6) */
    {
        int n = 100000;
        cf32 *x = malloc(sizeof(cf32) * n), *y = malloc(sizeof(cf32) * n);
        double f0 = 0.1, ppm = 1000;
        for (int i = 0; i < n; i++)
            x[i] = (cf32){(float)cos(2 * M_PI * f0 * i), (float)sin(2 * M_PI * f0 * i)};
        int mm = cs_clock_drift(x, n, 1e6, ppm, 0, y);
        CHECK(mm > n - 200 && mm < n);
        double acc = 0, ph0 = atan2(y[10].im, y[10].re), ph1 = atan2(y[mm - 10].im, y[mm - 10].re);
        acc = ph1 - ph0;
        double expect = 2 * M_PI * f0 * (1 + ppm * 1e-6) * (mm - 20);
        acc -= 2 * M_PI * round((acc - expect) / (2 * M_PI));
        CHECK_NEAR(acc, expect, 0.02);
        free(x);
        free(y);
    }

    /* AWGN: SNR medida */
    {
        int n = 100000;
        cf32 *x = malloc(sizeof(cf32) * n), *y = malloc(sizeof(cf32) * n);
        for (int i = 0; i < n; i++)
            x[i] = (cf32){0.5f, 0.0f};
        memcpy(y, x, sizeof(cf32) * n);
        cs_awgn(y, n, 20.0, &r);
        double pn = 0;
        for (int i = 0; i < n; i++)
            pn += (y[i].re - 0.5) * (y[i].re - 0.5) + y[i].im * y[i].im;
        CHECK_NEAR(10 * log10(0.25 / (pn / n)), 20.0, 0.2);
        free(x);
        free(y);
    }

    /* Cadena completa: sin NaN, longitud correcta, potencia razonable */
    {
        int n = 20000;
        cf32 *x = malloc(sizeof(cf32) * n), *y = malloc(sizeof(cf32) * n);
        for (int i = 0; i < n; i++)
            x[i] = (cf32){(float)(0.5 * cos(0.3 * i)), (float)(0.5 * sin(0.3 * i))};
        cs_cfg_t c = {.iq_gain = 1.05, .iq_phase_deg = 3, .use_multipath = 1, .phase_noise_hz = 50, .cfo_hz = 200,
                      .ppm = 50, .snr_db = 25};
        c.mp = (cs_multipath_t){.ntaps = 3, .delay_s = {0, 3e-6, 7e-6}, .power_db = {0, -3, -8}, .rice0 = 1, .k_db = 6,
                                .fd_hz = 20};
        int mm = cs_apply(x, n, 1e6, &c, &r, y);
        CHECK(mm > n - 10);
        bool fin = true;
        double p = 0;
        for (int i = 0; i < mm; i++) {
            fin &= isfinite(y[i].re) && isfinite(y[i].im);
            p += y[i].re * y[i].re + y[i].im * y[i].im;
        }
        CHECK(fin);
        CHECK(p / mm > 0.02 && p / mm < 2.0);
        free(x);
        free(y);
    }
    return TEST_SUMMARY();
}
