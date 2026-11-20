/*
 * aprs.c — Decodificador APRS / AX.25 sobre AFSK 1200 baudios (Bell 202).
 *
 * Capa física: radio FM de banda estrecha (144,800 MHz en Europa, 144,390 en
 * América). El audio lleva AFSK Bell 202: 1200 Hz = "1" (mark) y 2200 Hz = "0"
 * (space) a 1200 baudios, con fase continua. Datos: HDLC (hdlc.h) con NRZI
 * (0 = cambio de tono, 1 = mismo tono), FCS CRC-16/X-25 y bytes LSB primero.
 *
 * Cadena DSP (48 kHz, 40 muestras por símbolo):
 *   1. Discriminador FM → audio (sync.h, fmdisc). El desvío de portadora
 *      (CFO) es una constante: lo elimina un filtro paso alto de un polo.
 *   2. Paso bajo FIR a 3,3 kHz (el ruido del discriminador crece con f).
 *   3. Tres demoduladores AFSK por correladores en paralelo (afsk.h) con
 *      ganancias distintas del tono space (0,6 / 1 / 1,7): toleran el
 *      preénfasis de un transmisor (el tono agudo llega hasta ~6 dB más
 *      fuerte) o de-énfasis del receptor. Cada uno lleva su PLL de reloj,
 *      NRZI y HDLC; gana el que valida el CRC y los duplicados se descartan.
 *   4. AX.25: direcciones de 7 bytes (carácter<<1, SSID en el 7º), control
 *      0x03 (UI), PID 0xF0, información APRS.
 *
 * Formatos APRS (APRS101 caps. 5-11, 15-16): posición sin comprimir
 * ("DDMM.mmN/DDDMM.mmE"), comprimida (base 91: lat = 90 − v/380926,
 * lon = −180 + v/190463; rumbo = c·4°, velocidad = 1,08^s − 1 kn, altitud =
 * 1,002^cs ft), Mic-E (latitud en el campo destino, caps. 10), estado,
 * mensajes (":DESTINAT :texto{id"), objetos (";"), ítems (")") y
 * meteorología (símbolo "_" o paquetes "_" sin posición).
 *
 * state_json: tabla de estaciones con posición/último paquete; caducan a 1 h.
 */
#include "aprs.h"

#include "afsk.h"
#include "decoder.h"
#include "hdlc.h"
#include "../dsp/filter.h"
#include "../dsp/pulse.h"
#include "../dsp/sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define NSLICE 3
#define SPS_APRS 40 /* muestras por símbolo: 48000/1200 */
#define FLAG_TOL 1 /* errores de bit tolerados al extender la racha de banderas hacia atrás */
#define APRS_DELAY 18 /* FIR de 41 coef. (20) y correlador de símbolo, medido en tests/test_labels.c */
#define MAX_FRAME_BITS (400 * 8 + 64)
#define STATION_TTL 3600.0
#define MAX_STATIONS 1024
#define DEDUP_N 16

/* Copia acotada y terminada en NUL (sin avisos de truncado de snprintf). */
static void scpy(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ======================================================================= */
/*  AX.25                                                                  */
/* ======================================================================= */
bool ax25_callsign_ok(const char *cs)
{
    int n = 0;
    while (cs[n] && cs[n] != '-')
        n++;
    if (n < 1 || n > 6)
        return false;
    for (int i = 0; i < n; i++)
        if (!isupper((unsigned char)cs[i]) && !isdigit((unsigned char)cs[i]))
            return false;
    if (cs[n] == '-') {
        char *e;
        long v = strtol(cs + n + 1, &e, 10);
        if (*e || e == cs + n + 1 || v < 0 || v > 15)
            return false;
    }
    return true;
}

/* Decodifica una dirección de 7 bytes. Devuelve true si es la última. */
static bool addr_decode(const uint8_t *a, char *out, bool *repeated)
{
    int n = 0;
    for (int i = 0; i < 6; i++) {
        char c = (char)(a[i] >> 1);
        if (c != ' ')
            out[n++] = c;
    }
    int ssid = (a[6] >> 1) & 0x0F;
    if (ssid)
        n += snprintf(out + n, 8, "-%d", ssid);
    out[n] = '\0';
    if (repeated)
        *repeated = (a[6] & 0x80) != 0;
    return (a[6] & 1) != 0;
}

static int addr_encode(const char *cs, uint8_t *out, bool last, bool hbit)
{
    int i = 0;
    for (; cs[i] && cs[i] != '-' && i < 6; i++)
        out[i] = (uint8_t)(toupper((unsigned char)cs[i]) << 1);
    for (int k = i; k < 6; k++)
        out[k] = (uint8_t)(' ' << 1);
    int ssid = 0;
    const char *d = strchr(cs, '-');
    if (d)
        ssid = atoi(d + 1) & 0x0F;
    out[6] = (uint8_t)(0x60 | (ssid << 1) | (last ? 1 : 0) | (hbit ? 0x80 : 0));
    return 7;
}

int ax25_build_ui(const char *src, const char *dst, const char *path, const uint8_t *info, int ninfo, uint8_t *out,
                  int cap)
{
    char digis[8][12];
    int nd = 0;
    if (path && path[0]) {
        char tmp[120];
        scpy(tmp, sizeof tmp, path);
        for (char *t = strtok(tmp, ","); t && nd < 8; t = strtok(NULL, ","))
            scpy(digis[nd++], sizeof digis[0], t);
    }
    int n = 7 * (2 + nd) + 2 + ninfo;
    if (n > cap)
        return -1;
    int o = 0;
    o += addr_encode(dst, out + o, false, false);
    out[o - 1] |= 0x80; /* bit C del destino: trama de orden (AX.25 v2) */
    o += addr_encode(src, out + o, nd == 0, false);
    for (int i = 0; i < nd; i++)
        o += addr_encode(digis[i], out + o, i == nd - 1, false);
    out[o++] = 0x03;
    out[o++] = 0xF0;
    memcpy(out + o, info, (size_t)ninfo);
    return o + ninfo;
}

/* ======================================================================= */
/*  APRS                                                                   */
/* ======================================================================= */
static void clear_pkt(aprs_pkt_t *p)
{
    memset(p, 0, sizeof *p);
    p->alt_m = p->course_deg = p->speed_kn = 0;
    p->wind_dir = p->wind_mph = p->gust_mph = p->temp_f = p->rain1h_in = p->rain24h_in = p->rain_mid_in =
        p->hum_pct = p->baro_hpa = NAN;
}

static void copy_clean(char *dst, size_t cap, const uint8_t *s, int n)
{
    size_t k = 0;
    for (int i = 0; i < n && k + 1 < cap; i++)
        dst[k++] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : '.';
    dst[k] = '\0';
}

static double base91(const char *s, int n)
{
    double v = 0;
    for (int i = 0; i < n; i++)
        v = v * 91 + (s[i] - 33);
    return v;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
}

static bool all_digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i]))
            return false;
    return true;
}

/* Cuenta ' ' como '0' (ambigüedad de posición). */
static double num_amb(const char *s, int n)
{
    char b[16];
    for (int i = 0; i < n && i < 15; i++)
        b[i] = s[i] == ' ' ? '0' : s[i];
    b[n] = 0;
    return atof(b);
}

/* Posición a partir de s (len bytes). Devuelve nº de bytes consumidos o 0. */
static int parse_position(const char *s, int len, aprs_pkt_t *p)
{
    if (len >= 19 && (isdigit((unsigned char)s[0]) || s[0] == ' ') && s[4] == '.' && (s[7] == 'N' || s[7] == 'S') &&
        s[14] == '.' && (s[17] == 'E' || s[17] == 'W')) {
        double lat = num_amb(s, 2) + num_amb(s + 2, 5) / 60.0;
        double lon = num_amb(s + 9, 3) + num_amb(s + 12, 5) / 60.0;
        p->lat = s[7] == 'S' ? -lat : lat;
        p->lon = s[17] == 'W' ? -lon : lon;
        p->symtab = s[8];
        p->symcode = s[18];
        p->pos_valid = fabs(p->lat) <= 90 && fabs(p->lon) <= 180;
        return 19;
    }
    if (len >= 13 && (s[0] == '/' || s[0] == '\\' || isupper((unsigned char)s[0]) || (s[0] >= 'a' && s[0] <= 'j'))) {
        for (int i = 1; i < 9; i++)
            if (s[i] < 33 || s[i] > 123)
                return 0;
        p->symtab = s[0];
        p->lat = 90.0 - base91(s + 1, 4) / 380926.0;
        p->lon = -180.0 + base91(s + 5, 4) / 190463.0;
        p->symcode = s[9];
        p->pos_valid = true;
        if (s[10] != ' ') {
            int c = s[10] - 33, sp = s[11] - 33, t = s[12] - 33;
            if (((t >> 3) & 3) == 2) {
                p->has_alt = true;
                p->alt_m = pow(1.002, c * 91 + sp) * 0.3048;
            } else if (c >= 0 && c <= 89) {
                p->has_course = p->has_speed = true;
                p->course_deg = c * 4.0;
                p->speed_kn = pow(1.08, sp) - 1.0;
            }
        }
        return 13;
    }
    return 0;
}

/* "ddd/sss" rumbo/velocidad tras la posición; /A=nnnnnn altitud en pies. */
static void parse_comment(const char *c, aprs_pkt_t *p, bool allow_cs)
{
    if (allow_cs && strlen(c) >= 7 && c[3] == '/' && all_digits(c, 3) && all_digits(c + 4, 3)) {
        p->course_deg = atoi(c);
        p->speed_kn = atoi(c + 4);
        p->has_course = p->has_speed = true;
    }
    const char *a = strstr(c, "/A=");
    if (a && strlen(a) >= 9 && (all_digits(a + 3, 6) || (a[3] == '-' && all_digits(a + 4, 5)))) {
        p->alt_m = atoi(a + 3) * 0.3048;
        p->has_alt = true;
    }
    scpy(p->comment, sizeof p->comment, c);
}

/* Meteorología: campos tras el viento "ddd/sss". */
static void parse_wx(const char *s, aprs_pkt_t *p)
{
    p->wx = true;
    int i = 0, n = (int)strlen(s);
    if (n >= 7 && s[3] == '/' ) {
        if (isdigit((unsigned char)s[0]))
            p->wind_dir = atoi(s);
        if (isdigit((unsigned char)s[4]))
            p->wind_mph = atoi(s + 4);
        i = 7;
    }
    while (i < n) {
        char c = s[i];
        int w = 0;
        switch (c) {
        case 'c': case 'g': case 't': case 'r': case 'p': case 'P': case 'L': case 'l': case 's': case '#': w = 3; break;
        case 'h': w = 2; break;
        case 'b': w = 5; break;
        default: return;
        }
        if (i + 1 + w > n)
            return;
        char b[8];
        memcpy(b, s + i + 1, (size_t)w);
        b[w] = 0;
        bool miss = strchr(b, '.') || strchr(b, ' ');
        double v = atof(b);
        if (!miss) {
            switch (c) {
            case 'g': p->gust_mph = v; break;
            case 't': p->temp_f = v; break;
            case 'r': p->rain1h_in = v / 100.0; break;
            case 'p': p->rain24h_in = v / 100.0; break;
            case 'P': p->rain_mid_in = v / 100.0; break;
            case 'h': p->hum_pct = v == 0 ? 100.0 : v; break;
            case 'b': p->baro_hpa = v / 10.0; break;
            default: break;
            }
        }
        i += 1 + w;
    }
}

static const char *MICE_STD[8] = {"Emergency", "Priority", "Special", "Committed", "Returning", "In Service",
                                  "En Route", "Off Duty"};

/* Mic-E (APRS101 cap. 10). dst: 6 caracteres de destino, info: DTI + 8 bytes. */
static void parse_mice(const char *dst, const uint8_t *in, int n, aprs_pkt_t *p)
{
    if (strlen(dst) < 6 || n < 9)
        return;
    int d[6], bit[3] = {0, 0, 0};
    bool custom = false;
    for (int i = 0; i < 6; i++) {
        char c = dst[i];
        if (c >= '0' && c <= '9')
            d[i] = c - '0';
        else if (c >= 'A' && c <= 'J')
            d[i] = c - 'A', (i < 3 ? (bit[i] = 1, custom = true) : 0);
        else if (c == 'K' || c == 'L' || c == 'Z')
            d[i] = 0, (i < 3 ? (bit[i] = (c != 'L'), custom = custom || c == 'K') : 0);
        else if (c >= 'P' && c <= 'Y')
            d[i] = c - 'P', (i < 3 ? bit[i] = 1 : 0);
        else
            return;
    }
    bool north = dst[3] >= 'P', lon100 = dst[4] >= 'P', west = dst[5] >= 'P';
    double lat = d[0] * 10 + d[1] + (d[2] * 10 + d[3] + (d[4] * 10 + d[5]) / 100.0) / 60.0;
    int dl = in[1] - 28, ml = in[2] - 28, hl = in[3] - 28;
    if (lon100)
        dl += 100;
    if (dl >= 180 && dl <= 189)
        dl -= 80;
    else if (dl >= 190 && dl <= 199)
        dl -= 190;
    if (ml >= 60)
        ml -= 60;
    double lon = dl + (ml + hl / 100.0) / 60.0;
    int sp = (in[4] - 28) * 10 + (in[5] - 28) / 10;
    int dc = ((in[5] - 28) % 10) * 100 + (in[6] - 28);
    if (sp >= 800)
        sp -= 800;
    if (dc >= 400)
        dc -= 400;
    p->lat = north ? lat : -lat;
    p->lon = west ? -lon : lon;
    p->pos_valid = lat <= 90 && lon <= 180;
    p->speed_kn = sp;
    p->course_deg = dc;
    p->has_speed = p->has_course = true;
    p->symcode = (char)in[7];
    p->symtab = (char)in[8];
    int code = bit[0] * 4 + bit[1] * 2 + bit[2];
    snprintf(p->mice_msg, sizeof p->mice_msg, "%s%s", custom ? "Custom " : "", MICE_STD[code]);
    /* comentario; altitud "xxx}" en base 91 respecto a −10000 m */
    int s = 9;
    char com[200];
    copy_clean(com, sizeof com, in + s, n - s);
    for (int i = s; i + 3 < n; i++)
        if (in[i + 3] == '}' && in[i] >= 33 && in[i + 1] >= 33 && in[i + 2] >= 33) {
            p->alt_m = (in[i] - 33) * 8281 + (in[i + 1] - 33) * 91 + (in[i + 2] - 33) - 10000;
            p->has_alt = true;
            break;
        }
    scpy(p->comment, sizeof p->comment, com);
}

void aprs_parse_info(const char *dst, const uint8_t *in, int n, aprs_pkt_t *p)
{
    char s[262];
    copy_clean(s, sizeof s, in, n);
    scpy(p->info, sizeof p->info, s);
    snprintf(p->kind, sizeof p->kind, "other");
    if (n < 1)
        return;
    char dti = (char)in[0];
    const char *body = s + 1;
    int blen = n - 1;
    switch (dti) {
    case '!': case '=': case '/': case '@': {
        if (dti == '/' || dti == '@') {
            if (blen < 7)
                return;
            body += 7;
            blen -= 7;
        }
        int used = parse_position(body, blen, p);
        if (!used)
            return;
        snprintf(p->kind, sizeof p->kind, "position");
        if (p->symcode == '_') {
            const char *wx = body + used;
            snprintf(p->kind, sizeof p->kind, "weather");
            parse_wx(wx, p);
            scpy(p->comment, sizeof p->comment, wx);
        } else {
            parse_comment(body + used, p, used == 19);
        }
        break;
    }
    case '`': case '\'': case 0x1c: case 0x1d:
        parse_mice(dst, in, n, p);
        if (p->pos_valid)
            snprintf(p->kind, sizeof p->kind, "mic-e");
        break;
    case '>':
        snprintf(p->kind, sizeof p->kind, "status");
        if (blen >= 7 && s[7] == 'z' && all_digits(body, 6))
            scpy(p->status, sizeof p->status, body + 7);
        else
            scpy(p->status, sizeof p->status, body);
        break;
    case ':':
        if (n >= 11 && in[10] == ':') {
            snprintf(p->kind, sizeof p->kind, "message");
            memcpy(p->to, body, 9);
            p->to[9] = 0;
            trim(p->to);
            scpy(p->msg, sizeof p->msg, s + 11);
            char *br = strrchr(p->msg, '{');
            if (br) {
                scpy(p->msg_id, sizeof p->msg_id, br + 1);
                *br = 0;
            }
        }
        break;
    case ';':
        if (blen >= 18 && (s[10] == '*' || s[10] == '_')) {
            snprintf(p->kind, sizeof p->kind, "object");
            memcpy(p->obj_name, body, 9);
            p->obj_name[9] = 0;
            trim(p->obj_name);
            p->obj_live = s[10] == '*';
            int used = parse_position(body + 17, blen - 17, p);
            if (used) {
                if (p->symcode == '_') {
                    parse_wx(body + 17 + used, p);
                    scpy(p->comment, sizeof p->comment, body + 17 + used);
                } else {
                    parse_comment(body + 17 + used, p, used == 19);
                }
            }
        }
        break;
    case ')': {
        int k = 0;
        while (k < blen && k < 10 && body[k] != '!' && body[k] != '_')
            k++;
        if (k >= 3 && k < blen) {
            snprintf(p->kind, sizeof p->kind, "item");
            memcpy(p->obj_name, body, (size_t)k);
            p->obj_name[k] = 0;
            p->obj_live = body[k] == '!';
            int used = parse_position(body + k + 1, blen - k - 1, p);
            if (used)
                parse_comment(body + k + 1 + used, p, used == 19);
        }
        break;
    }
    case '_':
        if (blen >= 8 && all_digits(body, 8)) {
            snprintf(p->kind, sizeof p->kind, "weather");
            parse_wx(body + 8, p);
        }
        break;
    case 'T':
        if (n > 1 && in[1] == '#')
            snprintf(p->kind, sizeof p->kind, "telemetry");
        break;
    default:
        break;
    }
}

int aprs_parse_frame(const uint8_t *f, int n, aprs_pkt_t *p)
{
    clear_pkt(p);
    if (n < 16)
        return -1;
    int o = 0, na = 0;
    char call[10][14];
    bool last = false, rep[10];
    while (!last && na < 10 && o + 7 <= n) {
        for (int i = 0; i < 6; i++) { /* caracteres válidos: A-Z 0-9 espacio */
            char c = (char)(f[o + i] >> 1);
            if (!isupper((unsigned char)c) && !isdigit((unsigned char)c) && c != ' ')
                return -1;
            if (f[o + i] & 1)
                return -1;
        }
        last = addr_decode(f + o, call[na], &rep[na]);
        na++;
        o += 7;
    }
    if (!last || na < 2 || o + 2 > n)
        return -1;
    if ((f[o] & 0xEF) != 0x03 || f[o + 1] != 0xF0) /* UI, PID sin capa 3 */
        return -1;
    o += 2;
    scpy(p->dst, sizeof p->dst, call[0]);
    scpy(p->src, sizeof p->src, call[1]);
    size_t k = 0;
    p->path[0] = 0;
    for (int i = 2; i < na; i++)
        k += (size_t)snprintf(p->path + k, sizeof p->path - k, "%s%s%s", i > 2 ? "," : "", call[i], rep[i] ? "*" : "");
    aprs_parse_info(p->dst, f + o, n - o, p);
    return 0;
}

void aprs_text(const aprs_pkt_t *p, char *out, size_t cap)
{
    char body[200] = "";
    if (!strcmp(p->kind, "message"))
        snprintf(body, sizeof body, "→ %s: %s", p->to, p->msg);
    else if (!strcmp(p->kind, "status"))
        scpy(body, sizeof body, p->status);
    else if (p->pos_valid)
        snprintf(body, sizeof body, "%s%s%.4f,%.4f %.120s", p->obj_name, p->obj_name[0] ? " " : "", p->lat, p->lon,
                 p->comment);
    else if (p->wx)
        snprintf(body, sizeof body, "meteo %.150s", p->comment);
    else
        scpy(body, sizeof body, p->info);
    snprintf(out, cap, "APRS %s>%s [%s] %s", p->src, p->dst, p->kind, body);
}

/* ======================================================================= */
/*  Tabla de estaciones                                                    */
/* ======================================================================= */
typedef struct {
    char name[12], kind[16], path[60], comment[60];
    bool pos, object;
    double lat, lon, speed, course, alt, last, temp_f;
    char sym[3];
    unsigned pkts;
} station_t;

typedef struct {
    uint32_t h;
    long at;
} dedup_t;

typedef struct {
    hdlc_rx_t hdlc;
    afsk_t *demod;
    /* Racha de banderas (convención de posición, ver on_level): últimos 8 bits NRZI-decodificados,
     * 'pos' del último bit de la primera bandera de la racha y de la última bandera vista. */
    unsigned sr;
    uint64_t hist; /* últimos 64 bits NRZI-decodificados (el más reciente en el bit 0) */
    int prev_level;
    int64_t run0, run_last;
} chan_slice_t;

typedef struct {
    fmdisc_t disc;
    fir_f_t lpf;
    float hp;
    float *tmp, *tmp2;
    int cap;
    chan_slice_t sl[NSLICE];
    dedup_t dd[DEDUP_N];
    int dd_pos;
    long n;
    station_t *st;
    int nst;
    unsigned long total;
    bool loc_valid;
    double lat, lon;
    sb_t ev;
    dec_emit_fn emit;
    void *ctx;
    double blk_now;
    int cur_slicer;
    long blk_n0;              /* índice del flujo de la primera muestra de la llamada en curso */
    int64_t mk0, mk1;         /* inicio/fin de la trama en curso (índice del flujo) */
    uint8_t buf[512];
} aprs_t;

static station_t *get_station(aprs_t *a, const char *name, double now)
{
    for (int i = 0; i < a->nst; i++)
        if (!strcmp(a->st[i].name, name))
            return &a->st[i];
    if (a->nst >= MAX_STATIONS) {
        int old = 0;
        for (int i = 1; i < a->nst; i++)
            if (a->st[i].last < a->st[old].last)
                old = i;
        a->st[old] = a->st[--a->nst];
    }
    station_t *s = &a->st[a->nst++];
    memset(s, 0, sizeof *s);
    scpy(s->name, sizeof s->name, name);
    s->temp_f = NAN;
    (void)now;
    return s;
}

static void update_station(aprs_t *a, const aprs_pkt_t *p, double now)
{
    if (!strcmp(p->kind, "message") && !p->pos_valid) {
        station_t *s = get_station(a, p->src, now);
        s->last = now;
        s->pkts++;
        return;
    }
    station_t *s = get_station(a, p->obj_name[0] ? p->obj_name : p->src, now);
    s->last = now;
    s->pkts++;
    s->object = p->obj_name[0] != 0;
    scpy(s->kind, sizeof s->kind, p->kind);
    scpy(s->path, sizeof s->path, p->path);
    if (p->pos_valid) {
        s->pos = true;
        s->lat = p->lat;
        s->lon = p->lon;
        s->sym[0] = p->symtab;
        s->sym[1] = p->symcode;
        s->sym[2] = 0;
    }
    if (p->has_speed)
        s->speed = p->speed_kn;
    if (p->has_course)
        s->course = p->course_deg;
    if (p->has_alt)
        s->alt = p->alt_m;
    if (!isnan(p->temp_f))
        s->temp_f = p->temp_f;
    if (p->comment[0])
        snprintf(s->comment, sizeof s->comment, "%.59s", p->comment);
}

static void aprs_state_json(void *st, sb_t *sb, double now)
{
    aprs_t *a = st;
    int w = 0;
    for (int i = 0; i < a->nst; i++)
        if (now - a->st[i].last < STATION_TTL)
            a->st[w++] = a->st[i];
    a->nst = w;
    sb_printf(sb, "{\"packets\":%lu,\"count\":%d,\"stations\":[", a->total, a->nst);
    for (int i = 0; i < a->nst; i++) {
        const station_t *s = &a->st[i];
        if (i)
            sb_puts(sb, ",");
        sb_puts(sb, "{\"call\":");
        sb_json_str(sb, s->name);
        sb_puts(sb, ",\"kind\":");
        sb_json_str(sb, s->kind);
        if (s->object)
            sb_puts(sb, ",\"object\":true");
        if (s->pos) {
            sb_puts(sb, ",\"lat\":");
            sb_json_num(sb, s->lat, 5);
            sb_puts(sb, ",\"lon\":");
            sb_json_num(sb, s->lon, 5);
            sb_puts(sb, ",\"symbol\":");
            sb_json_str(sb, s->sym);
            if (a->loc_valid) {
                double p1 = a->lat * M_PI / 180, p2 = s->lat * M_PI / 180, dl = (s->lon - a->lon) * M_PI / 180;
                double h = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
                sb_puts(sb, ",\"dist_km\":");
                sb_json_num(sb, 2 * 6371.0 * asin(sqrt(h)), 1);
            }
        }
        sb_puts(sb, ",\"speed\":");
        sb_json_num(sb, s->speed, 0);
        sb_puts(sb, ",\"course\":");
        sb_json_num(sb, s->course, 0);
        sb_puts(sb, ",\"alt\":");
        sb_json_num(sb, s->alt, 0);
        sb_puts(sb, ",\"comment\":");
        sb_json_str(sb, s->comment);
        sb_puts(sb, ",\"path\":");
        sb_json_str(sb, s->path);
        sb_printf(sb, ",\"pkts\":%u,\"age\":", s->pkts);
        sb_json_num(sb, now - s->last, 0);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

/* ======================================================================= */
/*  Evento                                                                 */
/* ======================================================================= */
static void jnum(sb_t *e, const char *k, double v, int dec)
{
    if (isnan(v))
        return;
    sb_printf(e, ",\"%s\":", k);
    sb_json_num(e, v, dec);
}

static void emit_pkt(aprs_t *a, const aprs_pkt_t *p, double now)
{
    char text[400];
    aprs_text(p, text, sizeof text);
    sb_t *e = &a->ev;
    sb_clear(e);
    sb_puts(e, "{\"src\":");
    sb_json_str(e, p->src);
    sb_puts(e, ",\"dst\":");
    sb_json_str(e, p->dst);
    sb_puts(e, ",\"path\":");
    sb_json_str(e, p->path);
    sb_puts(e, ",\"kind\":");
    sb_json_str(e, p->kind);
    if (p->pos_valid) {
        jnum(e, "lat", p->lat, 6);
        jnum(e, "lon", p->lon, 6);
        sb_puts(e, ",\"symbol\":");
        char sym[3] = {p->symtab, p->symcode, 0};
        sb_json_str(e, sym);
    }
    if (p->has_speed)
        jnum(e, "speed_kn", p->speed_kn, 1);
    if (p->has_course)
        jnum(e, "course", p->course_deg, 0);
    if (p->has_alt)
        jnum(e, "alt_m", p->alt_m, 0);
    if (p->obj_name[0]) {
        sb_puts(e, ",\"object\":");
        sb_json_str(e, p->obj_name);
        sb_printf(e, ",\"live\":%s", p->obj_live ? "true" : "false");
    }
    if (!strcmp(p->kind, "message")) {
        sb_puts(e, ",\"to\":");
        sb_json_str(e, p->to);
        sb_puts(e, ",\"message\":");
        sb_json_str(e, p->msg);
        sb_puts(e, ",\"msg_id\":");
        sb_json_str(e, p->msg_id);
    }
    if (p->status[0]) {
        sb_puts(e, ",\"status\":");
        sb_json_str(e, p->status);
    }
    if (p->mice_msg[0]) {
        sb_puts(e, ",\"mice_msg\":");
        sb_json_str(e, p->mice_msg);
    }
    if (p->wx) {
        jnum(e, "wind_dir", p->wind_dir, 0);
        jnum(e, "wind_mph", p->wind_mph, 0);
        jnum(e, "gust_mph", p->gust_mph, 0);
        jnum(e, "temp_f", p->temp_f, 0);
        jnum(e, "rain_1h_in", p->rain1h_in, 2);
        jnum(e, "rain_24h_in", p->rain24h_in, 2);
        jnum(e, "rain_midnight_in", p->rain_mid_in, 2);
        jnum(e, "humidity", p->hum_pct, 0);
        jnum(e, "baro_hpa", p->baro_hpa, 1);
    }
    if (p->comment[0] && !p->wx) {
        sb_puts(e, ",\"comment\":");
        sb_json_str(e, p->comment);
    }
    sb_puts(e, ",\"info\":");
    sb_json_str(e, p->info);
    sb_puts(e, ",\"text\":");
    sb_json_str(e, text);
    sb_puts(e, "}");
    update_station(a, p, now);
    a->total++;
    if (a->emit)
        dec_emit_span(a->emit, a->ctx, a->mk0 - a->blk_n0, a->mk1 - a->blk_n0, e->data, e->len);
}

static uint32_t fnv(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

static void on_frame(aprs_t *a, const uint8_t *f, int n)
{
    uint32_t h = fnv(f, n);
    for (int i = 0; i < DEDUP_N; i++)
        if (a->dd[i].h == h && a->n - a->dd[i].at < (long)FS)
            return;
    aprs_pkt_t *p = malloc(sizeof *p);
    if (!p)
        return;
    if (aprs_parse_frame(f, n, p) == 0) {
        a->dd[a->dd_pos] = (dedup_t){h, a->n};
        a->dd_pos = (a->dd_pos + 1) % DEDUP_N;
        emit_pkt(a, p, a->blk_now + (double)a->n / FS);
    }
    free(p);
}

/* ======================================================================= */
/*  DSP                                                                    */
/* ======================================================================= */
/* Nivel de tono (1 = 1200 Hz) → NRZI + HDLC del demodulador en curso. */
static void on_level(void *ctx, int level)
{
    aprs_t *a = ctx;
    chan_slice_t *s = &a->sl[a->cur_slicer];
    s->hdlc.pos = afsk_index(s->demod);
    int nb = hdlc_rx_push_level(&s->hdlc, level);
    const bool frame = nb >= 17 * 8 && nb <= 400 * 8 && nb % 8 == 0;
    if (frame) {
        /* Convención de posición (P3d, igual en todos los decodificadores): el mensaje empieza en el
         * PRIMER símbolo de la señal transmitida, preámbulo incluido; aquí, la primera bandera de la
         * racha de banderas de relleno (TXDELAY) que precede a la trama, no la última (la de apertura
         * propiamente dicha). Termina en el último símbolo de la bandera de cierre. Cada bandera
         * empieza 7,5 símbolos antes del bit en que se detecta (8 bits, el último centrado en 'pos');
         * la de cierre acaba medio símbolo después. APRS_DELAY es el retardo del discriminador, del FIR
         * y del correlador (calibrado en tests/test_labels.c). */
        int64_t first = s->run_last > 0 && s->run0 > 0 ? s->run0 : s->hdlc.last_start;
        a->mk0 = first - (int64_t)(7.5 * SPS_APRS) - APRS_DELAY;
        a->mk1 = s->hdlc.last_end + SPS_APRS / 2 - APRS_DELAY;
        hdlc_rx_bytes(&s->hdlc, nb, a->buf);
        on_frame(a, a->buf, nb / 8);
    }
    /* Seguimiento de la racha de banderas (0x7E = 0,1,1,1,1,1,1,0 tras NRZI). Una bandera que cierra una
     * trama abre también la racha siguiente (puede servir de apertura a la próxima trama). */
    s->hist = (s->hist << 1) | (level == s->prev_level ? 1u : 0u);
    s->sr = (unsigned)(s->hist & 0xFFu);
    s->prev_level = level & 1;
    if (s->sr == 0x7Eu) {
        if (frame || s->hdlc.pos - s->run_last > 10 * SPS_APRS || s->run_last <= 0) {
            s->run0 = s->hdlc.pos;
            /* El demodulador tarda unos símbolos en engancharse y la primera bandera suele llegar
             * estropeada (en ~40 % de las tramas no se reconoce): se extiende la racha hacia atrás
             * (hasta 3 banderas) mientras el grupo de 8 bits anterior difiera de 0x7E en ≤ 2 bits. */
            for (int g = 1; g <= 3; g++) {
                unsigned grp = (unsigned)((s->hist >> (8 * g)) & 0xFFu);
                if (__builtin_popcount(grp ^ 0x7Eu) > FLAG_TOL)
                    break;
                s->run0 -= 8 * SPS_APRS;
            }
        }
        s->run_last = s->hdlc.pos;
    }
}

static void *aprs_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    aprs_t *a = calloc(1, sizeof *a);
    if (!a) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    fmdisc_init(&a->disc, FS, 3000.0);
    float h[41];
    fir_design_lowpass(h, 41, 3300.0 / FS, 5.0);
    static const double gains[NSLICE] = {0.6, 1.0, 1.7};
    bool ok = fir_f_init(&a->lpf, h, 41, 1) == 0;
    for (int i = 0; ok && i < NSLICE; i++) {
        ok = hdlc_rx_init(&a->sl[i].hdlc, MAX_FRAME_BITS) == 0;
        a->sl[i].demod = afsk_create(FS, 1200.0, 1200.0, 2200.0, gains[i], 0.75);
        ok = ok && a->sl[i].demod;
    }
    a->st = calloc(MAX_STATIONS, sizeof(station_t));
    if (!ok || !a->st) {
        snprintf(err, errlen, "sin memoria");
        goto fail;
    }
    if (cfg && cfg->loc_valid) {
        a->loc_valid = true;
        a->lat = cfg->lat;
        a->lon = cfg->lon;
    }
    return a;
fail:
    for (int i = 0; i < NSLICE; i++) {
        afsk_destroy(a->sl[i].demod);
        hdlc_rx_free(&a->sl[i].hdlc);
    }
    free(a->st);
    free(a);
    return NULL;
}

static void aprs_destroy(void *p)
{
    aprs_t *a = p;
    if (!a)
        return;
    for (int i = 0; i < NSLICE; i++) {
        afsk_destroy(a->sl[i].demod);
        hdlc_rx_free(&a->sl[i].hdlc);
    }
    fir_f_free(&a->lpf);
    free(a->tmp);
    free(a->tmp2);
    free(a->st);
    sb_free(&a->ev);
    free(a);
}

static void aprs_set_location(void *p, bool valid, double lat, double lon)
{
    aprs_t *a = p;
    a->loc_valid = valid;
    a->lat = lat;
    a->lon = lon;
}

static void aprs_process(void *p, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    aprs_t *a = p;
    if (n > a->cap) {
        a->cap = n + 4096;
        a->tmp = realloc(a->tmp, sizeof(float) * (size_t)a->cap);
        a->tmp2 = realloc(a->tmp2, sizeof(float) * (size_t)a->cap + 64);
    }
    a->emit = emit;
    a->ctx = ctx;
    a->blk_n0 = a->n;
    a->blk_now = now - (double)a->n / FS; /* tiempo de la muestra 0 del flujo */
    fmdisc_process(&a->disc, iq, n, a->tmp);
    /* paso alto de un polo (≈ 70 Hz): quita el desvío de portadora */
    for (int i = 0; i < n; i++) {
        a->hp += 0.009f * (a->tmp[i] - a->hp);
        a->tmp[i] -= a->hp;
    }
    int m = fir_f_process(&a->lpf, a->tmp, n, a->tmp2);
    long n0 = a->n;
    for (int s = 0; s < NSLICE; s++) {
        a->cur_slicer = s;
        a->n = n0;
        afsk_process(a->sl[s].demod, a->tmp2, m, on_level, a);
    }
    a->n = n0 + m;
}

const decoder_desc_t DEC_APRS = {
    .id = "aprs",
    .name = "APRS / AX.25",
    .group = "Radioafición",
    .desc = "APRS sobre AX.25 y AFSK 1200 Bell 202 (FM).",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 16000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {144800000LL, 0LL, 0LL, 0LL},
    .create = aprs_create,
    .destroy = aprs_destroy,
    .process = aprs_process,
    .state_json = aprs_state_json,
    .set_location = aprs_set_location,
};
