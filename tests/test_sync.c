/*
 * test_sync.c — Pruebas de src/dsp/sync.*: AGC, FM, PLL, Costas, FLL, Farrow,
 * Gardner, Mueller-Müller y correlador.
 */
#include "dsp/pulse.h"
#include "dsp/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rs_state = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void)
{
    rs_state ^= rs_state << 13; rs_state ^= rs_state >> 7; rs_state ^= rs_state << 17;
    return (uint32_t)(rs_state >> 16);
}
static double urand(void) { return (rnd() + 1.0) / 4294967297.0; }
static float gauss(void) { return (float)(sqrt(-2.0 * log(urand())) * cos(2 * M_PI * urand())); }

static double rc_pulse(double t, double beta)
{
    double d = 1.0 - (2 * beta * t) * (2 * beta * t);
    double sinc = t == 0 ? 1.0 : sin(M_PI * t) / (M_PI * t);
    if (fabs(d) < 1e-9)
        return M_PI / 4.0 * (sin(M_PI / (2 * beta)) / (M_PI / (2 * beta)));
    return sinc * cos(M_PI * beta * t) / d;
}

/* Forma de onda Σ s_k·rc(t − kT) muestreada en t = t0 + i·ts (en unidades de T). */
static void make_wave(const cf32 *sym, int nsym, double t0, double ts, int ns, double sigma, cf32 *out)
{
    for (int i = 0; i < ns; i++) {
        double t = t0 + i * ts, re = 0, im = 0;
        int k0 = (int)floor(t);
        for (int k = k0 - 8; k <= k0 + 8; k++) {
            if (k < 0 || k >= nsym) continue;
            double p = rc_pulse(t - k, 0.35);
            re += sym[k].re * p;
            im += sym[k].im * p;
        }
        out[i].re = (float)(re + sigma * gauss());
        out[i].im = (float)(im + sigma * gauss());
    }
}

/* Cuenta errores de decisión de 'out' frente a 'sym' probando desfases de índice. */
static double ser_best(const cf32 *out, int nout, const cf32 *sym, int nsym, int skip, int qpsk)
{
    double best = 1.0;
    for (int lag = -6; lag <= 6; lag++) {
        long err = 0, tot = 0;
        for (int i = skip; i < nout - 10; i++) {
            int k = i + lag;
            if (k < 0 || k >= nsym) continue;
            int bad = (out[i].re > 0) != (sym[k].re > 0);
            if (qpsk)
                bad |= (out[i].im > 0) != (sym[k].im > 0);
            err += bad;
            tot++;
        }
        if (tot > 100 && (double)err / tot < best)
            best = (double)err / tot;
    }
    return best;
}

static void test_symsync(void)
{
    enum { NSYM = 4000 };
    static cf32 sym[NSYM], wave[NSYM * 5], out[NSYM * 2];
    static float wr[NSYM * 5], orr[NSYM * 2];
    double eps = 2e-3, ts = 0.25 * (1 + eps);       /* reloj del receptor 0.2 % lento */
    int ns = (int)((NSYM - 20) / ts);
    for (int k = 0; k < NSYM; k++) {
        sym[k].re = (rnd() & 1) ? 1.f : -1.f;
        sym[k].im = 0.f;
    }
    for (int ted = 0; ted < 2; ted++) {
        make_wave(sym, NSYM, 0.37 * 0.25, ts, ns, 0.08, wave);
        symsync_t *s = symsync_create((ted_t)ted, SLICE_BPSK, 4.0, 0.01, 0.02, 0.707);
        CHECK(s != NULL);
        int n = symsync_process(s, wave, ns, out, symsync_max_out(s, ns));
        double ser = ser_best(out, n, sym, NSYM, 400, 0);
        printf("symsync %s BPSK complejo: %d simbolos, SER %.4f, periodo %.5f (esperado %.5f)\n",
               ted ? "M&M" : "Gardner", n, ser, symsync_period(s), 4.0 / (1 + eps));
        CHECK(ser < 0.002);
        CHECK(fabs(symsync_period(s) - 4.0 / (1 + eps)) < 0.01);
        symsync_destroy(s);
        /* real, procesado en bloques irregulares */
        for (int i = 0; i < ns; i++) wr[i] = wave[i].re;
        s = symsync_create((ted_t)ted, SLICE_BPSK, 4.0, 0.01, 0.02, 0.707);
        int w = 0, pos = 0;
        while (pos < ns) {
            int blk = 1 + (int)(rnd() % 777);
            blk = MIN(ns - pos, blk);
            w += symsync_process_f(s, wr + pos, blk, orr + w, symsync_max_out(s, blk));
            pos += blk;
        }
        for (int i = 0; i < w; i++) { out[i].re = orr[i]; out[i].im = 0; }
        ser = ser_best(out, w, sym, NSYM, 400, 0);
        printf("symsync %s BPSK real en bloques: SER %.4f\n", ted ? "M&M" : "Gardner", ser);
        CHECK(ser < 0.002);
        symsync_destroy(s);
    }
    /* QPSK con Gardner y desfase de muestreo distinto */
    for (int k = 0; k < NSYM; k++) {
        sym[k].re = (rnd() & 1) ? 0.7071f : -0.7071f;
        sym[k].im = (rnd() & 1) ? 0.7071f : -0.7071f;
    }
    make_wave(sym, NSYM, 0.8, ts, ns, 0.06, wave);
    symsync_t *s = symsync_create(TED_GARDNER, SLICE_QPSK, 4.0, 0.01, 0.02, 0.707);
    int n = symsync_process(s, wave, ns, out, symsync_max_out(s, ns));
    double ser = ser_best(out, n, sym, NSYM, 400, 1);
    printf("symsync Gardner QPSK: SER %.4f\n", ser);
    CHECK(ser < 0.002);
    symsync_destroy(s);
    /* M&M QPSK */
    s = symsync_create(TED_MM, SLICE_QPSK, 4.0, 0.01, 0.02, 0.707);
    make_wave(sym, NSYM, 0.2, ts, ns, 0.06, wave);
    n = symsync_process(s, wave, ns, out, symsync_max_out(s, ns));
    ser = ser_best(out, n, sym, NSYM, 400, 1);
    printf("symsync M&M QPSK: SER %.4f\n", ser);
    CHECK(ser < 0.002);
    symsync_destroy(s);
    /* PAM4 real con M&M */
    static float lv[NSYM];
    for (int k = 0; k < NSYM; k++) {
        static const float L[4] = {-1.f, -1.f / 3, 1.f / 3, 1.f};
        lv[k] = L[rnd() & 3];
        sym[k].re = lv[k];
        sym[k].im = 0;
    }
    make_wave(sym, NSYM, 0.5, ts, ns, 0.01, wave);
    for (int i = 0; i < ns; i++) wr[i] = wave[i].re;
    s = symsync_create(TED_MM, SLICE_PAM4, 4.0, 0.01, 0.01, 0.707);
    n = symsync_process_f(s, wr, ns, orr, symsync_max_out(s, ns));
    long bad = 0, tot = 0;
    double best = 1;
    for (int lag = -6; lag <= 6; lag++) {
        bad = tot = 0;
        for (int i = 600; i < n - 10; i++) {
            int k = i + lag;
            if (k < 0 || k >= NSYM) continue;
            float v = orr[i], q = v < -2.f / 3 ? -1.f : v < 0 ? -1.f / 3 : v < 2.f / 3 ? 1.f / 3 : 1.f;
            bad += fabsf(q - lv[k]) > 0.01f;
            tot++;
        }
        if (tot && (double)bad / tot < best) best = (double)bad / tot;
    }
    printf("symsync M&M PAM4 real (4FSK): SER %.4f\n", best);
    CHECK(best < 0.01);
    symsync_destroy(s);
}

static void test_farrow(void)
{
    float x[4], maxerr = 0;
    double f = 0.05;
    for (int mu10 = 0; mu10 < 10; mu10++) {
        float mu = mu10 / 10.0f;
        for (int k = 0; k < 4; k++) x[k] = (float)sin(2 * M_PI * f * (k - 1) + 0.3);
        float y = farrow3_f(x, mu);
        maxerr = fmaxf(maxerr, fabsf(y - (float)sin(2 * M_PI * f * mu + 0.3)));
    }
    CHECK(maxerr < 2e-3);
    cf32 c[4];
    for (int k = 0; k < 4; k++) { c[k].re = (float)cos(2 * M_PI * f * (k - 1)); c[k].im = (float)sin(2 * M_PI * f * (k - 1)); }
    cf32 y = farrow3_c(c, 0.5f);
    CHECK(fabsf(y.re - (float)cos(2 * M_PI * f * 0.5)) < 2e-3 && fabsf(y.im - (float)sin(2 * M_PI * f * 0.5)) < 2e-3);
    CHECK(farrow3_f(x, 0.0f) == x[1]);
}

static void test_agc_fm(void)
{
    agc_t a;
    agc_init(&a, 0.5f, 0.01f, 0.005f);
    static cf32 in[20000], out[20000];
    for (int i = 0; i < 20000; i++) {
        float amp = i < 8000 ? 0.01f : 50.f;
        float ph = 0.3f * (float)i;
        in[i].re = amp * cosf(ph); in[i].im = amp * sinf(ph);
    }
    agc_process(&a, in, 20000, out);
    float m1 = sqrtf(out[7990].re * out[7990].re + out[7990].im * out[7990].im);
    float m2 = sqrtf(out[19990].re * out[19990].re + out[19990].im * out[19990].im);
    printf("AGC: |y| antes del salto %.3f, tras el salto %.3f (objetivo 0.5)\n", m1, m2);
    CHECK(fabsf(m1 - 0.5f) < 0.05f && fabsf(m2 - 0.5f) < 0.05f);
    static float fi[5000], fo[5000];
    agc_init(&a, 1.0f, 0.01f, 0.01f);
    for (int i = 0; i < 5000; i++) fi[i] = 0.02f * sinf(0.2f * (float)i);
    agc_process_f(&a, fi, 5000, fo);
    CHECK(fabsf(fo[4999]) <= 1.6f && a.gain > 20.f);

    /* FM: m(t) = sin, desviación 5 kHz, fs = 48 kHz */
    fmdisc_t d;
    fmdisc_init(&d, 48000.0, 5000.0);
    static cf32 fm[4800];
    static float dem[4800];
    double ph = 0;
    for (int i = 0; i < 4800; i++) {
        double m = sin(2 * M_PI * 440.0 * i / 48000.0);
        ph += 2 * M_PI * 5000.0 * m / 48000.0;
        fm[i].re = (float)cos(ph); fm[i].im = (float)sin(ph);
    }
    fmdisc_process(&d, fm, 4800, dem);
    float err = 0;
    for (int i = 10; i < 4800; i++)
        err = fmaxf(err, fabsf(dem[i] - (float)sin(2 * M_PI * 440.0 * i / 48000.0)));
    CHECK(err < 0.02f);
}

static void test_pll_costas(void)
{
    enum { N = 6000 };
    static cf32 x[N], y[N];
    /* PLL sobre un piloto */
    pll_t p;
    pll_init(&p, 0.02, 0.707);
    float w0 = 0.05f;
    for (int i = 0; i < N; i++) {
        x[i].re = cosf(w0 * (float)i + 1.0f) + 0.05f * gauss();
        x[i].im = sinf(w0 * (float)i + 1.0f) + 0.05f * gauss();
    }
    pll_track_carrier(&p, x, N, y);
    printf("PLL: freq %.5f (real %.5f), fase residual %.3f rad\n", p.freq, w0, atan2f(y[N - 1].im, y[N - 1].re));
    CHECK(fabsf(p.freq - w0) < 0.002f);
    CHECK(fabsf(atan2f(y[N - 1].im, y[N - 1].re)) < 0.2f);
    CHECK(fabs(pll_freq_hz(&p, 1e6) - w0 * 1e6 / (2 * M_PI)) < 400);

    /* Costas BPSK / QPSK / 8PSK */
    for (int ord = 2; ord <= 8; ord *= 2) {
        float w = 0.01f;   /* rad/muestra */
        for (int i = 0; i < N; i++) {
            int k = (int)(rnd() % (uint32_t)ord);
            float a = ord == 4 ? (float)M_PI / 4 + k * (float)M_PI / 2 : k * 2.0f * (float)M_PI / ord;
            float ph = a + w * (float)i + 0.7f;
            x[i].re = cosf(ph) + 0.06f * gauss();
            x[i].im = sinf(ph) + 0.06f * gauss();
        }
        costas_t c;
        CHECK(costas_init(&c, ord, ord == 8 ? 0.01 : 0.02, 0.707) == 0);
        costas_process(&c, x, N, y);
        int bad = 0;
        for (int i = 3000; i < N; i++) {
            float ang = atan2f(y[i].im, y[i].re);
            float step = ord == 4 ? (float)M_PI / 2 : 2.0f * (float)M_PI / ord;
            float base = ord == 4 ? (float)M_PI / 4 : 0.0f;
            float q = (ang - base) / step;
            float dev = fabsf(q - roundf(q)) * step;
            bad += dev > (ord == 8 ? 0.2f : 0.35f);
        }
        printf("Costas orden %d: freq %.5f (real 0.01), puntos fuera de rejilla %d/3000\n", ord, c.pll.freq, bad);
        CHECK(fabsf(c.pll.freq - w) < 0.002f);
        CHECK(bad < 60);
    }
    CHECK(costas_init(&(costas_t){0}, 3, 0.01, 0.7) == -1);
}

static void test_fll(void)
{
    enum { N = 40000, SPS = 4 };
    static cf32 sym[N / SPS + 20], wave[N], y[N];
    int nsym = N / SPS;
    for (int k = 0; k < nsym; k++) {
        sym[k].re = (rnd() & 1) ? 0.7071f : -0.7071f;
        sym[k].im = (rnd() & 1) ? 0.7071f : -0.7071f;
    }
    make_wave(sym, nsym, 0.0, 1.0 / SPS, N - 40, 0.03, wave);
    double off = 0.012;     /* ciclos/muestra: 4.8 % de la tasa de símbolo */
    cf32 *xo = malloc(sizeof(cf32) * N);
    for (int i = 0; i < N; i++) {
        float ph = (float)(2 * M_PI * off * i + 0.4);
        xo[i].re = wave[i].re * cosf(ph) - wave[i].im * sinf(ph);
        xo[i].im = wave[i].re * sinf(ph) + wave[i].im * cosf(ph);
    }
    fll_t *f = fll_create_bandedge(SPS, 0.35, 0, 0.0004);
    CHECK(f != NULL);
    fll_process(f, xo, N, y);
    double est = fll_freq_hz(f, 1.0);
    printf("FLL band-edge: estimado %.5f ciclos/muestra (real %.5f)\n", est, off);
    CHECK(fabs(est - off) < 0.003);   /* jitter de patrón y sesgo ~0.002 ciclos/muestra */
    fll_destroy(f);

    /* DISC con M=4 sobre QPSK a 1 muestra/símbolo y offset grande */
    for (int i = 0; i < N; i++) {
        float ph = (float)(2 * M_PI * 0.03 * i);
        cf32 s = sym[i % nsym];
        xo[i].re = s.re * cosf(ph) - s.im * sinf(ph) + 0.03f * gauss();
        xo[i].im = s.re * sinf(ph) + s.im * cosf(ph) + 0.03f * gauss();
    }
    f = fll_create_disc(4, 0.002);
    fll_process(f, xo, N, y);
    est = fll_freq_hz(f, 1.0);
    printf("FLL disc M=4: estimado %.5f (real 0.03)\n", est);
    CHECK(fabs(est - 0.03) < 0.003);
    fll_destroy(f);
    double blk = cfo_estimate_mpower(xo, N, 4, 1e6);
    printf("CFO por bloque (x^4): %.0f Hz (real 30000)\n", blk);
    CHECK(fabs(blk - 30000.0) < 300.0);
    /* portadora M=1 */
    for (int i = 0; i < 4000; i++) {
        xo[i].re = cosf(2 * (float)M_PI * 0.02f * (float)i); xo[i].im = sinf(2 * (float)M_PI * 0.02f * (float)i);
    }
    f = fll_create_disc(1, 0.01);
    fll_process(f, xo, 4000, y);
    CHECK(fabs(fll_freq_hz(f, 1.0) - 0.02) < 0.001);
    fll_destroy(f);
    free(xo);
}

static void test_corr(void)
{
    enum { L = 63, N = 6000 };
    float ref[L];
    for (int i = 0; i < L; i++) ref[i] = (rnd() & 1) ? 1.f : -1.f;
    static cf32 x[N];
    for (int i = 0; i < N; i++) { x[i].re = 0.25f * gauss(); x[i].im = 0.25f * gauss(); }
    corr_t *c = corr_create_f(ref, L, 0.4f, L);
    CHECK(c != NULL);
    corr_peak_t pk[8];
    CHECK(corr_process(c, x, N, pk, 8) == 0);   /* solo ruido: sin falsas alarmas */
    corr_reset(c);
    /* inserta la palabra en 1234..1296, girada 60° y con amplitud 0.5 */
    float ca = cosf((float)M_PI / 3), sa = sinf((float)M_PI / 3);
    for (int i = 0; i < L; i++) {
        x[1234 + i].re += 0.5f * ref[i] * ca;
        x[1234 + i].im += 0.5f * ref[i] * sa;
        x[4000 + i].re += 0.5f * ref[i];     /* segunda aparición, sin giro */
    }
    int found = 0;
    corr_peak_t all[8];
    for (int pos = 0; pos < N;) {                 /* en bloques de tamaño irregular */
        int blk = 1 + (int)(rnd() % 300);
        blk = MIN(N - pos, blk);
        found += corr_process(c, x + pos, blk, all + found, 8 - found);
        pos += blk;
    }
    printf("Correlador: %d picos; pos %lld (%lld) metrica %.2f, fase %.2f rad\n", found, found > 0 ? (long long)all[0].pos : -1LL,
           1234LL + L - 1, found > 0 ? all[0].metric : 0, found > 0 ? atan2f(all[0].corr.im, all[0].corr.re) : 0);
    CHECK(found == 2);
    if (found == 2) {
        CHECK(all[0].pos == 1234 + L - 1 && all[1].pos == 4000 + L - 1);
        CHECK(all[0].metric > 0.4f && all[0].metric <= 1.0f);
        CHECK(fabsf(atan2f(all[0].corr.im, all[0].corr.re) - (float)M_PI / 3) < 0.25f);
    }
    corr_destroy(c);
    CHECK(corr_create_f(ref, 1, 0.5f, 1) == NULL);
}

int main(void)
{
    test_farrow();
    test_agc_fm();
    test_pll_costas();
    test_fll();
    test_corr();
    test_symsync();
    printf("test_sync: %d comprobaciones, %d fallos\n", g_n, g_fail);
    return g_fail != 0;
}
