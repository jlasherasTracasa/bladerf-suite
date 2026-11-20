/*
 * test_dvbt_rx.c — Receptor DVB-T completo (src/tv/dvbt_rx.c), AGENTS.md §7.
 *
 * Extremo a extremo: TS conocido → modulador (mod_dvbt.c) → canal (AWGN, CFO fraccional y
 * entero, deriva de reloj, eco) → receptor → TS idéntico. La cadena de codificación del
 * modulador se contrastó aparte, bit a bit, con GNU Radio gr-dtv (dtv.dvbt_*; solo
 * ejecutado como referencia, ver docs/protocolos/dvbt.md): 9 combinaciones de modo,
 * constelación y tasa, y el símbolo OFDM completo (pilotos, TPS con BCH y cell id).
 * Además: Viterbi rápido contra el codificador, y rendimiento (núcleos por señal en tiempo real).
 */
#include "test_util.h"

#include "tv/dvbt_rx.h"
#include "tv/dvbt_tab.h"
#include "decoders/dvbt_phy.h"
#include "tv/dvbt_vit.h"
#include "tx/mod_dvbt.h"

#include <time.h>

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#define NPK 4000
static uint8_t g_src[NPK * 188];
static long g_total, g_good, g_tei;

static void sink(void *ctx, const uint8_t *p, int n)
{
    (void)ctx;
    for (int i = 0; i < n; i++) {
        const uint8_t *q = p + i * 188;
        g_total++;
        if (q[1] & 0x80) {
            g_tei++;
            continue;
        }
        int idx = q[4] | (q[5] << 8);
        if (q[0] == 0x47 && idx < NPK && !memcmp(q, g_src + (size_t)idx * 188, 188))
            g_good++;
    }
}

static void run_case(const char *name, int n, int gidx, int cons, int rate, double snr, double cfo, double ppm,
                     int frames, bool echo, long min_good, double *cpu_per_s)
{
    dvbt_mod_cfg_t c;
    dvbt_mod_defaults(&c);
    c.n = n;
    c.guard = gidx;
    c.constellation = cons;
    c.hp = c.lp = rate;
    c.frames = frames;
    iqbuf_t o = {0};
    char err[200] = "";
    CHECK(dvbt_mod_run(&c, g_src, NPK, NULL, 0, &o, err, sizeof err) == 0);
    int m = o.n;
    if (echo) {
        cf32 *y = malloc(sizeof(cf32) * (size_t)m);
        memcpy(y, o.s, sizeof(cf32) * (size_t)m);
        for (int i = 800; i < m; i++) {
            o.s[i].re += 0.4f * y[i - 800].re;
            o.s[i].im += 0.4f * y[i - 800].im;
        }
        free(y);
    }
    cf32 *z = malloc(sizeof(cf32) * (size_t)m);
    int m2 = m;
    if (ppm != 0) {
        m2 = (int)(m / (1 + ppm * 1e-6)) - 4;
        chan_drift(o.s, m, ppm, z);
    } else
        memcpy(z, o.s, sizeof(cf32) * (size_t)m);
    chan_cfo(z, m2, DVBT_FS, cfo);
    uint32_t seed = 99;
    /* ruido con SNR respecto a la potencia MEDIA (chan_awgn usa solo las muestras altas: OFDM tiene PAPR alta) */
    double pw = 0;
    for (int i = 0; i < m2; i++)
        pw += (double)z[i].re * z[i].re + (double)z[i].im * z[i].im;
    double sg = sqrt(pw / m2 / pow(10.0, snr / 10.0) / 2.0);
    for (int i = 0; i < m2; i++) {
        z[i].re += (float)(sg * tu_gauss(&seed));
        z[i].im += (float)(sg * tu_gauss(&seed));
    }
    char e2[100];
    dvbt_rx_t *r = dvbt_rx_create(DVBT_FS, e2, sizeof e2);
    CHECK(r != NULL);
    g_total = g_good = g_tei = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < m2; i += 65536)
        dvbt_rx_process(r, z + i, MIN(65536, m2 - i), sink, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    dvbt_rx_stats_t st;
    dvbt_rx_get_stats(r, &st);
    printf("  [%s] TS %ld correctos de %ld (TEI %ld), lock=%d MER %.1f dB, %.2f núcleos\n", name, g_good, g_total, g_tei,
           st.lock, st.mer_db, dt / ((double)m2 / DVBT_FS));
    CHECK(g_good >= min_good);
    CHECK(g_total == g_good + g_tei);
    CHECK(st.mode_k == (n == 8192 ? 8 : 2));
    CHECK(st.guard_den == dvbt_guard_div(gidx));
    if (cpu_per_s)
        *cpu_per_s = dt / ((double)m2 / DVBT_FS);
    dvbt_rx_destroy(r);
    free(z);
    iqbuf_free(&o);
}

static void test_viterbi(void)
{
    const int nb = 200000;
    uint8_t *in = malloc((size_t)nb), *cod = malloc((size_t)nb * 2);
    int8_t *soft = calloc((size_t)nb * 2 + 64, 1);
    uint8_t *out = malloc((size_t)nb / 8 + 64);
    for (int rate = 0; rate < 5; rate++) {
        for (int i = 0; i < nb; i++)
            in[i] = (uint8_t)(rnd() & 1);
        uint32_t st = 0;
        dvbt_conv_encode(&st, in, nb / 7 * 7 / 7 * 7, rate, cod);
        const dvbt_punct_t *pu = dvbt_punct(rate);
        int nin = nb / 7 * 7, ci = 0;
        memset(soft, 0, (size_t)nin * 2);
        for (int i = 0; i < nin; i++) {
            int ph = i % pu->period;
            if (pu->x[ph])
                soft[2 * i] = cod[ci++] ? -40 : 40;
            if (pu->y[ph])
                soft[2 * i + 1] = cod[ci++] ? -40 : 40;
        }
        dvbt_vit_t *v = dvbt_vit_create();
        int nout = dvbt_vit_push(v, soft, nin, out);
        int err = 0;
        for (int i = 0; i < nout * 8; i++)
            err += ((out[i / 8] >> (7 - i % 8)) & 1) != in[i];
        CHECK(nout * 8 > nin - 400);
        CHECK(err == 0);
        dvbt_vit_destroy(v);
    }
    free(in); free(cod); free(soft); free(out);
}

int main(void)
{
    for (int i = 0; i < NPK; i++) {
        uint8_t *q = g_src + (size_t)i * 188;
        q[0] = 0x47; q[1] = 0x01; q[2] = 0; q[3] = (uint8_t)(0x10 | (i & 15)); q[4] = (uint8_t)i; q[5] = (uint8_t)(i >> 8);
        for (int j = 6; j < 188; j++)
            q[j] = (uint8_t)rnd();
    }
    test_viterbi();
    run_case("8k 1/4 64-QAM 2/3 ideal", 8192, 3, 2, 1, 40, 0, 0, 8, false, 3500, NULL);
    run_case("8k 1/8 16-QAM 3/4 CFO 3300 Hz", 8192, 2, 1, 2, 25, 3300, 0, 8, false, 2500, NULL);
    run_case("8k 1/32 QPSK 1/2 15 dB", 8192, 0, 0, 0, 15, 0, 0, 16, false, 2500, NULL);
    run_case("8k 1/4 64-QAM 7/8 eco 800 muestras 28 dB", 8192, 3, 2, 4, 28, 500, 0, 8, true, 4000, NULL);
    run_case("2k 1/4 64-QAM 2/3", 2048, 3, 2, 1, 30, 0, 0, 40, false, 8000, NULL);
    run_case("2k 1/16 16-QAM 3/4 CFO 100 Hz 10 ppm", 2048, 1, 1, 2, 20, 100, 10, 40, false, 6000, NULL);
    double cpu = 0;
    run_case("rendimiento 8k 1/4 64-QAM 2/3", 8192, 3, 2, 1, 35, 0, 0, 8, false, 3500, &cpu);
    CHECK(cpu < 1.5);
    return TEST_SUMMARY();
}
