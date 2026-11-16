/*
 * mod_aprs.c — Modulador APRS / AX.25: AFSK 1200 Bell 202 sobre FM, 48 ksps.
 *
 * Cadena de emisión (inversa de la del decodificador):
 *   información APRS → trama AX.25 UI (direcciones, 0x03, 0xF0) → HDLC (bits
 *   LSB primero, inserción de ceros, FCS CRC-16/X-25, banderas) → NRZI (0 =
 *   cambio de tono) → AFSK de fase continua: 1 = 1200 Hz, 0 = 2200 Hz a 1200
 *   baudios → FM con desviación de ±3 kHz para amplitud de audio 1,0.
 *
 * El indicativo de origen sale de params.callsign y NO puede estar vacío
 * (AGENTS.md §6: radioaficionado exige indicativo en la trama; tx.c lo inserta
 * desde --callsign). Si falta se usa el marcador N0CALL solo para el modo file.
 *
 * air_ok = true: 144,800 MHz está en la banda de aficionados de 2 m (144-146
 * MHz) y tx.c exige --callsign. Duración de la secuencia por defecto ≈ 3,5 s,
 * dentro del límite de 5 s por orden.
 *
 * Parámetros JSON: {"callsign":"EA2XYZ-9","dest":"APZBLF","path":"WIDE1-1",
 *  "type":"position|compressed|mic-e|status|message|object|weather" (por defecto
 *  una secuencia de position, compressed, mic-e, status y message), "lat":49.0583,"lon":-72.0292,"symbol":"/>",
 *  "comment":"texto","to":"N0CALL","text":"hola","id":"001","info":"cadena APRS cruda"}
 */
#include "mod.h"

#include "../decoders/aprs.h"
#include "../decoders/hdlc.h"
#include "../dsp/bits.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define BAUD 1200.0
#define DEV_HZ 3000.0

typedef struct {
    char callsign[16], dest[16], path[64], symbol[3], comment[80], to[12], text[70], id[8], type[16], info[200];
    double lat, lon, speed, course;
} params_t;

static void fmt_lat(char *o, double lat)
{
    double a = fabs(lat);
    int d = (int)a;
    double m = (a - d) * 60.0;
    snprintf(o, 16, "%02d%05.2f%c", d, m, lat >= 0 ? 'N' : 'S');
}

static void fmt_lon(char *o, double lon)
{
    double a = fabs(lon);
    int d = (int)a;
    double m = (a - d) * 60.0;
    snprintf(o, 16, "%03d%05.2f%c", d, m, lon >= 0 ? 'E' : 'W');
}

static void b91(char *o, double v, int n)
{
    long x = (long)v;
    for (int i = n - 1; i >= 0; i--) {
        o[i] = (char)(33 + x % 91);
        x /= 91;
    }
}

/* Construye la información APRS del tipo pedido. dst puede reescribirse (Mic-E). */
static int build_info(const params_t *p, const char *type, uint8_t *info, char *dst)
{
    char la[16], lo[16];
    fmt_lat(la, p->lat);
    fmt_lon(lo, p->lon);
    if (p->info[0] && !strcmp(type, "raw"))
        return snprintf((char *)info, 200, "%s", p->info);
    if (!strcmp(type, "position"))
        return snprintf((char *)info, 200, "!%s%c%s%c%.100s", la, p->symbol[0], lo, p->symbol[1], p->comment);
    if (!strcmp(type, "compressed")) {
        char c[14];
        c[0] = p->symbol[0];
        b91(c + 1, 380926.0 * (90.0 - p->lat), 4);
        b91(c + 5, 190463.0 * (180.0 + p->lon), 4);
        c[9] = p->symbol[1];
        c[10] = (char)(33 + (int)(p->course / 4.0));
        c[11] = (char)(33 + (int)lround(log(p->speed + 1.0) / log(1.08)));
        c[12] = (char)(33 + 0x20); /* T: GPS vigente */
        c[13] = 0;
        return snprintf((char *)info, 200, "=%s%.100s", c, p->comment);
    }
    if (!strcmp(type, "status"))
        return snprintf((char *)info, 200, ">%.100s", p->comment);
    if (!strcmp(type, "message"))
        return snprintf((char *)info, 200, ":%-9.9s:%.60s{%.5s", p->to, p->text, p->id);
    if (!strcmp(type, "object"))
        return snprintf((char *)info, 200, ";%-9.9s*092345z%s%c%s%c%.60s", "BLFOBJ", la, p->symbol[0], lo,
                        p->symbol[1], p->comment);
    if (!strcmp(type, "weather"))
        return snprintf((char *)info, 200, "!%s/%s_220/004g005t077r000p000P000h50b09900BLF", la, lo);
    if (!strcmp(type, "mic-e")) {
        /* Destino: 6 caracteres con los dígitos de latitud y los bits de N/S,
         * desplazamiento de longitud +100 y O/E; los tres primeros llevan el
         * mensaje estándar "Off Duty" (111 → 'P'..'Y'). */
        double a = fabs(p->lat);
        int deg = (int)a;
        double mi = (a - deg) * 60.0;
        int mm = (int)mi, hh = (int)lround((mi - mm) * 100.0);
        if (hh == 100) {
            hh = 0;
            mm++;
        }
        int dig[6] = {deg / 10, deg % 10, mm / 10, mm % 10, hh / 10, hh % 10};
        int ld = (int)fabs(p->lon);
        double lmi = (fabs(p->lon) - ld) * 60.0;
        int lm = (int)lmi, lh = (int)lround((lmi - lm) * 100.0);
        bool north = p->lat >= 0, lon100 = ld >= 100, west = p->lon < 0;
        bool flag[6] = {1, 1, 1, north, lon100, west};
        for (int i = 0; i < 6; i++)
            dst[i] = (char)((flag[i] ? 'P' : '0') + dig[i]);
        dst[6] = 0;
        int raw = lon100 ? ld - 100 : (ld < 10 ? ld + 190 : ld);
        int sp = (int)p->speed, dc = (int)p->course % 360;
        info[0] = '`';
        info[1] = (uint8_t)(raw + 28);
        info[2] = (uint8_t)((lm < 10 ? lm + 60 : lm) + 28);
        info[3] = (uint8_t)(lh + 28);
        info[4] = (uint8_t)(sp / 10 + 28);
        info[5] = (uint8_t)((sp % 10) * 10 + dc / 100 + 28);
        info[6] = (uint8_t)(dc % 100 + 28);
        info[7] = (uint8_t)p->symbol[1];
        info[8] = (uint8_t)p->symbol[0];
        int n = 9;
        n += snprintf((char *)info + n, 100, "%.60s", p->comment);
        return n;
    }
    return -1;
}

static void afsk_fm(iqbuf_t *o, const uint8_t *lev, int n, double *ph_audio, double *ph_fm)
{
    int sps = (int)lrint(FS / BAUD);
    iqbuf_reserve(o, n * sps + 64);
    for (int i = 0; i < n; i++) {
        double f = lev[i] ? 1200.0 : 2200.0;
        for (int k = 0; k < sps; k++) {
            *ph_audio += 2.0 * M_PI * f / FS;
            if (*ph_audio > 2.0 * M_PI)
                *ph_audio -= 2.0 * M_PI;
            *ph_fm += 2.0 * M_PI * DEV_HZ / FS * sin(*ph_audio);
            iqbuf_push(o, (cf32){(float)(0.9 * cos(*ph_fm)), (float)(0.9 * sin(*ph_fm))});
        }
    }
}

static void silence(iqbuf_t *o, int n)
{
    iqbuf_reserve(o, n);
    memset(o->s + o->n, 0, sizeof(cf32) * (size_t)n);
    o->n += n;
}

static int packet(const params_t *p, const char *type, iqbuf_t *out, double *pa, double *pf, char *err, size_t errlen)
{
    uint8_t info[240], frame[400], bits[400 * 8], hb[400 * 10], lev[400 * 10];
    char dst[16];
    snprintf(dst, sizeof dst, "%s", p->dest);
    int ni = build_info(p, type, info, dst);
    if (ni < 0 || ni > 200) {
        snprintf(err, errlen, "tipo de paquete APRS '%s' no válido", type);
        return -1;
    }
    int nf = ax25_build_ui(p->callsign, dst, p->path, info, ni, frame, sizeof frame);
    if (nf < 0) {
        snprintf(err, errlen, "trama AX.25 demasiado larga");
        return -1;
    }
    int nb = hdlc_bytes_to_bits(frame, nf, bits);
    int nh = hdlc_encode(bits, nb, 16, 3, hb, (int)sizeof hb);
    if (nh < 0) {
        snprintf(err, errlen, "trama demasiado larga");
        return -1;
    }
    nrzi_encode(hb, nh, lev, 1, 0);
    silence(out, 2400);
    afsk_fm(out, lev, nh, pa, pf);
    silence(out, 2400);
    return 0;
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    *p = (params_t){.lat = 49.058333, .lon = -72.029167, .speed = 36, .course = 88};
    snprintf(p->callsign, sizeof p->callsign, "N0CALL");
    snprintf(p->dest, sizeof p->dest, "APZBLF");
    snprintf(p->path, sizeof p->path, "WIDE1-1");
    snprintf(p->symbol, sizeof p->symbol, "/>");
    snprintf(p->comment, sizeof p->comment, "BladeRF Lab");
    snprintf(p->to, sizeof p->to, "N0CALL");
    snprintf(p->text, sizeof p->text, "Hola desde BladeRF Lab");
    snprintf(p->id, sizeof p->id, "001");
    if (js && js[0]) {
        jtok_t t[64];
        int nt = json_parse(js, strlen(js), t, 64);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        int i;
        double v;
#define STR(key, field)                                                                  \
    if ((i = json_obj_get(js, t, 0, key)) >= 0)                                          \
        json_str(js, t, i, p->field, sizeof p->field)
#define NUM(key, field)                                                                  \
    if ((i = json_obj_get(js, t, 0, key)) >= 0 && json_num(js, t, i, &v))                \
        p->field = v
        STR("callsign", callsign);
        STR("dest", dest);
        STR("path", path);
        STR("comment", comment);
        STR("to", to);
        STR("text", text);
        STR("id", id);
        STR("type", type);
        STR("info", info);
        char sy[8];
        if ((i = json_obj_get(js, t, 0, "symbol")) >= 0 && json_str(js, t, i, sy, sizeof sy) && strlen(sy) == 2) {
            p->symbol[0] = sy[0];
            p->symbol[1] = sy[1];
        }
        NUM("lat", lat);
        NUM("lon", lon);
        NUM("speed", speed);
        NUM("course", course);
    }
    for (char *c = p->callsign; *c; c++)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 32);
    if (!p->callsign[0]) {
        snprintf(err, errlen, "APRS exige un indicativo de origen no vacío (params.callsign / --callsign)");
        return -1;
    }
    if (!ax25_callsign_ok(p->callsign)) {
        snprintf(err, errlen, "indicativo '%s' no válido (1-6 caracteres A-Z/0-9 y -SSID)", p->callsign);
        return -1;
    }
    p->lat = CLAMP(p->lat, -89.99, 89.99);
    p->lon = CLAMP(p->lon, -179.99, 179.99);
    p->speed = CLAMP(p->speed, 0.0, 790.0);
    return 0;
}

static int aprs_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    out->rate = FS;
    double pa = 0.0, pf = 0.0;
    if (p.info[0] && !p.type[0])
        snprintf(p.type, sizeof p.type, "raw");
    if (p.type[0])
        return packet(&p, p.type, out, &pa, &pf, err, errlen);
    static const char *seq[] = {"position", "compressed", "mic-e", "status", "message"};
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++)
        if (packet(&p, seq[i], out, &pa, &pf, err, errlen) < 0)
            return -1;
    return 0;
}

const modulator_desc_t MOD_APRS = {
    .id = "aprs",
    .name = "APRS / AX.25",
    .air_ok = true,
    .modulate = aprs_modulate,
};
