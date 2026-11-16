/*
 * mod_adsb.c — Modulador ADS-B / Mode S (DF17) a 1090 MHz, 2 Msps.
 *
 * Construye las tramas que un avión emitiría y las modula como el estándar
 * (RTCA DO-260B / ICAO Anexo 10): modulación por posición de pulso (PPM).
 *
 *   - Cada bit dura 1 µs = 2 muestras a 2 Msps. Un 1 es un pulso en la primera
 *     mitad ("alto, bajo"); un 0, en la segunda ("bajo, alto").
 *   - Preámbulo de 8 µs (16 muestras): pulsos de 0,5 µs en 0, 1, 3,5 y 4,5 µs,
 *     es decir, las muestras 0, 2, 7 y 9 altas. Luego 112 bits = 224 muestras.
 *   - La trama de 112 bits: DF(5)=17, CA(3), ICAO(24), ME(56), PI(24) donde PI
 *     es el CRC-24 (polinomio 0xFFF409) de los 88 bits anteriores.
 *
 * Se genera una ráfaga con cuatro tipos de mensaje (ver J. Sun, "The 1090
 * Megahertz Riddle", cap. 3-6): identificación (TC 4), posición en vuelo par e
 * impar (TC 11, CPR de 17 bits) y velocidad (TC 19, subtipo 1).
 *
 * Parámetros JSON (todos opcionales):
 *   {"icao":"4840D6","callsign":"KLM1023","alt":38000,"lat":52.2572,
 *    "lon":3.9194,"gs":159,"trk":182.9,"vr":-832,"repeat":2}
 *
 * air_ok = false: ADS-B no puede radiarse (banda aeronáutica protegida).
 * El decodificador solo ve el módulo de la señal, por eso la fase de la
 * portadora compleja es arbitraria.
 */
#include "mod.h"

#include "../decoders/adsb.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMP 0.8f
#define GAP_SAMPLES 20000 /* 10 ms entre tramas */

typedef struct {
    uint32_t icao;
    char callsign[9];
    int alt_ft;
    double lat, lon, gs_kt, trk_deg;
    int vr_fpm;
    int repeat;
} params_t;

/* Escribe 'nbits' bits de v (MSB primero) en msg desde el bit 'pos' (0-based). */
static void put_bits(uint8_t *msg, int pos, int nbits, uint32_t v)
{
    for (int i = 0; i < nbits; i++) {
        int bit = (v >> (nbits - 1 - i)) & 1;
        int p = pos + i;
        if (bit)
            msg[p >> 3] |= (uint8_t)(1 << (7 - (p & 7)));
    }
}

/* Cierra una trama DF17: cabecera + ME + CRC. me: 7 bytes. */
static void build_df17(uint8_t msg[14], uint32_t icao, const uint8_t me[7])
{
    memset(msg, 0, 14);
    put_bits(msg, 0, 5, 17);
    put_bits(msg, 5, 3, 5); /* CA = 5: capacidad de nivel 2+ */
    put_bits(msg, 8, 24, icao);
    memcpy(msg + 4, me, 7);
    uint32_t crc = modes_crc24(msg, 88);
    put_bits(msg, 88, 24, crc);
}

static uint32_t cs_code(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (uint32_t)(c - 'A' + 1);
    if (c >= '0' && c <= '9')
        return (uint32_t)(c - '0' + 48);
    return 32; /* espacio */
}

static void me_ident(uint8_t me[7], const char *cs)
{
    memset(me, 0, 7);
    put_bits(me, 0, 5, 4);  /* TC 4: identificación, categoría A */
    put_bits(me, 5, 3, 0);  /* sin información de categoría */
    for (int i = 0; i < 8; i++)
        put_bits(me, 8 + 6 * i, 6, i < (int)strlen(cs) ? cs_code(cs[i]) : 32);
}

static double fmod_pos(double x, double y) { return x - y * floor(x / y); }

/* Codificación CPR (inversa de la de adsb.c): 17 bits por coordenada.
 * Latitud:  YZ = floor(2^17 · mod(lat, dlat)/dlat + 0,5), dlat = 360/(60 − i).
 * Longitud: igual con dlon = 360/max(NL(lat') − i, 1), siendo lat' la latitud
 * que reconstruye el receptor a partir de YZ (así emisor y receptor coinciden
 * incluso en el borde de una zona). */
static void cpr_encode(double lat, double lon, int odd, uint32_t *yz, uint32_t *xz)
{
    const double dlat = 360.0 / (60.0 - odd);
    double y = floor(131072.0 * fmod_pos(lat, dlat) / dlat + 0.5);
    double rlat = dlat * (floor(lat / dlat) + y / 131072.0);
    int nl = cpr_nl(rlat);
    double dlon = 360.0 / (double)MAX(nl - odd, 1);
    double x = floor(131072.0 * fmod_pos(lon, dlon) / dlon + 0.5);
    *yz = (uint32_t)y & 0x1FFFF;
    *xz = (uint32_t)x & 0x1FFFF;
}

static void me_position(uint8_t me[7], int alt_ft, int odd, double lat, double lon)
{
    memset(me, 0, 7);
    put_bits(me, 0, 5, 11); /* TC 11: posición en vuelo, altitud barométrica */
    uint32_t n = (uint32_t)((alt_ft + 1000) / 25) & 0x7FF;
    /* AC12: N de 11 bits con el bit Q (=1, pasos de 25 ft) intercalado */
    uint32_t ac = ((n >> 4) << 5) | 0x10 | (n & 0xF);
    put_bits(me, 8, 12, ac);
    put_bits(me, 21, 1, (uint32_t)odd);
    uint32_t yz, xz;
    cpr_encode(lat, lon, odd, &yz, &xz);
    put_bits(me, 22, 17, yz);
    put_bits(me, 39, 17, xz);
}

static void me_velocity(uint8_t me[7], double gs, double trk_deg, int vr_fpm)
{
    memset(me, 0, 7);
    put_bits(me, 0, 5, 19);
    put_bits(me, 5, 3, 1); /* subtipo 1: velocidad sobre el suelo, subsónica */
    double vx = gs * sin(trk_deg * M_PI / 180.0), vy = gs * cos(trk_deg * M_PI / 180.0);
    put_bits(me, 13, 1, vx < 0);
    put_bits(me, 14, 10, (uint32_t)(fabs(vx) + 0.5) + 1);
    put_bits(me, 24, 1, vy < 0);
    put_bits(me, 25, 10, (uint32_t)(fabs(vy) + 0.5) + 1);
    put_bits(me, 36, 1, vr_fpm < 0);
    put_bits(me, 37, 9, (uint32_t)(abs(vr_fpm) / 64) + 1);
}

/* Modula una trama de 112 bits como PPM a 2 Msps con fase de portadora ph. */
static void modulate_frame(iqbuf_t *o, const uint8_t msg[14], double ph)
{
    static const int pre[16] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
    const cf32 on = {(float)(AMP * cos(ph)), (float)(AMP * sin(ph))}, off = {0, 0};
    iqbuf_reserve(o, 240);
    for (int i = 0; i < 16; i++)
        iqbuf_push(o, pre[i] ? on : off);
    for (int b = 0; b < 112; b++) {
        int bit = (msg[b >> 3] >> (7 - (b & 7))) & 1;
        iqbuf_push(o, bit ? on : off);
        iqbuf_push(o, bit ? off : on);
    }
}

static void gap(iqbuf_t *o, int n)
{
    iqbuf_reserve(o, n);
    memset(o->s + o->n, 0, sizeof(cf32) * (size_t)n);
    o->n += n;
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    p->icao = 0x4840D6;
    snprintf(p->callsign, sizeof p->callsign, "KLM1023");
    p->alt_ft = 38000;
    p->lat = 52.2572;
    p->lon = 3.9194;
    p->gs_kt = 159.0;
    p->trk_deg = 182.9;
    p->vr_fpm = -832;
    p->repeat = 2;
    if (!js || !js[0])
        return 0;
    jtok_t t[64];
    int nt = json_parse(js, strlen(js), t, 64);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    char s[32];
    double v;
    int i;
    if ((i = json_obj_get(js, t, 0, "icao")) >= 0 && json_str(js, t, i, s, sizeof s))
        p->icao = (uint32_t)strtoul(s, NULL, 16) & 0xFFFFFF;
    if ((i = json_obj_get(js, t, 0, "callsign")) >= 0 && json_str(js, t, i, s, sizeof s)) {
        snprintf(p->callsign, sizeof p->callsign, "%.8s", s);
        for (char *c = p->callsign; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c = (char)(*c - 'a' + 'A');
    }
    if ((i = json_obj_get(js, t, 0, "alt")) >= 0 && json_num(js, t, i, &v))
        p->alt_ft = (int)CLAMP(v, -1000, 50175);
    if ((i = json_obj_get(js, t, 0, "lat")) >= 0 && json_num(js, t, i, &v))
        p->lat = CLAMP(v, -85, 85);
    if ((i = json_obj_get(js, t, 0, "lon")) >= 0 && json_num(js, t, i, &v))
        p->lon = CLAMP(v, -180, 180);
    if ((i = json_obj_get(js, t, 0, "gs")) >= 0 && json_num(js, t, i, &v))
        p->gs_kt = CLAMP(v, 0, 1000);
    if ((i = json_obj_get(js, t, 0, "trk")) >= 0 && json_num(js, t, i, &v))
        p->trk_deg = v;
    if ((i = json_obj_get(js, t, 0, "vr")) >= 0 && json_num(js, t, i, &v))
        p->vr_fpm = (int)CLAMP(v, -32000, 32000);
    if ((i = json_obj_get(js, t, 0, "repeat")) >= 0 && json_num(js, t, i, &v))
        p->repeat = (int)CLAMP(v, 1, 100);
    return 0;
}

static int adsb_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    uint8_t me[4][7], msg[14];
    me_ident(me[0], p.callsign);
    me_position(me[1], p.alt_ft, 0, p.lat, p.lon);
    me_position(me[2], p.alt_ft, 1, p.lat, p.lon);
    me_velocity(me[3], p.gs_kt, p.trk_deg, p.vr_fpm);

    out->rate = ADSB_RATE;
    gap(out, 400);
    double ph = 0.3; /* fase de portadora arbitraria */
    for (int r = 0; r < p.repeat; r++)
        for (int k = 0; k < 4; k++) {
            build_df17(msg, p.icao, me[k]);
            modulate_frame(out, msg, ph);
            ph += 0.7;
            gap(out, GAP_SAMPLES);
        }
    return 0;
}

const modulator_desc_t MOD_ADSB = {
    .id = "adsb",
    .name = "ADS-B / Mode S",
    .air_ok = false,
    .modulate = adsb_modulate,
};
