/*
 * mod_lte.c — Modulador LTE (señal de enlace descendente de una celda, ancho completo).
 *
 * Genera lo que una celda difundiría en todo su ancho (1,4 / 3 / 5 / 10 / 15 / 20 MHz = 6 / 15 /
 * 25 / 50 / 75 / 100 RB) a la tasa de muestreo estándar de ese ancho (1,92 / 3,84 / 7,68 / 15,36 /
 * 15,36 / 30,72 Msps; FFT de 128·2^m, SCS de 15 kHz):
 *   - PSS y SSS (FDD: símbolos 6 y 5 de las ranuras 0 y 10; TDD: SSS al final de las subtramas
 *     0/5 y PSS en el tercer símbolo de las subtramas 1/6) en las 72 subportadoras centrales,
 *   - CRS de los puertos 0 (y 1 con "ports":2) en todo el ancho, con la rejilla desplazada N_ID mod 6,
 *   - PBCH con una MIB real (36.331): ancho de banda, PHICH y SFN, con CRC-16 enmascarado por el nº
 *     de puertos, convolucional tail-biting 1/3, adaptación de velocidad, aleatorización y QPSK
 *     (SFBC de Alamouti con 2 puertos),
 *   - SIB1 (FDD, CP normal) en la subtrama 5 de las tramas pares: PCFICH (CFI), PDCCH con un DCI 1A
 *     de SI-RNTI en el CCE 0 y PDSCH con el mensaje SIB1 real (ASN.1 UPER de 36.331 §6.2.2), CRC-24A,
 *     turbocódigo, adaptación de velocidad (RV según la trama, 36.331 §5.2.1.2), aleatorización y
 *     QPSK (SFBC con 2 puertos),
 *   - resto de RE: QPSK aleatorio (tráfico ficticio) a la misma potencia.
 * Con 2 puertos se suman las dos antenas a través de un canal plano h = [1, 0,7·e^{j0,9}] para que el
 * receptor tenga que estimarlos por separado.
 *
 * Parámetros JSON (todos opcionales):
 *   {"pci":123,"cp":"normal|extended","duplex":"fdd|tdd","ports":1|2,
 *    "sfn":0,"frames":6,"bw_rb":6|15|25|50|75|100,"phich_res":2,"phich_ext":0,"seed":1,
 *    "sib1":true,"mcc":"214","mnc":"07","tac":4660,"cell_id":19088743,"band":20,"cfi":0,
 *    "rb_start":0,"nprb":0,"mcs":-1,"dci":"1a|1c","os":1}
 * ("os": 1, 2 ó 4 sobremuestrea la salida (FFT mayor con las mismas subportadoras) para probar capturas a más tasa que la del ancho.)
 * ("dci":"1c": DCI 1C con asignación distribuida, como hacen algunas redes reales; "nprb" = nº de
 * unidades de N_step PRB).
 *
 * Limitaciones: el PBCH con 4 puertos (SFBC+FSTD) no está implementado; con TDD o CP extendido no se
 * genera SIB1; todas las subtramas TDD se tratan como descendentes.
 * air_ok = false: banda celular licenciada (política §6).
 */
#include "mod.h"

#include "../decoders/lte_phy.h"
#include "../decoders/lte_sib.h"
#include "../dsp/fft.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int pci, ports, sfn, frames, bw_rb, phich_res, phich_ext, seed;
    bool ext, tdd, sib1, dci1c;
    char mcc[8], mnc[8];
    int tac, band, cfi, rb_start, nprb, mcs, os;
    uint32_t cell_id;
} params_t;

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static cf32 qpsk_random(uint32_t *s)
{
    uint32_t r = rnd(s);
    const float a = 0.70710678f;
    return (cf32){(r & 1) ? -a : a, (r & 2) ? -a : a};
}

static int parse(const char *js, params_t *p, char *err, size_t errlen)
{
    *p = (params_t){.pci = 301, .ports = 1, .sfn = 0, .frames = 6, .bw_rb = 6, .phich_res = 2, .seed = 1,
                    .sib1 = true, .tac = 4660, .band = 20, .cell_id = 0x1234567, .mcs = -1, .os = 1};
    strcpy(p->mcc, "001");
    strcpy(p->mnc, "01");
    if (!js || !*js)
        return 0;
    jtok_t t[96];
    int nt = json_parse(js, strlen(js), t, 96);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "parámetros JSON no válidos");
        return -1;
    }
    double v;
    char s[32];
    int i;
#define NUM(k, f) if ((i = json_obj_get(js, t, 0, k)) >= 0 && json_num(js, t, i, &v)) f
    NUM("pci", p->pci = (int)v);
    NUM("ports", p->ports = (int)v);
    NUM("sfn", p->sfn = (int)v);
    NUM("frames", p->frames = (int)v);
    NUM("bw_rb", p->bw_rb = (int)v);
    NUM("phich_res", p->phich_res = (int)v);
    NUM("phich_ext", p->phich_ext = (int)v);
    NUM("seed", p->seed = (int)v);
    NUM("tac", p->tac = (int)v);
    NUM("cell_id", p->cell_id = (uint32_t)v);
    NUM("band", p->band = (int)v);
    NUM("cfi", p->cfi = (int)v);
    NUM("rb_start", p->rb_start = (int)v);
    NUM("nprb", p->nprb = (int)v);
    NUM("mcs", p->mcs = (int)v);
    NUM("os", p->os = (int)v);
#undef NUM
    if ((i = json_obj_get(js, t, 0, "cp")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->ext = s[0] == 'e' || s[0] == 'E';
    if ((i = json_obj_get(js, t, 0, "duplex")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->tdd = s[0] == 't' || s[0] == 'T';
    if ((i = json_obj_get(js, t, 0, "mcc")) >= 0 && json_str(js, t, i, p->mcc, sizeof p->mcc)) {}
    if ((i = json_obj_get(js, t, 0, "mnc")) >= 0 && json_str(js, t, i, p->mnc, sizeof p->mnc)) {}
    bool bv;
    if ((i = json_obj_get(js, t, 0, "sib1")) >= 0 && json_bool(js, t, i, &bv))
        p->sib1 = bv;
    if ((i = json_obj_get(js, t, 0, "dci")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->dci1c = s[0] == '1' && (s[1] == 'c' || s[1] == 'C');
    if (p->pci < 0 || p->pci > 503 || p->ports < 1 || p->ports > 2 || p->frames < 1 || p->frames > 400) {
        snprintf(err, errlen, "pci 0..503, ports 1|2, frames 1..400");
        return -1;
    }
    if (p->bw_rb != 6 && p->bw_rb != 15 && p->bw_rb != 25 && p->bw_rb != 50 && p->bw_rb != 75 && p->bw_rb != 100) {
        snprintf(err, errlen, "bw_rb debe ser 6, 15, 25, 50, 75 o 100");
        return -1;
    }
    if (p->os != 1 && p->os != 2 && p->os != 4) {
        snprintf(err, errlen, "os (sobremuestreo) 1, 2 ó 4");
        return -1;
    }
    if (strlen(p->mcc) != 3 || (strlen(p->mnc) != 2 && strlen(p->mnc) != 3)) {
        snprintf(err, errlen, "mcc de 3 cifras y mnc de 2 ó 3");
        return -1;
    }
    return 0;
}

static int fft_for(int nrb)
{
    switch (nrb) {
    case 6: return 128;
    case 15: return 256;
    case 25: return 512;
    case 100: return 2048;
    default: return 1024;
    }
}

/* Rejilla de una subtrama: [puerto][símbolo 0..13][12·N_RB] */
typedef struct {
    int nsc, nrb, ports;
    cf32 *g[2];
} grid_t;

#define G(gr, p, l, k) ((gr)->g[p][(size_t)(l) * (size_t)(gr)->nsc + (size_t)(k)])

/* ventana j (0..71) de las 72 centrales → subportadora k */
static inline int kj(const grid_t *gr, int j) { return 6 * gr->nrb - 36 + j; }

static void put_pss_sss(grid_t *gr, const params_t *p, int ns)
{
    int nsym = lte_nsym(p->ext);
    int n_id1 = p->pci / 3, n_id2 = p->pci % 3;
    int half = ns >= 10;
    int sss_l = -1, pss_l = -1;
    if (!p->tdd) {
        if (ns % 10 == 0) {
            pss_l = nsym - 1;
            sss_l = nsym - 2;
        }
    } else {
        if (ns == 2 || ns == 12)
            pss_l = 2;
        if (ns == 1 || ns == 11)
            sss_l = nsym - 1;
    }
    int lbase = (ns & 1) * nsym;    /* G cubre la subtrama: la ranura impar empieza tras nsym símbolos */
    if (pss_l >= 0) {
        cf32 d[62];
        lte_pss_seq(n_id2, d);
        for (int port = 0; port < p->ports; port++)
            for (int j = 0; j < 72; j++)
                G(gr, port, lbase + pss_l, kj(gr, j)) = (cf32){0, 0};
        for (int n = 0; n < 62; n++)
            G(gr, 0, lbase + pss_l, kj(gr, n + 5)) = d[n];
    }
    if (sss_l >= 0) {
        int8_t d[62];
        lte_sss_seq(n_id1, n_id2, half, d);
        for (int port = 0; port < p->ports; port++)
            for (int j = 0; j < 72; j++)
                G(gr, port, lbase + sss_l, kj(gr, j)) = (cf32){0, 0};
        for (int n = 0; n < 62; n++)
            G(gr, 0, lbase + sss_l, kj(gr, n + 5)) = (cf32){(float)d[n], 0};
    }
}

static void put_crs(grid_t *gr, const params_t *p, int ns)
{
    int nsym = lte_nsym(p->ext);
    int ls[2] = {0, nsym - 3};
    cf32 *r = malloc(sizeof(cf32) * (size_t)(2 * gr->nrb));
    int lbase = (ns & 1) * nsym;
    for (int li = 0; li < 2; li++) {
        int l = ls[li];
        lte_crs_full(p->pci, ns, l, gr->nrb, p->ext, r);
        for (int port = 0; port < p->ports; port++) {
            int k0 = lte_crs_k0(p->pci, port, l);
            for (int m = 0; m < 2 * gr->nrb; m++) {
                G(gr, port, lbase + l, 6 * m + k0) = r[m];
                if (p->ports == 2)      /* la otra antena deja vacío ese RE */
                    G(gr, 1 - port, lbase + l, 6 * m + k0) = (cf32){0, 0};
            }
        }
    }
    free(r);
}

/* Pone 4 símbolos en 4 RE de un símbolo OFDM: un puerto directo; dos puertos con SFBC por pares
 * (a0, a1, −a1*, a0*) con la potencia repartida entre antenas (36.211 §6.3.4.3). */
static void put_sfbc(grid_t *gr, int l, const int *k, const cf32 *d, int n)
{
    if (gr->ports == 1) {
        for (int i = 0; i < n; i++)
            G(gr, 0, l, k[i]) = d[i];
        return;
    }
    const float s = 0.70710678f;
    for (int i = 0; i + 1 < n; i += 2) {
        cf32 x0 = d[i], x1 = d[i + 1];
        G(gr, 0, l, k[i]) = (cf32){x0.re * s, x0.im * s};
        G(gr, 0, l, k[i + 1]) = (cf32){x1.re * s, x1.im * s};
        G(gr, 1, l, k[i]) = (cf32){-x1.re * s, x1.im * s};
        G(gr, 1, l, k[i + 1]) = (cf32){x0.re * s, -x0.im * s};
    }
}

static cf32 qpsk_bits(int b0, int b1)
{
    const float a = 0.70710678f;
    return (cf32){a * (float)(1 - 2 * b0), a * (float)(1 - 2 * b1)};
}

static void put_pbch(grid_t *gr, const params_t *p, int sfn)
{
    lte_mib_t mib = {.bw_rb = p->bw_rb, .phich_ext = p->phich_ext, .phich_res = p->phich_res, .sfn8 = (sfn >> 2) & 255};
    uint8_t bits[1920];
    lte_pbch_encode(&mib, p->ports, p->pci, p->ext, bits);
    int eseg = p->ext ? 432 : 480, seg = sfn & 3;
    int lsym[288], jj[288];
    int n = lte_pbch_res(p->pci, p->ext, lsym, jj);
    cf32 d[288];
    for (int i = 0; i < n; i++) {
        const uint8_t *b = bits + seg * eseg + 2 * i;
        d[i] = qpsk_bits(b[0], b[1]);
    }
    for (int i = 0; i < n; i++) {
        int l = lte_nsym(p->ext) + lsym[i];    /* ranura 1 de la subtrama 0 */
        int k = kj(gr, jj[i]);
        if (p->ports == 1) {
            G(gr, 0, l, k) = d[i];
        } else {
            int a0 = i & ~1, b0 = a0 + 1;           /* par (2i, 2i+1): Alamouti */
            cf32 x0 = d[a0], x1 = d[b0];
            const float s = 0.70710678f;
            if (i == a0) {
                G(gr, 0, l, k) = (cf32){x0.re * s, x0.im * s};
                G(gr, 1, l, k) = (cf32){-x1.re * s, x1.im * s};   /* −x1* */
            } else {
                G(gr, 0, l, k) = (cf32){x1.re * s, x1.im * s};
                G(gr, 1, l, k) = (cf32){x0.re * s, -x0.im * s};   /* x0* */
            }
        }
    }
}

/* PCFICH + PDCCH (DCI 1A con SI-RNTI en el CCE 0) + PDSCH (SIB1) de la subtrama 5. */
static int put_sib1(grid_t *gr, const params_t *p, int sfn, const lte_ctrl_t *ctl, char *err, size_t errlen)
{
    int nrb = p->bw_rb, cfi = p->cfi ? p->cfi : (nrb == 6 ? 3 : 2);
    /* mensaje SIB1 */
    lte_sib1_t sb = {0};
    sb.nplmn = 1;
    for (int d = 0; d < 3; d++)
        sb.plmn[0].mcc[d] = p->mcc[d] - '0';
    sb.plmn[0].mnc_len = (int)strlen(p->mnc);
    for (int d = 0; d < sb.plmn[0].mnc_len; d++)
        sb.plmn[0].mnc[d] = p->mnc[d] - '0';
    sb.tac = p->tac & 0xFFFF;
    sb.cell_id = p->cell_id & 0xFFFFFFF;
    sb.q_rxlevmin = -65;
    sb.has_pmax = true;
    sb.p_max = 23;
    sb.band = p->band;
    sb.nsched = 2;
    sb.sched[0] = (lte_sched_t){0, 1, 1, {3}};
    sb.sched[1] = (lte_sched_t){0, 2, 2, {4, 5}};
    sb.si_window_idx = 5;
    uint8_t msg[400];
    int nbits = lte_sib1_encode(&sb, msg, 400);
    int mcs = p->mcs, tbs = 0;
    for (int m = 0; m < (p->dci1c ? 32 : 10); m++) {
        int t = p->dci1c ? lte_tbs_1c_get(m) : lte_tbs_qpsk_get(m, 3);
        if ((p->mcs < 0 || m == p->mcs) && t >= nbits) {
            mcs = m;
            tbs = t;
            break;
        }
    }
    if (!tbs) {
        snprintf(err, errlen, "SIB1 no cabe con ese MCS");
        return -1;
    }
    uint8_t tb[1800];
    memset(tb, 0, sizeof tb);
    memcpy(tb, msg, (size_t)nbits);
    int nprb = p->nprb ? p->nprb : (p->dci1c ? 2 : (nrb == 6 ? 4 : 6));
    int rb0 = p->rb_start;
    uint8_t mask[2][110];
    lte_dci1a_t dci = {.mcs = mcs, .rv = (int[]){0, 2, 3, 1}[(sfn / 2) & 3], .nprb1a = 3, .rb_start = rb0, .nprb = nprb};
    if (p->dci1c) {
        int step = nrb < 50 ? 2 : 4, nn = lte_type2_nvrb(nrb, false) / step;
        if (nprb > nn || lte_riv_encode(nn, rb0 < nn ? rb0 : 0, nprb) < 0) {
            snprintf(err, errlen, "asignación 1C fuera del ancho de banda");
            return -1;
        }
        dci.riv = lte_riv_encode(nn, rb0 < nn ? rb0 : 0, nprb);
        if (lte_type2_prb(nrb, true, false, step, dci.riv, mask) < 0) {
            snprintf(err, errlen, "asignación 1C no válida");
            return -1;
        }
    } else {
        if (rb0 + nprb > nrb) {
            snprintf(err, errlen, "asignación fuera del ancho de banda");
            return -1;
        }
        memset(mask, 0, sizeof mask);
        for (int rb = rb0; rb < rb0 + nprb; rb++)
            mask[0][rb] = mask[1][rb] = 1;
    }
    /* PCFICH */
    {
        uint8_t cw[32], sc[32];
        lte_pcfich_cw(cfi, cw);
        lte_gold(lte_pcfich_cinit(p->pci, 5), 32, sc);
        cf32 d[16];
        for (int i = 0; i < 16; i++)
            d[i] = qpsk_bits(cw[2 * i] ^ sc[2 * i], cw[2 * i + 1] ^ sc[2 * i + 1]);
        for (int a = 0; a < 4; a++)
            put_sfbc(gr, 0, ctl->regs[ctl->pcfich[a]].k, d + 4 * a, 4);
    }
    /* PDCCH */
    {
        int nreg = ctl->npdcch[cfi - 1], ncce = nreg / 9, L = ncce >= 4 ? 4 : 0;
        if (!L) {
            snprintf(err, errlen, "el control no tiene 4 CCE");
            return -1;
        }
        uint8_t db[64];
        int a = p->dci1c ? lte_dci1c_pack(&dci, nrb, db) : lte_dci1a_pack(&dci, nrb, db);
        int nsym = 4 * nreg;
        uint8_t *bits = calloc((size_t)(2 * nsym), 1), *sc = malloc((size_t)(2 * nsym));
        lte_dci_encode(db, a, LTE_SI_RNTI, 72 * L, bits);
        lte_gold(lte_pdcch_cinit(p->pci, 5), 2 * nsym, sc);
        for (int q = 0; q < nreg; q++) {
            cf32 d[4];
            for (int i = 0; i < 4; i++) {
                int n = 4 * q + i;
                d[i] = qpsk_bits(bits[2 * n] ^ sc[2 * n], bits[2 * n + 1] ^ sc[2 * n + 1]);
            }
            const lte_reg_t *rg = &ctl->regs[ctl->pdcch[cfi - 1][q]];
            put_sfbc(gr, rg->l, rg->k, d, 4);
        }
        free(bits);
        free(sc);
    }
    /* PDSCH */
    {
        int cap = 14 * gr->nsc, *ls = malloc(sizeof(int) * (size_t)cap), *ks = malloc(sizeof(int) * (size_t)cap);
        int nre = lte_pdsch_res_mask(p->pci, nrb, p->ports, cfi, mask, true, ls, ks, cap);
        uint8_t *bits = malloc((size_t)(2 * nre)), *sc = malloc((size_t)(2 * nre));
        int rv = (int[]){0, 2, 3, 1}[(sfn / 2) & 3];
        if (lte_dlsch_encode(tb, tbs, 2 * nre, rv, bits) < 0) {
            snprintf(err, errlen, "turbocódigo: tamaño no válido");
            free(ls); free(ks); free(bits); free(sc);
            return -1;
        }
        lte_gold(lte_pdsch_cinit(p->pci, 5, LTE_SI_RNTI), 2 * nre, sc);
        cf32 *d = malloc(sizeof(cf32) * (size_t)nre);
        for (int i = 0; i < nre; i++)
            d[i] = qpsk_bits(bits[2 * i] ^ sc[2 * i], bits[2 * i + 1] ^ sc[2 * i + 1]);
        for (int i = 0; i < nre;) {
            if (gr->ports == 2 && i + 1 < nre && ls[i] == ls[i + 1]) {
                int kk[2] = {ks[i], ks[i + 1]};
                put_sfbc(gr, ls[i], kk, d + i, 2);
                i += 2;
            } else {
                G(gr, 0, ls[i], ks[i]) = d[i];
                i++;
            }
        }
        free(d); free(ls); free(ks); free(bits); free(sc);
    }
    return 0;
}

static void ofdm_symbol(const fft_plan_t *plan, const grid_t *gr, int port, int l, int N, int ncp, iqbuf_t *out)
{
    cf32 *b = calloc((size_t)N, sizeof(cf32));
    for (int k = 0; k < gr->nsc; k++) {
        int f = k < 6 * gr->nrb ? k - 6 * gr->nrb : k - 6 * gr->nrb + 1;
        int bin = f >= 0 ? f : N + f;
        cf32 v = G(gr, port, l, k);
        b[bin].re = v.re;
        b[bin].im = -v.im;       /* IFFT = conj(FFT(conj(X))) / N */
    }
    fft_exec(plan, b);
    iqbuf_reserve(out, N + ncp);
    for (int n = N - ncp; n < N; n++)
        iqbuf_push(out, (cf32){b[n].re, -b[n].im});
    for (int n = 0; n < N; n++)
        iqbuf_push(out, (cf32){b[n].re, -b[n].im});
    free(b);
}

static int lte_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse(params, &p, err, errlen) < 0)
        return -1;
    int N = fft_for(p.bw_rb) * p.os, S = N / 128;
    fft_plan_t *plan = fft_plan_create(N);
    if (!plan) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    grid_t gr = {.nsc = 12 * p.bw_rb, .nrb = p.bw_rb, .ports = p.ports};
    for (int q = 0; q < p.ports; q++)
        gr.g[q] = malloc(sizeof(cf32) * 14 * (size_t)gr.nsc);
    lte_ctrl_t ctl;
    bool sib_on = p.sib1 && !p.tdd && !p.ext;
    if (sib_on && lte_ctrl_init(&ctl, p.pci, p.bw_rb, p.ports, p.phich_res) < 0) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    uint32_t seed = (uint32_t)p.seed * 2654435761u + 12345u;
    iqbuf_t o[2] = {{0}, {0}};
    int nsym = lte_nsym(p.ext);
    for (int f = 0; f < p.frames; f++) {
        int sfn = (p.sfn + f) & 1023;
        for (int sf = 0; sf < 10; sf++) {
            for (int port = 0; port < p.ports; port++)
                for (int i = 0; i < 14 * gr.nsc; i++)
                    gr.g[port][i] = qpsk_random(&seed);
            for (int ns = 2 * sf; ns < 2 * sf + 2; ns++) {
                put_pss_sss(&gr, &p, ns);
                put_crs(&gr, &p, ns);
            }
            if (sf == 0)
                put_pbch(&gr, &p, sfn);
            if (sib_on && sf == 5 && (sfn & 1) == 0 && put_sib1(&gr, &p, sfn, &ctl, err, errlen) < 0) {
                lte_ctrl_free(&ctl);
                fft_plan_destroy(plan);
                return -1;
            }
            for (int port = 0; port < p.ports; port++)
                for (int l = 0; l < 14; l++) {
                    int ll = p.ext ? l % 6 : l % 7;
                    if (p.ext && l >= 12)
                        continue;
                    ofdm_symbol(plan, &gr, port, l, N, lte_cp_len(p.ext, ll) * S, &o[port]);
                }
            (void)nsym;
        }
    }
    if (sib_on)
        lte_ctrl_free(&ctl);
    fft_plan_destroy(plan);
    for (int q = 0; q < p.ports; q++)
        free(gr.g[q]);
    /* canal plano hacia el receptor de una sola antena */
    cf32 h1 = {0.7f * cosf(0.9f), 0.7f * sinf(0.9f)};
    float mx = 1e-9f;
    for (int n = 0; n < o[0].n; n++) {
        cf32 v = o[0].s[n];
        if (p.ports == 2) {
            cf32 w = o[1].s[n];
            v.re += w.re * h1.re - w.im * h1.im;
            v.im += w.re * h1.im + w.im * h1.re;
        }
        o[0].s[n] = v;
        mx = fmaxf(mx, sqrtf(v.re * v.re + v.im * v.im));
    }
    iqbuf_free(&o[1]);
    float sc = 0.8f / mx;
    for (int n = 0; n < o[0].n; n++) {
        o[0].s[n].re *= sc;
        o[0].s[n].im *= sc;
    }
    *out = o[0];
    out->rate = 15000.0 * N;
    return 0;
}

const modulator_desc_t MOD_LTE = {
    .id = "lte",
    .name = "LTE",
    .air_ok = false,
    .modulate = lte_modulate,
};
