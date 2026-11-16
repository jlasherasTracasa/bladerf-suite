/*
 * mod_dab.c — Modulador DAB modo I (ETSI EN 300 401) a 2,048 MS/s.
 *
 * Genera tramas de transmisión completas con la cadena inversa del receptor:
 *
 *   FIG (0/0, 0/1, 0/2, 0/17, 1/0, 1/1) → FIB de 32 bytes con CRC-16 → grupos de
 *   3 FIB (768 bits) → dispersión de energía → convolucional K=7 r1/4 + cola →
 *   punzonado PI_16/PI_15/PI_X (2304 bits) → 4 grupos = 9216 bits = 3 símbolos
 *   de FIC → QPSK (bit n → parte real de la portadora Π(n), bit n+1536 → parte
 *   imaginaria) → modulación DIFERENCIAL respecto al símbolo anterior → IFFT de
 *   2048 con prefijo cíclico de 504.
 *
 * Trama = símbolo nulo (2656 muestras a 0) + PRS (símbolo de fase, referencia
 * conocida) + 3 símbolos de FIC + 72 símbolos de MSC rellenos con bits
 * pseudoaleatorios (no hay audio: el decodificador solo usa el FIC).
 *
 * Parámetros JSON (todos opcionales):
 *   {"eid":"C1D2","label":"BladeRF Lab DAB","frames":10,
 *    "services":[{"sid":"C201","label":"Radio Uno","subch":1,"kbps":128,
 *                 "prot":"3-A","dabplus":true,"pty":10}, ...]}
 * prot: "N-A" o "N-B" (EEP, N = 1..4) o "uep:<índice 0..63>" (forma corta; el
 * kbps lo da la tabla). Máx. 8 servicios, un componente de audio cada uno.
 *
 * Los textos se codifican con el juego EBU Latin tal cual (ASCII alfanumérico);
 * evitar '$', '\', '^' y '`', que en esa tabla son otros caracteres.
 *
 * air_ok = false: DAB es radiodifusión (AGENTS.md §6).
 */
#include "mod.h"

#include "../decoders/dab.h"
#include "../dsp/fft.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SVC 8

typedef struct {
    uint16_t sid;
    char label[17];
    int subch, kbps, level, kind, uep_idx, pty; /* kind: 0 UEP, 1 EEP-A, 2 EEP-B */
    bool dabplus;
    int size, start;
} svc_t;

typedef struct {
    uint16_t eid;
    char label[17];
    int frames, nsvc;
    svc_t svc[MAX_SVC];
} params_t;

static void set_label(char *dst, const char *src)
{
    snprintf(dst, 17, "%-16.16s", src);
}

static int parse_prot(const char *p, svc_t *s)
{
    if (!strncmp(p, "uep:", 4)) {
        int idx = atoi(p + 4), size, lvl, kbps;
        if (!dab_uep_info(idx, &size, &lvl, &kbps))
            return -1;
        s->kind = 0, s->uep_idx = idx, s->level = lvl, s->kbps = kbps, s->size = size;
        return 0;
    }
    int lvl = p[0] - '0';
    if (lvl < 1 || lvl > 4 || (p[1] != '-') || (p[2] != 'A' && p[2] != 'B'))
        return -1;
    static const int dA[4] = {12, 8, 6, 4}, dB[4] = {27, 21, 18, 15};
    s->level = lvl;
    s->kind = p[2] == 'A' ? 1 : 2;
    if (s->kind == 1) {
        if (s->kbps % 8)
            return -1;
        s->size = s->kbps / 8 * dA[lvl - 1];
    } else {
        if (s->kbps % 32)
            return -1;
        s->size = s->kbps / 32 * dB[lvl - 1];
    }
    return 0;
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    p->eid = 0xC1D2;
    set_label(p->label, "BladeRF Lab DAB");
    p->frames = 10;
    const struct {
        uint16_t sid;
        const char *lab, *prot;
        int kbps;
        bool plus;
    } def[3] = {{0xC201, "Radio Uno", "3-A", 128, true},
                {0xC202, "Radio Dos", "uep:24", 96, false},
                {0xC203, "Radio Tres", "4-B", 64, true}};
    for (int i = 0; i < 3; i++) {
        svc_t *s = &p->svc[i];
        s->sid = def[i].sid;
        set_label(s->label, def[i].lab);
        s->subch = i + 1;
        s->kbps = def[i].kbps;
        s->dabplus = def[i].plus;
        s->pty = 10 + i;
        parse_prot(def[i].prot, s);
    }
    p->nsvc = 3;
    if (!js || !js[0])
        return 0;
    jtok_t t[256];
    int nt = json_parse(js, strlen(js), t, 256);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    char s[64];
    double v;
    int i;
    if ((i = json_obj_get(js, t, 0, "eid")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->eid = (uint16_t)strtoul(s, NULL, 16);
    if ((i = json_obj_get(js, t, 0, "label")) >= 0 && json_str(js, t, i, s, sizeof s))
        set_label(p->label, s);
    if ((i = json_obj_get(js, t, 0, "frames")) >= 0 && json_num(js, t, i, &v))
        p->frames = (int)CLAMP(v, 4, 2000);
    if ((i = json_obj_get(js, t, 0, "services")) >= 0 && t[i].type == J_ARR) {
        p->nsvc = 0;
        for (int k = 0; k < t[i].size && p->nsvc < MAX_SVC; k++) {
            int e = json_arr_get(t, i, k), j;
            if (e < 0 || t[e].type != J_OBJ)
                continue;
            svc_t *sv = &p->svc[p->nsvc];
            memset(sv, 0, sizeof *sv);
            sv->sid = (uint16_t)(0xC200 + p->nsvc + 1);
            set_label(sv->label, "Servicio");
            sv->subch = p->nsvc + 1;
            sv->kbps = 64;
            sv->dabplus = true;
            char prot[24] = "3-A";
            if ((j = json_obj_get(js, t, e, "sid")) >= 0 && json_str(js, t, j, s, sizeof s))
                sv->sid = (uint16_t)strtoul(s, NULL, 16);
            if ((j = json_obj_get(js, t, e, "label")) >= 0 && json_str(js, t, j, s, sizeof s))
                set_label(sv->label, s);
            if ((j = json_obj_get(js, t, e, "subch")) >= 0 && json_num(js, t, j, &v))
                sv->subch = (int)CLAMP(v, 0, 63);
            if ((j = json_obj_get(js, t, e, "kbps")) >= 0 && json_num(js, t, j, &v))
                sv->kbps = (int)v;
            if ((j = json_obj_get(js, t, e, "prot")) >= 0)
                (void)json_str(js, t, j, prot, sizeof prot);
            bool b;
            if ((j = json_obj_get(js, t, e, "dabplus")) >= 0 && json_bool(js, t, j, &b))
                sv->dabplus = b;
            if ((j = json_obj_get(js, t, e, "pty")) >= 0 && json_num(js, t, j, &v))
                sv->pty = (int)CLAMP(v, 0, 31);
            if (parse_prot(prot, sv) < 0) {
                snprintf(err, errlen, "protección '%s' no válida o kbps incompatible", prot);
                return -1;
            }
            p->nsvc++;
        }
    }
    return 0;
}

/* ---- FIG ---------------------------------------------------------------- */
typedef struct {
    uint8_t d[32];
    int n;
} fig_t;

static void fig_begin(fig_t *f, int type)
{
    memset(f, 0, sizeof *f);
    f->d[0] = (uint8_t)(type << 5);
    f->n = 1;
}
static void fig_put(fig_t *f, int v) { f->d[f->n++] = (uint8_t)v; }
static void fig_end(fig_t *f) { f->d[0] |= (uint8_t)(f->n - 1); }

static int build_figs(const params_t *p, int frame, fig_t *figs)
{
    int nf = 0;
    fig_t *f;
    /* 0/0: EId + contador de CIF */
    f = &figs[nf++];
    fig_begin(f, 0);
    fig_put(f, 0x00);
    fig_put(f, p->eid >> 8);
    fig_put(f, p->eid & 255);
    int cif = frame * 4;
    fig_put(f, (cif / 250) % 20);
    fig_put(f, cif % 250);
    fig_end(f);
    /* 1/0: etiqueta del conjunto */
    f = &figs[nf++];
    fig_begin(f, 1);
    fig_put(f, 0x00);
    fig_put(f, p->eid >> 8);
    fig_put(f, p->eid & 255);
    for (int i = 0; i < 16; i++)
        fig_put(f, (uint8_t)p->label[i]);
    fig_put(f, 0xFF);
    fig_put(f, 0x00);
    fig_end(f);
    /* 0/1: subcanales (con direcciones de inicio consecutivas) */
    f = &figs[nf++];
    fig_begin(f, 0);
    fig_put(f, 0x01);
    int start = 0;
    for (int i = 0; i < p->nsvc; i++) {
        const svc_t *s = &p->svc[i];
        fig_put(f, (s->subch << 2) | (start >> 8));
        fig_put(f, start & 255);
        if (s->kind == 0) {
            fig_put(f, s->uep_idx);
        } else {
            fig_put(f, 0x80 | ((s->kind - 1) << 4) | ((s->level - 1) << 2) | (s->size >> 8));
            fig_put(f, s->size & 255);
        }
        start += s->size;
    }
    fig_end(f);
    /* 0/2: servicios y componentes */
    f = &figs[nf++];
    fig_begin(f, 0);
    fig_put(f, 0x02);
    for (int i = 0; i < p->nsvc; i++) {
        const svc_t *s = &p->svc[i];
        fig_put(f, s->sid >> 8);
        fig_put(f, s->sid & 255);
        fig_put(f, 1); /* un componente */
        fig_put(f, s->dabplus ? 63 : 0);
        fig_put(f, (s->subch << 2) | 2);
    }
    fig_end(f);
    /* 0/17: tipo de programa */
    f = &figs[nf++];
    fig_begin(f, 0);
    fig_put(f, 0x11);
    for (int i = 0; i < p->nsvc; i++) {
        fig_put(f, p->svc[i].sid >> 8);
        fig_put(f, p->svc[i].sid & 255);
        fig_put(f, 0x00);
        fig_put(f, p->svc[i].pty & 31);
    }
    fig_end(f);
    /* 1/1: etiquetas de servicio */
    for (int i = 0; i < p->nsvc; i++) {
        f = &figs[nf++];
        fig_begin(f, 1);
        fig_put(f, 0x01);
        fig_put(f, p->svc[i].sid >> 8);
        fig_put(f, p->svc[i].sid & 255);
        for (int c = 0; c < 16; c++)
            fig_put(f, (uint8_t)p->svc[i].label[c]);
        fig_put(f, 0xFF);
        fig_put(f, 0x00);
        fig_end(f);
    }
    return nf;
}

/* Reparte las FIG en 12 FIB de 30 bytes (de forma cíclica, rellenando con 0xFF + ceros). */
static void pack_fibs(const fig_t *figs, int nf, uint8_t fib[12][32])
{
    int k = 0;
    for (int b = 0; b < 12; b++) {
        memset(fib[b], 0, 32);
        int pos = 0;
        for (int tries = 0; tries < nf; tries++) {
            const fig_t *f = &figs[k % nf];
            if (pos + f->n > 30)
                break;
            memcpy(fib[b] + pos, f->d, (size_t)f->n);
            pos += f->n;
            k++;
        }
        if (pos < 30)
            fib[b][pos] = 0xFF; /* marcador de fin; el resto ya son ceros */
        dab_fib_set_crc(fib[b]);
    }
}

/* ---- OFDM --------------------------------------------------------------- */
static void put_symbol(iqbuf_t *o, fft_plan_t *plan, const cf32 *z, double gain)
{
    cf32 x[DAB_TU];
    for (int i = 0; i < DAB_TU; i++) { /* IFFT = conj(FFT(conj(z)))/N */
        x[i].re = z[i].re;
        x[i].im = -z[i].im;
    }
    fft_exec(plan, x);
    iqbuf_reserve(o, DAB_SYM);
    for (int i = -DAB_GUARD; i < DAB_TU; i++) {
        cf32 v = x[(i + DAB_TU) & (DAB_TU - 1)];
        v.re = (float)(gain * v.re / DAB_TU);
        v.im = (float)(-gain * v.im / DAB_TU);
        v.re = CLAMP(v.re, -0.9f, 0.9f);
        v.im = CLAMP(v.im, -0.9f, 0.9f);
        iqbuf_push(o, v);
    }
}

static int dab_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    fft_plan_t *plan = fft_plan_create(DAB_TU);
    if (!plan) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    const int16_t *map = dab_carrier_map();
    /* rms deseado 0,17 con 1536 portadoras unitarias: rms = √1536/2048 */
    const double gain = 0.17 / (sqrt((double)DAB_K) / DAB_TU);
    out->rate = DAB_RATE;
    iqbuf_reserve(out, p.frames * DAB_FRAME + 16);
    cf32 prs[DAB_TU], z[DAB_TU], zp[DAB_TU];
    dab_prs_spectrum(prs);
    uint32_t rng = 0x1234567u;
    for (int fr = 0; fr < p.frames; fr++) {
        fig_t figs[32];
        int nf = build_figs(&p, fr, figs);
        uint8_t fib[12][32];
        pack_fibs(figs, nf, fib);
        static uint8_t bits[9216];
        for (int q = 0; q < 4; q++)
            dab_fic_encode((const uint8_t(*)[32])fib[3 * q], bits + 2304 * q);
        iqbuf_reserve(out, DAB_FRAME);
        memset(out->s + out->n, 0, sizeof(cf32) * DAB_NULL);
        out->n += DAB_NULL;
        memcpy(zp, prs, sizeof zp);
        put_symbol(out, plan, zp, gain);
        for (int l = 1; l < DAB_NSYM; l++) {
            memcpy(z, zp, sizeof z);
            for (int n = 0; n < DAB_K; n++) {
                int b0, b1;
                if (l <= 3) {
                    b0 = bits[(l - 1) * 2 * DAB_K + n];
                    b1 = bits[(l - 1) * 2 * DAB_K + DAB_K + n];
                } else {
                    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
                    b0 = rng & 1;
                    b1 = (rng >> 8) & 1;
                }
                int k = map[n], bin = k >= 0 ? k : DAB_TU + k;
                /* y = ((1−2b0) + j(1−2b1))/√2 ;  Z_l = Z_{l−1}·y */
                float yr = (1 - 2 * b0) * (float)M_SQRT1_2, yi = (1 - 2 * b1) * (float)M_SQRT1_2;
                z[bin].re = zp[bin].re * yr - zp[bin].im * yi;
                z[bin].im = zp[bin].re * yi + zp[bin].im * yr;
            }
            put_symbol(out, plan, z, gain);
            memcpy(zp, z, sizeof zp);
        }
    }
    fft_plan_destroy(plan);
    return 0;
}

const modulator_desc_t MOD_DAB = {
    .id = "dab",
    .name = "DAB / DAB+",
    .air_ok = false,
    .modulate = dab_modulate,
};
