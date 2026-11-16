/*
 * nr_phy.c — Secuencias PSS/SSS de NR y rejilla GSCN. Ver nr_phy.h.
 */
#include "nr_phy.h"

#include <string.h>

void nr_pss_seq(int n_id2, int8_t d[127])
{
    uint8_t x[127 + 7];
    static const uint8_t init[7] = {0, 1, 1, 0, 1, 1, 1};   /* x(0)..x(6) */
    memcpy(x, init, 7);
    for (int i = 0; i < 127; i++)
        x[i + 7] = x[i + 4] ^ x[i];
    for (int n = 0; n < 127; n++)
        d[n] = (int8_t)(1 - 2 * x[(n + 43 * n_id2) % 127]);
}

void nr_sss_seq(int n_id1, int n_id2, int8_t d[127])
{
    uint8_t x0[127 + 7], x1[127 + 7];
    memset(x0, 0, sizeof x0);
    memset(x1, 0, sizeof x1);
    x0[0] = 1;
    x1[0] = 1;
    for (int i = 0; i < 127; i++) {
        x0[i + 7] = x0[i + 4] ^ x0[i];
        x1[i + 7] = x1[i + 1] ^ x1[i];
    }
    int m0 = 15 * (n_id1 / 112) + 5 * n_id2, m1 = n_id1 % 112;
    for (int n = 0; n < 127; n++)
        d[n] = (int8_t)((1 - 2 * x0[(n + m0) % 127]) * (1 - 2 * x1[(n + m1) % 127]));
}

/* TS 38.104 tabla 5.4.3.1-1:
 *   0–3000 MHz:        f = N·1200 kHz + M·50 kHz,   M ∈ {1,3,5}, N = 1..2499
 *   3000–24250 MHz:    f = 3000 MHz + N·1,44 MHz,   N = 0..14756
 *   24250–100000 MHz:  f = 24250,08 MHz + N·17,28 MHz, N = 0..4383 */
int nr_gscn_candidates(double lo_hz, double hi_hz, int64_t *out, int max)
{
    int n = 0;
    for (int N = 1; N <= 2499 && n < max; N++)
        for (int M = 1; M <= 5 && n < max; M += 2) {
            int64_t f = (int64_t)N * 1200000 + (int64_t)M * 50000;
            if (f >= lo_hz && f <= hi_hz)
                out[n++] = f;
        }
    int64_t n0 = (int64_t)floor((lo_hz - 3000e6) / 1.44e6);
    if (n0 < 0)
        n0 = 0;
    for (int64_t N = n0; N <= 14756 && n < max; N++) {
        int64_t f = 3000000000LL + N * 1440000;
        if (f > hi_hz)
            break;
        if (f >= lo_hz)
            out[n++] = f;
    }
    for (int64_t N = 0; N <= 4383 && n < max; N++) {
        int64_t f = 24250080000LL + N * 17280000;
        if (f > hi_hz)
            break;
        if (f >= lo_hz)
            out[n++] = f;
    }
    return n;
}
