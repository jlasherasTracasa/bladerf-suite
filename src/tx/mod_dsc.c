/*
 * mod_dsc.c — Modulador DSC VHF (ITU-R M.493), canal 70, 48 ksps.
 *
 * Construye llamadas completas: patrón de puntos de 200 bits alternos, secuencia
 * de fase (DX 125 / RX 111…104), el mensaje con diversidad temporal (cada
 * símbolo en DX y repetido en RX 5 huecos después), EOS y el carácter de control
 * (ECC = XOR desde el especificador de formato hasta el EOS). Cada símbolo son
 * 10 bits: 7 de información LSB primero + 3 con el nº de ceros.
 *
 * Capa física: FSK de fase continua a 1200 Bd con 1300 Hz = "1" y 2100 Hz =
 * "0" (polaridad sin verificar contra M.493; el decodificador prueba ambas),
 * como audio de un modulador FM (desviación pico 2,5 kHz, sin verificar).
 *
 * air_ok = false: la banda marítima VHF y el canal 70 son de socorro; nunca se
 * radia.
 *
 * Parámetros JSON: {"type":"distress|all_ships|individual|group|area" (por
 * defecto una llamada de cada), "mmsi":"224123456","to":"366123456",
 * "category":108,"nature":107,"lat":42.5,"lon":-3.5,"time":"1234",
 * "area":"0422080320","tc1":100,"tc2":126}
 */
#include "mod.h"

#include "../decoders/dsc.h"
#include "../util/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define DEV_HZ 2500.0
#define MAX_SYM 96

typedef struct {
    char mmsi[16], to[16], area[16], time[8], type[16];
    int category, nature, tc1, tc2;
    double lat, lon;
} params_t;

/* Cadena de dígitos → símbolos de 2 dígitos. n_sym símbolos (se rellena con 0). */
static void digits_to_syms(const char *d, int n_sym, int *out)
{
    for (int i = 0; i < n_sym; i++) {
        int a = 2 * i < (int)strlen(d) ? d[2 * i] - '0' : 0;
        int b = 2 * i + 1 < (int)strlen(d) ? d[2 * i + 1] - '0' : 0;
        out[i] = a * 10 + b;
    }
}

static void mmsi_syms(const char *mmsi, int *out)
{
    char d[12];
    snprintf(d, sizeof d, "%-9.9s0", mmsi); /* 9 dígitos + 0 de ampliación */
    for (char *c = d; *c; c++)
        if (*c == ' ')
            *c = '0';
    digits_to_syms(d, 5, out);
}

static void coords_syms(const params_t *p, int *out)
{
    double a = fabs(p->lat), o = fabs(p->lon);
    int q = (p->lat < 0 ? 2 : 0) + (p->lon < 0 ? 1 : 0);
    int latd = (int)a, latm = (int)((a - latd) * 60.0 + 0.5);
    int lond = (int)o, lonm = (int)((o - lond) * 60.0 + 0.5);
    char d[40];
    snprintf(d, sizeof d, "%d%02d%02d%03d%02d", q, latd, latm % 60, lond, lonm % 60);
    digits_to_syms(d, 5, out);
}

static int build_message(const params_t *p, const char *type, int *s)
{
    int n = 0;
    if (!strcmp(type, "distress")) {
        s[n++] = 112;
        mmsi_syms(p->mmsi, s + n);
        n += 5;
        s[n++] = p->nature;
        coords_syms(p, s + n);
        n += 5;
        digits_to_syms(p->time, 2, s + n);
        n += 2;
        s[n++] = 126; /* sin información de comunicación subsiguiente */
    } else {
        int fs = !strcmp(type, "all_ships") ? 116 : !strcmp(type, "individual") ? 120
                 : !strcmp(type, "group") ? 114 : !strcmp(type, "area") ? 102 : 0;
        if (!fs)
            return -1;
        s[n++] = fs;
        if (fs != 116) {
            if (fs == 102)
                digits_to_syms(p->area, 5, s + n);
            else
                mmsi_syms(p->to, s + n);
            n += 5;
        }
        s[n++] = p->category;
        mmsi_syms(p->mmsi, s + n);
        n += 5;
        s[n++] = p->tc1;
        s[n++] = p->tc2;
        s[n++] = 126; /* canal/frecuencia: sin información */
        s[n++] = 126;
    }
    s[n] = 127; /* EOS: fin de secuencia */
    n++;
    s[n] = dsc_ecc(s, n);
    return n + 1;
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    *p = (params_t){.category = 108, .nature = 107, .tc1 = 100, .tc2 = 126, .lat = 43.35, .lon = -8.4};
    snprintf(p->mmsi, sizeof p->mmsi, "224123456");
    snprintf(p->to, sizeof p->to, "366123456");
    snprintf(p->area, sizeof p->area, "0422080320");
    snprintf(p->time, sizeof p->time, "1234");
    if (js && js[0]) {
        jtok_t t[48];
        int nt = json_parse(js, strlen(js), t, 48);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        int i;
        double v;
#define STR(key, field)                                                                  \
    if ((i = json_obj_get(js, t, 0, key)) >= 0) {                                        \
        if (t[i].type == J_NUM && json_num(js, t, i, &v))                                \
            snprintf(p->field, sizeof p->field, "%.0f", v);                              \
        else                                                                             \
            json_str(js, t, i, p->field, sizeof p->field);                               \
    }
#define NUM(key, field)                                                                  \
    if ((i = json_obj_get(js, t, 0, key)) >= 0 && json_num(js, t, i, &v))                \
        p->field = (__typeof__(p->field))v
        STR("mmsi", mmsi);
        STR("to", to);
        STR("area", area);
        STR("time", time);
        if ((i = json_obj_get(js, t, 0, "type")) >= 0)
            json_str(js, t, i, p->type, sizeof p->type);
        NUM("category", category);
        NUM("nature", nature);
        NUM("tc1", tc1);
        NUM("tc2", tc2);
        NUM("lat", lat);
        NUM("lon", lon);
    }
    p->lat = CLAMP(p->lat, -89.9, 89.9);
    p->lon = CLAMP(p->lon, -179.9, 179.9);
    if (strlen(p->mmsi) != 9 || strspn(p->mmsi, "0123456789") != 9 || strspn(p->to, "0123456789") != strlen(p->to) ||
        strspn(p->area, "0123456789") != strlen(p->area) || strlen(p->time) != 4 ||
        strspn(p->time, "0123456789") != 4) {
        snprintf(err, errlen, "mmsi (9 dígitos), to, area (dígitos) o time (hhmm) no válidos");
        return -1;
    }
    for (int v = 0; v < 4; v++) {
        int c = v == 0 ? p->category : v == 1 ? p->nature : v == 2 ? p->tc1 : p->tc2;
        if (c < 0 || c > 127) {
            snprintf(err, errlen, "símbolo fuera de 0..127");
            return -1;
        }
    }
    return 0;
}

static void put_tone_bits(iqbuf_t *o, const uint8_t *bits, int n, double *pa, double *pf)
{
    const int sps = (int)lrint(FS / 1200.0);
    iqbuf_reserve(o, n * sps + 8);
    for (int i = 0; i < n; i++) {
        double f = bits[i] ? 1300.0 : 2100.0;
        for (int k = 0; k < sps; k++) {
            *pa += 2.0 * M_PI * f / FS;
            if (*pa > 2.0 * M_PI)
                *pa -= 2.0 * M_PI;
            *pf += 2.0 * M_PI * DEV_HZ / FS * sin(*pa);
            iqbuf_push(o, (cf32){(float)(0.9 * cos(*pf)), (float)(0.9 * sin(*pf))});
        }
    }
}

static void silence(iqbuf_t *o, int n)
{
    iqbuf_reserve(o, n);
    memset(o->s + o->n, 0, sizeof(cf32) * (size_t)n);
    o->n += n;
}

static int one_call(const params_t *p, const char *type, iqbuf_t *out, double *pa, double *pf, char *err, size_t errlen)
{
    int msg[MAX_SYM];
    int n = build_message(p, type, msg);
    if (n < 0) {
        snprintf(err, errlen, "tipo DSC '%s' no válido", type);
        return -1;
    }
    int ns = 2 * n + 16 + 12;
    int slot[2 * MAX_SYM + 20];
    for (int i = 0; i < ns; i++)
        slot[i] = 126;
    for (int k = 0; k < 6; k++)
        slot[2 * k] = 125; /* secuencia de fase, posiciones DX */
    for (int k = 0; k < 8; k++)
        slot[2 * k + 1] = 111 - k; /* posiciones RX: 111 … 104 */
    /* M.493: el especificador de formato va dos veces seguidas en DX; después de la secuencia, el
     * EOS se envía 3 veces en DX (el ECC va detrás del primero) y solo el primero y el ECC tienen
     * copia RX (Fig. 1b, §9). msg[] trae un FS, ..., EOS, ECC. */
    int ex[MAX_SYM + 4], nx = 0, last_rx;
    ex[nx++] = msg[0];
    ex[nx++] = msg[0];
    for (int i = 1; i < n; i++)
        ex[nx++] = msg[i];
    last_rx = nx - 1; /* ECC */
    ex[nx++] = msg[n - 2]; /* 2.º EOS */
    ex[nx++] = msg[n - 2]; /* 3.er EOS */
    for (int i = 0; i < nx; i++) {
        if (12 + 2 * i < ns) slot[12 + 2 * i] = ex[i];
        if (i <= last_rx && 17 + 2 * i < ns) slot[17 + 2 * i] = ex[i];
    }
    static uint8_t bits[200 + 10 * (2 * MAX_SYM + 20)];
    int nb = 0;
    for (int i = 0; i < 200; i++)
        bits[nb++] = (uint8_t)(i & 1);
    for (int i = 0; i < ns; i++) {
        dsc_sym_encode(slot[i], bits + nb);
        nb += 10;
    }
    silence(out, 4800);
    put_tone_bits(out, bits, nb, pa, pf);
    silence(out, 4800);
    return 0;
}

static int dsc_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    out->rate = FS;
    double pa = 0, pf = 0;
    if (p.type[0])
        return one_call(&p, p.type, out, &pa, &pf, err, errlen);
    static const char *seq[] = {"distress", "all_ships", "individual", "group", "area"};
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++)
        if (one_call(&p, seq[i], out, &pa, &pf, err, errlen) < 0)
            return -1;
    return 0;
}

const modulator_desc_t MOD_DSC = {
    .id = "dsc",
    .name = "DSC VHF (canal 70)",
    .air_ok = false,
    .modulate = dsc_modulate,
};
