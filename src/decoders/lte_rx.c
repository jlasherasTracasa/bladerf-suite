/*
 * lte_rx.c — Receptor de SIB1 (subtrama 5, FDD, CP normal, 1 ó 2 puertos). Ver lte_rx.h y lte_sib.h.
 *
 * Estimación de canal (36.211 §6.10.1): las CRS de cada puerto ocupan una de cada 6 subportadoras
 * en los símbolos 0 y 4 de cada ranura (símbolos 0, 4, 7 y 11 de la subtrama). En cada símbolo
 * piloto se obtiene H = Y·conj(r) en los 2·N_RB pilotos. Un error de temporización τ (en
 * muestras) mete en H una rampa de fase 2π·6τ/N por piloto: si se interpola linealmente con la
 * rampa puesta, entre pilotos queda un error de módulo (aliasing); por eso se estima la rampa con
 * arg Σ H_m·conj(H_{m-1}), se retira, se suaviza (1-2-1) e interpola y se vuelve a poner. En
 * tiempo se interpola linealmente entre los símbolos piloto. Los símbolos de datos se combinan
 * con MRC (un puerto) o con la decodificación de Alamouti (SFBC, dos puertos; 36.211 §6.3.4.3):
 *   x0 = h0*·r0 + h1·r1*,   x1 = h0*·r1 − h1·r0*
 * donde (r0, r1) son los dos RE contiguos del par y h0, h1 los canales de los puertos 0 y 1.
 * El valor blando de cada bit es la parte real/imaginaria de x (no se normaliza por |h|²: el
 * LLR óptimo de QPSK es proporcional a Re(h*·r)/σ²).
 */
#include "lte_rx.h"

#include "lte_phy.h"
#include "../dsp/fft.h"

#include <stdlib.h>
#include <string.h>

#define MAXN 2048

typedef struct {
    const lte_rxcfg_t *cfg;
    int nsc;                    /* 12·N_RB */
    int sf;
    int sm;                     /* suavizado en frecuencia: 1 = (1,2,1)/4, 2 = (1,2,3,2,1)/9 */
    cf32 *Y;                    /* [14][nsc] */
    cf32 *Hp[2];                /* [4][nsc] por puerto: canal en los símbolos piloto */
} rx_t;

static const int PIL_L[4] = {0, 4, 7, 11};

static inline cf32 cmul(cf32 a, cf32 b) { return (cf32){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
static inline cf32 cmulc(cf32 a, cf32 b) { return (cf32){a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im}; }

/* Subportadora k (0 = la más baja) → frecuencia en unidades de 15 kHz (sin DC) → bin. */
static inline int k_to_bin(int k, int nrb, int N)
{
    int f = k < 6 * nrb ? k - 6 * nrb : k - 6 * nrb + 1;
    return f >= 0 ? f : N + f;
}

static void sym_geometry(int N, int l, double *start, double *cp)
{
    double S = N / 128.0;
    int slot = l / 7, ll = l % 7;
    *start = slot * 960.0 * S + (ll == 0 ? 0.0 : (138.0 + (ll - 1) * 137.0) * S);
    *cp = (ll == 0 ? 10.0 : 9.0) * S;
}

static bool demod(rx_t *r, const cf32 *x, int nx, double t0, fft_plan_t *plan)
{
    int N = r->cfg->fft, nrb = r->cfg->nrb;
    double bias = 4.0 * N / 128.0;
    cf32 buf[MAXN];
    for (int l = 0; l < 14; l++) {
        double st, cp;
        sym_geometry(N, l, &st, &cp);
        double s0 = t0 + st + cp - bias;
        int is = (int)floor(s0);
        if (is < 0 || is + N > nx)
            return false;
        double sh = bias + (s0 - is);
        memcpy(buf, x + is, sizeof(cf32) * (size_t)N);
        fft_exec(plan, buf);
        for (int k = 0; k < r->nsc; k++) {
            int f = k < 6 * nrb ? k - 6 * nrb : k - 6 * nrb + 1;
            double a = 2.0 * M_PI * f * sh / N;
            r->Y[(size_t)l * r->nsc + k] = cmul(buf[k_to_bin(k, nrb, N)], (cf32){(float)cos(a), (float)sin(a)});
        }
    }
    return true;
}

/* Canal en los símbolos piloto para el puerto p (todas las subportadoras). */
static void chan_est(rx_t *r, int p)
{
    int nrb = r->cfg->nrb, nsc = r->nsc, np = 2 * nrb;
    cf32 *rs = malloc(sizeof(cf32) * (size_t)np), *H = malloc(sizeof(cf32) * (size_t)np);
    cf32 *Hs = malloc(sizeof(cf32) * (size_t)np);
    for (int pi = 0; pi < 4; pi++) {
        int l = PIL_L[pi], ll = l % 7, ns = 2 * r->sf + l / 7;
        lte_crs_full(r->cfg->pci, ns, ll, nrb, false, rs);
        int k0 = lte_crs_k0(r->cfg->pci, p, ll);
        for (int m = 0; m < np; m++)
            H[m] = cmulc(r->Y[(size_t)l * nsc + 6 * m + k0], rs[m]);
        cf32 z = {0, 0};
        for (int m = 1; m < np; m++) {
            cf32 v = cmulc(H[m], H[m - 1]);
            z.re += v.re;
            z.im += v.im;
        }
        double phi = atan2(z.im, z.re);
        for (int m = 0; m < np; m++) {
            double a = -phi * m;
            H[m] = cmul(H[m], (cf32){(float)cos(a), (float)sin(a)});
        }
        static const float W1[5] = {0, 1, 2, 1, 0}, W2[5] = {1, 2, 3, 2, 1};
        const float *W = r->sm >= 2 ? W2 : W1;
        float ws = W[0] + W[1] + W[2] + W[3] + W[4];
        for (int m = 0; m < np; m++) {
            float re = 0, im = 0;
            for (int t = -2; t <= 2; t++) {
                int mm = m + t < 0 ? 0 : (m + t > np - 1 ? np - 1 : m + t);
                re += W[t + 2] * H[mm].re;
                im += W[t + 2] * H[mm].im;
            }
            Hs[m] = (cf32){re / ws, im / ws};
        }
        cf32 *Hp = r->Hp[p] + (size_t)pi * nsc;
        for (int k = 0; k < nsc; k++) {
            double u = (k - k0) / 6.0;
            double uc = u < 0 ? 0 : (u > np - 1 ? np - 1 : u);
            int m = (int)floor(uc);
            if (m > np - 2)
                m = np - 2;
            if (m < 0)
                m = 0;
            double w = uc - m;
            cf32 h = np > 1 ? (cf32){(float)(Hs[m].re * (1 - w) + Hs[m + 1].re * w),
                                     (float)(Hs[m].im * (1 - w) + Hs[m + 1].im * w)}
                            : Hs[0];
            double a = phi * u;
            Hp[k] = cmul(h, (cf32){(float)cos(a), (float)sin(a)});
        }
    }
    free(rs);
    free(H);
    free(Hs);
}

static inline cf32 hat(const rx_t *r, int p, int l, int k)
{
    const int nsc = r->nsc;
    int i = 0;
    while (i < 3 && l >= PIL_L[i + 1])
        i++;
    const cf32 *a = r->Hp[p] + (size_t)i * nsc + k;
    if (l >= PIL_L[3])
        return *a;
    const cf32 *b = r->Hp[p] + (size_t)(i + 1) * nsc + k;
    float w = (float)(l - PIL_L[i]) / (float)(PIL_L[i + 1] - PIL_L[i]);
    return (cf32){a->re * (1 - w) + b->re * w, a->im * (1 - w) + b->im * w};
}

static inline cf32 yat(const rx_t *r, int l, int k) { return r->Y[(size_t)l * r->nsc + k]; }

/* Demodula una secuencia de n RE (n par con 2 puertos) → 2n valores blandos (positivo = bit 0). */
static void soft_res(const rx_t *r, const int *ls, const int *ks, int n, float *soft)
{
    int ports = r->cfg->ports;
    for (int i = 0; i < n; i++) {
        if (ports == 2 && i + 1 < n && (i & 1) == 0 && ls[i] == ls[i + 1]) {
            cf32 y0 = yat(r, ls[i], ks[i]), y1 = yat(r, ls[i + 1], ks[i + 1]);
            cf32 ha = hat(r, 0, ls[i], ks[i]), hb = hat(r, 0, ls[i + 1], ks[i + 1]);
            cf32 ga = hat(r, 1, ls[i], ks[i]), gb = hat(r, 1, ls[i + 1], ks[i + 1]);
            cf32 h0 = {(ha.re + hb.re) * 0.5f, (ha.im + hb.im) * 0.5f}, h1 = {(ga.re + gb.re) * 0.5f, (ga.im + gb.im) * 0.5f};
            cf32 x0 = cmulc(y0, h0), t = cmul(h1, (cf32){y1.re, -y1.im});
            x0.re += t.re;
            x0.im += t.im;
            cf32 x1 = cmulc(y1, h0);
            t = cmul(h1, (cf32){y0.re, -y0.im});
            x1.re -= t.re;
            x1.im -= t.im;
            soft[2 * i] = x0.re;
            soft[2 * i + 1] = x0.im;
            soft[2 * i + 2] = x1.re;
            soft[2 * i + 3] = x1.im;
            i++;
        } else {
            cf32 x = cmulc(yat(r, ls[i], ks[i]), hat(r, 0, ls[i], ks[i]));
            soft[2 * i] = x.re;
            soft[2 * i + 1] = x.im;
        }
    }
}

/* QPSK de la modulación del control: bit 0 ↔ signo + (como lte_pbch_decode). Desaleatoriza. */
static void descramble(float *soft, int n, uint32_t cinit)
{
    uint8_t *c = malloc((size_t)n);
    lte_gold(cinit, n, c);
    for (int i = 0; i < n; i++)
        if (c[i])
            soft[i] = -soft[i];
    free(c);
}

static int decode_pcfich(const rx_t *r, const lte_ctrl_t *c, double *margin)
{
    int ls[16], ks[16], n = 0;
    for (int a = 0; a < 4; a++)
        for (int i = 0; i < 4; i++) {
            ls[n] = 0;
            ks[n] = c->regs[c->pcfich[a]].k[i];
            n++;
        }
    float soft[32];
    soft_res(r, ls, ks, 16, soft);
    descramble(soft, 32, lte_pcfich_cinit(r->cfg->pci, r->sf));
    double best = -1e30, second = -1e30;
    int cfi = 0;
    for (int f = 1; f <= 3; f++) {
        uint8_t cw[32];
        lte_pcfich_cw(f, cw);
        double s = 0;
        for (int i = 0; i < 32; i++)
            s += cw[i] ? -soft[i] : soft[i];
        if (s > best) {
            second = best;
            best = s;
            cfi = f;
        } else if (s > second) {
            second = s;
        }
    }
    double tot = 0;
    for (int i = 0; i < 32; i++)
        tot += fabs(soft[i]);
    (void)second;
    *margin = best / (tot + 1e-9);   /* 1 = los 32 bits concuerdan; ≈ 0,3 con ruido */
    return best > 0 ? cfi : 0;
}

/* Con un CFI supuesto: PDCCH (búsqueda ciega de DCI con SI-RNTI) y PDSCH. 1 si decodifica SIB1. */
static int try_cfi(const rx_t *r, const lte_ctrl_t *ctl, int cfi, lte_sib1_t *out, lte_rx_info_t *info)
{
    const lte_rxcfg_t *cfg = r->cfg;
    /* --- PDCCH: valores blandos de todos los REG, desaleatorizados --- */
    int nreg = ctl->npdcch[cfi - 1], nsym = 4 * nreg;
    int ret = 0;
    int *ls = NULL, *ks = NULL;
    uint8_t *tb = NULL;
    float *soft = NULL;
    int *pls = malloc(sizeof(int) * (size_t)nsym), *pks = malloc(sizeof(int) * (size_t)nsym);
    float *psoft = malloc(sizeof(float) * 2 * (size_t)nsym);
    for (int q = 0; q < nreg; q++) {
        const lte_reg_t *g = &ctl->regs[ctl->pdcch[cfi - 1][q]];
        for (int i = 0; i < 4; i++) {
            pls[4 * q + i] = g->l;
            pks[4 * q + i] = g->k[i];
        }
    }
    soft_res(r, pls, pks, nsym, psoft);
    descramble(psoft, 2 * nsym, lte_pdcch_cinit(cfg->pci, r->sf));
    free(pls);
    free(pks);
    pls = pks = NULL;
    int ncce = nreg / 9;
    int sizes[3][2] = {{1, lte_dci1a_size(cfg->nrb)}, {1, 15 + lte_riv_bits(cfg->nrb)}, {2, lte_dci1c_size(cfg->nrb)}};
    lte_dci1a_t d = {0};
    bool found = false;
    for (int L = 4; L <= 8 && !found; L *= 2)
        for (int m = 0; m < (ncce < 16 ? ncce : 16) / L && !found; m++) {
            int cce = L * m;
            if (cce + L > ncce)
                continue;
            for (int s = 0; s < 3 && !found; s++) {
                if (s == 1 && sizes[1][1] == sizes[0][1])
                    continue;
                uint8_t dci[128];
                int rn = lte_dci_decode(psoft + 72 * cce, 72 * L, sizes[s][1], dci);
                if (rn != LTE_SI_RNTI)
                    continue;
                if (sizes[s][0] == 1 ? lte_dci1a_unpack(dci, sizes[s][1], cfg->nrb, &d) < 0
                                     : lte_dci1c_unpack(dci, sizes[s][1], cfg->nrb, &d) < 0)
                    continue;
                info->dci_fmt = sizes[s][0];
                info->dci_cce = cce;
                info->dci_l = L;
                found = true;
            }
        }
    free(psoft);
    psoft = NULL;
    if (!found || !d.valid)
        goto fin;
    info->mcs = d.mcs;
    info->rv = d.rv;
    info->rb_start = d.rb_start;
    info->nprb = d.nprb;
    info->nprb1a = d.nprb1a;
    info->distributed = d.distributed;
    int rv_sfn = (int[]){0, 2, 3, 1}[(cfg->sfn / 2) & 3];
    int tbs_c[2] = {0, 0};
    uint8_t mask[2][110];
    if (d.is1c) {
        tbs_c[0] = lte_tbs_1c_get(d.mcs);
        d.rv = rv_sfn;
        info->rv = rv_sfn;
        if (lte_type2_prb(cfg->nrb, true, d.gap2, cfg->nrb < 50 ? 2 : 4, d.riv, mask) < 0)
            goto fin;
    } else {
        if (d.mcs > 9)
            goto fin;      /* 16QAM: no es SIB1 */
        tbs_c[0] = lte_tbs_qpsk_get(d.mcs, d.nprb1a);
        int np;
        if (d.distributed)
            np = lte_type2_prb(cfg->nrb, true, d.gap2, 1, d.riv, mask);
        else {
            memset(mask, 0, sizeof mask);
            for (int rb = d.rb_start; rb < d.rb_start + d.nprb; rb++)
                mask[0][rb] = mask[1][rb] = 1;
            np = d.nprb;
        }
        if (np < 0)
            goto fin;
        tbs_c[1] = lte_tbs_qpsk_get(d.mcs, np);
        info->nprb = np;
    }

    /* --- PDSCH --- */
    int cap = 14 * r->nsc;
    ls = malloc(sizeof(int) * (size_t)cap);
    ks = malloc(sizeof(int) * (size_t)cap);
    int nre = lte_pdsch_res_mask(cfg->pci, cfg->nrb, cfg->ports, cfi, mask, true, ls, ks, cap);
    soft = malloc(sizeof(float) * 2 * (size_t)nre);
    soft_res(r, ls, ks, nre, soft);
    descramble(soft, 2 * nre, lte_pdsch_cinit(cfg->pci, r->sf, LTE_SI_RNTI));
    tb = malloc(6200);
    for (int i = 0; i < 2 && !ret; i++) {
        if (tbs_c[i] <= 0 || (i == 1 && tbs_c[1] == tbs_c[0]))
            continue;
        info->tbs_tried++;
        if (lte_dlsch_decode(soft, 2 * nre, tbs_c[i], d.rv, 8, tb) && lte_sib1_decode(tb, tbs_c[i], out) == 0) {
            info->tbs = tbs_c[i];
            ret = 1;
        }
    }
fin:
    free(psoft);
    free(pls);
    free(pks);
    free(tb);
    free(soft);
    free(ls);
    free(ks);
    return ret;
}

static int rx_once(const lte_rxcfg_t *cfg, const cf32 *x, int nx, double t0, lte_sib1_t *out, lte_rx_info_t *info, int sm)
{
    memset(info, 0, sizeof *info);
    if (cfg->fft > MAXN || cfg->ports < 1 || cfg->ports > 2)
        return 0;
    rx_t r = {.cfg = cfg, .nsc = 12 * cfg->nrb, .sf = cfg->sf_set ? cfg->sf : 5, .sm = sm};
    fft_plan_t *plan = fft_plan_create(cfg->fft);
    lte_ctrl_t ctl;
    int ret = 0;
    r.Y = malloc(sizeof(cf32) * 14 * (size_t)r.nsc);
    for (int p = 0; p < 2; p++)
        r.Hp[p] = calloc((size_t)4 * (size_t)r.nsc, sizeof(cf32));
    bool ctl_ok = false;
    if (!plan || !r.Y || !r.Hp[0] || !r.Hp[1])
        goto done;
    if (!demod(&r, x, nx, t0, plan)) {
        ret = -1;
        goto done;
    }
    for (int p = 0; p < cfg->ports; p++)
        chan_est(&r, p);
    if (lte_ctrl_init(&ctl, cfg->pci, cfg->nrb, cfg->ports, cfg->phich_res) < 0)
        goto done;
    ctl_ok = true;
    int cfi = decode_pcfich(&r, &ctl, &info->pcfich_margin);
    info->cfi = cfi;
    if (!cfi)
        cfi = 3;      /* PCFICH ilegible: se prueban todos */

    static const int order[3][3] = {{1, 2, 3}, {2, 1, 3}, {3, 1, 2}};
    for (int i = 0; i < 3 && !ret; i++) {
        int c1 = order[cfi - 1][i];
        if (i > 0 && info->pcfich_margin > 0.8)
            break;          /* el PCFICH es fiable: no se prueban los otros CFI */
        ret = try_cfi(&r, &ctl, c1, out, info);
        if (ret)
            info->cfi = c1;
    }
done:
    if (ctl_ok)
        lte_ctrl_free(&ctl);
    free(r.Y);
    free(r.Hp[0]);
    free(r.Hp[1]);
    fft_plan_destroy(plan);
    return ret;
}

/* Primera pasada con el suavizado ligero; si no se halla ni un DCI de SI-RNTI (celda débil o
 * canal casi plano), segunda con suavizado fuerte del canal (gana unos 2 dB con pilotos ruidosos). */
int lte_rx_sib1(const lte_rxcfg_t *cfg, const cf32 *x, int nx, double t0, lte_sib1_t *out, lte_rx_info_t *info)
{
    int r = rx_once(cfg, x, nx, t0, out, info, 1);
    if (r != 0 || info->dci_fmt)
        return r;
    lte_rx_info_t i2;
    r = rx_once(cfg, x, nx, t0, out, &i2, 2);
    if (r > 0 || i2.dci_fmt)
        *info = i2;
    return r;
}
