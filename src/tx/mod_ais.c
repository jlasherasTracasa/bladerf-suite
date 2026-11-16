/*
 * mod_ais.c — Modulador AIS (ITU-R M.1371): GMSK BT 0,4, 9600 bit/s, 48 ksps.
 *
 * Construye ráfagas con mensajes de los tipos 1, 5, 18, 24 (A y B), 19, 21, 4 y
 * 27, cada una como una ráfaga: secuencia de entrenamiento de 24 bits,
 * bandera, carga + FCS con inserción de ceros, bandera, colchón. Todo el flujo
 * se codifica NRZI y se modula en GMSK:
 *
 *   frecuencia instantánea = Σ a_k·g(t − kT),  a_k = ±1 (nivel NRZI)
 *   g = pulso gaussiano (BT 0,4) ⊛ rectángulo de 1 símbolo, de área 1
 *   fase  φ[n] = φ[n−1] + (π/2)·y[n]            (índice de modulación 0,5)
 *
 * es decir, un símbolo gira la fase ±π/2 y la desviación es ±2400 Hz.
 * Los campos van como en la tabla de gpsd (AIVDM/AIVDO protocol decoding).
 *
 * air_ok = false: AIS opera en la banda marítima protegida (nunca se radia).
 *
 * Parámetros JSON (todos opcionales): {"mmsi":265547250,"lat":57.66,"lon":11.83,
 * "sog":13.9,"cog":40.4,"hdg":41,"navstat":0,"name":"EVER DIADEM","callsign":"3FOF8",
 * "shiptype":70,"dest":"NEW YORK","imo":9134270,"bow":225,"stern":70,"port":1,"stbd":31,
 * "types":[1,5,18,24,19,21,4,27]}
 */
#include "mod.h"

#include "../decoders/ais.h"
#include "../decoders/hdlc.h"
#include "../dsp/bits.h"
#include "../dsp/pulse.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000.0
#define SPS 5
#define MAXB 1200

typedef struct {
    uint32_t mmsi;
    double lat, lon, sog, cog;
    int hdg, navstat, shiptype, imo, bow, stern, port, stbd;
    char name[21], callsign[8], dest[21];
    int types[16], ntypes;
} params_t;

static void put(uint8_t *b, int pos, int len, uint32_t v)
{
    for (int i = 0; i < len; i++)
        b[pos + i] = (uint8_t)((v >> (len - 1 - i)) & 1);
}

static void put_text(uint8_t *b, int pos, int nchars, const char *s)
{
    for (int i = 0; i < nchars; i++) {
        int c = i < (int)strlen(s) ? (unsigned char)s[i] : '@';
        if (c >= 'a' && c <= 'z')
            c -= 32;
        put(b, pos + 6 * i, 6, (uint32_t)(c >= 64 ? c - 64 : c));
    }
}

static int build(const params_t *p, int type, uint8_t *b)
{
    memset(b, 0, MAXB);
    uint32_t mmsi = p->mmsi;
    int n = 168;
    if (type == 18 || type == 19 || type == 24)
        mmsi = p->mmsi + 1; /* barco de clase B distinto */
    if (type == 4)
        mmsi = 2241022;     /* estación base de ejemplo */
    if (type == 21)
        mmsi = 993123456;   /* ayuda a la navegación virtual */
    put(b, 0, 6, (uint32_t)type);
    put(b, 8, 30, mmsi);
    uint32_t lon = (uint32_t)lrint(p->lon * 600000.0) & 0xFFFFFFF, lat = (uint32_t)lrint(p->lat * 600000.0) & 0x7FFFFFF;
    uint32_t sp = (uint32_t)lrint(p->sog * 10), co = (uint32_t)lrint(p->cog * 10);
    switch (type) {
    case 1:
        put(b, 38, 4, (uint32_t)p->navstat);
        put(b, 42, 8, 128); /* sin información de giro */
        put(b, 50, 10, sp);
        put(b, 60, 1, 1);
        put(b, 61, 28, lon);
        put(b, 89, 27, lat);
        put(b, 116, 12, co);
        put(b, 128, 9, (uint32_t)p->hdg);
        put(b, 137, 6, 33);
        break;
    case 5:
        n = 424;
        put(b, 38, 2, 0);
        put(b, 40, 30, (uint32_t)p->imo);
        put_text(b, 70, 7, p->callsign);
        put_text(b, 112, 20, p->name);
        put(b, 232, 8, (uint32_t)p->shiptype);
        put(b, 240, 9, (uint32_t)p->bow);
        put(b, 249, 9, (uint32_t)p->stern);
        put(b, 258, 6, (uint32_t)p->port);
        put(b, 264, 6, (uint32_t)p->stbd);
        put(b, 270, 4, 1);
        put(b, 274, 4, 12);
        put(b, 278, 5, 24);
        put(b, 283, 5, 20);
        put(b, 288, 6, 30);
        put(b, 294, 8, 122);
        put_text(b, 302, 20, p->dest);
        break;
    case 18:
        put(b, 46, 10, sp);
        put(b, 56, 1, 1);
        put(b, 57, 28, lon);
        put(b, 85, 27, lat);
        put(b, 112, 12, co);
        put(b, 124, 9, (uint32_t)p->hdg);
        put(b, 133, 6, 40);
        break;
    case 19:
        n = 312;
        put(b, 46, 10, sp);
        put(b, 56, 1, 1);
        put(b, 57, 28, lon);
        put(b, 85, 27, lat);
        put(b, 112, 12, co);
        put(b, 124, 9, (uint32_t)p->hdg);
        put(b, 133, 6, 41);
        put_text(b, 143, 20, p->name);
        put(b, 263, 8, 37);
        put(b, 271, 9, (uint32_t)p->bow);
        put(b, 280, 9, (uint32_t)p->stern);
        put(b, 289, 6, (uint32_t)p->port);
        put(b, 295, 6, (uint32_t)p->stbd);
        break;
    case 21:
        n = 272;
        put(b, 38, 5, 1); /* tipo de ayuda: punto de referencia */
        put_text(b, 43, 20, "BOYA PRUEBA");
        put(b, 163, 1, 1);
        put(b, 164, 28, lon);
        put(b, 192, 27, lat);
        put(b, 219, 9, 5);
        put(b, 228, 9, 5);
        put(b, 237, 6, 5);
        put(b, 243, 6, 5);
        put(b, 253, 6, 60);
        break;
    case 24: /* se devuelve la parte A; la B se pide como type 240 */
        put(b, 38, 2, 0);
        put_text(b, 40, 20, p->name);
        break;
    case 240:
        put(b, 0, 6, 24);
        put(b, 38, 2, 1);
        put(b, 40, 8, 37);
        put_text(b, 48, 3, "FEC");
        put_text(b, 90, 7, p->callsign);
        put(b, 132, 9, (uint32_t)p->bow);
        put(b, 141, 9, (uint32_t)p->stern);
        put(b, 150, 6, (uint32_t)p->port);
        put(b, 156, 6, (uint32_t)p->stbd);
        break;
    case 4:
        put(b, 38, 14, 2026);
        put(b, 52, 4, 11);
        put(b, 56, 5, 15);
        put(b, 61, 5, 12);
        put(b, 66, 6, 30);
        put(b, 72, 6, 0);
        put(b, 78, 1, 1);
        put(b, 79, 28, lon);
        put(b, 107, 27, lat);
        put(b, 134, 4, 1);
        break;
    case 27:
        n = 96;
        put(b, 38, 1, 1);
        put(b, 40, 4, (uint32_t)p->navstat);
        put(b, 44, 18, (uint32_t)lrint(p->lon * 600.0) & 0x3FFFF);
        put(b, 62, 17, (uint32_t)lrint(p->lat * 600.0) & 0x1FFFF);
        put(b, 79, 6, (uint32_t)lrint(p->sog));
        put(b, 85, 9, (uint32_t)lrint(p->cog));
        break;
    default:
        return -1;
    }
    return n;
}

/* Un mensaje → ráfaga GMSK añadida a 'o'. */
static int burst(iqbuf_t *o, const uint8_t *msg, int nbits, double *phase)
{
    static uint8_t train[24];
    uint8_t line[MAXB * 2], lev[MAXB * 2];
    float sym[MAXB * 2], *y;
    for (int i = 0; i < 24; i++)
        train[i] = (uint8_t)(i & 1);
    memcpy(line, train, 24);
    /* los campos M.1371 van con cada octeto invertido respecto al flujo HDLC */
    uint8_t wire[MAXB];
    if (nbits > MAXB)
        return -1;
    ais_unscramble_bytes(msg, nbits, wire);
    int n = hdlc_encode(wire, nbits, 1, 1, line + 24, (int)sizeof line - 24 - 32);
    if (n < 0)
        return -1;
    n += 24;
    for (int i = 0; i < 16; i++) /* colchón: sigue conmutando (retardo de propagación) */
        line[n++] = 1;
    nrzi_encode(line, n, lev, 0, 0);
    for (int i = 0; i < n; i++)
        sym[i] = lev[i] ? 1.0f : -1.0f;
    float h[40];
    int nt = pulse_gmsk(h, 0.4, SPS, 3);
    int ny = n * SPS + nt - 1;
    y = malloc(sizeof(float) * (size_t)ny);
    if (!y)
        return -1;
    ny = pulse_shape_f(sym, n, h, nt, SPS, y);
    iqbuf_reserve(o, ny + 4000);
    int ramp = 40;
    for (int i = 0; i < ny; i++) {
        *phase += (M_PI / 2.0) * y[i];
        double a = 0.9;
        if (i < ramp)
            a *= 0.5 - 0.5 * cos(M_PI * i / ramp);
        else if (i >= ny - ramp)
            a *= 0.5 - 0.5 * cos(M_PI * (ny - 1 - i) / ramp);
        iqbuf_push(o, (cf32){(float)(a * cos(*phase)), (float)(a * sin(*phase))});
    }
    free(y);
    int gap = 2400; /* 50 ms */
    iqbuf_reserve(o, gap);
    memset(o->s + o->n, 0, sizeof(cf32) * (size_t)gap);
    o->n += gap;
    return 0;
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    *p = (params_t){.mmsi = 265547250, .lat = 57.66035, .lon = 11.832976, .sog = 13.9, .cog = 40.4, .hdg = 41,
                    .navstat = 0, .shiptype = 70, .imo = 9134270, .bow = 225, .stern = 70, .port = 1, .stbd = 31};
    snprintf(p->name, sizeof p->name, "EVER DIADEM");
    snprintf(p->callsign, sizeof p->callsign, "3FOF8");
    snprintf(p->dest, sizeof p->dest, "NEW YORK");
    int def[] = {1, 5, 18, 24, 19, 21, 4, 27};
    p->ntypes = 8;
    memcpy(p->types, def, sizeof def);
    if (!js || !js[0])
        return 0;
    jtok_t t[96];
    int nt = json_parse(js, strlen(js), t, 96);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    double v;
    int i;
#define NUM(key, field, cast)                                                                \
    if ((i = json_obj_get(js, t, 0, key)) >= 0 && json_num(js, t, i, &v))                    \
        p->field = (cast)v
#define STR(key, field)                                                                      \
    if ((i = json_obj_get(js, t, 0, key)) >= 0)                                              \
        json_str(js, t, i, p->field, sizeof p->field)
    NUM("mmsi", mmsi, uint32_t);
    NUM("lat", lat, double);
    NUM("lon", lon, double);
    NUM("sog", sog, double);
    NUM("cog", cog, double);
    NUM("hdg", hdg, int);
    NUM("navstat", navstat, int);
    NUM("shiptype", shiptype, int);
    NUM("imo", imo, int);
    NUM("bow", bow, int);
    NUM("stern", stern, int);
    NUM("port", port, int);
    NUM("stbd", stbd, int);
    STR("name", name);
    STR("callsign", callsign);
    STR("dest", dest);
    if ((i = json_obj_get(js, t, 0, "types")) >= 0 && t[i].type == J_ARR) {
        p->ntypes = 0;
        for (int k = 0; k < t[i].size && k < 16; k++) {
            int e = json_arr_get(t, i, k);
            if (e >= 0 && json_num(js, t, e, &v))
                p->types[p->ntypes++] = (int)v;
        }
    }
    p->lat = CLAMP(p->lat, -90.0, 90.0);
    p->lon = CLAMP(p->lon, -180.0, 180.0);
    p->sog = CLAMP(p->sog, 0.0, 102.2);
    p->cog = CLAMP(p->cog, 0.0, 359.9);
    if (p->mmsi == 0 || p->mmsi > 999999998u) {
        snprintf(err, errlen, "mmsi fuera de rango");
        return -1;
    }
    return 0;
}

static int ais_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    out->rate = RATE;
    iqbuf_reserve(out, 8000);
    memset(out->s, 0, sizeof(cf32) * 4800);
    out->n = 4800;
    double phase = 0.4;
    uint8_t msg[MAXB];
    for (int k = 0; k < p.ntypes; k++) {
        int type = p.types[k], n = build(&p, type, msg);
        if (n < 0) {
            snprintf(err, errlen, "tipo AIS %d no soportado", type);
            return -1;
        }
        if (burst(out, msg, n, &phase) < 0) {
            snprintf(err, errlen, "mensaje demasiado largo");
            return -1;
        }
        if (type == 24) { /* parte B a continuación */
            n = build(&p, 240, msg);
            put(msg, 8, 30, p.mmsi + 1);
            if (burst(out, msg, n, &phase) < 0)
                return -1;
        }
    }
    return 0;
}

const modulator_desc_t MOD_AIS = {
    .id = "ais",
    .name = "AIS (GMSK 9600)",
    .air_ok = false,
    .modulate = ais_modulate,
};
