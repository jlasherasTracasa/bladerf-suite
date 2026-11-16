/*
 * mod_rds.c — Modulador de FM comercial con RDS (banda base a 250 kS/s).
 *
 * Construye lo que una emisora transmite y que el decodificador rds.c espera:
 *
 *   MPX(t) = a·[ (L+R)/2 + (L−R)/2·sin(2·ϑ) ]  +  p·sin(ϑ)  +  r·rds(t)
 *   ϑ = 2π·19 kHz·t   (piloto; 38 kHz = 2ϑ, 57 kHz = 3ϑ: todo enganchado en fase)
 *
 * con L = tono de audio y R = 0, de modo que el tono aparece tanto en L+R como
 * en las bandas laterales de L−R (que llegan hasta 53 kHz si audio_hz = 15 kHz:
 * el peor caso para el filtro de 57 kHz del receptor).
 *
 * RDS: grupos de 4 bloques (16 bits de datos + 10 de control con su offset,
 * ver rds.h), codificación diferencial b'[k] = b[k] XOR b'[k−1], pulso bifásico
 * (un periodo de seno, "+ −" suavizado) a 1187,5 bit/s = 19000/16 y portadora
 * cos(3ϑ) (BPSK de 57 kHz). La FM se genera integrando la fase con ±75 kHz de
 * desvío para MPX = ±1 y amplitud 0,8.
 *
 * Secuencia de grupos: en cada paso k se envía un 0A (segmento k mod 4: PS, TA,
 * MS y un par de AF), un 2A (segmento k mod nseg del RadioText) y, cada 6
 * pasos, un 4A (hora y fecha). La emisión dura 'seconds' segundos.
 *
 * "groups":["62040130966B594C",…] (16 dígitos hex = 4 bloques de datos) transmite
 * esos grupos tal cual, en bucle (para probar con vectores reales publicados).
 *
 * Parámetros JSON (todos opcionales):
 *   {"pi":"C201","ps":"BLADERF","rt":"Texto de RadioText","pty":10,"tp":true,
 *    "ta":false,"ms":true,"af":[100300000,98500000],"ct":"2026-11-15 12:34",
 *    "ct_off":2 (medias horas),"seconds":5,"audio_hz":1000,"audio_level":0.4,
 *    "pilot":0.09,"rds_level":0.035}
 * (rds_level = desvío de RDS relativo a 75 kHz: 0,035 ≈ 2,6 kHz; la norma
 * admite hasta ±7,5 kHz de desvío de la subportadora.)
 *
 * air_ok = false: la FM comercial es banda de radiodifusión (AGENTS.md §6).
 */
#include "mod.h"

#include "../decoders/rds.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t pi;
    char ps[9], rt[65];
    int pty, tp, ta, ms;
    int64_t af[25];
    int naf;
    int y, mo, d, hh, mm, ct_off;
    double seconds, audio_hz, audio_level, pilot, rds_level;
    uint16_t raw[64][4]; /* grupos literales ("groups"), p. ej. vectores reales */
    int nraw;
} params_t;

/* Los textos de RDS son EBU Latin; para ASCII basta con copiar los bytes. */
static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    p->pi = 0xC201;
    snprintf(p->ps, sizeof p->ps, "BLADERF ");
    snprintf(p->rt, sizeof p->rt, "BladeRF Lab RDS: hola mundo!");
    p->pty = 10;
    p->tp = 1;
    p->ta = 0;
    p->ms = 1;
    p->af[0] = 100300000;
    p->af[1] = 98500000;
    p->af[2] = 104700000;
    p->naf = 3;
    p->y = 2026, p->mo = 11, p->d = 15, p->hh = 12, p->mm = 34, p->ct_off = 2;
    p->seconds = 5.0;
    p->nraw = 0;
    p->audio_hz = 1000.0;
    p->audio_level = 0.4;
    p->pilot = 0.09;
    p->rds_level = 0.035;
    if (!js || !js[0])
        return 0;
    jtok_t t[320];
    int nt = json_parse(js, strlen(js), t, 320);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    char s[96];
    double v;
    bool b;
    int i;
    if ((i = json_obj_get(js, t, 0, "pi")) >= 0) {
        if (json_str(js, t, i, s, sizeof s))
            p->pi = (uint16_t)strtoul(s, NULL, 16);
        else if (json_num(js, t, i, &v))
            p->pi = (uint16_t)v;
    }
    if ((i = json_obj_get(js, t, 0, "ps")) >= 0 && json_str(js, t, i, s, sizeof s))
        snprintf(p->ps, sizeof p->ps, "%-8.8s", s);
    if ((i = json_obj_get(js, t, 0, "rt")) >= 0)
        (void)json_str(js, t, i, p->rt, sizeof p->rt);
    if ((i = json_obj_get(js, t, 0, "groups")) >= 0 && t[i].type == J_ARR) {
        for (int k = 0; k < t[i].size && p->nraw < 64; k++) {
            int e = json_arr_get(t, i, k);
            if (e < 0 || !json_str(js, t, e, s, sizeof s) || strlen(s) != 16)
                continue;
            for (int w = 0; w < 4; w++) {
                char q[5] = {s[4 * w], s[4 * w + 1], s[4 * w + 2], s[4 * w + 3], 0};
                p->raw[p->nraw][w] = (uint16_t)strtoul(q, NULL, 16);
            }
            p->nraw++;
        }
    }
    if ((i = json_obj_get(js, t, 0, "pty")) >= 0 && json_num(js, t, i, &v))
        p->pty = (int)CLAMP(v, 0, 31);
    if ((i = json_obj_get(js, t, 0, "tp")) >= 0 && json_bool(js, t, i, &b))
        p->tp = b;
    if ((i = json_obj_get(js, t, 0, "ta")) >= 0 && json_bool(js, t, i, &b))
        p->ta = b;
    if ((i = json_obj_get(js, t, 0, "ms")) >= 0 && json_bool(js, t, i, &b))
        p->ms = b;
    if ((i = json_obj_get(js, t, 0, "af")) >= 0 && t[i].type == J_ARR) {
        p->naf = 0;
        for (int k = 0; k < t[i].size && p->naf < 25; k++) {
            int e = json_arr_get(t, i, k);
            if (e >= 0 && json_num(js, t, e, &v))
                p->af[p->naf++] = (int64_t)v;
        }
    }
    if ((i = json_obj_get(js, t, 0, "ct")) >= 0 && json_str(js, t, i, s, sizeof s)) {
        int y, mo, d, hh, mm;
        if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &hh, &mm) == 5) {
            p->y = y, p->mo = mo, p->d = d, p->hh = hh, p->mm = mm;
        }
    }
    if ((i = json_obj_get(js, t, 0, "ct_off")) >= 0 && json_num(js, t, i, &v))
        p->ct_off = (int)CLAMP(v, -31, 31);
    if ((i = json_obj_get(js, t, 0, "seconds")) >= 0 && json_num(js, t, i, &v))
        p->seconds = CLAMP(v, 0.5, 600);
    if ((i = json_obj_get(js, t, 0, "audio_hz")) >= 0 && json_num(js, t, i, &v))
        p->audio_hz = CLAMP(v, 20, 15000);
    if ((i = json_obj_get(js, t, 0, "audio_level")) >= 0 && json_num(js, t, i, &v))
        p->audio_level = CLAMP(v, 0, 0.6);
    if ((i = json_obj_get(js, t, 0, "pilot")) >= 0 && json_num(js, t, i, &v))
        p->pilot = CLAMP(v, 0, 0.12);
    if ((i = json_obj_get(js, t, 0, "rds_level")) >= 0 && json_num(js, t, i, &v))
        p->rds_level = CLAMP(v, 0.001, 0.1);
    return 0;
}

/* ---- grupos ------------------------------------------------------------- */
typedef struct {
    uint8_t *bits; /* un bit por byte, ya con CRC y offsets (antes de la diferencial) */
    int n, cap;
} bitbuf_t;

static void put_group(bitbuf_t *bb, const uint16_t w[4], bool vb)
{
    if (bb->n + 104 > bb->cap) {
        bb->cap = (bb->n + 104) * 2;
        bb->bits = realloc(bb->bits, (size_t)bb->cap);
    }
    static const int offs_a[4] = {RDS_OFF_A, RDS_OFF_B, RDS_OFF_C, RDS_OFF_D};
    for (int k = 0; k < 4; k++) {
        int off = offs_a[k];
        if (k == 2 && vb)
            off = RDS_OFF_CP;
        uint32_t blk = rds_block_encode(w[k], off);
        for (int i = 25; i >= 0; i--)
            bb->bits[bb->n++] = (blk >> i) & 1;
    }
}

static int af_code(int64_t hz)
{
    int c = (int)((hz - 87500000LL + 50000) / 100000);
    return c >= 1 && c <= 204 ? c : 205;
}

static int build_bits(const params_t *p, bitbuf_t *bb)
{
    int ngroups = (int)ceil(p->seconds * RDS_BITRATE / 104.0);
    int rtlen = (int)strlen(p->rt);
    uint8_t rt[64];
    memset(rt, ' ', sizeof rt);
    memcpy(rt, p->rt, (size_t)rtlen);
    int nseg = (rtlen + 4) / 4; /* incluye el 0x0D final si cabe */
    if (rtlen < 64)
        rt[rtlen] = 0x0D;
    if (nseg > 16)
        nseg = 16;
    /* Lista de AF en bytes: [224+n, f1, f2, …] */
    uint8_t afb[64];
    int nafb = 0;
    afb[nafb++] = (uint8_t)(224 + p->naf);
    for (int i = 0; i < p->naf; i++)
        afb[nafb++] = (uint8_t)af_code(p->af[i]);
    if (nafb & 1)
        afb[nafb++] = 205;
    int mjd = rds_date_to_mjd(p->y, p->mo, p->d);
    int utc_min = p->hh * 60 + p->mm - p->ct_off * 30;
    int utc_mjd = mjd;
    if (utc_min < 0) {
        utc_min += 1440;
        utc_mjd--;
    } else if (utc_min >= 1440) {
        utc_min -= 1440;
        utc_mjd++;
    }
    int step = 0, afpos = 0, g = 0;
    if (p->nraw > 0) {
        for (; g < ngroups; g++)
            put_group(bb, p->raw[g % p->nraw], (p->raw[g % p->nraw][1] >> 11) & 1);
        return bb->n;
    }
    uint16_t pty_tp = (uint16_t)((p->tp << 10) | (p->pty << 5));
    while (g < ngroups) {
        uint16_t w[4];
        /* 0A */
        int seg = step & 3;
        w[0] = p->pi;
        w[1] = (uint16_t)(0x0000 | pty_tp | (p->ta << 4) | (p->ms << 3) | (1 << 2) | seg);
        w[2] = (uint16_t)((afb[afpos] << 8) | afb[afpos + 1]);
        afpos = (afpos + 2) % nafb;
        w[3] = (uint16_t)(((uint8_t)p->ps[seg * 2] << 8) | (uint8_t)p->ps[seg * 2 + 1]);
        put_group(bb, w, false);
        if (++g >= ngroups)
            break;
        /* 2A */
        int s2 = step % nseg;
        w[1] = (uint16_t)(0x2000 | pty_tp | (0 << 4) | s2);
        w[2] = (uint16_t)((rt[s2 * 4] << 8) | rt[s2 * 4 + 1]);
        w[3] = (uint16_t)((rt[s2 * 4 + 2] << 8) | rt[s2 * 4 + 3]);
        put_group(bb, w, false);
        if (++g >= ngroups)
            break;
        if (step % 6 == 5) {
            w[1] = (uint16_t)(0x4000 | pty_tp | ((utc_mjd >> 15) & 3));
            w[2] = (uint16_t)(((utc_mjd & 0x7FFF) << 1) | (utc_min / 60 >> 4));
            int sign = p->ct_off < 0, mag = abs(p->ct_off);
            w[3] = (uint16_t)(((utc_min / 60 & 15) << 12) | ((utc_min % 60) << 6) | (sign << 5) | mag);
            put_group(bb, w, false);
            g++;
        }
        step++;
    }
    return bb->n;
}

static int rds_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    bitbuf_t bb = {0};
    int nbits = build_bits(&p, &bb);
    /* codificación diferencial: b'[k] = b[k] XOR b'[k−1] */
    int8_t *sgn = malloc((size_t)nbits);
    int prev = 0;
    for (int k = 0; k < nbits; k++) {
        prev ^= bb.bits[k];
        sgn[k] = prev ? 1 : -1;
    }
    out->rate = RDS_RATE;
    int ns = (int)((double)nbits / RDS_BITRATE * RDS_RATE);
    iqbuf_reserve(out, ns + 16);
    double ph_fm = 0.0;
    for (int n = 0; n < ns; n++) {
        double t = (double)n / RDS_RATE;
        double th = 2.0 * M_PI * 19000.0 * t;
        double bpos = t * RDS_BITRATE;
        int k = (int)bpos;
        double u = bpos - k;
        double pulse = k < nbits ? sgn[k] * sin(2.0 * M_PI * u) : 0.0;
        double tone = sin(2.0 * M_PI * p.audio_hz * t);
        double mpx = p.audio_level * 0.5 * tone * (1.0 + sin(2.0 * th)) + p.pilot * sin(th) +
                     p.rds_level * pulse * cos(3.0 * th);
        ph_fm += 2.0 * M_PI * 75000.0 * mpx / RDS_RATE;
        cf32 s = {(float)(0.8 * cos(ph_fm)), (float)(0.8 * sin(ph_fm))};
        iqbuf_push(out, s);
    }
    free(sgn);
    free(bb.bits);
    return 0;
}

const modulator_desc_t MOD_RDS = {
    .id = "rds",
    .name = "RDS",
    .air_ok = false,
    .modulate = rds_modulate,
};
