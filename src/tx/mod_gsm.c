/*
 * mod_gsm.c — Modulador de la portadora BCCH de GSM (ranura 0 de cada trama) a 4 muestras
 * por símbolo (1 083 333 Hz).
 *
 * Emite tramas TDMA completas de 8 ranuras (625 muestras cada una):
 *   - ranura 0, multitrama de control de 51 tramas: FCCH (FN mod 51 = 0,10,20,30,40: 148
 *     bits a cero = tono puro a +67,7 kHz), SCH (1,11,21,31,41: BSIC + número de trama
 *     reducido T1/T2/T3', paridad de 10 bits, convolucional K=5 1/2 y secuencia de
 *     entrenamiento ampliada de 64 bits), BCCH (2–5: bloque de 23 octetos de System
 *     Information con código Fire de 40 bits, convolucional, entrelazado diagonal sobre
 *     4 ráfagas, secuencia de entrenamiento TSC = BCC) y relleno aleatorio en el resto;
 *   - ranuras 1–7: ráfagas normales con datos ALEATORIOS (relleno; no hay tráfico ni
 *     canales de aviso ni identidades de abonado en ninguna parte de la señal).
 * Mensajes del BCCH: SI3 (tipo 0x1B) en TC = (FN div 51) mod 8 = 2 y 6, SI4 en 3 y 7 y
 * relleno de tipo 0x19/0x1A en el resto.
 *
 * Modulación: GMSK de BT = 0,3 por integración de fase exacta: φ[n] = (π/2)·Σ_i α_i·q(n −
 * 4i) con α_i = 1 − 2(d_i ⊕ d_{i−1}) (codificación diferencial de 45.004) y q el
 * integral del pulso de frecuencia gaussiano×rectángulo (pulse_gmsk). Entre ráfagas hay
 * 33 muestras de guarda con amplitud cero (rampas de 6 muestras).
 *
 * Parámetros JSON (opcionales):
 *   {"ncc":3,"bcc":5,"fn":816,"frames":161,"mcc":214,"mnc":7,"mnc_digits":2,
 *    "lac":4660,"ci":22136,"seed":1}
 * air_ok = false: banda celular licenciada (§6).
 */
#include "mod.h"

#include "../decoders/gsm_phy.h"
#include "../dsp/pulse.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int ncc, bcc, mcc, mnc, mnc_digits, lac, ci, frames, seed;
    uint32_t fn;
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
    *p = (params_t){.ncc = 3, .bcc = 5, .mcc = 214, .mnc = 7, .mnc_digits = 2, .lac = 4660, .ci = 22136, .frames = 161,
                    .seed = 1, .fn = 816};
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
#define GETI(key, field) \
    if ((i = json_obj_get(js, t, 0, key)) >= 0 && json_num(js, t, i, &v)) p->field = (int)v
    GETI("ncc", ncc);
    GETI("bcc", bcc);
    GETI("mcc", mcc);
    GETI("mnc", mnc);
    GETI("mnc_digits", mnc_digits);
    GETI("lac", lac);
    GETI("ci", ci);
    GETI("frames", frames);
    GETI("seed", seed);
    if ((i = json_obj_get(js, t, 0, "fn")) >= 0 && json_num(js, t, i, &v))
        p->fn = (uint32_t)v;
#undef GETI
    if (p->ncc < 0 || p->ncc > 7 || p->bcc < 0 || p->bcc > 7 || p->frames < 1 || p->frames > 2000 ||
        p->mcc < 0 || p->mcc > 999 || p->mnc < 0 || p->mnc > 999 || (p->mnc_digits != 2 && p->mnc_digits != 3) ||
        p->lac < 0 || p->lac > 65535 || p->ci < 0 || p->ci > 65535 || p->fn >= 2715648) {
        snprintf(err, errlen, "ncc/bcc 0..7, mcc/mnc 0..999, lac/ci 0..65535, frames 1..2000, fn < 2715648");
        return -1;
    }
    return 0;
}

/* Una ráfaga GMSK de 148 bits → 625 muestras (592 de ráfaga + 33 de guarda a cero). */
static void gmsk_burst(const uint8_t *bits, cf32 *out)
{
    float h[20];
    int hn = pulse_gmsk(h, 0.3, 4, 4);
    float fr[148 * 4 + 32];
    memset(fr, 0, sizeof fr);
    int prev = 0;
    for (int i = 0; i < 148; i++) {
        float a = 1.0f - 2.0f * (float)(bits[i] ^ prev);
        prev = bits[i];
        for (int k = 0; k < hn; k++) {
            int n = 4 * i + k - 8;
            if (n >= 0 && n < 148 * 4)
                fr[n] += a * h[k];
        }
    }
    double ph = 0;
    for (int n = 0; n < GSM_SLOT; n++) {
        if (n < 148 * 4) {
            ph += (M_PI / 2.0) * fr[n];
            float w = 1.0f;
            if (n < 6)
                w = 0.5f - 0.5f * cosf((float)M_PI * (n + 0.5f) / 6.0f);
            else if (n >= 148 * 4 - 6)
                w = 0.5f - 0.5f * cosf((float)M_PI * (148 * 4 - n - 0.5f) / 6.0f);
            out[n] = (cf32){w * (float)cos(ph), w * (float)sin(ph)};
        } else {
            out[n] = (cf32){0, 0};
        }
    }
}

static void normal_burst(const uint8_t e[114], int tsc, uint8_t bits[148])
{
    memset(bits, 0, 148);
    memcpy(bits + 3, e, 57);
    bits[60] = 1;
    memcpy(bits + 61, GSM_TSC[tsc], 26);
    bits[87] = 1;
    memcpy(bits + 88, e + 57, 57);
}

static int gsm_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse(params, &p, err, errlen) < 0)
        return -1;
    uint32_t seed = (uint32_t)p.seed * 2654435761u + 7u;
    int bsic = p.ncc * 8 + p.bcc;
    iqbuf_t o = {0};
    iqbuf_reserve(&o, p.frames * GSM_FRAME + 64);
    uint8_t blk[4][114];
    memset(blk, 0, sizeof blk);
    for (int f = 0; f < p.frames; f++) {
        uint32_t fn = (p.fn + (uint32_t)f) % 2715648;
        int m = (int)(fn % 51);
        for (int slot = 0; slot < 8; slot++) {
            uint8_t bits[148];
            if (slot == 0 && (m % 10) == 0 && m <= 40) {            /* FCCH */
                memset(bits, 0, sizeof bits);
            } else if (slot == 0 && (m % 10) == 1 && m <= 41) {     /* SCH */
                uint8_t c[78];
                gsm_sch_encode(bsic, fn, c);
                memset(bits, 0, sizeof bits);
                memcpy(bits + 3, c, 39);
                memcpy(bits + 42, GSM_SCH_TRAIN, 64);
                memcpy(bits + 106, c + 39, 39);
            } else if (slot == 0 && m >= 2 && m <= 5) {             /* BCCH */
                if (m == 2) {
                    int tc = (int)((fn / 51) % 8);
                    uint8_t l3[23], c[456];
                    if (tc == 2 || tc == 6)
                        gsm_si_build(0x1B, p.mcc, p.mnc, p.mnc_digits, p.lac, p.ci, l3);
                    else if (tc == 3 || tc == 7)
                        gsm_si_build(0x1C, p.mcc, p.mnc, p.mnc_digits, p.lac, p.ci, l3);
                    else {
                        memset(l3, 0x2B, 23);
                        l3[0] = 0x2D;
                        l3[1] = 0x06;
                        l3[2] = tc == 0 ? 0x19 : 0x1A;
                    }
                    gsm_bcch_encode(l3, c);
                    for (int k = 0; k < 456; k++) {
                        int b, j;
                        gsm_interleave_pos(k, &b, &j);
                        blk[b][j] = c[k];
                    }
                }
                normal_burst(blk[m - 2], p.bcc, bits);
            } else {                                                 /* relleno aleatorio */
                uint8_t e[114];
                for (int i = 0; i < 114; i++)
                    e[i] = (uint8_t)(rnd(&seed) & 1);
                normal_burst(e, p.bcc, bits);
            }
            gmsk_burst(bits, o.s + o.n);
            o.n += GSM_SLOT;
        }
    }
    for (int i = 0; i < o.n; i++) {
        o.s[i].re *= 0.8f;
        o.s[i].im *= 0.8f;
    }
    *out = o;
    out->rate = GSM_RATE;
    return 0;
}

const modulator_desc_t MOD_GSM = {
    .id = "gsm",
    .name = "GSM",
    .air_ok = false,
    .modulate = gsm_modulate,
};
