/*
 * test_fec.c — Pruebas de src/dsp/fec.*
 *
 * Fuentes de los parámetros de Reed-Solomon (consultadas el 2026-11-15):
 *   - CCSDS 131.0-B (TM Synchronization and Channel Coding), RS(255,223) E=16:
 *     campo x^8+x^7+x^2+x+1 (0x187), raíces α^{11·j}, j = 112..143 (fcr 112, prim 11).
 *   - VDL modo 2: dumpvdl2 (src/rs.c) usa init_rs_char(8, 0x187, 120, 1, N−K, 0)
 *     con RS(255,249) acortado (ICAO Anexo 10 vol. III, ARINC 631).
 *   - Vaisala RS41: rs41ecc.c de zilog80/rs1729 (github.com/rs1729/RS): RS(255,231),
 *     t = 12, 2 palabras entrelazadas por trama. El polinomio 0x11D y fcr 0 salen
 *     de una línea comentada de ese fichero y no están confirmados; se validará
 *     con tramas reales (ver informe).
 *   - DVB RS(204,188) (EN 300 744) y DAB RS(120,110) (EN 300 401): 0x11D, g = Π(x−α^i), i = 0..2t−1.
 * BCH(31,21) de POCSAG: g(x) = x^10+x^9+x^8+x^6+x^5+x^3+1 (ITU-R M.584-2).
 * Golay: Golay (1949); g(x) = x^11+x^10+x^6+x^5+x^4+x^2+1.
 */
#include "dsp/bits.h"
#include "dsp/fec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t rs_state = 0x123456789ABCDEFULL;
static uint32_t rnd(void)
{
    rs_state ^= rs_state << 13; rs_state ^= rs_state >> 7; rs_state ^= rs_state << 17;
    return (uint32_t)(rs_state >> 16);
}
static double urand(void) { return (rnd() + 1.0) / 4294967297.0; }
static float gauss(void) { return (float)(sqrt(-2.0 * log(urand())) * cos(2 * M_PI * urand())); }

/* ------------------------------ Viterbi --------------------------------- */
static double sim_conv(const conv_code_t *c, const conv_punct_t *p, double rate, double ebn0_db,
                       conv_term_t term, int soft, int nfr, int nbits)
{
    viterbi_t *v = viterbi_create(c, nbits);
    CHECK(v != NULL);
    int cap = (nbits + c->k) * c->n;
    uint8_t *in = malloc((size_t)nbits), *cw = malloc((size_t)cap), *out = malloc((size_t)nbits);
    float *rx = malloc((size_t)cap * sizeof(float));
    int8_t *q = malloc((size_t)cap);
    double sigma = sqrt(1.0 / (2.0 * rate * pow(10.0, ebn0_db / 10.0)));
    long errs = 0, tot = 0;
    for (int f = 0; f < nfr; f++) {
        for (int i = 0; i < nbits; i++)
            in[i] = (uint8_t)(rnd() & 1);
        int nc = conv_encode(c, in, nbits, cw, term, p);
        for (int i = 0; i < nc; i++) {
            float s = (cw[i] ? 1.f : -1.f) + (float)sigma * gauss();
            rx[i] = soft ? s : (s > 0 ? 1.f : -1.f);
            q[i] = (int8_t)CLAMP(rx[i] * 40.f, -127.f, 127.f);
        }
        int r = soft == 2 ? viterbi_decode_i8(v, q, nc, nbits, term, p, out)
                          : viterbi_decode_f(v, rx, nc, nbits, term, p, out);
        CHECK(r >= 0);
        for (int i = 0; i < nbits; i++)
            errs += out[i] != in[i];
        tot += nbits;
    }
    free(in); free(cw); free(out); free(rx); free(q);
    viterbi_destroy(v);
    return (double)errs / (double)tot;
}

static void test_viterbi(void)
{
    conv_code_t codes[] = {conv_nasa_k7(), conv_ccsds_k7(), conv_gsm_k5(), conv_dab_k7(),
                           conv_lte_k7_r3(), conv_k9_r2(), conv_k9_r3()};
    /* 1. sin ruido: ida y vuelta con los tres modos de terminación */
    for (size_t ci = 0; ci < ARRAY_LEN(codes); ci++)
        for (int term = 0; term < 3; term++) {
            double b = sim_conv(&codes[ci], NULL, 1.0 / codes[ci].n, 40.0, (conv_term_t)term, 1, 3, 120);
            CHECK(b == 0.0);
        }
    /* punzonados DVB sin ruido */
    const char *rates[] = {"2/3", "3/4", "5/6", "7/8"};
    for (int i = 0; i < 4; i++) {
        conv_code_t c = conv_nasa_k7();
        const conv_punct_t *p = conv_punct_dvb(rates[i]);
        CHECK(sim_conv(&c, p, 0.7, 40.0, CONV_TERM_ZERO, 2, 2, 700) == 0.0);
    }
    /* 2. BER con y sin código, BPSK+AWGN, Eb/N0 = 4 dB, K=7 r=1/2.
     * Sin codificar: Q(sqrt(2·Eb/N0)) = 1.25e-2. */
    conv_code_t c = conv_nasa_k7();
    double eb = 2.5, unc = 0.5 * erfc(sqrt(pow(10.0, eb / 10.0)));
    double ber_soft = sim_conv(&c, NULL, 0.5, eb, CONV_TERM_ZERO, 1, 100, 1000);
    double ber_i8 = sim_conv(&c, NULL, 0.5, eb, CONV_TERM_ZERO, 2, 100, 1000);
    double ber_hard = sim_conv(&c, NULL, 0.5, eb, CONV_TERM_ZERO, 0, 100, 1000);
    printf("Viterbi K=7 r1/2 a Eb/N0=%.1f dB: sin codigo %.2e | duro %.2e | blando float %.2e | blando int8 %.2e\n",
           eb, unc, ber_hard, ber_soft, ber_i8);
    CHECK(ber_soft < unc / 20.0);
    CHECK(ber_i8 < unc / 15.0);
    CHECK(ber_hard > ber_soft);
    /* tail-biting con ruido */
    conv_code_t lte = conv_lte_k7_r3();
    double ber_tb = sim_conv(&lte, NULL, 1.0 / 3, 3.0, CONV_TAILBITING, 1, 100, 200);
    printf("Viterbi tail-biting K=7 r1/3 a 3 dB: BER %.2e (sin codigo %.2e)\n", ber_tb, 0.5 * erfc(sqrt(pow(10.0, 0.3))));
    CHECK(ber_tb < 2e-3);
    /* punzonado 3/4 a 6 dB */
    double ber_p = sim_conv(&c, conv_punct_dvb("3/4"), 0.75, 6.0, CONV_TERM_ZERO, 1, 50, 1000);
    printf("Viterbi 3/4 a 6 dB: BER %.2e (sin codigo %.2e)\n", ber_p, 0.5 * erfc(sqrt(pow(10.0, 0.6))));
    CHECK(ber_p < 0.5 * erfc(sqrt(pow(10.0, 0.6))) / 10);
    /* errores de canal reportados */
    viterbi_t *v = viterbi_create(&c, 100);
    uint8_t in[100], cw[300], out[100];
    int8_t soft[300];
    for (int i = 0; i < 100; i++)
        in[i] = (uint8_t)(rnd() & 1);
    int nc = conv_encode(&c, in, 100, cw, CONV_TERM_ZERO, NULL);
    CHECK(nc == 212);
    for (int i = 0; i < nc; i++)
        soft[i] = cw[i] ? 100 : -100;
    soft[10] = (int8_t)-soft[10];
    soft[50] = (int8_t)-soft[50];
    CHECK(viterbi_decode_i8(v, soft, nc, 100, CONV_TERM_ZERO, NULL, out) == 2);
    CHECK(!memcmp(in, out, 100));
    CHECK(viterbi_decode_i8(v, soft, nc - 5, 100, CONV_TERM_ZERO, NULL, out) == -1);   /* faltan símbolos */
    CHECK(viterbi_decode_i8(v, soft, nc, 101, CONV_TERM_ZERO, NULL, out) == -1);       /* supera max_info_bits */
    viterbi_destroy(v);
}

/* --------------------------- Reed-Solomon ------------------------------- */
static void rs_trial(const rs_t *rs, int nblock, const char *name)
{
    int nr = rs->nroots, t = nr / 2, nd = nblock - nr;
    uint8_t data[255], cw[255], rx[255];
    int ok_t = 0, det = 0, ok_e = 0, ntr = 60;
    for (int trial = 0; trial < ntr; trial++) {
        for (int i = 0; i < nd; i++)
            data[i] = (uint8_t)rnd();
        memcpy(cw, data, (size_t)nd);
        rs_encode(rs, data, nd, cw + nd);
        CHECK(rs_decode(rs, memcpy(rx, cw, (size_t)nblock), nblock, NULL, 0) == 0);
        /* exactamente t errores en posiciones distintas */
        memcpy(rx, cw, (size_t)nblock);
        int pos[130];
        for (int e = 0; e < t + 1; e++) {
            int p, dup;
            do {
                p = (int)(rnd() % (uint32_t)nblock);
                dup = 0;
                for (int q = 0; q < e; q++)
                    dup |= pos[q] == p;
            } while (dup);
            pos[e] = p;
        }
        for (int e = 0; e < t; e++)
            rx[pos[e]] ^= (uint8_t)(1 + rnd() % 255);
        int r = rs_decode(rs, rx, nblock, NULL, 0);
        if (r == t && !memcmp(rx, cw, (size_t)nblock))
            ok_t++;
        /* t+1 errores: detectado, o al menos nunca se devuelve la palabra original por error */
        memcpy(rx, cw, (size_t)nblock);
        for (int e = 0; e <= t; e++)
            rx[pos[e]] ^= (uint8_t)(1 + rnd() % 255);
        r = rs_decode(rs, rx, nblock, NULL, 0);
        if (r < 0)
            det++;
        /* borrados + errores con 2e+s = nroots */
        memcpy(rx, cw, (size_t)nblock);
        int ne = nr / 4, ns = nr - 2 * ne;
        int era[130];
        for (int i = 0; i < ne + ns; i++) {
            int p, dup;
            do {
                p = (int)(rnd() % (uint32_t)nblock);
                dup = 0;
                for (int q = 0; q < i; q++)
                    dup |= (q < ns ? era[q] : pos[q]) == p;
                if (i >= ns)
                    for (int q = 0; q < ns; q++)
                        dup |= era[q] == p;
            } while (dup);
            if (i < ns)
                era[i] = p;
            else
                pos[i] = p;
        }
        for (int i = 0; i < ns; i++)
            rx[era[i]] ^= (uint8_t)(rnd() % 256);          /* valor aleatorio (puede ser 0) */
        for (int i = ns; i < ns + ne; i++)
            rx[pos[i]] ^= (uint8_t)(1 + rnd() % 255);
        r = rs_decode(rs, rx, nblock, era, ns);
        if (r >= 0 && !memcmp(rx, cw, (size_t)nblock))
            ok_e++;
    }
    printf("RS %-14s n=%3d nroots=%2d: t errores %d/%d, t+1 detectados %d/%d, borrados+errores %d/%d\n",
           name, nblock, nr, ok_t, ntr, det, ntr, ok_e, ntr);
    CHECK(ok_t == ntr);
    CHECK(ok_e == ntr);
    CHECK(det >= (nr <= 6 ? ntr * 7 / 10 : ntr - 2));   /* P(falso corregido) ~ 1/t! */
}

static void test_rs(void)
{
    rs_t rs;
    CHECK(rs_init_ccsds(&rs) == 0);
    /* g(x) de CCSDS es palindrómico (por fcr=112, prim=11: raíces simétricas) */
    int sym = 1;
    for (int m = 1; m < 32; m++)
        sym &= rs.gen[31 - m] == rs.gen[m - 1];
    CHECK(sym && rs.gen[31] == 1);
    rs_trial(&rs, 255, "CCSDS 255,223");
    rs_trial(&rs, 200, "CCSDS corto");
    CHECK(rs_init_rs41(&rs) == 0);
    rs_trial(&rs, 255, "RS41 255,231");
    rs_trial(&rs, 132 + 24, "RS41 acortado");
    CHECK(rs_init_vdl2(&rs) == 0);
    rs_trial(&rs, 255, "VDL2 255,249");
    rs_trial(&rs, 100, "VDL2 acortado");
    CHECK(rs_init_dvb(&rs) == 0);
    rs_trial(&rs, 204, "DVB 204,188");
    CHECK(rs_init_dab(&rs) == 0);
    rs_trial(&rs, 120, "DAB 120,110");
    CHECK(rs_init(&rs, 0x11D, 1, 1, 6) == 0);
    rs_trial(&rs, 64, "generico fcr=1");
    /* parámetros inválidos */
    CHECK(rs_init(&rs, 0x101, 0, 1, 4) == -1);     /* x^8+1 no es primitivo */
    CHECK(rs_init(&rs, 0x11D, 0, 5 * 17, 4) == -1);   /* prim no coprimo con 255 */
}

/* -------------------------------- BCH ----------------------------------- */
static void test_bch(void)
{
    bch_t b;
    CHECK(bch_init_pocsag(&b) == 0);
    CHECK(b.n == 31 && b.k == 21 && b.t == 2);
    uint32_t g = 0;
    for (int i = 0; i <= b.np; i++)
        g |= (uint32_t)b.gen[i] << i;
    CHECK(g == 0x769);
    int bad = 0, det3 = 0, tot3 = 0;
    for (int tr = 0; tr < 400; tr++) {
        uint32_t d = rnd() & 0x1FFFFF, cw = bch_encode_u32(&b, d, 21);
        CHECK((cw >> 10) == d);
        uint32_t w = cw;
        CHECK(bch_decode_u32(&b, &w, 31) == 0);
        for (int i = 0; i < 31; i++)
            for (int j = i; j < 31; j++) {
                w = cw ^ (1u << i);
                if (j != i)
                    w ^= 1u << j;
                int r = bch_decode_u32(&b, &w, 31);
                if (r != (i == j ? 1 : 2) || w != cw)
                    bad++;
            }
        if (tr < 40) {   /* 3 errores: palabra de 32 bits con paridad par (POCSAG/FLEX) */
            uint32_t w32 = (cw << 1) | (uint32_t)__builtin_parity(cw);
            int p[3] = {(int)(rnd() % 32), (int)(rnd() % 32), (int)(rnd() % 32)};
            if (p[0] == p[1] || p[1] == p[2] || p[0] == p[2])
                continue;
            uint32_t r32 = w32 ^ (1u << p[0]) ^ (1u << p[1]) ^ (1u << p[2]);
            uint32_t r31 = r32 >> 1;
            int r = bch_decode_u32(&b, &r31, 31);
            int parity_ok = __builtin_parity((r31 << 1) | (r32 & 1)) == 0;
            tot3++;
            if (r < 0 || !parity_ok)
                det3++;
        }
    }
    printf("BCH(31,21): errores 1-2 mal corregidos: %d; 3 errores detectados con paridad: %d/%d\n", bad, det3, tot3);
    CHECK(bad == 0);
    CHECK(det3 == tot3);
    /* BCH genéricos */
    struct { int m, poly, t, n, k; } gens[] = {{4, 0x13, 2, 15, 7}, {4, 0x13, 3, 15, 5}, {6, 0x43, 2, 63, 51},
                                               {6, 0x43, 4, 63, 39}, {7, 0x89, 3, 127, 106}, {8, 0x11D, 5, 255, 215}};
    for (size_t gi = 0; gi < ARRAY_LEN(gens); gi++) {
        bch_t c;
        CHECK(bch_init(&c, gens[gi].m, gens[gi].poly, gens[gi].t) == 0);
        CHECK(c.n == gens[gi].n && c.k == gens[gi].k);
        int okc = 0, trials = 100;
        for (int tr = 0; tr < trials; tr++) {
            uint8_t data[256], par[336], cw[600];
            int nd = c.k - (tr % 3 == 2 ? 3 : 0);       /* algunos acortados */
            if (nd < 1 || nd > 255) nd = 1;
            for (int i = 0; i < nd; i++)
                data[i] = (uint8_t)(rnd() & 1);
            bch_encode(&c, data, nd, par);
            memcpy(cw, data, (size_t)nd);
            memcpy(cw + nd, par, (size_t)c.np);
            int nb = nd + c.np;
            uint8_t rx[600];
            memcpy(rx, cw, (size_t)nb);
            int used[8] = {0};
            for (int e = 0; e < c.t; e++) {
                int p, dup;
                do { p = (int)(rnd() % (uint32_t)nb); dup = 0; for (int q = 0; q < e; q++) dup |= used[q] == p; } while (dup);
                used[e] = p;
                rx[p] ^= 1;
            }
            if (bch_decode(&c, rx, nb) == c.t && !memcmp(rx, cw, (size_t)nb))
                okc++;
        }
        CHECK(okc == trials);
    }
}

/* ------------------------------- Golay ---------------------------------- */
static void test_golay(void)
{
    static golay_t g;
    golay_init(&g);
    CHECK(golay23_encode(1) == ((1u << 11) | 0x475));   /* x^11 mod g = g - x^11 = 0x475 */
    uint32_t c0 = golay23_encode(0xABC);
    CHECK((c0 >> 11) == 0xABC);
    /* mínimo peso 7 */
    int wmin = 99;
    for (uint32_t d = 1; d < 4096; d++)
        wmin = MIN(wmin, __builtin_popcount(golay23_encode(d)));
    CHECK(wmin == 7);
    int bad23 = 0, bad24 = 0, det4 = 0, tot4 = 0, mis4_23 = 0;
    for (uint32_t d = 0; d < 4096; d += 7) {
        uint32_t c = golay23_encode(d), c24 = golay24_encode(d);
        for (int i = -1; i < 23; i++)
            for (int j = i; j < 23; j++)
                for (int k = j; k < 23; k++) {
                    uint32_t e = 0;
                    int ok = 1;
                    int idx[3] = {i, j, k};
                    for (int q = 0; q < 3; q++) {
                        if (idx[q] < 0) continue;
                        if (e & (1u << idx[q])) ok = 0;
                        e |= 1u << idx[q];
                    }
                    if (!ok) continue;
                    uint32_t w = c ^ e;
                    if (golay23_decode(&g, &w) != __builtin_popcount(e) || w != c)
                        bad23++;
                    uint32_t w24 = c24 ^ e;
                    if (golay24_decode(&g, &w24) != __builtin_popcount(e) || w24 != c24)
                        bad24++;
                }
        /* 3 errores con el bit de paridad del (24,12) incluido */
        uint32_t w24 = c24 ^ (1u << 23) ^ 3u;
        if (golay24_decode(&g, &w24) != 3 || w24 != c24)
            bad24++;
        /* 4 errores en el (24,12): siempre detectados */
        for (int tr = 0; tr < 50; tr++) {
            uint32_t e = 0;
            while (__builtin_popcount(e) < 4)
                e |= 1u << (rnd() % 24);
            uint32_t w = c24 ^ e;
            tot4++;
            if (golay24_decode(&g, &w) < 0)
                det4++;
            uint32_t w2 = c ^ (e & 0x7FFFFF);
            if (__builtin_popcount(e & 0x7FFFFF) == 4) {
                golay23_decode(&g, &w2);
                mis4_23 += w2 != c;     /* el perfecto siempre "corrige" a otra palabra */
            }
        }
    }
    printf("Golay: errores <=3 mal corregidos (23/24): %d/%d; 4 errores detectados en (24,12): %d/%d\n", bad23, bad24, det4, tot4);
    CHECK(bad23 == 0 && bad24 == 0);
    CHECK(det4 == tot4);
    CHECK(mis4_23 > 0);
}

/* ------------------------------ Hamming --------------------------------- */
static void test_hamming(void)
{
    int (*inits[])(hamming_t *) = {hamming_init_7_4, hamming_init_13_9, hamming_init_15_11, hamming_init_16_11};
    int ks[] = {4, 9, 11, 11}, ns[] = {7, 13, 15, 16};
    for (int h = 0; h < 4; h++) {
        hamming_t hm;
        CHECK(inits[h](&hm) == 0);
        CHECK(hm.n == ns[h]);
        int bad = 0, det2 = 0, tot2 = 0;
        for (uint32_t d = 0; d < (1u << ks[h]); d++) {
            uint32_t c = hamming_encode(&hm, d);
            CHECK(hamming_data(&hm, c) == d);
            uint32_t w = c;
            if (hamming_decode(&hm, &w) != 0)
                bad++;
            for (int i = 0; i < hm.n; i++) {
                w = c ^ (1u << i);
                if (hamming_decode(&hm, &w) != 1 || w != c)
                    bad++;
                if (hm.ext)
                    for (int j = i + 1; j < hm.n; j++) {
                        w = c ^ (1u << i) ^ (1u << j);
                        tot2++;
                        det2 += hamming_decode(&hm, &w) == -1;
                    }
            }
        }
        CHECK(bad == 0);
        CHECK(det2 == tot2);
    }
    hamming_t bad;
    CHECK(hamming_init(&bad, 12, 4, 0) == -1);     /* 12 datos no caben con r=4 */
    uint16_t dup[2] = {3, 3};
    CHECK(hamming_init_cols(&bad, 2, 3, 0, dup) == -1);
}

/* ------------------------------- QR(16,7) -------------------------------- */
static void test_qr(void)
{
    linblock_t q;
    CHECK(linblock_init_qr16_7(&q) == 0);
    printf("QR(16,7): dmin=%d t=%d\n", q.dmin, q.t);
    CHECK(q.dmin == 6 && q.t == 2);
    int bad = 0, det3 = 0, tot3 = 0;
    for (uint32_t d = 0; d < 128; d++) {
        uint32_t c = linblock_encode(&q, d);
        for (int i = 0; i < 16; i++)
            for (int j = i; j < 16; j++) {
                uint32_t w = c ^ (1u << i) ^ (j != i ? 1u << j : 0), dd = 0;
                if (linblock_decode(&q, &w, &dd) != (i == j ? 1 : 2) || w != c || dd != d)
                    bad++;
                for (int k = j + 1; k < 16; k++) {
                    if (i == j) continue;
                    uint32_t w3 = c ^ (1u << i) ^ (1u << j) ^ (1u << k);
                    tot3++;
                    det3 += linblock_decode(&q, &w3, NULL) == -1;
                }
            }
    }
    CHECK(bad == 0);
    CHECK(det3 == tot3);
}

int main(void)
{
    test_viterbi();
    test_rs();
    test_bch();
    test_golay();
    test_hamming();
    test_qr();
    printf("test_fec: %d comprobaciones, %d fallos\n", g_n, g_fail);
    return g_fail != 0;
}
