/*
 * lte_sib.c — Cadena PCFICH / PDCCH / PDSCH / turbo / SIB1 de LTE. Ver lte_sib.h para la
 * teoría y lte_tables.h para las tablas de la norma.
 */
#include "lte_sib.h"

#include "lte_phy.h"
#include "lte_tables.h"
#include "../dsp/fec.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/*  CRC-24A                                                                */
/* ---------------------------------------------------------------------- */
uint32_t lte_crc24a(const uint8_t *bits, int n)
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((r >> 23) & 1) ^ (bits[i] & 1);
        r = (r << 1) & 0xFFFFFF;
        if (fb)
            r ^= 0x864CFB;
    }
    return r;
}

int lte_tbs_qpsk_get(int itbs, int nprb)
{
    if (itbs < 0 || itbs > 9 || nprb < 1 || nprb > 110)
        return 0;
    return lte_tbs_qpsk[itbs][nprb - 1];
}

int lte_tbs_1c_get(int mcs) { return mcs >= 0 && mcs < 32 ? lte_tbs_1c[mcs] : 0; }

/* ---------------------------------------------------------------------- */
/*  Turbocódigo (36.212 §5.1.3.2)                                          */
/* ---------------------------------------------------------------------- */
static int qpp_index(int K)
{
    if (K >= 40 && K <= 512 && (K & 7) == 0)
        return (K - 40) / 8;
    if (K >= 528 && K <= 1024 && (K & 15) == 0)
        return 60 + (K - 528) / 16;
    if (K >= 1056 && K <= 2048 && (K & 31) == 0)
        return 92 + (K - 1056) / 32;
    if (K >= 2112 && K <= 6144 && (K & 63) == 0)
        return 124 + (K - 2112) / 64;
    return -1;
}

/* π(i) = (f1·i + f2·i²) mod K */
static void qpp_perm(int K, int *pi)
{
    int idx = qpp_index(K);
    uint64_t f1 = lte_qpp_f1[idx], f2 = lte_qpp_f2[idx];
    for (int i = 0; i < K; i++)
        pi[i] = (int)((f1 * (uint64_t)i + f2 * (uint64_t)i * (uint64_t)i) % (uint64_t)K);
}

/* Máquina de estados del RSC: estado = r0<<2 | r1<<1 | r2; g0 = 1+D²+D³ (realimentación), g1 = 1+D+D³. */
static inline void rsc_step(int s, int u, int *ns, int *par)
{
    int r0 = (s >> 2) & 1, r1 = (s >> 1) & 1, r2 = s & 1;
    int in = u ^ r2 ^ r1;
    *par = r2 ^ r0 ^ in;
    *ns = (in << 2) | (r0 << 1) | r1;
}

/* Codifica K bits → d0,d1,d2 de K+4 (con la terminación del trellis de §5.1.3.2.2). */
static void turbo_encode(const uint8_t *c, int K, const int *pi, uint8_t *d0, uint8_t *d1, uint8_t *d2)
{
    int s1 = 0, s2 = 0;
    for (int i = 0; i < K; i++) {
        int p1, p2, n1, n2;
        rsc_step(s1, c[i], &n1, &p1);
        rsc_step(s2, c[pi[i]], &n2, &p2);
        d0[i] = c[i];
        d1[i] = (uint8_t)p1;
        d2[i] = (uint8_t)p2;
        s1 = n1;
        s2 = n2;
    }
    int x1[3], z1[3], x2[3], z2[3];
    for (int t = 0; t < 3; t++) {
        int u = ((s1 >> 1) & 1) ^ (s1 & 1), n;
        x1[t] = u;
        rsc_step(s1, u, &n, &z1[t]);
        s1 = n;
        u = ((s2 >> 1) & 1) ^ (s2 & 1);
        x2[t] = u;
        rsc_step(s2, u, &n, &z2[t]);
        s2 = n;
    }
    d0[K] = (uint8_t)x1[0]; d0[K + 1] = (uint8_t)z1[1]; d0[K + 2] = (uint8_t)x2[0]; d0[K + 3] = (uint8_t)z2[1];
    d1[K] = (uint8_t)z1[0]; d1[K + 1] = (uint8_t)x1[2]; d1[K + 2] = (uint8_t)z2[0]; d1[K + 3] = (uint8_t)x2[2];
    d2[K] = (uint8_t)x1[1]; d2[K + 1] = (uint8_t)z1[2]; d2[K + 2] = (uint8_t)x2[1]; d2[K + 3] = (uint8_t)z2[2];
}

/* Mapa del búfer circular (36.212 §5.1.4.1): wmap[j] = flujo·D + posición, o −1 si <NULL>. */
static const uint8_t TC_P[32] = {0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
                                 1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31};

static int rm_circ_build(int D, int *wmap)
{
    int R = (D + 31) / 32, kpi = 32 * R, nd = kpi - D;
    for (int k = 0; k < kpi; k++) {
        int col = k / R, row = k % R;
        int y0 = row * 32 + TC_P[col];
        wmap[k] = y0 >= nd ? y0 - nd : -1;
        wmap[kpi + 2 * k] = y0 >= nd ? D + (y0 - nd) : -1;
        int y2 = (TC_P[col] + 32 * row + 1) % kpi;
        wmap[kpi + 2 * k + 1] = y2 >= nd ? 2 * D + (y2 - nd) : -1;
    }
    return 3 * kpi;
}

static int rm_k0(int D, int rv)
{
    int R = (D + 31) / 32, ncb = 3 * 32 * R;
    return R * (2 * ((ncb + 8 * R - 1) / (8 * R)) * rv + 2);
}

int lte_dlsch_encode(const uint8_t *tb, int tbs, int E, int rv, uint8_t *out)
{
    int K = tbs + 24;
    if (K > 6144 || qpp_index(K) < 0)
        return -1;
    uint8_t *c = malloc((size_t)K + 8 * (size_t)(K + 4) * 3);
    int *pi = malloc(sizeof(int) * (size_t)K);
    int D = K + 4, R = (D + 31) / 32;
    int *wmap = malloc(sizeof(int) * 3 * 32 * (size_t)R);
    uint8_t *d = malloc(3 * (size_t)D);
    if (!c || !pi || !wmap || !d) {
        free(c); free(pi); free(wmap); free(d);
        return -1;
    }
    memcpy(c, tb, (size_t)tbs);
    uint32_t crc = lte_crc24a(tb, tbs);
    for (int i = 0; i < 24; i++)
        c[tbs + i] = (uint8_t)((crc >> (23 - i)) & 1);
    qpp_perm(K, pi);
    turbo_encode(c, K, pi, d, d + D, d + 2 * D);
    int ncb = rm_circ_build(D, wmap);
    int k0 = rm_k0(D, rv), j = k0, n = 0;
    while (n < E) {
        int m = wmap[j % ncb];
        if (m >= 0)
            out[n++] = d[m];
        j++;
    }
    free(c); free(pi); free(wmap); free(d);
    return E;
}

/* SISO max-log-MAP de 8 estados sobre n = K+3 etapas (las 3 últimas son la terminación). */
static void siso(int n, int K, const float *ls, const float *la, const float *lp, float *ext, float *alpha)
{
    int nst[8][2], par[8][2];      /* tabla del trellis: local (cada instancia corre en su hilo) */
    for (int s = 0; s < 8; s++)
        for (int u = 0; u < 2; u++)
            rsc_step(s, u, &nst[s][u], &par[s][u]);
    const float NEG = -1e30f;
    float gam[8][2];
    for (int s = 0; s < 8; s++)
        alpha[s] = s == 0 ? 0.f : NEG;
    for (int t = 0; t < n; t++) {
        float a = 0.5f * (ls[t] + (t < K ? la[t] : 0.f)), b = 0.5f * lp[t];
        float *an = alpha + (size_t)(t + 1) * 8, *ac = alpha + (size_t)t * 8;
        for (int s = 0; s < 8; s++)
            an[s] = NEG;
        for (int s = 0; s < 8; s++) {
            for (int u = 0; u < 2; u++) {
                if (t >= K && u != (((s >> 1) & 1) ^ (s & 1)))
                    continue;
                float g = (u ? -a : a) + (par[s][u] ? -b : b);
                float v = ac[s] + g;
                if (v > an[nst[s][u]])
                    an[nst[s][u]] = v;
            }
        }
        float m = an[0];
        for (int s = 1; s < 8; s++)
            m = fmaxf(m, an[s]);
        for (int s = 0; s < 8; s++)
            an[s] -= m;
    }
    float bn[8], bc[8];
    for (int s = 0; s < 8; s++)
        bn[s] = s == 0 ? 0.f : NEG;
    for (int t = n - 1; t >= 0; t--) {
        float a = 0.5f * (ls[t] + (t < K ? la[t] : 0.f)), b = 0.5f * lp[t];
        const float *ac = alpha + (size_t)t * 8;
        for (int s = 0; s < 8; s++)
            bc[s] = NEG;
        float l0 = NEG, l1 = NEG;
        for (int s = 0; s < 8; s++) {
            for (int u = 0; u < 2; u++) {
                if (t >= K && u != (((s >> 1) & 1) ^ (s & 1)))
                    continue;
                gam[s][u] = (u ? -a : a) + (par[s][u] ? -b : b);
                float v = gam[s][u] + bn[nst[s][u]];
                if (v > bc[s])
                    bc[s] = v;
                float w = ac[s] + v;
                if (u == 0)
                    l0 = fmaxf(l0, w);
                else
                    l1 = fmaxf(l1, w);
            }
        }
        if (t < K)
            ext[t] = (l0 - l1) - ls[t] - la[t];
        float m = bc[0];
        for (int s = 1; s < 8; s++)
            m = fmaxf(m, bc[s]);
        for (int s = 0; s < 8; s++)
            bn[s] = bc[s] - m;
    }
}

bool lte_dlsch_decode(const float *soft, int E, int tbs, int rv, int iters, uint8_t *tb)
{
    int K = tbs + 24;
    if (K > 6144 || qpp_index(K) < 0)
        return false;
    int D = K + 4, R = (D + 31) / 32;
    int *pi = malloc(sizeof(int) * (size_t)K);
    int *wmap = malloc(sizeof(int) * 3 * 32 * (size_t)R);
    float *dl = calloc(3 * (size_t)D, sizeof(float));
    size_t ns = (size_t)K + 3;
    float *bufs = malloc(sizeof(float) * (ns * 8 + (ns + 1) * 8));
    uint8_t *c = malloc((size_t)K);
    if (!pi || !wmap || !dl || !bufs || !c) {
        free(pi); free(wmap); free(dl); free(bufs); free(c);
        return false;
    }
    qpp_perm(K, pi);
    int ncb = rm_circ_build(D, wmap);
    int j = rm_k0(D, rv);
    for (int n = 0; n < E;) {
        int m = wmap[j % ncb];
        j++;
        if (m >= 0)
            dl[m] += soft[n++];
    }
    /* flujos de entrada de cada decodificador */
    float *ls1 = bufs, *ls2 = ls1 + ns, *lp1 = ls2 + ns, *lp2 = lp1 + ns, *e1 = lp2 + ns, *e2 = e1 + ns, *la1 = e2 + ns;
    float *la2 = la1 + ns;
    float *alpha = bufs + ns * 8;
    float *d0 = dl, *d1 = dl + D, *d2 = dl + 2 * D;
    for (int i = 0; i < K; i++) {
        ls1[i] = d0[i];
        ls2[i] = d0[pi[i]];
        lp1[i] = d1[i];
        lp2[i] = d2[i];
    }
    /* colas: x1 = d0[K], d2[K], d1[K+1]; z1 = d1[K], d0[K+1], d2[K+1]; x2 = d0[K+2], d2[K+2], d1[K+3];
     * z2 = d1[K+2], d0[K+3], d2[K+3] */
    ls1[K] = d0[K];     lp1[K] = d1[K];
    ls1[K + 1] = d2[K]; lp1[K + 1] = d0[K + 1];
    ls1[K + 2] = d1[K + 1]; lp1[K + 2] = d2[K + 1];
    ls2[K] = d0[K + 2];     lp2[K] = d1[K + 2];
    ls2[K + 1] = d2[K + 2]; lp2[K + 1] = d0[K + 3];
    ls2[K + 2] = d1[K + 3]; lp2[K + 2] = d2[K + 3];
    float mx = 1e-9f;
    for (int i = 0; i < 3 * D; i++)
        mx = fmaxf(mx, fabsf(dl[i]));
    float sc = 8.0f / mx;       /* LLR del orden de unidades (max-log es invariante a la escala) */
    for (size_t i = 0; i < ns; i++) {
        ls1[i] *= sc; ls2[i] *= sc; lp1[i] *= sc; lp2[i] *= sc;
    }
    memset(la1, 0, sizeof(float) * ns);
    memset(la2, 0, sizeof(float) * ns);
    bool ok = false;
    for (int it = 0; it < iters && !ok; it++) {
        siso((int)ns, K, ls1, la1, lp1, e1, alpha);
        for (int i = 0; i < K; i++)
            la2[i] = 0.75f * e1[pi[i]];
        siso((int)ns, K, ls2, la2, lp2, e2, alpha);
        for (int i = 0; i < K; i++)
            la1[pi[i]] = 0.75f * e2[i];
        for (int i = 0; i < K; i++)
            c[i] = (ls1[i] + e1[i] + la1[i]) < 0.f ? 1 : 0;
        uint32_t crc = lte_crc24a(c, tbs), rx = 0;
        for (int i = 0; i < 24; i++)
            rx = (rx << 1) | c[tbs + i];
        ok = crc == rx;
    }
    if (ok)
        memcpy(tb, c, (size_t)tbs);
    free(pi); free(wmap); free(dl); free(bufs); free(c);
    return ok;
}

/* ---------------------------------------------------------------------- */
/*  Canal de control                                                       */
/* ---------------------------------------------------------------------- */
int lte_ctrl_symbols(int nrb, int cfi) { return nrb <= 10 ? cfi + 1 : cfi; }

int lte_phich_groups(int nrb, int phich_res)
{
    static const double ng[4] = {1.0 / 6.0, 0.5, 1.0, 2.0};
    return (int)ceil(ng[phich_res & 3] * nrb / 8.0);
}

static const uint8_t PDCCH_PERM[32] = {1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
                                       0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30};

void lte_ctrl_free(lte_ctrl_t *c)
{
    free(c->regs);
    for (int i = 0; i < 3; i++)
        free(c->pdcch[i]);
    memset(c, 0, sizeof *c);
}

/* Numeración de los REG (36.211 §6.2.4): dentro de cada RB, pasadas sucesivas por los símbolos de
 * control; los símbolos con CRS (REG de 6 subportadoras con 2 CRS, "n = 2") entran en las pasadas 0 y 2,
 * los de 4 subportadoras libres ("n = 3") en las tres. */
int lte_ctrl_init(lte_ctrl_t *c, int pci, int nrb, int ports, int phich_res)
{
    memset(c, 0, sizeof *c);
    if (ports < 1 || ports > 2)
        return -1;
    c->pci = pci;
    c->nrb = nrb;
    c->ports = ports;
    c->phich_res = phich_res;
    int maxsym = nrb <= 10 ? 4 : 3, vo = pci % 3;
    int per[4] = {2, 3, 3, 3};
    int total = 0;
    for (int i = 0; i < maxsym; i++)
        total += nrb * per[i];
    c->regs = calloc((size_t)total, sizeof(lte_reg_t));
    uint8_t *assigned = calloc((size_t)total, 1);
    if (!c->regs || !assigned) {
        free(assigned);
        lte_ctrl_free(c);
        return -1;
    }
    c->nregs = total;
    int j[4] = {0, 0, 0, 0}, k = 0, i = 0, jmax = 0, prb = 0;
    while (k < total) {
        int n = per[i];
        if (n == 3 || (n == 2 && jmax != 1)) {
            lte_reg_t *r = &c->regs[k];
            r->l = i;
            int k0 = prb * 12;
            r->k0 = k0 + j[i] * (n == 2 ? 6 : 4);
            if (n == 2) {
                int b = k0 + j[i] * 6, q = 0;
                for (int a = 0; a < vo; a++)
                    r->k[q++] = b + a;
                for (int a = 0; a < 2; a++)
                    r->k[q++] = b + a + vo + 1;
                int z = q;
                for (int a = 0; a < 4 - z; a++)
                    r->k[q++] = b + vo + 3 + a + 1;
            } else {
                for (int a = 0; a < 4; a++)
                    r->k[a] = k0 + j[i] * 4 + a;
            }
            j[i]++;
            k++;
        }
        i++;
        if (i == maxsym) {
            i = 0;
            jmax++;
        }
        if (jmax == 3) {
            prb++;
            memset(j, 0, sizeof j);
            jmax = 0;
        }
    }
    /* PCFICH (36.211 §6.7.4) */
    int khat = 6 * (pci % (2 * nrb));
    for (int a = 0; a < 4; a++) {
        int kk = (khat + (a * nrb / 2) * 6) % (nrb * 12), f = -1;
        for (int r = 0; r < total; r++)
            if (c->regs[r].l == 0 && c->regs[r].k0 == kk) {
                f = r;
                break;
            }
        if (f < 0) {
            free(assigned);
            lte_ctrl_free(c);
            return -1;
        }
        c->pcfich[a] = f;
        assigned[f] = 1;
    }
    /* PHICH (36.211 §6.9.3, duración normal: todos los REG en el símbolo 0) */
    int *list0 = malloc(sizeof(int) * (size_t)total);
    int n0 = 0;
    for (int r = 0; r < total; r++)
        if (c->regs[r].l == 0 && !assigned[r])
            list0[n0++] = r;
    int ng = lte_phich_groups(nrb, phich_res);
    for (int m = 0; m < ng; m++)
        for (int a = 0; a < 3; a++) {
            int ni = ((pci * n0 / n0) + m + a * n0 / 3) % n0;
            assigned[list0[ni]] = 1;
        }
    free(list0);
    /* PDCCH: REG libres entrelazados (matriz de 32 columnas) y desplazados N_ID */
    for (int cfi = 0; cfi < 3; cfi++) {
        int ncs = lte_ctrl_symbols(nrb, cfi + 1);
        int *tmp = malloc(sizeof(int) * (size_t)total), m = 0;
        for (int r = 0; r < total; r++)
            if (c->regs[r].l < ncs && !assigned[r])
                tmp[m++] = r;
        c->pdcch[cfi] = malloc(sizeof(int) * (size_t)(m + 1));
        int nrows = (m - 1) / 32 + 1, ndummy = 32 * nrows - m, kk = 0;
        for (int col = 0; col < 32; col++)
            for (int row = 0; row < nrows; row++)
                if (row * 32 + PDCCH_PERM[col] >= ndummy) {
                    int pos = row * 32 + PDCCH_PERM[col] - ndummy;
                    int kp = ((kk - pci) % m + m) % m;
                    c->pdcch[cfi][pos] = tmp[kp];
                    kk++;
                }
        c->npdcch[cfi] = (m / 9) * 9;
        free(tmp);
    }
    free(assigned);
    return 0;
}

/* Tabla 5.3.4-1 de 36.212: CFI 1 → (0,1,1)×10 + (0,1), CFI 2 → (1,0,1)…, CFI 3 → (1,1,0)… */
void lte_pcfich_cw(int cfi, uint8_t bits[32])
{
    static const uint8_t pat[3][3] = {{0, 1, 1}, {1, 0, 1}, {1, 1, 0}};
    for (int i = 0; i < 32; i++)
        bits[i] = pat[cfi - 1][i % 3];
}

uint32_t lte_pcfich_cinit(int pci, int sf)
{
    return (uint32_t)(sf + 1) * (uint32_t)(2 * pci + 1) * 512u + (uint32_t)pci;
}
uint32_t lte_pdcch_cinit(int pci, int sf) { return (uint32_t)sf * 512u + (uint32_t)pci; }
uint32_t lte_pdsch_cinit(int pci, int sf, int rnti)
{
    return ((uint32_t)rnti << 14) + (uint32_t)sf * 512u + (uint32_t)pci;
}

/* ---------------------------------------------------------------------- */
/*  DCI                                                                    */
/* ---------------------------------------------------------------------- */
int lte_riv_bits(int n)
{
    int v = n * (n + 1) / 2, b = 0;
    while ((1 << b) < v)
        b++;
    return b;
}

static bool ambiguous_size(int n)
{
    static const int t[10] = {12, 14, 16, 20, 24, 26, 32, 40, 44, 56};
    for (int i = 0; i < 10; i++)
        if (t[i] == n)
            return true;
    return false;
}

int lte_dci1a_size(int nrb)
{
    int n = 15 + lte_riv_bits(nrb);
    return ambiguous_size(n) ? n + 1 : n;
}



int lte_riv_encode(int n, int start, int len)
{
    return (len - 1 <= n / 2) ? n * (len - 1) + start : n * (n - len + 1) + (n - 1 - start);
}

bool lte_riv_decode(int n, int riv, int *start, int *len)
{
    int l = riv / n + 1, s = riv % n;
    if (s + l > n) {
        l = n - l + 2;
        s = n - 1 - s;
    }
    if (l < 1 || s < 0 || s + l > n)
        return false;
    *start = s;
    *len = l;
    return true;
}

static void put_bits(uint8_t *b, int *p, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        b[(*p)++] = (uint8_t)((v >> i) & 1);
}
static uint32_t get_bits(const uint8_t *b, int *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (b[(*p)++] & 1);
    return v;
}

int lte_dci1a_pack(const lte_dci1a_t *d, int nrb, uint8_t *bits)
{
    int sz = lte_dci1a_size(nrb), p = 0;
    memset(bits, 0, (size_t)sz);
    put_bits(bits, &p, 1, 1);                       /* formato 1A */
    put_bits(bits, &p, d->distributed ? 1 : 0, 1);
    put_bits(bits, &p, (uint32_t)lte_riv_encode(nrb, d->rb_start, d->nprb), lte_riv_bits(nrb));
    put_bits(bits, &p, (uint32_t)d->mcs, 5);
    put_bits(bits, &p, (uint32_t)d->harq, 3);
    put_bits(bits, &p, 0, 1);                       /* NDI reservado con SI-RNTI */
    put_bits(bits, &p, (uint32_t)d->rv, 2);
    put_bits(bits, &p, d->nprb1a == 3 ? 1 : 0, 2);  /* TPC: MSB reservado, LSB = N_PRB^1A */
    return sz;
}

int lte_dci1a_unpack(const uint8_t *bits, int nbits, int nrb, lte_dci1a_t *d)
{
    memset(d, 0, sizeof *d);
    int rb = lte_riv_bits(nrb);
    if (nbits < 15 + rb || bits[0] != 1)
        return -1;
    int p = 1;
    d->distributed = get_bits(bits, &p, 1);
    int riv = (int)get_bits(bits, &p, rb);
    d->mcs = (int)get_bits(bits, &p, 5);
    d->harq = (int)get_bits(bits, &p, 3);
    int ndi = (int)get_bits(bits, &p, 1);   /* con SI-RNTI: reservado, o bit de "gap" si es distribuido y N_RB ≥ 50 */
    d->rv = (int)get_bits(bits, &p, 2);
    p++;
    d->nprb1a = get_bits(bits, &p, 1) ? 3 : 2;
    d->riv = riv;
    if (d->distributed) {
        d->gap2 = nrb >= 50 && ndi;
        d->valid = true;
        return 0;
    }
    if (!lte_riv_decode(nrb, riv, &d->rb_start, &d->nprb))
        return -1;
    d->valid = true;
    return 0;
}

/* Adaptación de velocidad del convolucional (36.212 §5.1.4.2): D = bits por flujo. map[k] = flujo·D + posición. */
static int conv_rm_map(int D, int *map)
{
    int R = (D + 31) / 32, nd = 32 * R - D, n = 0;
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < 32 * R; k++) {
            int y = PDCCH_PERM[k / R] + 32 * (k % R);
            if (y >= nd)
                map[n++] = s * D + (y - nd);
        }
    return n;
}

int lte_dci_encode(const uint8_t *dci, int a, uint16_t rnti, int E, uint8_t *out)
{
    uint8_t c[128], coded[3 * 128];
    if (a + 16 > 128)
        return -1;
    memcpy(c, dci, (size_t)a);
    uint16_t crc = (uint16_t)(lte_crc16(dci, a) ^ rnti);
    for (int i = 0; i < 16; i++)
        c[a + i] = (uint8_t)((crc >> (15 - i)) & 1);
    int D = a + 16;
    conv_code_t code = conv_lte_k7_r3();
    conv_encode(&code, c, D, coded, CONV_TAILBITING, NULL);
    int map[3 * 128], nm = conv_rm_map(D, map);
    for (int k = 0; k < E; k++) {
        int m = map[k % nm], s = m / D, t = m % D;
        out[k] = coded[3 * t + s];
    }
    return E;
}

int lte_dci_decode(const float *soft, int E, int a, uint8_t *dci)
{
    int D = a + 16;
    if (D > 128)
        return -1;
    int map[3 * 128], nm = conv_rm_map(D, map);
    float acc[3 * 128];
    memset(acc, 0, sizeof(float) * (size_t)(3 * D));
    for (int k = 0; k < E; k++)
        acc[map[k % nm]] += -soft[k];       /* convención de fec.h: positivo = bit 1 */
    float mx = 1e-9f;
    for (int i = 0; i < 3 * D; i++)
        mx = fmaxf(mx, fabsf(acc[i]));
    int8_t sv[3 * 128];
    for (int t = 0; t < D; t++)
        for (int s = 0; s < 3; s++)
            sv[3 * t + s] = (int8_t)lrintf(CLAMP(acc[s * D + t] / mx * 127.0f, -127.0f, 127.0f));
    conv_code_t code = conv_lte_k7_r3();
    viterbi_t *v = viterbi_create(&code, D);
    if (!v)
        return -1;
    uint8_t out[128];
    int r = viterbi_decode_i8(v, sv, 3 * D, D, CONV_TAILBITING, NULL, out);
    viterbi_destroy(v);
    if (r < 0)
        return -1;
    uint16_t crc = lte_crc16(out, a), rx = 0;
    for (int i = 0; i < 16; i++)
        rx = (uint16_t)((rx << 1) | out[a + i]);
    memcpy(dci, out, (size_t)a);
    return crc ^ rx;
}

/* ---------------------------------------------------------------------- */
/*  RE del PDSCH y CRS                                                     */
/* ---------------------------------------------------------------------- */
int lte_crs_k0(int pci, int port, int l)
{
    int v = (l == 0) ? (port == 0 ? 0 : 3) : (port == 0 ? 3 : 0);
    return (v + pci % 6) % 6;
}

void lte_crs_full(int pci, int ns, int l, int nrb, bool ext, cf32 *r)
{
    uint32_t cinit = (1u << 10) * (uint32_t)(7 * (ns + 1) + l + 1) * (uint32_t)(2 * pci + 1) + (uint32_t)(2 * pci) + (ext ? 0u : 1u);
    uint8_t c[4 * 110 + 8];
    lte_gold(cinit, 4 * 110, c);
    const float a = 0.70710678f;
    for (int m = 0; m < 2 * nrb; m++) {
        int mp = m + 110 - nrb;
        r[m].re = a * (float)(1 - 2 * c[2 * mp]);
        r[m].im = a * (float)(1 - 2 * c[2 * mp + 1]);
    }
}

int lte_pdsch_res_mask(int pci, int nrb, int ports, int cfi, const uint8_t mask[2][110], bool sf5_sync, int *ls,
                       int *ks, int cap)
{
    int n = 0, l0 = lte_ctrl_symbols(nrb, cfi);
    for (int l = l0; l < 14; l++) {
        int ll = l % 7;
        const uint8_t *m = mask[l / 7];
        for (int rb = 0; rb < nrb; rb++) {
            if (!m[rb])
                continue;
            for (int a = 0; a < 12; a++) {
                int k = rb * 12 + a;
                if (ll == 0 || ll == 4) {
                    bool crs = false;
                    for (int p = 0; p < ports; p++)
                        if (k % 6 == lte_crs_k0(pci, p, ll))
                            crs = true;
                    if (crs)
                        continue;
                }
                if (sf5_sync && (l == 5 || l == 6) && k >= 6 * nrb - 36 && k < 6 * nrb + 36)
                    continue;
                if (n < cap) {
                    ls[n] = l;
                    ks[n] = k;
                }
                n++;
            }
        }
    }
    return n;
}

int lte_pdsch_res(int pci, int nrb, int ports, int cfi, int rb_start, int nprb, bool sf5_sync, int *ls, int *ks,
                  int cap)
{
    uint8_t mask[2][110];
    memset(mask, 0, sizeof mask);
    for (int rb = rb_start; rb < rb_start + nprb && rb < 110; rb++)
        mask[0][rb] = mask[1][rb] = 1;
    return lte_pdsch_res_mask(pci, nrb, ports, cfi, mask, sf5_sync, ls, ks, cap);
}

/* ---------------------------------------------------------------------- */
/*  Asignación de recursos de tipo 2 (36.213 §7.1.6.3) y DCI 1C            */
/* ---------------------------------------------------------------------- */
static int rbg_p(int nrb) { return nrb <= 10 ? 1 : (nrb <= 26 ? 2 : (nrb <= 63 ? 3 : 4)); }

static int type2_ngap(int nrb, bool gap1)
{
    if (nrb <= 10) return nrb / 2;
    if (nrb == 11) return 4;
    if (nrb <= 19) return 8;
    if (nrb <= 26) return 12;
    if (nrb <= 44) return 18;
    if (nrb <= 49) return 27;
    if (nrb <= 63) return gap1 ? 27 : 9;
    if (nrb <= 79) return gap1 ? 32 : 16;
    return gap1 ? 48 : 16;
}

/* N_VRB^DL (36.211 §6.2.3.2) */
static int type2_nvrb(int nrb, bool gap1)
{
    int g = type2_ngap(nrb, gap1);
    return gap1 ? 2 * (g < nrb - g ? g : nrb - g) : (nrb / g) * 2 * g;
}

int lte_type2_nvrb(int nrb, bool gap2) { return type2_nvrb(nrb, !gap2); }

int lte_type2_prb(int nrb, bool distributed, bool gap2, int step, int riv, uint8_t mask[2][110])
{
    memset(mask, 0, 2 * 110);
    int nvrb = distributed ? type2_nvrb(nrb, !gap2) : nrb, n = nvrb / step, start, len;
    if (n < 1 || !lte_riv_decode(n, riv, &start, &len))
        return -1;
    start *= step;
    len *= step;
    if (start + len > nvrb)
        return -1;
    if (!distributed) {
        for (int i = 0; i < len; i++)
            mask[0][start + i] = mask[1][start + i] = 1;
        return len;
    }
    int P = rbg_p(nrb);
    int ntil = gap2 ? 2 * type2_nvrb(nrb, true) : type2_nvrb(nrb, true);
    int ngap = type2_ngap(nrb, !gap2);
    int nrow = ((ntil + 4 * P - 1) / (4 * P)) * P, nnull = 4 * nrow - ntil;
    for (int i = 0; i < len; i++) {
        int nv = i + start, nt = nv % ntil, base = ntil * (nv / ntil);
        int p1 = 2 * nrow * (nt % 2) + nt / 2 + base;
        int p2 = nrow * (nt % 4) + nt / 4 + base;
        int odd;
        if (nnull && nt >= ntil - nnull && (nt % 2) == 1)
            odd = p1 - nrow;
        else if (nnull && nt >= ntil - nnull && (nt % 2) == 0)
            odd = p1 - nrow + nnull / 2;
        else if (nnull && nt < ntil - nnull && (nt % 4) >= 2)
            odd = p2 - nnull / 2;
        else
            odd = p2;
        int even = (odd + ntil / 2) % ntil + base;
        int prb[2] = {odd, even};
        for (int sl = 0; sl < 2; sl++) {
            int q = prb[sl] < ntil / 2 ? prb[sl] : prb[sl] + ngap - ntil / 2;
            if (q < 0 || q >= nrb)
                return -1;
            mask[sl][q] = 1;
        }
    }
    int cnt = 0;
    for (int rb = 0; rb < nrb; rb++)
        cnt += mask[0][rb];
    return cnt;
}

static int dci1c_riv_bits(int nrb)
{
    int step = nrb < 50 ? 2 : 4;
    return lte_riv_bits(type2_nvrb(nrb, true) / step);
}

int lte_dci1c_pack(const lte_dci1a_t *d, int nrb, uint8_t *bits)
{
    int p = 0;
    if (nrb >= 50)
        put_bits(bits, &p, d->gap2 ? 1 : 0, 1);
    put_bits(bits, &p, (uint32_t)d->riv, dci1c_riv_bits(nrb));
    put_bits(bits, &p, (uint32_t)d->mcs, 5);
    return p;
}

int lte_dci1c_unpack(const uint8_t *bits, int nbits, int nrb, lte_dci1a_t *d)
{
    memset(d, 0, sizeof *d);
    int rb = dci1c_riv_bits(nrb), p = 0;
    if (nbits < rb + 5 + (nrb >= 50))
        return -1;
    d->is1c = d->distributed = true;
    if (nrb >= 50)
        d->gap2 = get_bits(bits, &p, 1);
    d->riv = (int)get_bits(bits, &p, rb);
    d->mcs = (int)get_bits(bits, &p, 5);
    d->valid = true;
    return 0;
}

/* ---------------------------------------------------------------------- */
/*  ASN.1 UPER de SIB1                                                     */
/* ---------------------------------------------------------------------- */
typedef struct {
    uint8_t *b;
    int n, cap;
    bool err;
} bw_t;

static void bw_put(bw_t *w, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (w->n >= w->cap) {
            w->err = true;
            return;
        }
        w->b[w->n++] = (uint8_t)((v >> i) & 1);
    }
}

typedef struct {
    const uint8_t *b;
    int n, p;
    bool err;
} br_t;

static uint32_t br_get(br_t *r, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (r->p >= r->n) {
            r->err = true;
            return 0;
        }
        v = (v << 1) | (r->b[r->p++] & 1);
    }
    return v;
}

static const int WIN_MS[7] = {1, 2, 5, 10, 15, 20, 40};
int lte_si_window_ms(int idx) { return idx >= 0 && idx < 7 ? WIN_MS[idx] : 0; }
int lte_si_period_rf(int idx) { return idx >= 0 && idx < 7 ? 8 << idx : 0; }

int lte_sib1_encode(const lte_sib1_t *s, uint8_t *bits, int cap)
{
    bw_t w = {bits, 0, cap, false};
    bw_put(&w, 0, 1);                       /* BCCH-DL-SCH-MessageType: c1 */
    bw_put(&w, 1, 1);                       /* c1: systemInformationBlockType1 */
    bw_put(&w, s->has_pmax ? 1 : 0, 1);     /* preámbulo: p-Max, tdd-Config, nonCriticalExtension */
    bw_put(&w, 0, 1);
    bw_put(&w, 0, 1);
    bw_put(&w, 0, 1);                       /* cellAccessRelatedInfo: csg-Identity ausente */
    bw_put(&w, (uint32_t)(s->nplmn - 1), 3);
    for (int i = 0; i < s->nplmn; i++) {
        const lte_plmn_t *p = &s->plmn[i];
        bw_put(&w, 1, 1);                   /* mcc presente */
        for (int d = 0; d < 3; d++)
            bw_put(&w, (uint32_t)p->mcc[d], 4);
        bw_put(&w, p->mnc_len == 3 ? 1 : 0, 1);
        for (int d = 0; d < p->mnc_len; d++)
            bw_put(&w, (uint32_t)p->mnc[d], 4);
        bw_put(&w, p->reserved ? 0 : 1, 1); /* {reserved, notReserved} */
    }
    bw_put(&w, (uint32_t)s->tac, 16);
    bw_put(&w, s->cell_id, 28);
    bw_put(&w, s->barred ? 0 : 1, 1);       /* {barred, notBarred} */
    bw_put(&w, s->intra_freq_not_allowed ? 1 : 0, 1);
    bw_put(&w, s->csg_ind ? 1 : 0, 1);
    bw_put(&w, 0, 1);                       /* cellSelectionInfo: q-RxLevMinOffset ausente */
    bw_put(&w, (uint32_t)(s->q_rxlevmin + 70), 6);
    if (s->has_pmax)
        bw_put(&w, (uint32_t)(s->p_max + 30), 6);
    bw_put(&w, (uint32_t)(s->band - 1), 6);
    bw_put(&w, (uint32_t)(s->nsched - 1), 5);
    for (int i = 0; i < s->nsched; i++) {
        bw_put(&w, (uint32_t)s->sched[i].period_idx, 3);
        bw_put(&w, (uint32_t)s->sched[i].nsib, 5);
        for (int j = 0; j < s->sched[i].nsib; j++) {
            bw_put(&w, 0, 1);               /* SIB-Type: marca de extensión */
            bw_put(&w, (uint32_t)(s->sched[i].sib[j] - 3), 4);
        }
    }
    bw_put(&w, (uint32_t)s->si_window_idx, 3);
    bw_put(&w, (uint32_t)s->value_tag, 5);
    return w.err ? -1 : w.n;
}

int lte_sib1_decode(const uint8_t *bits, int nbits, lte_sib1_t *s)
{
    br_t r = {bits, nbits, 0, false};
    memset(s, 0, sizeof *s);
    if (br_get(&r, 1) != 0 || br_get(&r, 1) != 1)
        return -1;
    int has_pmax = (int)br_get(&r, 1), has_tdd = (int)br_get(&r, 1);
    br_get(&r, 1);                          /* nonCriticalExtension: no se interpreta */
    int has_csg_id = (int)br_get(&r, 1);
    s->nplmn = (int)br_get(&r, 3) + 1;
    if (s->nplmn > 6)
        return -1;
    for (int i = 0; i < s->nplmn; i++) {
        lte_plmn_t *p = &s->plmn[i];
        int has_mcc = (int)br_get(&r, 1);
        if (!has_mcc)
            return -1;                      /* se exige MCC propio (el heredado exigiría la lista anterior) */
        for (int d = 0; d < 3; d++) {
            p->mcc[d] = (int)br_get(&r, 4);
            if (p->mcc[d] > 9)
                return -1;
        }
        p->mnc_len = 2 + (int)br_get(&r, 1);
        for (int d = 0; d < p->mnc_len; d++) {
            p->mnc[d] = (int)br_get(&r, 4);
            if (p->mnc[d] > 9)
                return -1;
        }
        p->reserved = br_get(&r, 1) == 0;
    }
    s->tac = (int)br_get(&r, 16);
    s->cell_id = br_get(&r, 28);
    s->barred = br_get(&r, 1) == 0;
    s->intra_freq_not_allowed = br_get(&r, 1);
    s->csg_ind = br_get(&r, 1);
    if (has_csg_id)
        br_get(&r, 27);
    int has_offset = (int)br_get(&r, 1);
    int q = (int)br_get(&r, 6) - 70;
    if (q > -22)
        return -1;
    s->q_rxlevmin = q;
    if (has_offset)
        br_get(&r, 3);
    if (has_pmax) {
        s->has_pmax = true;
        s->p_max = (int)br_get(&r, 6) - 30;
        if (s->p_max > 33)
            return -1;
    }
    s->band = (int)br_get(&r, 6) + 1;
    int nsi = (int)br_get(&r, 5) + 1;       /* hasta 32 mensajes SI: se leen todos y se guardan 8 */
    s->nsched = nsi < 8 ? nsi : 8;
    for (int i = 0; i < nsi; i++) {
        lte_sched_t tmp, *e = i < 8 ? &s->sched[i] : &tmp;
        e->period_idx = (int)br_get(&r, 3);
        if (e->period_idx > 6)
            return -1;
        e->nsib = (int)br_get(&r, 5);
        if (e->nsib > 16)
            return -1;
        for (int j = 0; j < e->nsib; j++) {
            if (br_get(&r, 1))
                return -1;
            e->sib[j] = (int)br_get(&r, 4) + 3;
        }
    }
    if (has_tdd)
        br_get(&r, 7);
    s->si_window_idx = (int)br_get(&r, 3);
    if (s->si_window_idx > 6)
        return -1;
    s->value_tag = (int)br_get(&r, 5);
    return r.err ? -1 : 0;
}

int lte_dci1c_size(int nrb) { return dci1c_riv_bits(nrb) + 5 + (nrb >= 50 ? 1 : 0); }
