/*
 * dvbt_tab.c — Tablas de la codificación de canal de DVB-T (ver dvbt_tab.h).
 * ETSI EN 300 744 V1.6.2: 4.3.4 (entrelazado interno, perforado), 4.3.5 (mapeo).
 */
#include "dvbt_tab.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Entrelazado de símbolo (4.3.4.2): H(q) con una PRBS de Nr−1 bits.
 *   R'_i[Nr−2…0]: i = 0,1 → 0; i = 2 → 0…01; i > 2: desplazamiento a la derecha con
 *   realimentación x^(Nr-1)… (2k: R'[9] = R'₋₁[0] ⊕ R'₋₁[3]; 8k: R'[11] = R'₋₁[0] ⊕
 *   R'₋₁[1] ⊕ R'₋₁[4] ⊕ R'₋₁[6]).  R_i = permutación de bits de R'_i (tablas de la
 *   norma); H(q) = (i mod 2)·2^(Nr−1) + Σ R_i[j]·2^j y solo se aceptan H(q) < Nmax.
 * ------------------------------------------------------------------------- */
static uint16_t g_symint[2][6048];
static uint32_t *g_bitmap[2][3][2][2][2]; /* [modo][v/2−1][hier][parity][stream] */

static void gen_symint(int N, uint16_t *H)
{
    static const int p8[12] = {7, 1, 4, 2, 9, 6, 8, 10, 0, 3, 11, 5}; /* R'_i bit j → R_i bit p[j] (8k) */
    static const int p2[10] = {4, 3, 9, 6, 2, 8, 1, 5, 7, 0};         /* (2k) */
    const int Nr = N == 8192 ? 13 : 11, nmax = N == 8192 ? 6048 : 1512;
    const int *pm = N == 8192 ? p8 : p2;
    uint32_t r = 0; /* R'_i, Nr−1 bits */
    int q = 0;
    for (int i = 0; i < N && q < nmax; i++) {
        if (i == 0 || i == 1)
            r = 0;
        else if (i == 2)
            r = 1;
        else {
            uint32_t fb = N == 8192 ? ((r ^ (r >> 1) ^ (r >> 4) ^ (r >> 6)) & 1) : ((r ^ (r >> 3)) & 1);
            r = (r >> 1) | (fb << (Nr - 2));
        }
        uint32_t R = 0;
        for (int j = 0; j < Nr - 1; j++)
            if ((r >> j) & 1)
                R |= 1u << pm[j];
        uint32_t h = (uint32_t)(i & 1) * (1u << (Nr - 1)) + R;
        if (h < (uint32_t)nmax)
            H[q++] = (uint16_t)h;
    }
}

static void ensure(void);
const uint16_t *dvbt_symint(int n)
{
    ensure();
    return g_symint[n == 8192];
}

/* Desfases del entrelazado de bits: H_e(w) = (w + off[e]) mod 126 (4.3.4.2, I0…I5). */
static const int BOFF[6] = {0, 63, 105, 42, 21, 84};

/* Posición que ocupa el bit y_e dentro del grupo de v bits de x' (demux, 4.3.4.1). */
static void demux_pos(int v, bool hier, int stream, int *pos, int *nsub, int *first)
{
    /* pos[e] = índice dentro del grupo del flujo; first = primera e del flujo */
    static const int nh[7][6] = {{0}, {0}, {0, 1}, {0}, {0, 2, 1, 3}, {0}, {0, 2, 4, 1, 3, 5}};
    for (int e = 0; e < 6; e++)
        pos[e] = -1;
    if (!hier) {
        for (int k = 0; k < v; k++)
            pos[nh[v][k]] = k;
        *nsub = v;
        *first = 0;
    } else if (stream == 0) {
        pos[0] = 0;
        pos[1] = 1;
        *nsub = 2;
        *first = 0;
    } else {
        /* LP: mismo orden relativo que el caso no jerárquico, sin y0,y1 */
        int k = 0;
        for (int j = 0; j < v; j++)
            if (nh[v][j] >= 2)
                pos[nh[v][j]] = k++;
        *nsub = v - 2;
        *first = 2;
    }
}

static void build_bitmap(int mode, int v, bool hier, int parity, int stream, uint32_t *dst)
{
    const int N = mode ? 8192 : 2048, nd = dvbt_ndata(N);
    const uint16_t *H = g_symint[mode];
    uint16_t Hinv[6048];
    for (int q = 0; q < nd; q++)
        Hinv[H[q]] = (uint16_t)q;
    int pos[6], nsub, first;
    demux_pos(v, hier, stream, pos, &nsub, &first);
    (void)first;
    for (int p = 0; p < nd; p++) {
        int q = parity == 0 ? Hinv[p] : H[p]; /* índice de palabra antes del entrelazado de símbolo */
        int b = q / 126, w = q % 126;
        for (int i = 0; i < v; i++) {
            if (pos[i] < 0) {
                dst[p * v + i] = 0xFFFFFFFFu;
                continue;
            }
            int m = 126 * b + (w + BOFF[i]) % 126; /* B_i(w) = A_i(H_i(w)) */
            dst[p * v + i] = (uint32_t)(nsub * m + pos[i]);
        }
    }
}

static void init_tables(void)
{
    gen_symint(2048, g_symint[0]);
    gen_symint(8192, g_symint[1]);
}

static pthread_once_t g_once2 = PTHREAD_ONCE_INIT;
static void ensure(void) { pthread_once(&g_once2, init_tables); }

const uint32_t *dvbt_bitmap(int n, int v, bool hier, int parity, int stream)
{
    ensure();
    int mode = n == 8192, vi = v / 2 - 1;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    uint32_t **slot = &g_bitmap[mode][vi][hier ? 1 : 0][parity & 1][stream ? 1 : 0];
    if (!*slot) {
        *slot = malloc(sizeof(uint32_t) * (size_t)dvbt_ndata(n) * (size_t)v);
        if (*slot)
            build_bitmap(mode, v, hier, parity & 1, stream ? 1 : 0, *slot);
    }
    uint32_t *r = *slot;
    pthread_mutex_unlock(&mu);
    return r;
}

int dvbt_stream_bits(int n, int v, bool hier, int stream)
{
    int nd = dvbt_ndata(n);
    if (!hier)
        return nd * v;
    return stream ? nd * (v - 2) : nd * 2;
}

/* ---------------------------------------------------------------------------
 * Constelaciones (4.3.5, figuras 9-11). Eje I: bits y0 (signo), y2, y4; eje Q:
 * y1, y3, y5. Magnitud = α + 2·j con j(y2 y4) = 3,2,1,0 para 00,01,11,10 (64-QAM) y
 * j(y2) = 1,0 para 0,1 (16-QAM). Normalización: 1/√(2·E[nivel²]).
 * ------------------------------------------------------------------------- */
static float axis_level(int v, int alpha, unsigned sgn, unsigned b1, unsigned b2)
{
    float m;
    if (v == 2)
        return sgn ? -1.0f : 1.0f; /* escala aparte */
    if (v == 4)
        m = (float)alpha + 2.0f * (b1 ? 0.0f : 1.0f);
    else {
        int j = b1 ? (b2 ? 1 : 0) : (b2 ? 2 : 3);
        m = (float)alpha + 2.0f * (float)j;
    }
    return sgn ? -m : m;
}

static float qam_norm(int v, int alpha)
{
    if (v == 2)
        return 1.0f / sqrtf(2.0f);
    double s = 0;
    int nl = v == 4 ? 2 : 4;
    for (int j = 0; j < nl; j++) {
        double l = alpha + 2.0 * j;
        s += l * l;
    }
    s /= nl;
    return (float)(1.0 / sqrt(2.0 * s));
}

cf32 dvbt_qam_point(int v, int alpha, unsigned word)
{
    float c = qam_norm(v, alpha);
    unsigned y0 = word & 1, y1 = (word >> 1) & 1, y2 = (word >> 2) & 1, y3 = (word >> 3) & 1, y4 = (word >> 4) & 1,
             y5 = (word >> 5) & 1;
    cf32 p;
    p.re = axis_level(v, alpha, y0, y2, y4) * c;
    p.im = axis_level(v, alpha, y1, y3, y5) * c;
    return p;
}

static dvbt_llrtab_t *g_llr[3][3];
const dvbt_llrtab_t *dvbt_llrtab(int v, int alpha)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int vi = v / 2 - 1, ai = alpha == 4 ? 2 : alpha == 2 ? 1 : 0;
    pthread_mutex_lock(&mu);
    if (!g_llr[vi][ai]) {
        dvbt_llrtab_t *t = calloc(1, sizeof *t);
        if (t) {
            t->nab = v / 2;
            float c = qam_norm(v, alpha), top = (v == 2 ? 1.0f : (alpha + (v == 4 ? 2 : 6))) * c;
            float hi = top * 1.5f + 0.2f;
            t->lo = -hi;
            t->inv = (float)(DVBT_LLR_NT - 1) / (2 * hi);
            for (int ix = 0; ix < DVBT_LLR_NT; ix++) {
                float x = t->lo + (float)ix / t->inv;
                for (int a = 0; a < t->nab; a++) {
                    float d0 = 1e30f, d1 = 1e30f;
                    for (unsigned w = 0; w < (1u << t->nab); w++) {
                        /* palabra del eje: bit0 = y0|y1 (signo), bit1 = y2|y3, bit2 = y4|y5 */
                        unsigned s = w & 1, b1 = (w >> 1) & 1, b2 = (w >> 2) & 1;
                        float lev = axis_level(v, alpha, s, b1, b2) * c;
                        float d = (x - lev) * (x - lev);
                        if ((w >> a) & 1)
                            d1 = fminf(d1, d);
                        else
                            d0 = fminf(d0, d);
                    }
                    t->t[a][ix] = d1 - d0;
                }
            }
        }
        g_llr[vi][ai] = t;
    }
    const dvbt_llrtab_t *r = g_llr[vi][ai];
    pthread_mutex_unlock(&mu);
    return r;
}

/* ---------------------------------------------------------------------------
 * Perforado y codificador convolucional.
 * ------------------------------------------------------------------------- */
const dvbt_punct_t *dvbt_punct(int rate)
{
    static const dvbt_punct_t P[5] = {
        {1, 2, {1}, {1}},
        {2, 3, {1, 0}, {1, 1}},
        {3, 4, {1, 0, 1}, {1, 1, 0}},
        {5, 6, {1, 0, 1, 0, 1}, {1, 1, 0, 1, 0}},
        {7, 8, {1, 0, 0, 0, 1, 0, 1}, {1, 1, 1, 1, 0, 1, 0}},
    };
    return rate >= 0 && rate < 5 ? &P[rate] : NULL;
}

int dvbt_conv_encode(uint32_t *state, const uint8_t *bits, int nin, int rate, uint8_t *out)
{
    const dvbt_punct_t *pu = dvbt_punct(rate);
    uint32_t s = *state; /* últimos 6 bits de entrada; bit 5 = el más reciente */
    int no = 0, ph = 0;
    for (int i = 0; i < nin; i++) {
        uint32_t reg = ((uint32_t)(bits[i] & 1) << 6) | s;
        uint8_t x = (uint8_t)__builtin_parity(reg & 0x79u), y = (uint8_t)__builtin_parity(reg & 0x5Bu);
        if (pu->x[ph])
            out[no++] = x;
        if (pu->y[ph])
            out[no++] = y;
        ph = ph + 1 == pu->period ? 0 : ph + 1;
        s = (s >> 1) | ((uint32_t)(bits[i] & 1) << 5);
    }
    *state = s;
    return no;
}

static uint8_t g_prbs[1503];
static pthread_once_t g_once3 = PTHREAD_ONCE_INIT;
static void prbs_gen(void)
{
    /* registro x1…x15 (x1 = bit 0) con 100101010000000; salida = x14 ⊕ x15, que realimenta x1 */
    uint32_t r = 0;
    const int init[15] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 15; i++)
        r |= (uint32_t)init[i] << i;
    for (int i = 0; i < 1503; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            uint32_t fb = ((r >> 13) ^ (r >> 14)) & 1;
            r = ((r << 1) | fb) & 0x7FFF;
            b = (uint8_t)((b << 1) | fb);
        }
        g_prbs[i] = b;
    }
}

const uint8_t *dvbt_prbs1503(void)
{
    pthread_once(&g_once3, prbs_gen);
    return g_prbs;
}
