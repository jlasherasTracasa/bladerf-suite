/*
 * mod_nr.c — Modulador de ráfagas SS/PBCH (SSB) de 5G NR (3GPP TS 38.211 §7.4).
 *
 * Genera banda base con el SSB centrado en 0 Hz (la subportadora 120 = SS_REF a
 * 0 Hz), ya que la herramienta de emisión (tx.c) lo desplaza a la frecuencia pedida:
 * para que el decodificador lo encuentre hay que ponerlo en un GSCN, p. ej.
 * --gen nr --gen-freq 3499680000 --gen-center 3500000000 --gen-rate 15360000.
 *
 * Cada SSB son 4 símbolos OFDM con prefijo cíclico normal (CP = N·9/128, N = rate/SCS):
 *   símbolo 0: PSS (127 BPSK en las subportadoras 56..182 del SSB)
 *   símbolo 1: PBCH (aquí QPSK aleatorio, sin polar ni DMRS) en 0..239
 *   símbolo 2: SSS en 56..182 y PBCH en 0..47 y 192..239
 *   símbolo 3: PBCH en 0..239
 * Los SSB de una ráfaga van seguidos (no se reproduce la disposición exacta de los
 * casos A/B/C de la norma) y la ráfaga se repite cada 20 ms (periodicidad inicial).
 *
 * Parámetros JSON (opcionales):
 *   {"pci":123,"scs":30,"rate":15360000,"nssb":2,"periods":3,"period_ms":20,
 *    "start_ms":1.5,"seed":1}
 * "rate" debe ser múltiplo de scs. No hay PBCH real (polar): no se emite MIB.
 * air_ok = false: bandas celulares licenciadas (§6).
 */
#include "mod.h"

#include "../decoders/nr_phy.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int pci, scs, nssb, periods, seed;
    double rate, period_ms, start_ms;
} params_t;

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static int parse(const char *js, params_t *p, char *err, size_t errlen)
{
    *p = (params_t){.pci = 321, .scs = 30, .nssb = 2, .periods = 3, .seed = 1, .rate = 15360000, .period_ms = 20,
                    .start_ms = 1.5};
    if (!js || !*js)
        return 0;
    jtok_t t[32];
    int nt = json_parse(js, strlen(js), t, 32);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "parámetros JSON no válidos");
        return -1;
    }
    double v;
    int i;
    if ((i = json_obj_get(js, t, 0, "pci")) >= 0 && json_num(js, t, i, &v))
        p->pci = (int)v;
    if ((i = json_obj_get(js, t, 0, "scs")) >= 0 && json_num(js, t, i, &v))
        p->scs = (int)v;
    if ((i = json_obj_get(js, t, 0, "rate")) >= 0 && json_num(js, t, i, &v))
        p->rate = v;
    if ((i = json_obj_get(js, t, 0, "nssb")) >= 0 && json_num(js, t, i, &v))
        p->nssb = (int)v;
    if ((i = json_obj_get(js, t, 0, "periods")) >= 0 && json_num(js, t, i, &v))
        p->periods = (int)v;
    if ((i = json_obj_get(js, t, 0, "period_ms")) >= 0 && json_num(js, t, i, &v))
        p->period_ms = v;
    if ((i = json_obj_get(js, t, 0, "start_ms")) >= 0 && json_num(js, t, i, &v))
        p->start_ms = v;
    if ((i = json_obj_get(js, t, 0, "seed")) >= 0 && json_num(js, t, i, &v))
        p->seed = (int)v;
    if (p->pci < 0 || p->pci > 1007 || (p->scs != 15 && p->scs != 30) || p->nssb < 1 || p->nssb > 8 ||
        p->periods < 1 || p->periods > 50 || p->rate < 4e6 || p->rate > 130e6 ||
        fabs(p->rate / (p->scs * 1000.0) - round(p->rate / (p->scs * 1000.0))) > 1e-9) {
        snprintf(err, errlen, "pci 0..1007, scs 15|30, rate múltiplo del SCS, nssb 1..8, periods 1..50");
        return -1;
    }
    return 0;
}

/* Un símbolo OFDM: a[k] k = 0..239 (subportadora del SSB; (0,0) = sin energía). */
static void put_symbol(iqbuf_t *o, const cf32 *a, int N, int ncp, const cf32 *tw)
{
    cf32 *x = malloc(sizeof(cf32) * (size_t)N);
    memset(x, 0, sizeof(cf32) * (size_t)N);
    for (int k = 0; k < NR_SSB_SC; k++) {
        if (a[k].re == 0 && a[k].im == 0)
            continue;
        int f = k - NR_SSB_REF;
        int fb = ((f % N) + N) % N;
        long idx = 0;
        for (int n = 0; n < N; n++) {
            const cf32 w = tw[idx];
            x[n].re += a[k].re * w.re - a[k].im * w.im;
            x[n].im += a[k].re * w.im + a[k].im * w.re;
            idx += fb;
            if (idx >= N)
                idx -= N;
        }
    }
    iqbuf_reserve(o, N + ncp);
    for (int n = N - ncp; n < N; n++)
        o->s[o->n++] = x[n];
    for (int n = 0; n < N; n++)
        o->s[o->n++] = x[n];
    free(x);
}

static int nr_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse(params, &p, err, errlen) < 0)
        return -1;
    int N = (int)lround(p.rate / (p.scs * 1000.0));
    int ncp = (int)lround(N * 9.0 / 128.0);
    int symlen = N + ncp;
    cf32 *tw = malloc(sizeof(cf32) * (size_t)N);
    for (int m = 0; m < N; m++) {
        tw[m].re = (float)cos(2.0 * M_PI * m / N);
        tw[m].im = (float)sin(2.0 * M_PI * m / N);
    }
    int8_t pss[127], sss[127];
    nr_pss_seq(p.pci % 3, pss);
    nr_sss_seq(p.pci / 3, p.pci % 3, sss);
    uint32_t seed = (uint32_t)p.seed * 2654435761u + 1u;
    int per = (int)lround(p.period_ms * 1e-3 * p.rate);
    int start = (int)lround(p.start_ms * 1e-3 * p.rate);
    int total = start + (p.periods - 1) * per + p.nssb * 4 * symlen + (int)(0.001 * p.rate);
    iqbuf_t o = {0};
    iqbuf_reserve(&o, total + 16);
    out->rate = p.rate;
    for (int pr = 0; pr < p.periods; pr++) {
        int t0 = start + pr * per;
        while (o.n < t0)
            o.s[o.n++] = (cf32){0, 0};
        for (int s = 0; s < p.nssb; s++)
            for (int sym = 0; sym < 4; sym++) {
                cf32 a[NR_SSB_SC];
                memset(a, 0, sizeof a);
                const float q = 0.70710678f;
                for (int k = 0; k < NR_SSB_SC; k++) {
                    bool pbch = sym == 1 || sym == 3 || (sym == 2 && (k < 48 || k >= 192));
                    if (pbch) {
                        uint32_t r = rnd(&seed);
                        a[k] = (cf32){(r & 1) ? -q : q, (r & 2) ? -q : q};
                    }
                }
                if (sym == 0)
                    for (int n = 0; n < 127; n++)
                        a[NR_PSS_K0 + n] = (cf32){(float)pss[n], 0};
                if (sym == 2)
                    for (int n = 0; n < 127; n++)
                        a[NR_PSS_K0 + n] = (cf32){(float)sss[n], 0};
                put_symbol(&o, a, N, ncp, tw);
            }
    }
    while (o.n < total)
        o.s[o.n++] = (cf32){0, 0};
    free(tw);
    float mx = 1e-9f;
    for (int i = 0; i < o.n; i++)
        mx = fmaxf(mx, fabsf(o.s[i].re) > fabsf(o.s[i].im) ? fabsf(o.s[i].re) : fabsf(o.s[i].im));
    float sc = 0.8f / mx;
    for (int i = 0; i < o.n; i++) {
        o.s[i].re *= sc;
        o.s[i].im *= sc;
    }
    iqbuf_free(out);
    *out = o;
    out->rate = p.rate;
    return 0;
}

const modulator_desc_t MOD_NR = {
    .id = "nr",
    .name = "5G NR (SSB)",
    .air_ok = false,
    .modulate = nr_modulate,
};
