/*
 * fec.c — Implementación de FEC. Ver fec.h para la teoría y las convenciones.
 */
#include "fec.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/*  Convolucional                                                          */
/* ======================================================================= */
static conv_code_t mk(int k, int n, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t inv)
{
    conv_code_t cc = {k, n, {a, b, c, d}, inv};
    return cc;
}
conv_code_t conv_nasa_k7(void) { return mk(7, 2, 0171, 0133, 0, 0, 0); }
conv_code_t conv_ccsds_k7(void) { return mk(7, 2, 0171, 0133, 0, 0, 2); }
conv_code_t conv_gsm_k5(void) { return mk(5, 2, 023, 033, 0, 0, 0); }
conv_code_t conv_dab_k7(void) { return mk(7, 4, 0133, 0171, 0145, 0133, 0); }
conv_code_t conv_lte_k7_r3(void) { return mk(7, 3, 0133, 0171, 0165, 0, 0); }
conv_code_t conv_k9_r2(void) { return mk(9, 2, 0561, 0753, 0, 0, 0); }
conv_code_t conv_k9_r3(void) { return mk(9, 3, 0557, 0663, 0711, 0, 0); }

/* Patrones de punzonado DVB (ETSI EN 300 744 tabla 13); orden [paso][X,Y]. */
static const uint8_t P23[] = {1, 1, 0, 1};
static const uint8_t P34[] = {1, 1, 0, 1, 1, 0};
static const uint8_t P56[] = {1, 1, 0, 1, 1, 0, 0, 1, 1, 0};
static const uint8_t P78[] = {1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0};
static const conv_punct_t PUNCT[] = {
    {2, 2, P23}, {3, 2, P34}, {5, 2, P56}, {7, 2, P78},
};

const conv_punct_t *conv_punct_dvb(const char *rate)
{
    if (!strcmp(rate, "2/3")) return &PUNCT[0];
    if (!strcmp(rate, "3/4")) return &PUNCT[1];
    if (!strcmp(rate, "5/6")) return &PUNCT[2];
    if (!strcmp(rate, "7/8")) return &PUNCT[3];
    return NULL;
}

/* Salida del paso para el registro sr (K bits, entrada actual en el MSB). */
static inline unsigned conv_out(const conv_code_t *c, uint32_t sr)
{
    unsigned o = 0;
    for (int j = 0; j < c->n; j++)
        o |= (unsigned)(__builtin_parity(sr & c->poly[j]) ^ ((c->invert >> j) & 1)) << j;
    return o;
}

int conv_encode(const conv_code_t *c, const uint8_t *in, int n, uint8_t *out, conv_term_t term,
                const conv_punct_t *p)
{
    int K = c->k, nstate_mask = (1 << (K - 1)) - 1, w = 0, step = 0;
    uint32_t state = 0;
    if (term == CONV_TAILBITING) {
        if (n < K - 1)
            return -1;
        for (int i = 0; i < K - 1; i++)
            state |= (uint32_t)(in[n - 1 - i] & 1) << (K - 2 - i);
    }
    int total = n + (term == CONV_TERM_ZERO ? K - 1 : 0);
    for (int t = 0; t < total; t++, step++) {
        uint32_t b = t < n ? (in[t] & 1) : 0;
        uint32_t sr = (b << (K - 1)) | state;
        unsigned o = conv_out(c, sr);
        for (int j = 0; j < c->n; j++)
            if (!p || p->keep[(step % p->period) * p->n + j])
                out[w++] = (uint8_t)((o >> j) & 1);
        state = (sr >> 1) & (uint32_t)nstate_mask;
    }
    return w;
}

/* ======================================================================= */
/*  Viterbi                                                                */
/* ======================================================================= */
#define TB_MULT 6   /* pasos de prefijo/sufijo en tail-biting = TB_MULT·K */

struct viterbi {
    conv_code_t code;
    int ns;                 /* nº de estados 2^(K-1) */
    int words;              /* palabras de 64 bits de decisión por paso */
    int max_steps;
    uint8_t pat[512];       /* patrón de salida por registro (K bits) */
    float pm[2][256];
    uint64_t *dec;          /* max_steps · words */
};

viterbi_t *viterbi_create(const conv_code_t *code, int max_info_bits)
{
    if (code->k < 2 || code->k > 9 || code->n < 2 || code->n > 4 || max_info_bits < 1)
        return NULL;
    viterbi_t *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->code = *code;
    v->ns = 1 << (code->k - 1);
    v->words = v->ns > 64 ? v->ns / 64 : 1;
    v->max_steps = max_info_bits + code->k - 1 + 2 * TB_MULT * code->k;
    v->dec = calloc((size_t)v->max_steps * (size_t)v->words, sizeof(uint64_t));
    if (!v->dec) {
        free(v);
        return NULL;
    }
    for (int sr = 0; sr < (1 << code->k); sr++)
        v->pat[sr] = (uint8_t)conv_out(code, (uint32_t)sr);
    return v;
}

void viterbi_destroy(viterbi_t *v)
{
    if (v) {
        free(v->dec);
        free(v);
    }
}

typedef struct {
    const int8_t *i8;
    const float *f;
    int nsoft;
    const conv_punct_t *p;
    int n_code;             /* salidas por paso */
    int kept_pp;            /* símbolos transmitidos por periodo */
    int prefix[65];         /* símbolos transmitidos antes del paso t del periodo */
} symsrc_t;

static inline float sym_at(const symsrc_t *s, int idx)
{
    return s->i8 ? (float)s->i8[idx] * (1.0f / 127.0f) : s->f[idx];
}

/* Símbolos del paso lógico u (0..nsteps-1) tras reinsertar borrados. Devuelve
 * false si faltan datos. */
static bool sym_fetch(const symsrc_t *s, int u, float *out)
{
    int n = s->n_code;
    if (!s->p) {
        if ((u + 1) * n > s->nsoft)
            return false;
        for (int j = 0; j < n; j++)
            out[j] = sym_at(s, u * n + j);
        return true;
    }
    int per = s->p->period, ph = u % per;
    int idx = (u / per) * s->kept_pp + s->prefix[ph];
    for (int j = 0; j < n; j++) {
        if (s->p->keep[ph * n + j]) {
            if (idx >= s->nsoft)
                return false;
            out[j] = sym_at(s, idx++);
        } else {
            out[j] = 0.0f;
        }
    }
    return true;
}

static int vit_run(viterbi_t *v, symsrc_t *src, int n_info, conv_term_t term, uint8_t *out_bits)
{
    const int K = v->code.k, NS = v->ns, n = v->code.n, W = v->words;
    int tail = term == CONV_TERM_ZERO ? K - 1 : 0;
    int L = term == CONV_TAILBITING ? MIN(n_info, TB_MULT * K) : 0;
    int total = L + n_info + tail + L;   /* pasos ejecutados */
    if (n_info < 1 || total > v->max_steps)
        return -1;
    if (src->p && (src->p->n != n || src->p->period < 1 || src->p->period > 64))
        return -1;
    if (term == CONV_TAILBITING && n_info < K - 1)
        return -1;
    if (src->p) {
        int acc = 0;
        for (int t = 0; t < src->p->period; t++) {
            src->prefix[t] = acc;
            for (int j = 0; j < n; j++)
                acc += src->p->keep[t * n + j] ? 1 : 0;
        }
        src->prefix[src->p->period] = acc;
        src->kept_pp = acc;
    }
    src->n_code = n;
    /* Se comprueba de antemano que hay símbolos para todos los pasos de datos. */
    float tmp[4];
    if (!sym_fetch(src, n_info + tail - 1, tmp))
        return -1;

    float *pm = v->pm[0], *nm = v->pm[1];
    if (term == CONV_TAILBITING) {
        for (int s = 0; s < NS; s++)
            pm[s] = 0.0f;
    } else {
        for (int s = 0; s < NS; s++)
            pm[s] = -1e9f;
        pm[0] = 0.0f;
    }
    const uint8_t *pat = v->pat;
    const int hi = 1 << (K - 1), nsm = NS - 1;
    for (int t = 0; t < total; t++) {
        int u = t < L ? n_info - L + t : (t < L + n_info + tail ? t - L : t - L - n_info - tail);
        float sym[4] = {0, 0, 0, 0};
        if (u < n_info + tail)
            sym_fetch(src, u, sym);
        /* métricas de rama de los 2^n patrones */
        float bm[16];
        for (int p = 0; p < (1 << n); p++) {
            float m = 0;
            for (int j = 0; j < n; j++)
                m += ((p >> j) & 1) ? sym[j] : -sym[j];
            bm[p] = m;
        }
        uint64_t *d = v->dec + (size_t)t * (size_t)W;
        for (int w = 0; w < W; w++)
            d[w] = 0;
        bool forced = t >= L + n_info && term == CONV_TERM_ZERO;   /* entrada 0 forzada en la cola */
        for (int ns2 = 0; ns2 < NS; ns2++) {
            int b = ns2 >> (K - 2);
            if (forced && b) {
                nm[ns2] = -1e9f;
                continue;
            }
            int s0 = (ns2 << 1) & nsm, s1 = s0 | 1;
            int base = b ? hi : 0;
            float m0 = pm[s0] + bm[pat[base | s0]];
            float m1 = pm[s1] + bm[pat[base | s1]];
            if (m1 > m0) {
                nm[ns2] = m1;
                d[ns2 >> 6] |= 1ULL << (ns2 & 63);
            } else {
                nm[ns2] = m0;
            }
        }
        /* normalización para que las métricas no crezcan sin límite */
        float ref = nm[0];
        for (int s = 0; s < NS; s++)
            nm[s] -= ref;
        float *sw = pm; pm = nm; nm = sw;
    }
    /* estado final */
    int state = 0;
    if (term != CONV_TERM_ZERO) {
        float best = pm[0];
        for (int s = 1; s < NS; s++)
            if (pm[s] > best) {
                best = pm[s];
                state = s;
            }
    }
    /* traceback + recuento de discrepancias en la parte útil */
    int mism = 0;
    for (int t = total - 1; t >= 0; t--) {
        const uint64_t *d = v->dec + (size_t)t * (size_t)W;
        int bit = (int)((d[state >> 6] >> (state & 63)) & 1);
        int b = state >> (K - 2);
        int prev = ((state << 1) & nsm) | bit;
        int u = t < L ? -1 : (t < L + n_info + tail ? t - L : -1);
        if (u >= 0) {
            if (u < n_info)
                out_bits[u] = (uint8_t)b;
            float sym[4];
            if (sym_fetch(src, u, sym)) {
                unsigned o = pat[(b ? hi : 0) | prev];
                for (int j = 0; j < n; j++)
                    if (sym[j] != 0.0f && (sym[j] > 0) != (int)((o >> j) & 1))
                        mism++;
            }
        }
        state = prev;
    }
    return mism;
}

int viterbi_decode_i8(viterbi_t *v, const int8_t *soft, int nsoft, int n_info, conv_term_t term,
                      const conv_punct_t *punct, uint8_t *out_bits)
{
    symsrc_t s;
    memset(&s, 0, sizeof s);
    s.i8 = soft; s.nsoft = nsoft; s.p = punct;
    return vit_run(v, &s, n_info, term, out_bits);
}

int viterbi_decode_f(viterbi_t *v, const float *soft, int nsoft, int n_info, conv_term_t term,
                     const conv_punct_t *punct, uint8_t *out_bits)
{
    symsrc_t s;
    memset(&s, 0, sizeof s);
    s.f = soft; s.nsoft = nsoft; s.p = punct;
    return vit_run(v, &s, n_info, term, out_bits);
}

/* ======================================================================= */
/*  Reed-Solomon GF(2^8)                                                   */
/* ======================================================================= */
static inline uint8_t gmul(const rs_t *r, uint8_t a, uint8_t b)
{
    return (a && b) ? r->exp[r->log[a] + r->log[b]] : 0;
}
static inline uint8_t gdiv(const rs_t *r, uint8_t a, uint8_t b)   /* b ≠ 0 */
{
    return a ? r->exp[r->log[a] + 255 - r->log[b]] : 0;
}
static inline uint8_t gpow_alpha(const rs_t *r, long e)           /* α^e, e puede ser negativo */
{
    e %= 255;
    if (e < 0)
        e += 255;
    return r->exp[e];
}

static int gcd(int a, int b) { return b ? gcd(b, a % b) : a; }

int rs_init(rs_t *rs, int gfpoly, int fcr, int prim, int nroots)
{
    if (nroots < 1 || nroots > 128 || gcd(prim, 255) != 1 || prim < 1 || gfpoly < 0x100 || gfpoly > 0x1FF)
        return -1;
    memset(rs, 0, sizeof *rs);
    rs->nroots = nroots; rs->fcr = fcr; rs->prim = prim; rs->gfpoly = gfpoly;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        rs->exp[i] = (uint8_t)x;
        rs->log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= gfpoly;
        if (x == 1 && i < 254)
            return -1;      /* orden < 255: no primitivo */
    }
    if (x != 1)
        return -1;
    for (int i = 255; i < 512; i++)
        rs->exp[i] = rs->exp[i - 255];
    /* g(x) = Π (x + α^{(fcr+i)·prim}); g[j] = coef. de x^(deg-j) */
    uint8_t g[130];
    int deg;
    memset(g, 0, sizeof g);
    g[0] = 1;
    deg = 0;
    for (int i = 0; i < nroots; i++) {
        uint8_t root = gpow_alpha(rs, (long)(fcr + i) * prim), ng[130];
        memset(ng, 0, sizeof ng);
        for (int j = 0; j <= deg; j++) {
            ng[j] ^= g[j];                              /* ·x */
            ng[j + 1] ^= gmul(rs, g[j], root);          /* ·root */
        }
        deg++;
        memcpy(g, ng, sizeof g);
    }
    for (int i = 0; i < nroots; i++)
        rs->gen[i] = g[i + 1];
    return 0;
}

void rs_encode(const rs_t *rs, const uint8_t *data, int ndata, uint8_t *parity)
{
    int nr = rs->nroots;
    memset(parity, 0, (size_t)nr);
    for (int i = 0; i < ndata; i++) {
        uint8_t fb = data[i] ^ parity[0];
        memmove(parity, parity + 1, (size_t)nr - 1);
        parity[nr - 1] = 0;
        if (fb)
            for (int j = 0; j < nr; j++)
                parity[j] ^= gmul(rs, fb, rs->gen[j]);
    }
}

static void rs_syndromes(const rs_t *rs, const uint8_t *blk, int nb, uint8_t *s)
{
    for (int j = 0; j < rs->nroots; j++) {
        uint8_t root = gpow_alpha(rs, (long)(rs->fcr + j) * rs->prim), acc = 0;
        for (int i = 0; i < nb; i++)
            acc = (uint8_t)(gmul(rs, acc, root) ^ blk[i]);
        s[j] = acc;
    }
}

int rs_decode(const rs_t *rs, uint8_t *block, int nblock, const int *erasures, int n_era)
{
    const int nr = rs->nroots;
    if (nblock < nr + 1 || nblock > 255 || n_era < 0 || n_era > nr)
        return -1;
    uint8_t s[130], orig[255];
    rs_syndromes(rs, block, nblock, s);
    bool any = false;
    for (int j = 0; j < nr; j++)
        any |= s[j] != 0;
    if (!any)
        return 0;     /* palabra válida (los borrados acertaron o no hacían falta) */

    /* Λ(x) inicial = localizador de borrados Π(1 + X_e·x), X = α^{prim·(nblock-1-i)} */
    uint8_t lam[132] = {1}, bpoly[132] = {1};
    for (int e = 0; e < n_era; e++) {
        if (erasures[e] < 0 || erasures[e] >= nblock)
            return -1;
        uint8_t X = gpow_alpha(rs, (long)rs->prim * (nblock - 1 - erasures[e]));
        for (int j = e + 1; j >= 1; j--)
            lam[j] ^= gmul(rs, lam[j - 1], X);
    }
    memcpy(bpoly, lam, sizeof bpoly);
    int el = n_era;
    /* Berlekamp-Massey con borrados (como libfec) */
    for (int r = n_era + 1; r <= nr; r++) {
        uint8_t discr = 0;
        for (int i = 0; i < r; i++)
            discr ^= gmul(rs, lam[i], s[r - 1 - i]);
        uint8_t t[132];
        if (!discr) {
            memmove(bpoly + 1, bpoly, 131);
            bpoly[0] = 0;
            continue;
        }
        t[0] = lam[0];
        for (int i = 0; i < 131; i++)
            t[i + 1] = (uint8_t)(lam[i + 1] ^ gmul(rs, discr, bpoly[i]));
        if (2 * el <= r + n_era - 1) {
            el = r + n_era - el;
            for (int i = 0; i < 132; i++)
                bpoly[i] = gdiv(rs, lam[i], discr);
        } else {
            memmove(bpoly + 1, bpoly, 131);
            bpoly[0] = 0;
        }
        memcpy(lam, t, 132);
    }
    int deg = 0;
    for (int i = 0; i <= nr && i < 132; i++)
        if (lam[i])
            deg = i;
    if (deg == 0 || deg > nr)
        return -1;
    /* Chien: busca i tales que Λ(X_i^{-1}) = 0 */
    int loc[130], nloc = 0;
    uint8_t xinv[130];
    for (int i = 0; i < nblock; i++) {
        long pwr = nblock - 1 - i;
        uint8_t xi = gpow_alpha(rs, -(long)rs->prim * pwr), acc = 0;
        for (int j = deg; j >= 0; j--)
            acc = (uint8_t)(gmul(rs, acc, xi) ^ lam[j]);
        if (!acc) {
            if (nloc >= 130)
                return -1;
            xinv[nloc] = xi;
            loc[nloc++] = i;
        }
    }
    if (nloc != deg)
        return -1;
    /* Ω(x) = S(x)Λ(x) mod x^deg */
    uint8_t om[132] = {0};
    for (int i = 0; i < deg; i++)
        for (int j = 0; j <= i; j++)
            om[i] ^= gmul(rs, s[i - j], lam[j]);
    memcpy(orig, block, (size_t)nblock);
    for (int k = 0; k < nloc; k++) {
        uint8_t xi = xinv[k], num = 0, den = 0;
        for (int j = deg - 1; j >= 0; j--)
            num = (uint8_t)(gmul(rs, num, xi) ^ om[j]);
        /* Λ'(x) = Σ_{j impar} λ_j x^{j-1} */
        for (int j = (deg % 2 ? deg : deg - 1); j >= 1; j -= 2) {
            uint8_t term = lam[j];
            for (int q = 0; q < j - 1; q++)
                term = gmul(rs, term, xi);
            den ^= term;
        }
        if (!den) {
            memcpy(block, orig, (size_t)nblock);
            return -1;
        }
        long pwr = nblock - 1 - loc[k];
        uint8_t xk1f = gpow_alpha(rs, (long)rs->prim * pwr * (1 - rs->fcr));   /* X^{1−fcr} */
        uint8_t val = gmul(rs, xk1f, gdiv(rs, num, den));
        block[loc[k]] ^= val;
    }
    /* verificación final: los síndromes deben anularse */
    rs_syndromes(rs, block, nblock, s);
    for (int j = 0; j < nr; j++)
        if (s[j]) {
            memcpy(block, orig, (size_t)nblock);
            return -1;
        }
    return nloc;
}

int rs_init_ccsds(rs_t *rs) { return rs_init(rs, 0x187, 112, 11, 32); }
int rs_init_rs41(rs_t *rs) { return rs_init(rs, 0x11D, 0, 1, 24); }
int rs_init_vdl2(rs_t *rs) { return rs_init(rs, 0x187, 120, 1, 6); }
int rs_init_dvb(rs_t *rs) { return rs_init(rs, 0x11D, 0, 1, 16); }
int rs_init_dab(rs_t *rs) { return rs_init(rs, 0x11D, 0, 1, 10); }

/* ======================================================================= */
/*  BCH binario                                                            */
/* ======================================================================= */
static inline int bmul(const bch_t *b, int x, int y)
{
    return (x && y) ? b->exp[b->log[x] + b->log[y]] : 0;
}

int bch_init(bch_t *b, int m, int prim_poly, int t)
{
    if (m < 3 || m > 10 || t < 1)
        return -1;
    memset(b, 0, sizeof *b);
    b->m = m; b->n = (1 << m) - 1; b->t = t;
    int x = 1, n = b->n;
    for (int i = 0; i < n; i++) {
        b->exp[i] = (int16_t)x;
        b->log[x] = (int16_t)i;
        x <<= 1;
        if (x & (1 << m))
            x ^= prim_poly;
        if (x == 1 && i < n - 1)
            return -1;
    }
    if (x != 1)
        return -1;
    for (int i = n; i < 2 * n + 2 && i < 2048; i++)
        b->exp[i] = b->exp[i - n];
    /* raíces: unión de los conjuntos ciclotómicos de 1..2t */
    uint8_t is_root[1024] = {0};
    for (int i = 1; i <= 2 * t; i++) {
        int j = i % n;
        if (is_root[j])
            continue;
        do {
            is_root[j] = 1;
            j = (j * 2) % n;
        } while (j != i % n);
    }
    int poly[340] = {1}, deg = 0;
    for (int r = 1; r < n; r++) {
        if (!is_root[r])
            continue;
        if (deg + 1 >= 336)
            return -1;
        int root = b->exp[r], np[340] = {0};
        for (int j = 0; j <= deg; j++) {      /* poly[j] = coef. de x^j */
            np[j + 1] ^= poly[j];
            np[j] ^= bmul(b, poly[j], root);
        }
        deg++;
        memcpy(poly, np, sizeof np);
    }
    for (int j = 0; j <= deg; j++) {
        if (poly[j] > 1)
            return -1;      /* debería ser binario */
        b->gen[j] = (uint8_t)poly[j];
    }
    b->np = deg;
    b->k = b->n - deg;
    if (b->k < 1)
        return -1;
    return 0;
}

int bch_init_pocsag(bch_t *b) { return bch_init(b, 5, 0x25, 2); }

void bch_encode(const bch_t *b, const uint8_t *data, int nbits_data, uint8_t *parity)
{
    int np = b->np;
    memset(parity, 0, (size_t)np);
    for (int i = 0; i < nbits_data; i++) {
        uint8_t fb = (uint8_t)((data[i] ^ parity[0]) & 1);
        memmove(parity, parity + 1, (size_t)np - 1);
        parity[np - 1] = 0;
        if (fb)
            for (int j = 0; j < np; j++)
                parity[j] ^= b->gen[np - 1 - j];
    }
}

int bch_decode(const bch_t *b, uint8_t *bits, int nbits)
{
    const int t = b->t, n = b->n, T2 = 2 * t;
    if (nbits < b->np + 1 || nbits > n)
        return -1;
    int s[2 * 40 + 2];
    if (T2 > 80)
        return -1;
    bool any = false;
    for (int j = 1; j <= T2; j++) {
        int root = b->exp[j % n], acc = 0;
        for (int i = 0; i < nbits; i++)
            acc = bmul(b, acc, root) ^ (bits[i] & 1);
        s[j] = acc;
        any |= acc != 0;
    }
    if (!any)
        return 0;
    /* Berlekamp-Massey */
    int C[90] = {1}, B[90] = {1}, T[90];
    int L = 0, m = 1, bb = 1;
    for (int nn = 0; nn < T2; nn++) {
        int d = s[nn + 1];
        for (int i = 1; i <= L; i++)
            d ^= bmul(b, C[i], s[nn + 1 - i]);
        if (!d) {
            m++;
            continue;
        }
        int coef = bmul(b, d, b->exp[(n - b->log[bb]) % n]);   /* d / bb */
        memcpy(T, C, sizeof T);
        for (int i = 0; i + m < 90; i++)
            C[i + m] ^= bmul(b, coef, B[i]);
        if (2 * L <= nn) {
            L = nn + 1 - L;
            memcpy(B, T, sizeof B);
            bb = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (L > t)
        return -1;
    int found = 0, pos[40];
    for (int i = 0; i < nbits; i++) {
        int pw = nbits - 1 - i;                /* bit i ↔ x^pw */
        int xi = b->exp[(n - pw % n) % n], acc = 0;   /* α^{-pw} */
        for (int j = L; j >= 0; j--)
            acc = bmul(b, acc, xi) ^ C[j];
        if (!acc) {
            if (found >= 40)
                return -1;
            pos[found++] = i;
        }
    }
    if (found != L)
        return -1;
    for (int i = 0; i < found; i++)
        bits[pos[i]] ^= 1;
    return found;
}

uint32_t bch_encode_u32(const bch_t *b, uint32_t data, int nbits_data)
{
    uint8_t d[32], p[336];
    for (int i = 0; i < nbits_data; i++)
        d[i] = (uint8_t)((data >> (nbits_data - 1 - i)) & 1);
    bch_encode(b, d, nbits_data, p);
    uint32_t w = data;
    for (int i = 0; i < b->np; i++)
        w = (w << 1) | p[i];
    return w;
}

int bch_decode_u32(const bch_t *b, uint32_t *word, int nbits)
{
    if (nbits > 32 || nbits < 1)
        return -1;
    uint8_t d[32];
    for (int i = 0; i < nbits; i++)
        d[i] = (uint8_t)((*word >> (nbits - 1 - i)) & 1);
    int r = bch_decode(b, d, nbits);
    if (r > 0) {
        uint32_t w = 0;
        for (int i = 0; i < nbits; i++)
            w = (w << 1) | d[i];
        *word = w;
    }
    return r;
}

/* ======================================================================= */
/*  Golay                                                                  */
/* ======================================================================= */
#define GOLAY_G 0xC75u

static uint32_t golay_rem(uint32_t v)       /* v mod g(x), v de hasta 23 bits */
{
    for (int i = 22; i >= 11; i--)
        if (v & (1u << i))
            v ^= GOLAY_G << (i - 11);
    return v & 0x7FF;
}

uint32_t golay23_encode(uint32_t data12)
{
    uint32_t d = (data12 & 0xFFF) << 11;
    return d | golay_rem(d);
}

uint32_t golay24_encode(uint32_t data12)
{
    uint32_t c = golay23_encode(data12);
    return c | ((uint32_t)__builtin_parity(c) << 23);
}

void golay_init(golay_t *g)
{
    memset(g, 0, sizeof *g);
    /* Código perfecto: los 1+23+253+1771 = 2048 patrones de ≤ 3 errores tienen
     * síndromes distintos. */
    for (int a = -1; a < 23; a++)
        for (int b = a; b < 23; b++)
            for (int c = b; c < 23; c++) {
                uint32_t e = 0;
                int idx[3] = {a, b, c};
                bool ok = true;
                for (int q = 0; q < 3; q++) {
                    if (idx[q] < 0)
                        continue;
                    if (e & (1u << idx[q]))
                        ok = false;
                    e |= 1u << idx[q];
                }
                if (!ok)
                    continue;
                g->err[golay_rem(e)] = e;
            }
}

int golay23_decode(const golay_t *g, uint32_t *cw)
{
    uint32_t e = g->err[golay_rem(*cw & 0x7FFFFF)];
    *cw = (*cw ^ e) & 0x7FFFFF;
    return __builtin_popcount(e);
}

int golay24_decode(const golay_t *g, uint32_t *cw)
{
    uint32_t c23 = *cw & 0x7FFFFF, pbit = (*cw >> 23) & 1;
    uint32_t e = g->err[golay_rem(c23)];
    c23 ^= e;
    int fixed = __builtin_popcount(e);
    if ((uint32_t)__builtin_parity(c23) != pbit)
        fixed++;               /* el bit de paridad también estaba mal */
    if (fixed > 3)
        return -1;
    *cw = c23 | ((uint32_t)__builtin_parity(c23) << 23);
    return fixed;
}

/* ======================================================================= */
/*  Hamming                                                                */
/* ======================================================================= */
int hamming_init_cols(hamming_t *h, int k, int r, int ext, const uint16_t *cols)
{
    if (k < 1 || k > 26 || r < 2 || r > 8 || ext < 0 || ext > 1)
        return -1;
    memset(h, 0, sizeof *h);
    h->k = k; h->r = r; h->ext = ext; h->n = k + r + ext;
    for (int s = 0; s < 256; s++)
        h->pos_of[s] = -1;
    for (int i = 0; i < k; i++) {
        uint16_t c = cols[i];
        if (!c || c >= (1u << r) || h->pos_of[c] >= 0)
            return -1;
        h->col[i] = c;
        h->pos_of[c] = (int16_t)i;
    }
    for (int j = 0; j < r; j++) {
        uint16_t c = (uint16_t)(1u << j);
        if (h->pos_of[c] >= 0)
            return -1;
        h->pos_of[c] = (int16_t)(k + j);   /* p_j está en el índice k+j */
    }
    return 0;
}

int hamming_init(hamming_t *h, int k, int r, int ext)
{
    uint16_t cols[32];
    int n = 0;
    for (int v = 3; v < (1 << r) && n < k; v++)
        if (v & (v - 1))
            cols[n++] = (uint16_t)v;
    if (n < k)
        return -1;
    return hamming_init_cols(h, k, r, ext, cols);
}

int hamming_init_7_4(hamming_t *h) { return hamming_init(h, 4, 3, 0); }
int hamming_init_13_9(hamming_t *h) { return hamming_init(h, 9, 4, 0); }
int hamming_init_15_11(hamming_t *h) { return hamming_init(h, 11, 4, 0); }
int hamming_init_16_11(hamming_t *h) { return hamming_init(h, 11, 4, 1); }

uint32_t hamming_encode(const hamming_t *h, uint32_t data)
{
    uint32_t par = 0;
    for (int i = 0; i < h->k; i++)
        if ((data >> (h->k - 1 - i)) & 1)
            par ^= h->col[i];
    /* p0 es el primero transmitido: va al bit más alto del campo de paridad */
    uint32_t pf = 0;
    for (int j = 0; j < h->r; j++)
        pf |= ((par >> j) & 1u) << (h->r - 1 - j);
    uint32_t cw = (data << (h->r + h->ext)) | (pf << h->ext);
    if (h->ext)
        cw |= (uint32_t)__builtin_parity(cw >> 1);
    return cw;
}

int hamming_decode(const hamming_t *h, uint32_t *cw)
{
    uint32_t w = *cw >> h->ext, syn = 0;
    for (int i = 0; i < h->k; i++)
        if ((w >> (h->k + h->r - 1 - i)) & 1)
            syn ^= h->col[i];
    for (int j = 0; j < h->r; j++)
        if ((w >> (h->r - 1 - j)) & 1)
            syn ^= 1u << j;
    int odd = h->ext ? __builtin_parity(*cw & ((h->n >= 32) ? 0xFFFFFFFFu : ((1u << h->n) - 1))) : 1;
    if (!syn) {
        if (h->ext && odd) {
            *cw ^= 1u;      /* error solo en la paridad global */
            return 1;
        }
        return 0;
    }
    if (h->ext && !odd)
        return -1;           /* número par de errores: doble, detectado */
    int p = h->pos_of[syn];
    if (p < 0)
        return -1;           /* síndrome fuera de la palabra acortada */
    int bit_from_lsb = (h->n - 1 - p);
    *cw ^= 1u << bit_from_lsb;
    return 1;
}

/* ======================================================================= */
/*  Bloque lineal                                                          */
/* ======================================================================= */
int linblock_init(linblock_t *lb, int n, int k, const uint32_t *rows)
{
    if (k < 1 || k > 12 || n < k || n > 32)
        return -1;
    lb->n = n; lb->k = k;
    lb->dmin = n + 1;
    for (uint32_t d = 0; d < (1u << k); d++) {
        uint32_t c = 0;
        for (int i = 0; i < k; i++)
            if ((d >> (k - 1 - i)) & 1)
                c ^= rows[i];
        lb->cw[d] = c;
        if (d && __builtin_popcount(c) < lb->dmin)
            lb->dmin = __builtin_popcount(c);
    }
    lb->t = (lb->dmin - 1) / 2;
    return 0;
}

uint32_t linblock_encode(const linblock_t *lb, uint32_t data) { return lb->cw[data & ((1u << lb->k) - 1)]; }

int linblock_decode(const linblock_t *lb, uint32_t *cw, uint32_t *data)
{
    int best = 99, ties = 0;
    uint32_t bd = 0;
    for (uint32_t d = 0; d < (1u << lb->k); d++) {
        int dist = __builtin_popcount(*cw ^ lb->cw[d]);
        if (dist < best) {
            best = dist; bd = d; ties = 0;
        } else if (dist == best) {
            ties++;
        }
    }
    if (best > lb->t || ties)
        return -1;
    *cw = lb->cw[bd];
    if (data)
        *data = bd;
    return best;
}

int linblock_init_qr16_7(linblock_t *lb)
{
    /* QR(17,9,5): g(x) = x^8+x^5+x^4+x^3+1 (factor de x^17−1). Palabra de 16
     * bits = 7 datos | 8 paridades | paridad global (tras acortar 2 datos). */
    uint32_t rows[7];
    for (int i = 0; i < 7; i++) {
        uint32_t m = (1u << (6 - i)) << 8;         /* x^(6−i)·x^8 */
        uint32_t r = m;
        for (int b = 16; b >= 8; b--)
            if (r & (1u << b))
                r ^= 0x139u << (b - 8);
        uint32_t c17 = m | r;                       /* 7 datos en bits 14..8, paridad 7..0 */
        rows[i] = (c17 << 1) | (uint32_t)__builtin_parity(c17);
    }
    return linblock_init(lb, 16, 7, rows);
}
