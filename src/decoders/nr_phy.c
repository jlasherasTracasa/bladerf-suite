/*
 * nr_phy.c — Secuencias PSS/SSS de NR y rejilla GSCN. Ver nr_phy.h.
 */
#include "nr_phy.h"

#include "nr_tables.h"

#include <math.h>
#include <stdlib.h>
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

/* ---------------------------------------------------------------------- */
/*  PBCH de NR                                                             */
/* ---------------------------------------------------------------------- */
/* Gold de longitud 31 (38.211 §5.2.1): igual que la de LTE. */
static void nr_gold(uint32_t cinit, int n, uint8_t *out)
{
    uint32_t x1 = 1, x2 = cinit;
    for (int i = 0; i < 1600 + n; i++) {
        if (i >= 1600)
            out[i - 1600] = (uint8_t)((x1 ^ x2) & 1);
        uint32_t f1 = ((x1 >> 3) ^ x1) & 1;
        uint32_t f2 = ((x2 >> 3) ^ (x2 >> 2) ^ (x2 >> 1) ^ x2) & 1;
        x1 = (x1 >> 1) | (f1 << 30);
        x2 = (x2 >> 1) | (f2 << 30);
    }
}

void nr_dmrs_seq(int pci, int ibar, cf32 r[144])
{
    uint32_t cinit = (uint32_t)((((uint64_t)(ibar + 1) * (uint64_t)(pci / 4 + 1)) << 11) + ((uint64_t)(ibar + 1) << 6) +
                                (uint64_t)(pci % 4));
    cinit &= 0x7FFFFFFFu;
    uint8_t c[288];
    nr_gold(cinit, 288, c);
    const float a = 0.70710678f;
    for (int m = 0; m < 144; m++)
        r[m] = (cf32){a * (float)(1 - 2 * c[2 * m]), a * (float)(1 - 2 * c[2 * m + 1])};
}

/* DMRS en k ≡ v (mod 4): símbolo 1 (60), símbolo 2 (12 abajo + 12 arriba), símbolo 3 (60). */
void nr_dmrs_re_pos(int pci, int m, int *l, int *k)
{
    int v = pci % 4;
    if (m < 60) {
        *l = 1;
        *k = 4 * m + v;
    } else if (m < 72) {
        *l = 2;
        *k = 4 * (m - 60) + v;
    } else if (m < 84) {
        *l = 2;
        *k = 192 + 4 * (m - 72) + v;
    } else {
        *l = 3;
        *k = 4 * (m - 84) + v;
    }
}

/* RE de datos: el resto de los de los símbolos 1, 2 (0..47 y 192..239) y 3, en orden de k y luego l. */
void nr_pbch_re_pos(int pci, int n, int *l, int *k)
{
    int v = pci % 4;
    int cnt = 0;
    for (int ll = 1; ll <= 3; ll++)
        for (int kk = 0; kk < 240; kk++) {
            if (ll == 2 && kk >= 48 && kk < 192)
                continue;
            if (kk % 4 == v)
                continue;
            if (cnt++ == n) {
                *l = ll;
                *k = kk;
                return;
            }
        }
    *l = *k = -1;
}

static const int PB_G[32] = {16, 23, 18, 17, 8, 30, 10, 6, 24, 7, 0, 5, 3, 2, 1, 4,
                             9, 11, 12, 13, 14, 15, 19, 20, 21, 22, 25, 26, 27, 28, 29, 31};

/* gCRC24C (38.212 §5.1): D^24+D^23+D^21+D^20+D^17+D^15+D^13+D^12+D^8+D^4+D^2+D+1 */
static uint32_t crc24c(const uint8_t *b, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((r >> 23) & 1) ^ (b[i] & 1);
        r = (r << 1) & 0xFFFFFF;
        if (fb)
            r ^= 0xB2B117;
    }
    return r;
}

static void mib_pack(const nr_mib_t *m, uint8_t a[32])
{
    uint8_t p[24];
    int q = 0;
    p[q++] = 0;                                         /* mensaje BCCH-BCH tipo MIB */
    for (int i = 5; i >= 0; i--)
        p[q++] = (uint8_t)((m->sfn >> (4 + i)) & 1);    /* systemFrameNumber: 6 MSB */
    p[q++] = (uint8_t)(m->scs_common & 1);
    for (int i = 3; i >= 0; i--)
        p[q++] = (uint8_t)((m->k_ssb >> i) & 1);        /* ssb-SubcarrierOffset: 4 LSB */
    p[q++] = m->dmrs_pos == 3 ? 1 : 0;
    for (int i = 3; i >= 0; i--)
        p[q++] = (uint8_t)((m->coreset0 >> i) & 1);
    for (int i = 3; i >= 0; i--)
        p[q++] = (uint8_t)((m->ss0 >> i) & 1);
    p[q++] = m->barred ? 0 : 1;                         /* {barred, notBarred} */
    p[q++] = m->intra_freq_not_allowed ? 1 : 0;         /* {allowed, notAllowed} */
    p[q++] = 0;                                         /* spare */
    int js = 0, jo = 14;
    for (int i = 0; i < 24; i++) {
        if (i >= 1 && i < 7)
            a[PB_G[js++]] = p[i];
        else
            a[PB_G[jo++]] = p[i];
    }
    for (int i = 3; i >= 0; i--)
        a[PB_G[js++]] = (uint8_t)((m->sfn >> i) & 1);   /* 4 LSB del SFN */
    a[PB_G[10]] = (uint8_t)(m->hrf & 1);
    a[PB_G[11]] = (uint8_t)((m->k_ssb >> 4) & 1);       /* L_max ≠ 64: MSB de k_SSB */
    a[PB_G[12]] = 0;
    a[PB_G[13]] = 0;
}

static void mib_unpack(const uint8_t a[32], nr_mib_t *m)
{
    uint8_t p[24];
    int js = 0, jo = 14;
    for (int i = 0; i < 24; i++) {
        if (i >= 1 && i < 7)
            p[i] = a[PB_G[js++]];
        else
            p[i] = a[PB_G[jo++]];
    }
    int sfn = 0;
    for (int i = 1; i <= 6; i++)
        sfn = (sfn << 1) | p[i];
    sfn <<= 4;
    for (int i = 0; i < 4; i++)
        sfn |= a[PB_G[js++]] << (3 - i);
    memset(m, 0, sizeof *m);
    m->sfn = sfn;
    m->hrf = a[PB_G[10]];
    m->scs_common = p[7];
    m->k_ssb = (a[PB_G[11]] << 4) | (p[8] << 3) | (p[9] << 2) | (p[10] << 1) | p[11];
    m->dmrs_pos = p[12] ? 3 : 2;
    m->coreset0 = (p[13] << 3) | (p[14] << 2) | (p[15] << 1) | p[16];
    m->ss0 = (p[17] << 3) | (p[18] << 2) | (p[19] << 1) | p[20];
    m->barred = p[21] == 0;
    m->intra_freq_not_allowed = p[22] == 1;
}

/* Aleatorización del contenido (38.212 §7.1.2): los bits del SFN 2º/3º, HRF (y los de ssb_idx con L = 64)
 * no se aleatorizan ni gastan secuencia. v = 2·SFN3 + SFN2; se avanza M·v con M = A − 3 = 29. */
static void scramble_a(int pci, const uint8_t a[32], uint8_t o[32])
{
    int v = 2 * a[PB_G[7]] + a[PB_G[8]];
    uint8_t c[29 * 4 + 32];
    nr_gold((uint32_t)pci, 29 * v + 32, c);
    int j = 29 * v;
    for (int i = 0; i < 32; i++) {
        uint8_t s = 0;
        if (!(i == PB_G[10] || i == PB_G[8] || i == PB_G[7]))
            s = c[j++];
        o[i] = a[i] ^ s;
    }
}

static int block_il(int n, int N)
{
    static const int P[32] = {0, 1, 2, 4, 3, 5, 6, 7, 8, 16, 9, 17, 10, 18, 11, 19,
                              12, 20, 13, 21, 14, 22, 15, 23, 24, 25, 26, 28, 27, 29, 30, 31};
    return P[32 * n / N] * (N / 32) + n % (N / 32);
}

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

/* Conjunto de información: las 56 posiciones más fiables de N = 512 (las últimas de Q), ordenadas. */
static void info_set(uint16_t kset[56])
{
    memcpy(kset, nr_polar_q512 + 512 - 56, 56 * sizeof(uint16_t));
    qsort(kset, 56, sizeof(uint16_t), cmp_u16);
}

/* c (56 bits) ↔ c' (entrelazado del downlink, tabla 5.3.1.1-1). */
static void il_map(int dir, const uint8_t *in, uint8_t *out)
{
    int k = 0;
    for (int m = 0; m < 164; m++)
        if (nr_polar_il[m] >= 164 - 56) {
            int pi = nr_polar_il[m] - (164 - 56);
            if (dir)
                out[k] = in[pi];
            else
                out[pi] = in[k];
            k++;
        }
}

void nr_pbch_encode(const nr_mib_t *m, int pci, int v, cf32 sym[NR_PBCH_RE])
{
    uint8_t a[32], ap[32], c[56], cp[56], u[512] = {0}, x[512], e[864];
    mib_pack(m, a);
    scramble_a(pci, a, ap);
    memcpy(c, ap, 32);
    uint32_t crc = crc24c(ap, 32);
    for (int i = 0; i < 24; i++)
        c[32 + i] = (uint8_t)((crc >> (23 - i)) & 1);
    il_map(1, c, cp);
    uint16_t ks[56];
    info_set(ks);
    for (int i = 0; i < 56; i++)
        u[ks[i]] = cp[i];
    memcpy(x, u, 512);
    for (int len = 1; len < 512; len <<= 1)
        for (int i = 0; i < 512; i += 2 * len)
            for (int j = 0; j < len; j++)
                x[i + j] ^= x[i + j + len];
    for (int k = 0; k < 864; k++)
        e[k] = x[block_il(k % 512, 512)];
    uint8_t sc[864 * 8];
    nr_gold((uint32_t)pci, 864 * (v + 1), sc);
    const float s = 0.70710678f;
    for (int i = 0; i < NR_PBCH_RE; i++) {
        int b0 = e[2 * i] ^ sc[864 * v + 2 * i], b1 = e[2 * i + 1] ^ sc[864 * v + 2 * i + 1];
        sym[i] = (cf32){s * (float)(1 - 2 * b0), s * (float)(1 - 2 * b1)};
    }
}

/* --- Decodificador de cancelación sucesiva con lista (SCL, L = 8), recursión por mitades --- */
#define SCL_L 8
#define PN 512

typedef struct {
    float al[2 * PN];       /* al[2^s + j]: LLR de entrada del nodo activo de la etapa s (tamaño 2^s) */
    uint8_t be[2 * PN];     /* be[2^s + j]: palabra de código decidida del nodo */
    uint8_t u[PN];
    float pm;
} scl_path_t;

typedef struct {
    scl_path_t p[SCL_L], q[SCL_L];
    int np;
    uint8_t frozen[PN];
} scl_t;

static inline float f_llr(float a, float b)
{
    float s = (a < 0) != (b < 0) ? -1.f : 1.f;
    return s * fminf(fabsf(a), fabsf(b));
}

static void scl_leaf(scl_t *d, int idx)
{
    if (d->frozen[idx]) {
        for (int i = 0; i < d->np; i++) {
            scl_path_t *p = &d->p[i];
            float l = p->al[1];
            if (l < 0)
                p->pm -= l;
            p->be[1] = 0;
            p->u[idx] = 0;
        }
        return;
    }
    typedef struct {
        float pm;
        int pi, bit;
    } cand_t;
    cand_t cand[2 * SCL_L];
    int nc = 0;
    for (int i = 0; i < d->np; i++) {
        float l = d->p[i].al[1];
        cand[nc++] = (cand_t){d->p[i].pm + (l < 0 ? -l : 0), i, 0};
        cand[nc++] = (cand_t){d->p[i].pm + (l > 0 ? l : 0), i, 1};
    }
    /* los L menores (selección simple) */
    int keep = nc < SCL_L ? nc : SCL_L;
    for (int a = 0; a < keep; a++) {
        int b = a;
        for (int c = a + 1; c < nc; c++)
            if (cand[c].pm < cand[b].pm)
                b = c;
        cand_t t = cand[a];
        cand[a] = cand[b];
        cand[b] = t;
    }
    for (int a = 0; a < keep; a++) {
        d->q[a] = d->p[cand[a].pi];
        d->q[a].pm = cand[a].pm;
        d->q[a].be[1] = (uint8_t)cand[a].bit;
        d->q[a].u[idx] = (uint8_t)cand[a].bit;
    }
    memcpy(d->p, d->q, sizeof(scl_path_t) * (size_t)keep);
    d->np = keep;
}

static void scl_node(scl_t *d, int s, int off)
{
    if (s == 0) {
        scl_leaf(d, off);
        return;
    }
    int n = 1 << s, h = n >> 1;
    for (int i = 0; i < d->np; i++) {
        scl_path_t *p = &d->p[i];
        for (int j = 0; j < h; j++)
            p->al[h + j] = f_llr(p->al[n + j], p->al[n + h + j]);
    }
    scl_node(d, s - 1, off);
    for (int i = 0; i < d->np; i++) {
        scl_path_t *p = &d->p[i];
        for (int j = 0; j < h; j++) {
            p->be[n + j] = p->be[h + j];                              /* guarda la mitad izquierda */
            p->al[h + j] = p->al[n + h + j] + (p->be[h + j] ? -p->al[n + j] : p->al[n + j]);
        }
    }
    scl_node(d, s - 1, off + h);
    for (int i = 0; i < d->np; i++) {
        scl_path_t *p = &d->p[i];
        for (int j = 0; j < h; j++) {
            uint8_t r = p->be[h + j];
            p->be[n + h + j] = r;
            p->be[n + j] ^= r;
        }
    }
}

bool nr_pbch_decode(const float *soft, int pci, int v, nr_mib_t *m)
{
    uint8_t sc_buf[864 * 8];
    nr_gold((uint32_t)pci, 864 * (v + 1), sc_buf);
    float y[512], dl[512];
    memset(y, 0, sizeof y);
    for (int k = 0; k < 864; k++)
        y[k % 512] += sc_buf[864 * v + k] ? -soft[k] : soft[k];
    for (int n = 0; n < 512; n++)
        dl[block_il(n, 512)] = y[n];
    float mx = 1e-9f;
    for (int i = 0; i < 512; i++)
        mx = fmaxf(mx, fabsf(dl[i]));
    for (int i = 0; i < 512; i++)
        dl[i] *= 16.0f / mx;
    uint16_t ks[56];
    info_set(ks);
    scl_t *d = calloc(1, sizeof *d);
    if (!d)
        return false;
    memset(d->frozen, 1, sizeof d->frozen);
    for (int i = 0; i < 56; i++)
        d->frozen[ks[i]] = 0;
    d->np = 1;
    memcpy(&d->p[0].al[PN], dl, sizeof dl);
    scl_node(d, 9, 0);
    bool ok = false;
    for (int pth = 0; pth < d->np && !ok; pth++) {
        uint8_t cp[56], c[56], ap[32], a[32];
        for (int i = 0; i < 56; i++)
            cp[i] = d->p[pth].u[ks[i]];
        il_map(0, cp, c);
        uint32_t crc = crc24c(c, 32), rx = 0;
        for (int i = 0; i < 24; i++)
            rx = (rx << 1) | c[32 + i];
        if (crc != rx)
            continue;
        memcpy(ap, c, 32);
        scramble_a(pci, ap, a);         /* el XOR es su propia inversa; v sale de bits sin aleatorizar */
        mib_unpack(a, m);
        ok = true;
    }
    free(d);
    return ok;
}
