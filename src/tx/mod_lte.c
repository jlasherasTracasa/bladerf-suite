/*
 * mod_lte.c — Modulador LTE (señal de enlace descendente de una celda a 1,92 Msps).
 *
 * Genera lo que una celda difundiría en los 6 RB centrales (1,08 MHz), que es lo
 * que cabe en la captura de 1,92 Msps del decodificador (FFT de 128, SCS 15 kHz):
 *   - PSS y SSS (FDD: símbolos 6 y 5 de las ranuras 0 y 10; TDD: SSS al final de
 *     las subtramas 0/5 y PSS en el tercer símbolo de las subtramas 1/6),
 *   - CRS de los puertos 0 (y 1 con "ports":2), con la rejilla desplazada N_ID mod 6,
 *   - PBCH con una MIB real (36.331): ancho de banda, PHICH y SFN, con CRC-16
 *     enmascarado por el nº de puertos, convolucional tail-biting 1/3, adaptación
 *     de velocidad, aleatorización y QPSK (SFBC de Alamouti con 2 puertos),
 *   - resto de RE: QPSK aleatorio (tráfico PDSCH ficticio) a la misma potencia.
 * Con 2 puertos se suman las dos antenas a través de un canal plano
 * h = [1, 0,7·e^{j0,9}] para que el receptor tenga que estimarlos por separado.
 *
 * Parámetros JSON (todos opcionales):
 *   {"pci":123,"cp":"normal|extended","duplex":"fdd|tdd","ports":1|2,
 *    "sfn":0,"frames":6,"bw_rb":50,"phich_res":2,"phich_ext":0,"seed":1}
 *
 * Limitaciones: el PBCH con 4 puertos (SFBC+FSTD) no está implementado; la señal
 * ocupa solo los 6 RB centrales aunque la MIB anuncie "bw_rb"; todas las
 * subtramas TDD se tratan como descendentes.
 * air_ok = false: banda celular licenciada (política §6).
 */
#include "mod.h"

#include "../decoders/lte_phy.h"
#include "../dsp/fft.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int pci, ports, sfn, frames, bw_rb, phich_res, phich_ext, seed;
    bool ext, tdd;
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
    *p = (params_t){.pci = 301, .ports = 1, .sfn = 0, .frames = 6, .bw_rb = 50, .phich_res = 2, .seed = 1};
    if (!js || !*js)
        return 0;
    jtok_t t[64];
    int nt = json_parse(js, strlen(js), t, 64);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "parámetros JSON no válidos");
        return -1;
    }
    double v;
    char s[32];
    int i;
    if ((i = json_obj_get(js, t, 0, "pci")) >= 0 && json_num(js, t, i, &v))
        p->pci = (int)v;
    if ((i = json_obj_get(js, t, 0, "ports")) >= 0 && json_num(js, t, i, &v))
        p->ports = (int)v;
    if ((i = json_obj_get(js, t, 0, "sfn")) >= 0 && json_num(js, t, i, &v))
        p->sfn = (int)v;
    if ((i = json_obj_get(js, t, 0, "frames")) >= 0 && json_num(js, t, i, &v))
        p->frames = (int)v;
    if ((i = json_obj_get(js, t, 0, "bw_rb")) >= 0 && json_num(js, t, i, &v))
        p->bw_rb = (int)v;
    if ((i = json_obj_get(js, t, 0, "phich_res")) >= 0 && json_num(js, t, i, &v))
        p->phich_res = (int)v;
    if ((i = json_obj_get(js, t, 0, "phich_ext")) >= 0 && json_num(js, t, i, &v))
        p->phich_ext = (int)v;
    if ((i = json_obj_get(js, t, 0, "seed")) >= 0 && json_num(js, t, i, &v))
        p->seed = (int)v;
    if ((i = json_obj_get(js, t, 0, "cp")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->ext = s[0] == 'e' || s[0] == 'E';
    if ((i = json_obj_get(js, t, 0, "duplex")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->tdd = s[0] == 't' || s[0] == 'T';
    if (p->pci < 0 || p->pci > 503 || p->ports < 1 || p->ports > 2 || p->frames < 1 || p->frames > 400) {
        snprintf(err, errlen, "pci 0..503, ports 1|2, frames 1..400");
        return -1;
    }
    return 0;
}

/* rejilla de una ranura: [puerto][símbolo][72] */
typedef cf32 slot_grid_t[2][7][72];

static void put_pss_sss(slot_grid_t g, const params_t *p, int ns)
{
    int nsym = lte_nsym(p->ext);
    int n_id1 = p->pci / 3, n_id2 = p->pci % 3;
    int half = ns >= 10;
    int sss_l = -1, pss_l = -1;
    if (!p->tdd) {
        if (ns == 0 || ns == 10) {
            pss_l = nsym - 1;
            sss_l = nsym - 2;
        }
    } else {
        if (ns == 2 || ns == 12)
            pss_l = 2;
        if (ns == 1 || ns == 11)
            sss_l = nsym - 1;
    }
    if (pss_l >= 0) {
        cf32 d[62];
        lte_pss_seq(n_id2, d);
        for (int port = 0; port < p->ports; port++)
            memset(g[port][pss_l], 0, sizeof g[port][pss_l]);
        for (int n = 0; n < 62; n++)
            g[0][pss_l][n + 5] = d[n];
        if (p->ports == 2)
            for (int n = 0; n < 62; n++)
                g[1][pss_l][n + 5] = (cf32){0, 0};   /* PSS solo por la antena 0 */
    }
    if (sss_l >= 0) {
        int8_t d[62];
        lte_sss_seq(n_id1, n_id2, half, d);
        for (int port = 0; port < p->ports; port++)
            memset(g[port][sss_l], 0, sizeof g[port][sss_l]);
        for (int n = 0; n < 62; n++)
            g[0][sss_l][n + 5] = (cf32){(float)d[n], 0};
    }
}

static void put_crs(slot_grid_t g, const params_t *p, int ns)
{
    int nsym = lte_nsym(p->ext);
    int ls[2] = {0, nsym - 3};
    for (int li = 0; li < 2; li++) {
        int l = ls[li];
        cf32 r[12];
        lte_crs_seq(p->pci, ns, l, p->ext, r);
        for (int port = 0; port < p->ports; port++) {
            int j0 = lte_crs_j0(p->pci, port, ns, l, p->ext);
            for (int m = 0; m < 12; m++) {
                g[port][l][j0 + 6 * m] = r[m];
                if (p->ports == 2) { /* la otra antena deja vacío ese RE */
                    int other = 1 - port;
                    g[other][l][j0 + 6 * m] = (cf32){0, 0};
                }
            }
        }
    }
}

static void put_pbch(slot_grid_t g, const params_t *p, int sfn)
{
    lte_mib_t mib = {.bw_rb = p->bw_rb, .phich_ext = p->phich_ext, .phich_res = p->phich_res, .sfn8 = (sfn >> 2) & 255};
    uint8_t bits[1920];
    lte_pbch_encode(&mib, p->ports, p->pci, p->ext, bits);
    int eseg = p->ext ? 432 : 480, seg = sfn & 3;
    int lsym[288], jj[288];
    int n = lte_pbch_res(p->pci, p->ext, lsym, jj);
    const float a = 0.70710678f;
    cf32 d[288];
    for (int i = 0; i < n; i++) {
        const uint8_t *b = bits + seg * eseg + 2 * i;
        d[i] = (cf32){a * (float)(1 - 2 * b[0]), a * (float)(1 - 2 * b[1])};
    }
    for (int i = 0; i < n; i++) {
        if (p->ports == 1) {
            g[0][lsym[i]][jj[i]] = d[i];
        } else {
            int a0 = i & ~1, b0 = a0 + 1;           /* par (2i, 2i+1): Alamouti */
            cf32 x0 = d[a0], x1 = d[b0];
            const float s = 0.70710678f;
            if (i == a0) {
                g[0][lsym[i]][jj[i]] = (cf32){x0.re * s, x0.im * s};
                g[1][lsym[i]][jj[i]] = (cf32){-x1.re * s, x1.im * s};   /* −x1* */
            } else {
                g[0][lsym[i]][jj[i]] = (cf32){x1.re * s, x1.im * s};
                g[1][lsym[i]][jj[i]] = (cf32){x0.re * s, -x0.im * s};   /* x0* */
            }
        }
    }
}

static void ofdm_symbol(const fft_plan_t *plan, const cf32 *re72, int ncp, iqbuf_t *out)
{
    cf32 b[LTE_N];
    memset(b, 0, sizeof b);
    for (int j = 0; j < 72; j++) {
        int k = lte_f_to_bin(lte_win_to_f(j));
        b[k].re = re72[j].re;
        b[k].im = -re72[j].im;       /* IFFT = conj(FFT(conj(X))) / N */
    }
    fft_exec(plan, b);
    iqbuf_reserve(out, LTE_N + ncp);
    for (int n = LTE_N - ncp; n < LTE_N; n++)
        iqbuf_push(out, (cf32){b[n].re, -b[n].im});
    for (int n = 0; n < LTE_N; n++)
        iqbuf_push(out, (cf32){b[n].re, -b[n].im});
}

static int lte_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse(params, &p, err, errlen) < 0)
        return -1;
    fft_plan_t *plan = fft_plan_create(LTE_N);
    if (!plan) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    uint32_t seed = (uint32_t)p.seed * 2654435761u + 12345u;
    iqbuf_t o[2] = {{0}, {0}};
    int nsym = lte_nsym(p.ext);
    for (int f = 0; f < p.frames; f++) {
        int sfn = (p.sfn + f) & 1023;
        for (int ns = 0; ns < 20; ns++) {
            slot_grid_t g;
            for (int port = 0; port < p.ports; port++)
                for (int l = 0; l < nsym; l++)
                    for (int j = 0; j < 72; j++)
                        g[port][l][j] = qpsk_random(&seed);
            put_pss_sss(g, &p, ns);
            put_crs(g, &p, ns);
            if (ns == 1)
                put_pbch(g, &p, sfn);
            for (int port = 0; port < p.ports; port++)
                for (int l = 0; l < nsym; l++)
                    ofdm_symbol(plan, g[port][l], lte_cp_len(p.ext, l), &o[port]);
        }
    }
    fft_plan_destroy(plan);
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
    out->rate = LTE_FS;
    return 0;
}

const modulator_desc_t MOD_LTE = {
    .id = "lte",
    .name = "LTE",
    .air_ok = false,
    .modulate = lte_modulate,
};
