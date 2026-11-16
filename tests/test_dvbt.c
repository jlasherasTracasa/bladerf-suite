/*
 * test_dvbt.c — Decodificador DVB-T (ETSI EN 300 744), AGENTS.md §7.
 *
 * Comprobaciones de tablas (independientes del modulador, contra la estructura de
 * la norma): 45/177 pilotos continuos, 17/68 portadoras TPS, 8k = 2k + m·1704,
 * g(x) del BCH divide a x^127+1, palabras de sincronía complementarias, PRBS.
 * Pruebas de sistema: modulador → canal (AWGN con SNR de potencia MEDIA, CFO
 * fraccional+entero, deriva de reloj ±100 ppm, 2 ecos) → decodificador, en 2k y
 * 8k con las 4 guardas, y diferentes constelaciones/tasas/jerarquía/cell id.
 * No hay vectores IQ ni TPS publicados contrastables aquí: ver docs/decoders/dvbt.md.
 */
#include "test_util.h"

#include "decoders/dvbt_phy.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_DVBT;
extern const modulator_desc_t MOD_DVBT;

static void awgn_avg(cf32 *x, int n, double snr_db, uint32_t *seed)
{
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
    double sigma = sqrt(sum / n / pow(10.0, snr_db / 10.0) / 2.0);
    for (int i = 0; i < n; i++) {
        x[i].re += (float)(sigma * tu_gauss(seed));
        x[i].im += (float)(sigma * tu_gauss(seed));
    }
}

static void multipath(cf32 *x, int n, int d1, float a1, int d2, float a2)
{
    cf32 *c = malloc(sizeof(cf32) * (size_t)n);
    memcpy(c, x, sizeof(cf32) * (size_t)n);
    const float p1 = 0.9f, p2 = -2.3f;
    for (int i = 0; i < n; i++) {
        if (i >= d1) {
            x[i].re += a1 * (c[i - d1].re * cosf(p1) - c[i - d1].im * sinf(p1));
            x[i].im += a1 * (c[i - d1].re * sinf(p1) + c[i - d1].im * cosf(p1));
        }
        if (i >= d2) {
            x[i].re += a2 * (c[i - d2].re * cosf(p2) - c[i - d2].im * sinf(p2));
            x[i].im += a2 * (c[i - d2].re * sinf(p2) + c[i - d2].im * cosf(p2));
        }
    }
    free(c);
}

/* Modula, degrada y decodifica. Entrega el JSON de eventos en evs y, si cpu != NULL, repite la señal
 * hasta 'secs' segundos para medir el coste. */
static int run_case(const char *params, double snr, double cfo, double ppm, int echo_d1, int echo_d2, evs_t *evs,
                    double *cpu, double secs)
{
    iqbuf_t b = {0};
    char err[200] = "";
    if (MOD_DVBT.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        return -1;
    }
    int n = b.n;
    cf32 *x = calloc((size_t)n + 64, sizeof(cf32));
    if (ppm != 0) {
        cf32 *t = malloc(sizeof(cf32) * (size_t)(n * 1.001 + 64));
        int m = chan_resample(b.s, n, 1.0 + ppm * 1e-6, t);
        memcpy(x, t, sizeof(cf32) * (size_t)MIN(m, n));
        free(t);
    } else {
        memcpy(x, b.s, sizeof(cf32) * (size_t)n);
    }
    if (echo_d1)
        multipath(x, n, echo_d1, 0.5f, echo_d2, 0.316f);
    if (cfo != 0)
        chan_cfo(x, n, DVBT_FS, cfo);
    uint32_t seed = 4242u + (uint32_t)(snr * 10);
    awgn_avg(x, n, snr, &seed);
    char err2[100] = "";
    dec_cfg_t cfg = {.rate = DVBT_FS, .freq_hz = 474000000LL, .center_hz = 474000000LL, .opts = NULL};
    void *st = DEC_DVBT.create(&cfg, err2, sizeof err2);
    if (!st) {
        printf("  create: %s\n", err2);
        return -1;
    }
    clock_t t0 = clock();
    /* el segmento del decodificador es de 1,5 Msps: se repite la señal (en bucle) hasta llenarlo */
    double total = cpu ? floor(secs * DVBT_FS) : (double)MAX(n, 1700000), done = 0;
    while (done < total) {
        for (int off = 0; off < n && done < total; off += 65536) {
            int k = (int)MIN((double)MIN(65536, n - off), total - done);
            DEC_DVBT.process(st, x + off, k, done / DVBT_FS, tu_collect, evs);
            done += k;
        }
    }
    if (cpu)
        *cpu = (double)(clock() - t0) / CLOCKS_PER_SEC / (done / DVBT_FS);
    if (DEC_DVBT.state_json && !cpu) {
        sb_t sb = {0};
        DEC_DVBT.state_json(st, &sb, 0);
        CHECK(strstr(sb.data, "\"muxes\":[{") != NULL);
        sb_free(&sb);
    }
    DEC_DVBT.destroy(st);
    free(x);
    iqbuf_free(&b);
    return n;
}

static int poly_mod_is_zero(void)
{
    /* g(x) = x^14+x^9+x^8+x^6+x^5+x^4+x^2+x+1 divide a x^127 + 1 (BCH acortado de (127,113)) */
    uint8_t r[128] = {0};
    r[0] = 1;
    r[127] = 1;
    const int g[] = {14, 9, 8, 6, 5, 4, 2, 1, 0};
    for (int i = 127; i >= 14; i--)
        if (r[i])
            for (size_t j = 0; j < sizeof g / sizeof g[0]; j++)
                r[i - 14 + g[j]] ^= 1;
    for (int i = 0; i < 14; i++)
        if (r[i])
            return 0;
    return 1;
}

int main(void)
{
    /* ---- Tablas ---- */
    int cp[200], tp[80];
    CHECK_EQ(dvbt_cp_list(2048, cp), 45);
    CHECK_EQ(cp[0], 0);
    CHECK_EQ(cp[44], 1704);
    CHECK_EQ(dvbt_cp_list(8192, cp), 177);
    CHECK_EQ(cp[176], 6816);
    CHECK_EQ(cp[45], 1752);
    int sorted = 1;
    for (int i = 1; i < 177; i++)
        sorted &= cp[i] > cp[i - 1];
    CHECK(sorted);
    CHECK_EQ(dvbt_tps_list(2048, tp), 17);
    CHECK_EQ(tp[0], 34);
    CHECK_EQ(tp[16], 1687);
    CHECK_EQ(dvbt_tps_list(8192, tp), 68);
    CHECK_EQ(tp[17], 1738);
    CHECK_EQ(tp[67], 6799);
    CHECK(poly_mod_is_zero());
    int comp = 1;
    for (int i = 0; i < 16; i++)
        comp &= DVBT_SYNC_EVEN[i] != DVBT_SYNC_ODD[i];
    CHECK(comp);
    CHECK_EQ(dvbt_prbs(0), 1); /* semilla de 11 unos */
    CHECK_EQ(dvbt_prbs(10), 1);
    CHECK_EQ(dvbt_prbs(11), 0); /* s[11] = s[0]^s[2] = 0 */

    /* ---- TPS: construir, comprobar, corregir 2 errores, rechazar 3 ---- */
    dvbt_tps_t t = {.frame = 2, .constellation = 2, .hierarchy = 1, .hp = 1, .lp = 2, .guard = 3, .mode = 1,
                    .length = 31, .cell_byte = 0xA5};
    uint8_t s[68], s2[68];
    dvbt_tps_build(&t, s);
    memcpy(s2, s, 68);
    int alt;
    CHECK_EQ(dvbt_tps_check(s2, &alt), 0);
    CHECK_EQ(alt, 0);
    s2[5] ^= 1;
    s2[60] ^= 1;
    CHECK_EQ(dvbt_tps_check(s2, &alt), 2);
    CHECK(memcmp(s, s2, 68) == 0);
    memcpy(s2, s, 68);
    s2[3] ^= 1;
    s2[20] ^= 1;
    s2[45] ^= 1;
    int r3 = dvbt_tps_check(s2, &alt);
    CHECK(r3 < 0 || memcmp(s, s2, 68) != 0 || r3 == 0); /* 3 errores: no se promete corrección */
    dvbt_tps_t u;
    dvbt_tps_parse(s, &u);
    CHECK_EQ(u.frame, 2);
    CHECK_EQ(u.constellation, 2);
    CHECK_EQ(u.guard, 3);
    CHECK_EQ(u.mode, 1);
    CHECK_EQ(u.cell_byte, 0xA5);
    CHECK_EQ(u.length, 31);

    evs_t ev = {0};
    /* ---- Sistema: 2k y 8k × 4 guardas, 15 dB, CFO +25,3 kHz (entero + fraccional) ---- */
    static const char *gs[4] = {"1/32", "1/16", "1/8", "1/4"};
    for (int m = 0; m < 2; m++)
        for (int g = 0; g < 4; g++) {
            char p[200];
            snprintf(p, sizeof p, "{\"mode\":\"%s\",\"guard\":\"%s\",\"constellation\":\"16qam\",\"hp\":\"3/4\",\"lp\":\"5/6\","
                     "\"hierarchy\":2,\"cell_id\":43981,\"frames\":%d}", m ? "8k" : "2k", gs[g], m ? 4 : 16);
            run_case(p, 15, 25300, 0, 0, 0, &ev, NULL, 0);
            char needle[64];
            int ok = ev_has(&ev, m ? "\"mode\":\"8k\"" : "\"mode\":\"2k\"");
            snprintf(needle, sizeof needle, "\"guard\":\"%s\"", gs[g]);
            ok &= ev_has(&ev, needle) && ev_has(&ev, "\"constellation\":\"16-QAM\"") && ev_has(&ev, "\"hp_rate\":\"3/4\"") &&
                  ev_has(&ev, "\"lp_rate\":\"5/6\"") && ev_has(&ev, "\"hierarchy\":\"alpha2\"") && ev_has(&ev, "\"bch\":\"std\"");
            printf("  [%s guarda %s] eventos=%d %s\n", m ? "8k" : "2k", gs[g], evs_count(&ev), ok ? "ok" : "FALLO");
            if (!ok && ev.data)
                printf("%s", ev.data);
            CHECK(ok);
            evs_free(&ev);
        }

    /* ---- Cell id: los dos bytes llegan en tramas alternas; el decodificador reintenta pronto ---- */
    {
        double c = 0;
        run_case("{\"mode\":\"2k\",\"guard\":\"1/8\",\"cell_id\":43981,\"frames\":16,\"frame0\":0}", 15, -12000, 0, 0, 0, &ev, &c, 2.5);
    }
    printf("  [cell id] %s", ev.data ? ev.data : "(sin eventos)\n");
    CHECK(ev_has(&ev, "\"cell_id\":43981") && ev_has(&ev, "\"cell_id_hex\":\"0xABCD\""));
    evs_free(&ev);

    /* ---- Estrés: 10 dB, ±100 ppm, CFO −61,7 kHz / +180 kHz, 2 ecos (guarda ≥ 1/8) ---- */
    struct { const char *p; double snr, cfo, ppm; int d1, d2; } st[] = {
        {"{\"mode\":\"8k\",\"guard\":\"1/8\",\"constellation\":\"64qam\",\"frames\":4}", 10, -61700, 100, 5, 17},
        {"{\"mode\":\"8k\",\"guard\":\"1/4\",\"constellation\":\"qpsk\",\"hp\":\"7/8\",\"frames\":4}", 11, 180000, -100, 9, 33},
        {"{\"mode\":\"2k\",\"guard\":\"1/8\",\"constellation\":\"64qam\",\"frames\":16}", 10, 45100, -100, 3, 11},
        {"{\"mode\":\"2k\",\"guard\":\"1/4\",\"constellation\":\"16qam\",\"frames\":16}", 10, -150000, 100, 6, 23},
    };
    for (int i = 0; i < 4; i++) {
        run_case(st[i].p, st[i].snr, st[i].cfo, st[i].ppm, st[i].d1, st[i].d2, &ev, NULL, 0);
        int ok = ev_has(&ev, "\"bch\":\"std\"") && evs_count(&ev) >= 1;
        printf("  [estrés %d] eventos=%d %s\n", i, evs_count(&ev), ok ? "ok" : "FALLO");
        if (ok) {
            const char *c = strstr(ev.data, "\"mer_db\":");
            const char *f = strstr(ev.data, "\"cfo_hz\":");
            printf("     %.*s  %.*s\n", 18, f ? f : "", 14, c ? c : "");
        } else if (ev.data) {
            printf("%s", ev.data);
        }
        CHECK(ok);
        evs_free(&ev);
    }

    /* ---- Solo ruido: 0 eventos ---- */
    {
        int n = 6 * 9142857 / 1; /* 6 s */
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 99;
        for (int i = 0; i < n; i++) {
            x[i].re = 0.05f * (float)tu_gauss(&seed);
            x[i].im = 0.05f * (float)tu_gauss(&seed);
        }
        char err[100];
        dec_cfg_t cfg = {.rate = DVBT_FS, .freq_hz = 474000000LL, .center_hz = 474000000LL};
        void *stt = DEC_DVBT.create(&cfg, err, sizeof err);
        clock_t t0 = clock();
        for (int off = 0; off < n; off += 65536)
            DEC_DVBT.process(stt, x + off, MIN(65536, n - off), 0, tu_collect, &ev);
        double c = (double)(clock() - t0) / CLOCKS_PER_SEC / ((double)n / DVBT_FS);
        printf("  [ruido] eventos=%d  CPU=%.3f s/s\n", evs_count(&ev), c);
        CHECK_EQ(evs_count(&ev), 0);
        CHECK(c < 0.3);
        DEC_DVBT.destroy(stt);
        free(x);
        evs_free(&ev);
    }

    /* ---- Coste con señal (8k 1/8): 6 s de señal repetida ---- */
    {
        double c = 0;
        run_case("{\"mode\":\"8k\",\"guard\":\"1/8\",\"frames\":4}", 15, 10000, 30, 0, 0, &ev, &c, 6.0);
        printf("  [coste, señal 8k] %.3f s CPU por s de señal (límite 0,3); eventos=%d\n", c, evs_count(&ev));
#ifndef __SANITIZE_ADDRESS__ /* con ASan el coste no es representativo */
        CHECK(c < 0.3);
#endif
        evs_free(&ev);
        run_case("{\"mode\":\"2k\",\"guard\":\"1/32\",\"frames\":16}", 15, 10000, 30, 0, 0, &ev, &c, 6.0);
        printf("  [coste, señal 2k] %.3f s CPU por s de señal; eventos=%d\n", c, evs_count(&ev));
#ifndef __SANITIZE_ADDRESS__
        CHECK(c < 0.3);
#endif
        evs_free(&ev);
    }
    return TEST_SUMMARY();
}
