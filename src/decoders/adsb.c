/*
 * adsb.c — Decodificador Mode S / ADS-B. Ver adsb.h y docs/ADSB.md.
 *
 * Numeración de bits: como en la bibliografía, los campos se describen con
 * bits numerados desde 1 (el primer bit de la trama es el bit 1). La función
 * getbits(msg, a, b) devuelve los bits a..b inclusive, así el código se puede
 * comparar directamente con las tablas del libro de referencia.
 */
#include "adsb.h"

#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_AIRCRAFT 1024
#define AIRCRAFT_TTL 60.0     /* s sin mensajes antes de olvidar una aeronave */
#define CPR_PAIR_MAX_DT 10.0  /* s máximos entre trama par e impar */
#define PREAMBLE_SAMPLES 16
#define LONG_MSG_BITS 112
#define SHORT_MSG_BITS 56
#define MSG_SAMPLES (PREAMBLE_SAMPLES + 2 * LONG_MSG_BITS)

struct adsb {
    aircraft_t ac[MAX_AIRCRAFT];
    int nac;
    float *mag;           /* magnitudes del bloque + cola del anterior */
    int mag_cap;
    int tail;             /* muestras arrastradas del bloque anterior */
    uint32_t err_syndrome[LONG_MSG_BITS]; /* síndrome de un error en el bit i */
    bool rx_valid;
    double rx_lat, rx_lon;
    unsigned long total, total_prev;
    double rate_t0;
    float msg_rate;
    dec_emit_fn emit;     /* notificación de tramas DF17/18 (opcional) */
    void *emit_ctx;
    sb_t evsb;            /* búfer reutilizable para componer eventos */
    adsb_opts_t opt;
    adsb_stats_t st;
    unsigned long frames_prev;
    uint64_t base;        /* índice absoluto de la muestra mag[0] (para deduplicar) */
    struct { uint64_t pos; } recent[16]; /* últimas tramas aceptadas */
    int nrecent;
    int cur_fix;          /* bits corregidos de la trama en curso (0, 1, 2) */
    bool cur_crc;         /* la trama en curso viene del entramado CRC */
};

/* Fiabilidad mínima de un bit para que se le permita ser "el erróneo" y
 * nº de bits peor valorados que se exploran en la corrección. */
#define FIX_CAND 12
#define FIX_CONF_MAX 0.6f
/* Entramado CRC deslizante: confianza media mínima de la trama completa. */
#define SLIDE_CONF_MIN 0.45f

/* ======================================================================= */
/*  CRC-24 de Mode S                                                       */
/* ======================================================================= */
/* Polinomio generador G(x) = x^24 + 0xFFF409 (los 24 bits bajos). La paridad
 * de la trama es el resto de dividir los datos·x^24 entre G(x). Se calcula
 * byte a byte con una tabla de 256 entradas. */
static uint32_t crc_table[256];

static void crc_init(void)
{
    static bool done = false;
    if (done)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 16;
        for (int b = 0; b < 8; b++)
            c = (c & 0x800000) ? (c << 1) ^ 0xFFF409 : c << 1;
        crc_table[i] = c & 0xFFFFFF;
    }
    done = true;
}

uint32_t modes_crc24(const uint8_t *msg, int nbits_data)
{
    crc_init();
    uint32_t crc = 0;
    for (int i = 0; i < nbits_data / 8; i++)
        crc = ((crc << 8) ^ crc_table[((crc >> 16) ^ msg[i]) & 0xFF]) & 0xFFFFFF;
    return crc;
}

/* Síndrome = CRC(datos) XOR campo de paridad (los últimos 24 bits). */
static uint32_t syndrome(const uint8_t *msg, int nbits)
{
    int nb = nbits / 8;
    uint32_t parity = ((uint32_t)msg[nb - 3] << 16) | ((uint32_t)msg[nb - 2] << 8) | msg[nb - 1];
    return modes_crc24(msg, nbits - 24) ^ parity;
}

/* ======================================================================= */
/*  Utilidades de bits                                                     */
/* ======================================================================= */
static uint32_t getbits(const uint8_t *m, int first, int last)
{
    uint32_t v = 0;
    for (int b = first; b <= last; b++) {
        int i = b - 1;
        v = (v << 1) | ((m[i >> 3] >> (7 - (i & 7))) & 1);
    }
    return v;
}

static void flipbit(uint8_t *m, int i) { m[i >> 3] ^= (uint8_t)(1 << (7 - (i & 7))); }

/* ======================================================================= */
/*  Tabla de aeronaves                                                     */
/* ======================================================================= */
static aircraft_t *find_ac(adsb_t *a, uint32_t icao)
{
    for (int i = 0; i < a->nac; i++)
        if (a->ac[i].icao == icao)
            return &a->ac[i];
    return NULL;
}

static aircraft_t *get_ac(adsb_t *a, uint32_t icao, double now)
{
    aircraft_t *p = find_ac(a, icao);
    if (p)
        return p;
    if (a->nac >= MAX_AIRCRAFT)
        return NULL;
    p = &a->ac[a->nac++];
    memset(p, 0, sizeof *p);
    p->icao = icao;
    p->first_seen = now;
    p->rssi_db = -100.f;
    return p;
}

static void expire(adsb_t *a, double now)
{
    int w = 0;
    for (int i = 0; i < a->nac; i++)
        if (now - a->ac[i].last_seen < AIRCRAFT_TTL)
            a->ac[w++] = a->ac[i];
    a->nac = w;
}

const aircraft_t *adsb_find(adsb_t *a, uint32_t icao) { return find_ac(a, icao); }
unsigned long adsb_total_msgs(const adsb_t *a) { return a->total; }

/* ======================================================================= */
/*  Campos comunes                                                         */
/* ======================================================================= */

/* Código transpondedor (ID13). Orden de bits: C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4.
 * Cada letra es un dígito octal (A4 A2 A1); se devuelve como número decimal
 * con esos 4 dígitos, p. ej. 7700. */
static int decode_id13(uint32_t f)
{
#define B(i) ((f >> (12 - (i))) & 1)
    int A = (B(5) << 2) | (B(3) << 1) | B(1);
    int Bd = (B(11) << 2) | (B(9) << 1) | B(7);
    int C = (B(4) << 2) | (B(2) << 1) | B(0);
    int D = (B(12) << 2) | (B(10) << 1) | B(8);
#undef B
    return A * 1000 + Bd * 100 + C * 10 + D;
}

/* Altitud de 13 bits (AC13, respuestas Mode S). Bit M (7º) = metros; bit Q
 * (9º) = incrementos de 25 ft. Solo se decodifica el caso habitual Q=1. */
static bool decode_ac13(uint32_t f, int *alt)
{
    if (f == 0)
        return false;
    bool m = (f >> 6) & 1, q = (f >> 4) & 1;
    if (m || !q)
        return false;
    /* Quitar los bits M y Q: quedan 11 bits = N, altitud = 25·N − 1000 ft */
    uint32_t n = ((f & 0x1F80) >> 2) | ((f & 0x0020) >> 1) | (f & 0x000F);
    *alt = (int)n * 25 - 1000;
    return true;
}

/* Altitud de 12 bits (mensajes de posición ADS-B). Bit Q = 8º. */
static bool decode_ac12(uint32_t f, int *alt)
{
    if (f == 0)
        return false;
    if (!((f >> 4) & 1))
        return false; /* código Gillham (Q=0): transpondedores antiguos */
    uint32_t n = ((f & 0x0FE0) >> 1) | (f & 0x000F);
    *alt = (int)n * 25 - 1000;
    return true;
}

/* ======================================================================= */
/*  CPR (Compact Position Reporting)                                       */
/* ======================================================================= */
/* La latitud y longitud se envían con 17 bits como fracción dentro de una
 * "zona". Hay dos rejillas ligeramente distintas (trama par: 60 zonas de
 * latitud; impar: 59). Con una trama de cada tipo se resuelve la posición
 * sin ambigüedad (decodificación global). Con una sola trama y una posición
 * de referencia cercana (< 180 NM) se resuelve localmente. */

#define CPR_NZ 15

static double cpr_mod(double x, double y) { return x - y * floor(x / y); }

/* Número de zonas de longitud a una latitud dada (NL). */
int cpr_nl(double lat)
{
    lat = fabs(lat);
    if (lat < 1e-9)
        return 59;
    if (fabs(lat - 87.0) < 1e-9)
        return 2;
    if (lat > 87.0)
        return 1;
    double a = 1.0 - cos(M_PI / (2.0 * CPR_NZ));
    double b = cos(M_PI / 180.0 * lat);
    return (int)floor(2.0 * M_PI / acos(1.0 - a / (b * b)));
}

/* Decodificación global con un par par/impar. 'newest' indica cuál es la
 * trama más reciente (0 par, 1 impar). Para superficie el ámbito es 90°. */
static bool cpr_global(const aircraft_t *p, int newest, bool surface, double ref_lat, double ref_lon, double *olat,
                       double *olon)
{
    const double span = surface ? 90.0 : 360.0;
    const double latE = p->cpr_lat[0] / 131072.0, lonE = p->cpr_lon[0] / 131072.0;
    const double latO = p->cpr_lat[1] / 131072.0, lonO = p->cpr_lon[1] / 131072.0;
    const double dlatE = span / 60.0, dlatO = span / 59.0;

    double j = floor(59.0 * latE - 60.0 * latO + 0.5);
    double rlatE = dlatE * (cpr_mod(j, 60.0) + latE);
    double rlatO = dlatO * (cpr_mod(j, 59.0) + latO);
    if (!surface) {
        if (rlatE >= 270.0) rlatE -= 360.0;
        if (rlatO >= 270.0) rlatO -= 360.0;
    } else {
        /* En superficie hay 4 soluciones de latitud; se elige la del
         * hemisferio de la referencia (el receptor). */
        if (ref_lat < 0) {
            rlatE -= 90.0;
            rlatO -= 90.0;
        }
    }
    if (rlatE < -90 || rlatE > 90 || rlatO < -90 || rlatO > 90)
        return false;
    if (cpr_nl(rlatE) != cpr_nl(rlatO))
        return false; /* el avión cruzó de zona entre tramas: esperar otro par */

    double lat = newest == 0 ? rlatE : rlatO;
    int nl = cpr_nl(lat);
    int ni = MAX(nl - newest, 1);
    double m = floor(lonE * (nl - 1) - lonO * nl + 0.5);
    double lon = (span / ni) * (cpr_mod(m, ni) + (newest == 0 ? lonE : lonO));
    if (surface) {
        /* Cuatro soluciones separadas 90°: la más cercana a la referencia */
        double best = lon, bestd = 1e9;
        for (int k = 0; k < 4; k++) {
            double c = lon + 90.0 * k;
            c = cpr_mod(c + 180.0, 360.0) - 180.0;
            double dd = fabs(cpr_mod(c - ref_lon + 180.0, 360.0) - 180.0);
            if (dd < bestd) {
                bestd = dd;
                best = c;
            }
        }
        lon = best;
    } else if (lon >= 180.0) {
        lon -= 360.0;
    }
    *olat = lat;
    *olon = lon;
    return true;
}

/* Decodificación local con una sola trama y una referencia cercana. */
static bool cpr_local(int odd, bool surface, int ylat, int xlon, double ref_lat, double ref_lon, double *olat,
                      double *olon)
{
    const double span = surface ? 90.0 : 360.0;
    const double y = ylat / 131072.0, x = xlon / 131072.0;
    double dlat = span / (60.0 - odd);
    double j = floor(ref_lat / dlat) + floor(cpr_mod(ref_lat, dlat) / dlat - y + 0.5);
    double lat = dlat * (j + y);
    if (lat < -90 || lat > 90)
        return false;
    double dlon = span / MAX(cpr_nl(lat) - odd, 1);
    double m = floor(ref_lon / dlon) + floor(cpr_mod(ref_lon, dlon) / dlon - x + 0.5);
    double lon = dlon * (m + x);
    *olat = lat;
    *olon = lon;
    return true;
}

/* Distancia de gran círculo en km (fórmula del semiverseno / haversine). */
static double distance_km(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0, d2r = M_PI / 180.0;
    double dlat = (lat2 - lat1) * d2r, dlon = (lon2 - lon1) * d2r;
    double h = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dlon / 2) * sin(dlon / 2);
    return 2 * R * asin(fmin(1.0, sqrt(h)));
}

/* Comprueba que una posición nueva es verosímil: no salta más de lo que un
 * avión podría recorrer (≈ 2000 km/h + margen) ni está fuera del alcance. */
static bool position_plausible(adsb_t *a, const aircraft_t *p, double lat, double lon, double now)
{
    if (a->rx_valid && distance_km(a->rx_lat, a->rx_lon, lat, lon) > 700.0)
        return false;
    if (p->pos_valid) {
        double dt = fmax(now - p->pos_time, 1.0);
        double d = distance_km(p->lat, p->lon, lat, lon);
        if (d > 2.0 + dt * 0.6) /* 0,6 km/s ≈ 2160 km/h */
            return false;
    }
    return true;
}

static void handle_position(adsb_t *a, aircraft_t *p, const uint8_t *me, bool surface, double now)
{
    int odd = (int)getbits(me, 22, 22);
    p->cpr_lat[odd] = (int)getbits(me, 23, 39);
    p->cpr_lon[odd] = (int)getbits(me, 40, 56);
    p->cpr_time[odd] = now;
    p->cpr_surface[odd] = surface;

    double lat, lon;
    bool ok = false;
    double ref_lat = a->rx_valid ? a->rx_lat : 0.0, ref_lon = a->rx_valid ? a->rx_lon : 0.0;

    /* 1) Global si hay par reciente del mismo tipo */
    if (p->cpr_time[0] > 0 && p->cpr_time[1] > 0 && fabs(p->cpr_time[0] - p->cpr_time[1]) <= CPR_PAIR_MAX_DT &&
        p->cpr_surface[0] == p->cpr_surface[1]) {
        if (!surface || a->rx_valid || p->pos_valid) {
            if (p->pos_valid) {
                ref_lat = p->lat;
                ref_lon = p->lon;
            }
            ok = cpr_global(p, odd, surface, ref_lat, ref_lon, &lat, &lon);
        }
    }
    /* 2) Local respecto a la última posición conocida del propio avión */
    if (!ok && p->pos_valid && now - p->pos_time < 30.0)
        ok = cpr_local(odd, surface, p->cpr_lat[odd], p->cpr_lon[odd], p->lat, p->lon, &lat, &lon);

    if (ok && position_plausible(a, p, lat, lon, now)) {
        p->lat = lat;
        p->lon = lon;
        p->pos_valid = true;
        p->pos_time = now;
    } else if (ok) {
        /* Inverosímil: descartar el par para no arrastrar el error */
        p->cpr_time[0] = p->cpr_time[1] = 0;
    }
}

/* ======================================================================= */
/*  Mensajes ADS-B (campo ME de 56 bits, DF17/18)                          */
/* ======================================================================= */
static const char CALLSIGN_CHARS[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

static void decode_me(adsb_t *a, aircraft_t *p, const uint8_t *me, double now)
{
    int tc = (int)getbits(me, 1, 5);
    p->adsb = true;

    if (tc >= 1 && tc <= 4) {
        /* Identificación y categoría */
        int ca = (int)getbits(me, 6, 8);
        p->category[0] = (char)('A' + (4 - tc));
        p->category[1] = (char)('0' + ca);
        p->category[2] = '\0';
        char cs[9];
        for (int i = 0; i < 8; i++)
            cs[i] = CALLSIGN_CHARS[getbits(me, 9 + 6 * i, 14 + 6 * i)];
        cs[8] = '\0';
        for (int i = 7; i >= 0 && (cs[i] == ' ' || cs[i] == '#'); i--)
            cs[i] = '\0';
        if (cs[0])
            memcpy(p->callsign, cs, sizeof cs);
    } else if (tc >= 5 && tc <= 8) {
        /* Posición en superficie */
        p->on_ground = true;
        handle_position(a, p, me, true, now);
        int mov = (int)getbits(me, 6, 12);
        /* Velocidad en superficie: escala no lineal (DO-260B, tabla 2-73) */
        double gs = -1;
        if (mov == 1) gs = 0;
        else if (mov >= 2 && mov <= 8) gs = 0.125 + (mov - 2) * 0.125;
        else if (mov >= 9 && mov <= 12) gs = 1 + (mov - 9) * 0.25;
        else if (mov >= 13 && mov <= 38) gs = 2 + (mov - 13) * 0.5;
        else if (mov >= 39 && mov <= 93) gs = 15 + (mov - 39) * 1.0;
        else if (mov >= 94 && mov <= 108) gs = 70 + (mov - 94) * 2.0;
        else if (mov >= 109 && mov <= 123) gs = 100 + (mov - 109) * 5.0;
        else if (mov == 124) gs = 175;
        if (gs >= 0) {
            p->gs_kt = (float)gs;
            p->vel_valid = true;
        }
        if (getbits(me, 13, 13)) {
            p->track_deg = getbits(me, 14, 20) * 360.0f / 128.0f;
        }
    } else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
        /* Posición en vuelo + altitud (barométrica 9–18, GNSS 20–22) */
        p->on_ground = false;
        int alt;
        if (decode_ac12(getbits(me, 9, 20), &alt)) {
            if (tc <= 18) {
                p->altitude_ft = alt;
                p->alt_valid = true;
            } else {
                p->altitude_gnss_ft = alt;
                p->gnss_valid = true;
            }
        }
        handle_position(a, p, me, false, now);
    } else if (tc == 19) {
        /* Velocidad */
        int st = (int)getbits(me, 6, 8);
        if (st == 1 || st == 2) {
            int dew = (int)getbits(me, 14, 14), vew = (int)getbits(me, 15, 24);
            int dns = (int)getbits(me, 25, 25), vns = (int)getbits(me, 26, 35);
            if (vew && vns) {
                double k = st == 2 ? 4.0 : 1.0; /* supersónico: unidades ×4 */
                double vx = (dew ? -1 : 1) * (vew - 1) * k;
                double vy = (dns ? -1 : 1) * (vns - 1) * k;
                p->gs_kt = (float)sqrt(vx * vx + vy * vy);
                double trk = atan2(vx, vy) * 180.0 / M_PI;
                p->track_deg = (float)(trk < 0 ? trk + 360.0 : trk);
                p->vel_valid = true;
            }
        } else if (st == 3 || st == 4) {
            if (getbits(me, 14, 14)) {
                p->heading_deg = getbits(me, 15, 24) * 360.0f / 1024.0f;
            }
            int as = (int)getbits(me, 26, 35);
            if (as) {
                p->ias_kt = (float)((as - 1) * (st == 4 ? 4 : 1));
                p->air_valid = true;
            }
        }
        int svr = (int)getbits(me, 37, 37), vr = (int)getbits(me, 38, 46);
        if (vr) {
            p->vrate_fpm = (svr ? -1 : 1) * (vr - 1) * 64;
            p->vr_valid = true;
        }
    } else if (tc == 28) {
        /* Estado de emergencia/prioridad y squawk */
        if (getbits(me, 6, 8) == 1) {
            p->emergency = (int)getbits(me, 9, 11);
            uint32_t id = getbits(me, 12, 24);
            if (id) {
                p->squawk = decode_id13(id);
                p->squawk_valid = true;
            }
        }
    }
}

/* ======================================================================= */
/*  Trama completa                                                         */
/* ======================================================================= */
static int msg_bits_for_df(int df) { return (df & 0x10) ? LONG_MSG_BITS : SHORT_MSG_BITS; }

/* Evento JSON por trama (solo con opts {"raw":true}) de una trama DF17/18 ya aplicada a la tabla. Solo lleva los
 * campos que ESA trama aporta (el estado acumulado va en state_json). */
static void emit_event(adsb_t *a, const aircraft_t *p, int df, const uint8_t *me, double now, const uint8_t *msg)
{
    if (!a->emit)
        return;
    int tc = (int)getbits(me, 1, 5);
    sb_t *s = &a->evsb;
    sb_clear(s);
    char text[160];
    int tl = snprintf(text, sizeof text, "%06X", p->icao);
    sb_printf(s, "{\"icao\":\"%06X\",\"df\":%d,\"tc\":%d,\"hex\":\"", p->icao, df, tc);
    for (int i = 0; i < 14; i++)
        sb_printf(s, "%02X", msg[i]);
    sb_printf(s, "\"");
    if (a->cur_fix)
        sb_printf(s, ",\"fix\":%d", a->cur_fix);
    if (a->cur_crc)
        sb_puts(s, ",\"crc\":true");
    if (tc >= 1 && tc <= 4) {
        sb_puts(s, ",\"cs\":");
        sb_json_str(s, p->callsign);
        sb_printf(s, ",\"cat\":\"%s\"", p->category);
        snprintf(text + tl, sizeof text - (size_t)tl, " identificación %s (cat. %s)", p->callsign, p->category);
    } else if ((tc >= 5 && tc <= 18) || (tc >= 20 && tc <= 22)) {
        if (tc >= 5 && tc <= 8) {
            sb_puts(s, ",\"gnd\":true");
            snprintf(text + tl, sizeof text - (size_t)tl, " posición en superficie");
        } else {
            int alt = tc <= 18 ? p->altitude_ft : p->altitude_gnss_ft;
            bool av = tc <= 18 ? p->alt_valid : p->gnss_valid;
            if (av)
                sb_printf(s, ",\"alt\":%d", alt);
            snprintf(text + tl, sizeof text - (size_t)tl, " posición, altitud %d ft", av ? alt : 0);
        }
        if (p->pos_valid && p->pos_time == now) {
            sb_printf(s, ",\"lat\":%.5f,\"lon\":%.5f", p->lat, p->lon);
            tl = (int)strlen(text);
            snprintf(text + tl, sizeof text - (size_t)tl, " (%.4f, %.4f)", p->lat, p->lon);
        }
    } else if (tc == 19) {
        if (p->vel_valid) {
            sb_printf(s, ",\"gs\":%.1f,\"trk\":%.1f", p->gs_kt, p->track_deg);
            tl = (int)strlen(text);
            snprintf(text + tl, sizeof text - (size_t)tl, " velocidad %.0f kt rumbo %.0f°", p->gs_kt, p->track_deg);
        }
        if (p->vr_valid)
            sb_printf(s, ",\"vr\":%d", p->vrate_fpm);
    } else if (tc == 28) {
        if (p->squawk_valid)
            sb_printf(s, ",\"sq\":\"%04d\"", p->squawk);
        sb_printf(s, ",\"emerg\":%d", p->emergency);
        tl = (int)strlen(text);
        snprintf(text + tl, sizeof text - (size_t)tl, " estado/emergencia %d", p->emergency);
    } else {
        tl = (int)strlen(text);
        snprintf(text + tl, sizeof text - (size_t)tl, " tipo %d", tc);
    }
    sb_puts(s, ",\"text\":");
    sb_json_str(s, text);
    sb_puts(s, "}");
    a->emit(a->emit_ctx, s->data, s->len);
}

/* ---- Corrección de errores guiada por la confianza por bit --------------
 * El CRC-24 es lineal: el síndrome de una trama con errores en los bits i y j
 * es err_syndrome[i] ^ err_syndrome[j]. Buscar CUALQUIER par entre 112 bits da
 * 6 216 hipótesis para solo 2^24 síndromes: 0,04 % de falsos positivos por
 * trama. Pero la demodulación PPM sabe qué bits dudó: la confianza de un bit
 * es |a−b|/(a+b) entre las dos mitades de su microsegundo (≈1 pulso limpio,
 * ≈0 empate). Los errores reales caen casi siempre en los peor valorados, así
 * que solo se prueban los FIX_CAND más dudosos (y con confianza < FIX_CONF_MAX):
 * 12 bits → 78 hipótesis (≈ 5·10⁻⁶ por trama). Si coincide más de una hipótesis
 * se descarta por ambigua. Landry et al., Appl. Sci. 13(1):493.
 * Devuelve nº de bits corregidos (0..2) con sus índices en fx[], o −1. */
static int fix_by_confidence(const adsb_t *a, uint32_t syn, const float *conf, int fx[2])
{
    int cand[FIX_CAND], nc = 0;
    float cv[FIX_CAND];
    for (int i = 5; i < LONG_MSG_BITS; i++) { /* los 5 bits de DF no se tocan */
        if (conf[i] >= FIX_CONF_MAX)
            continue;
        int p;
        if (nc < FIX_CAND)
            p = nc++;
        else if (conf[i] < cv[nc - 1])
            p = nc - 1; /* sustituye al menos dudoso de los candidatos */
        else
            continue;
        while (p > 0 && cv[p - 1] > conf[i]) { /* inserción ordenada por confianza creciente */
            cv[p] = cv[p - 1];
            cand[p] = cand[p - 1];
            p--;
        }
        cv[p] = conf[i];
        cand[p] = i;
    }
    int found = 0, nb = 0, f[2] = {-1, -1};
    for (int x = 0; x < nc; x++) {
        if (a->err_syndrome[cand[x]] == syn) {
            if (!found++) { nb = 1; f[0] = cand[x]; }
        }
        if (a->opt.fix_max >= 2)
            for (int y = x + 1; y < nc; y++)
                if ((a->err_syndrome[cand[x]] ^ a->err_syndrome[cand[y]]) == syn) {
                    if (!found++) { nb = 2; f[0] = cand[x]; f[1] = cand[y]; }
                }
    }
    if (found != 1)
        return -1;
    fx[0] = f[0];
    fx[1] = f[1];
    return nb;
}

/* Tipos de ME plausibles tras una corrección (se descarta TC 0, 23–27 y 30,
 * reservados): una trama "arreglada" con un TC absurdo es casi seguro falsa. */
static bool tc_plausible(const uint8_t *msg)
{
    int tc = (int)getbits(msg, 33, 37);
    return (tc >= 1 && tc <= 22) || tc == 28 || tc == 29 || tc == 31;
}

/* Una aeronave "conocida" ya mandó tramas aceptadas sin corrección. Las
 * correcciones y el entramado CRC solo se admiten para ellas: así un falso
 * positivo exige además coincidir con una dirección ICAO viva (~2⁻²⁴·N). */
static bool ac_known(adsb_t *a, uint32_t icao)
{
    const aircraft_t *p = find_ac(a, icao);
    return p && (p->adsb || p->msgs >= 2);
}

/* ---- Eventos ---------------------------------------------------------- */
static const char *EMERG_NAMES[] = {"ninguna",           "emergencia general",       "emergencia médica",
                                    "combustible mínimo", "sin comunicaciones",       "interferencia ilícita",
                                    "aeronave derribada", "reservado"};

static void emit_json(adsb_t *a, const char *kind, const aircraft_t *p, const char *text, int sq, int emerg)
{
    sb_t *s = &a->evsb;
    sb_clear(s);
    sb_printf(s, "{\"icao\":\"%06X\",\"kind\":\"%s\"", p->icao, kind);
    if (p->callsign[0]) {
        sb_puts(s, ",\"cs\":");
        sb_json_str(s, p->callsign);
    }
    if (sq >= 0)
        sb_printf(s, ",\"sq\":\"%04d\"", sq);
    if (emerg >= 0)
        sb_printf(s, ",\"emerg\":%d", emerg);
    sb_puts(s, ",\"text\":");
    sb_json_str(s, text);
    sb_puts(s, "}");
    a->emit(a->emit_ctx, s->data, s->len);
}

/* Modo por defecto: un evento solo cuando aparece una aeronave ADS-B nueva o
 * cambian su indicativo, squawk o emergencia (decenas de eventos por minuto
 * en vez de cientos por segundo). */
static void emit_changes(adsb_t *a, aircraft_t *p)
{
    if (!a->emit || !p->adsb)
        return;
    char text[160];
    if (!p->announced) {
        p->announced = true;
        snprintf(text, sizeof text, "Nuevo %06X%s%s", p->icao, p->callsign[0] ? " " : "", p->callsign);
        emit_json(a, "new", p, text, p->squawk_valid ? p->squawk : -1, p->emergency ? p->emergency : -1);
        snprintf(p->ev_callsign, sizeof p->ev_callsign, "%s", p->callsign);
        p->ev_squawk = p->squawk_valid ? p->squawk : 0;
        p->ev_emerg = p->emergency;
        return;
    }
    if (p->callsign[0] && strcmp(p->callsign, p->ev_callsign) != 0) {
        snprintf(text, sizeof text, "Indicativo %06X %s", p->icao, p->callsign);
        snprintf(p->ev_callsign, sizeof p->ev_callsign, "%s", p->callsign);
        emit_json(a, "callsign", p, text, -1, -1);
    }
    if (p->squawk_valid && p->squawk != p->ev_squawk) {
        const char *w = p->squawk == 7500   ? " (secuestro)"
                        : p->squawk == 7600 ? " (fallo de radio)"
                        : p->squawk == 7700 ? " (emergencia)"
                                            : "";
        snprintf(text, sizeof text, "Squawk %06X %04d%s", p->icao, p->squawk, w);
        p->ev_squawk = p->squawk;
        emit_json(a, "squawk", p, text, p->squawk, -1);
    }
    if (p->emergency != p->ev_emerg) {
        int e = p->emergency;
        snprintf(text, sizeof text, "Emergencia %06X: %s", p->icao, EMERG_NAMES[e >= 0 && e < 7 ? e : 7]);
        p->ev_emerg = e;
        emit_json(a, "emerg", p, text, -1, e);
    }
}

/* Procesa una trama ya demodulada. conf: confianza de cada bit (NULL si la
 * trama viene de texto hexadecimal). slide: la trama sale del entramado CRC
 * deslizante (exige aeronave conocida). Devuelve el DF si se aceptó o −1. */
static int handle_frame(adsb_t *a, uint8_t *msg, double now, float rssi_db, const float *conf, bool slide)
{
    int df = (int)getbits(msg, 1, 5);
    int nbits = msg_bits_for_df(df);
    uint32_t syn = syndrome(msg, nbits);
    uint32_t icao = 0;
    aircraft_t *p = NULL;
    int nfix = 0;

    if (slide && df != 17 && df != 18)
        return -1;
    switch (df) {
    case 17:
    case 18:
        if (syn != 0) {
            if (a->opt.fix_max < 1)
                return -1;
            int fx[2] = {-1, -1};
            if (conf) {
                nfix = fix_by_confidence(a, syn, conf, fx);
                if (nfix < 0) {
                    a->st.rejected++;
                    return -1;
                }
            } else {
                /* Texto hexadecimal (pruebas): sin confianza, un bit en cualquier posición */
                for (int i = 5; i < LONG_MSG_BITS && !nfix; i++)
                    if (a->err_syndrome[i] == syn) {
                        fx[0] = i;
                        nfix = 1;
                    }
                if (!nfix)
                    return -1;
            }
            for (int k = 0; k < nfix; k++)
                flipbit(msg, fx[k]);
            /* Salvaguardas de las correcciones guiadas: aeronave conocida y TC creíble */
            if (conf && (!ac_known(a, getbits(msg, 9, 32)) || !tc_plausible(msg)))
                return -1;
        } else if (slide && !ac_known(a, getbits(msg, 9, 32))) {
            return -1;
        }
        if (df == 18 && getbits(msg, 6, 8) != 0)
            return -1; /* DF18 con CF≠0: TIS-B/ADS-R con otros formatos */
        icao = getbits(msg, 9, 32);
        p = get_ac(a, icao, now);
        if (!p)
            return -1;
        uint8_t me[7];
        for (int i = 0; i < 7; i++)
            me[i] = msg[4 + i];
        decode_me(a, p, me, now);
        break;
    case 11:
        /* All-call: el resto puede contener el identificador del
         * interrogador (≤ 7 bits), así que se aceptan síndromes pequeños. */
        if (syn & 0xFFFF80)
            return -1;
        icao = getbits(msg, 9, 32);
        p = get_ac(a, icao, now);
        if (!p)
            return -1;
        break;
    case 0:
    case 4:
    case 5:
    case 16:
    case 20:
    case 21: {
        /* Dirección/paridad: el síndrome ES la dirección ICAO. Solo se
         * acepta si la aeronave ya se conoce, para no inventar aviones. */
        icao = syn;
        p = find_ac(a, icao);
        if (!p)
            return -1;
        uint32_t f13 = getbits(msg, 20, 32);
        if (df == 5 || df == 21) {
            if (f13) {
                p->squawk = decode_id13(f13);
                p->squawk_valid = true;
            }
        } else {
            int alt;
            if (decode_ac13(f13, &alt)) {
                p->altitude_ft = alt;
                p->alt_valid = true;
            }
        }
        if (df == 0 || df == 4 || df == 5 || df == 20 || df == 21) {
            /* Campo VS/FS: bit de "en tierra" */
            int fs = (int)getbits(msg, 6, 8);
            if (df != 0 && (fs == 1 || fs == 3))
                p->on_ground = true;
        }
        break;
    }
    default:
        return -1;
    }

    p->last_seen = now;
    p->msgs++;
    if (rssi_db > -99.f)
        p->rssi_db = p->msgs == 1 ? rssi_db : p->rssi_db + 0.2f * (rssi_db - p->rssi_db);
    a->total++;
    a->st.frames++;
    if (nfix == 1)
        a->st.fix1++;
    else if (nfix == 2)
        a->st.fix2++;
    if (slide)
        a->st.crc_rec++;
    if (a->opt.raw) {
        if (df == 17 || df == 18) {
            a->cur_fix = nfix;
            a->cur_crc = slide;
            emit_event(a, p, df, msg + 4, now, msg); /* el campo ME empieza en el byte 4 */
        }
    } else {
        emit_changes(a, p);
    }
    return df;
}

/* ======================================================================= */
/*  API                                                                    */
/* ======================================================================= */
adsb_t *adsb_create(void)
{
    crc_init();
    adsb_t *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->opt.fix_max = 2;
    /* Síndrome de un error en cada posición de una trama larga: se pone un 1
     * en el bit i de una trama nula y se calcula su síndrome. */
    for (int i = 0; i < LONG_MSG_BITS; i++) {
        uint8_t m[14] = {0};
        flipbit(m, i);
        a->err_syndrome[i] = syndrome(m, LONG_MSG_BITS);
    }
    return a;
}

void adsb_destroy(adsb_t *a)
{
    if (!a)
        return;
    free(a->mag);
    sb_free(&a->evsb);
    free(a);
}

void adsb_reset(adsb_t *a)
{
    a->nac = 0;
    a->tail = 0;
}

void adsb_set_options(adsb_t *a, const adsb_opts_t *o)
{
    a->opt = *o;
    a->opt.fix_max = CLAMP(o->fix_max, 0, 2);
}

void adsb_get_stats(const adsb_t *a, adsb_stats_t *st)
{
    *st = a->st;
    st->fps = a->msg_rate;
}

void adsb_set_receiver(adsb_t *a, bool valid, double lat, double lon)
{
    a->rx_valid = valid;
    a->rx_lat = lat;
    a->rx_lon = lon;
}

int adsb_decode_hex(adsb_t *a, const char *hex, double now)
{
    uint8_t msg[14] = {0};
    size_t n = strlen(hex);
    if (n != 14 && n != 28)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1)
            return -1;
        msg[i] = (uint8_t)v;
    }
    return handle_frame(a, msg, now, -100.f, NULL, false);
}

/* Demodula 112 bits PPM desde d (= primer bit de datos): bit = 1 si la primera
 * mitad del microsegundo supera a la segunda. conf[b] = |x−y|/(x+y) ∈ [0,1]:
 * con SNR alta ≈ 1; en un empate (ruido, interferencia) ≈ 0. Devuelve la
 * confianza media y el nivel medio de las mitades ganadoras. */
static float demod_bits(const float *d, uint8_t msg[14], float conf[LONG_MSG_BITS], float *level)
{
    float sum = 0, lv = 0;
    memset(msg, 0, 14);
    for (int b = 0; b < LONG_MSG_BITS; b++) {
        float x = d[2 * b], y = d[2 * b + 1];
        if (x > y)
            msg[b >> 3] |= (uint8_t)(1 << (7 - (b & 7)));
        float t = x + y;
        conf[b] = t > 0 ? fabsf(x - y) / t : 0.f;
        sum += conf[b];
        lv += fmaxf(x, y);
    }
    *level = lv / LONG_MSG_BITS;
    return sum / LONG_MSG_BITS;
}

/* Deduplicado del entramado deslizante: la misma trama puede aparecer por la
 * vía del preámbulo y por la del CRC. Se compara la posición absoluta de su
 * primer bit de datos (±2 muestras). */
static bool recent_has(const adsb_t *a, uint64_t pos)
{
    for (int i = 0; i < a->nrecent && i < 16; i++)
        if ((a->recent[i].pos > pos ? a->recent[i].pos - pos : pos - a->recent[i].pos) <= 2)
            return true;
    return false;
}

static void recent_add(adsb_t *a, uint64_t pos)
{
    int i = a->nrecent++ & 15;
    a->recent[i].pos = pos;
}

/* Entramado por CRC deslizante (Stream1090, M. Puchol/G. Brown): en vez de
 * exigir el preámbulo, se prueba CADA muestra como primer bit de una trama
 * DF17/18 y manda el CRC. Recupera tramas con el preámbulo pisado por otra
 * transmisión (solapamientos son habituales en zonas con mucho tráfico).
 *
 * Riesgo: sin preámbulo hay 2·10⁶ hipótesis por segundo y un CRC de 24 bits
 * deja pasar 2⁻²⁴ de ellas al azar (≈ 0,01 tramas falsas/s de haber filtrado
 * solo por el CRC). Se acota con cuatro filtros encadenados:
 *   1) los 5 primeros bits deben dar DF17 o DF18 (descarta 15/16 de las muestras);
 *   2) confianza media de los 112 bits ≥ SLIDE_CONF_MIN (el ruido da ≈ 0,3);
 *   3) CRC válido (o corregible con ≤ 2 bits poco fiables, como siempre);
 *   4) la dirección ICAO ya debe ser de una aeronave conocida, es decir,
 *      vista antes en tramas limpias: un falso positivo exige además acertar
 *      una de las ~100 direcciones vivas (~10⁻⁵ por candidato espurio).
 * El coste: una aeronave que SOLO llega por esta vía no se descubre; hace
 * falta una trama normal suya antes. */
static int slide_try(adsb_t *a, const float *s, int j, double tj)
{
    const float *d = s + PREAMBLE_SAMPLES;
    int df5 = 0;
    for (int b = 0; b < 5; b++)
        df5 = (df5 << 1) | (d[2 * b] > d[2 * b + 1]);
    if (df5 != 17 && df5 != 18)
        return -1;
    uint8_t msg[14];
    float conf[LONG_MSG_BITS], level;
    if (demod_bits(d, msg, conf, &level) < SLIDE_CONF_MIN)
        return -1;
    uint64_t pos = a->base + (uint64_t)j + PREAMBLE_SAMPLES;
    if (recent_has(a, pos))
        return -1;
    int ok = handle_frame(a, msg, tj, 20.f * log10f(level / 2048.f + 1e-9f), conf, true);
    if (ok >= 0)
        recent_add(a, pos);
    return ok;
}

void adsb_process(adsb_t *a, const int16_t *iq, int n, double now)
{
    const int tail0 = a->tail;
    const int need = a->tail + n;
    if (need > a->mag_cap) {
        a->mag = realloc(a->mag, sizeof(float) * (size_t)need);
        a->mag_cap = need;
    }
    float *m = a->mag;
    /* Magnitud |I + jQ| de cada muestra (tras la cola del bloque anterior) */
    for (int i = 0; i < n; i++) {
        float I = iq[2 * i], Q = iq[2 * i + 1];
        m[a->tail + i] = sqrtf(I * I + Q * Q);
    }
    const int total = need;
    const int last = total - MSG_SAMPLES;
    const bool slide = a->opt.crcframe;
    int j = 0;
    for (; j < last; j++) {
        const float *s = &m[j];
        const double tj = now + (double)(j - tail0) / ADSB_RATE; /* hora de la muestra j */
        bool pre = s[0] > s[1] && s[1] < s[2] && s[2] > s[3] && s[3] < s[0] && s[4] < s[0] && s[5] < s[0] &&
                   s[6] < s[0] && s[7] > s[8] && s[8] < s[9] && s[9] > s[6];
        if (pre) {
            /* Los huecos (4,5 y 11–14) deben estar claramente por debajo de los
             * pulsos: umbral = media de los pulsos / 1,5. */
            float high = (s[0] + s[2] + s[7] + s[9]) / 6.0f;
            pre = !(s[4] >= high || s[5] >= high || s[11] >= high || s[12] >= high || s[13] >= high || s[14] >= high);
        }
        int ok = -1;
        if (pre) {
            /* Demodulación PPM con confianza por bit */
            uint8_t msg[14];
            float conf[LONG_MSG_BITS], lvl;
            demod_bits(s + PREAMBLE_SAMPLES, msg, conf, &lvl);
            int df = (int)(msg[0] >> 3);
            if (df == 0 || df == 4 || df == 5 || df == 11 || df == 16 || df == 17 || df == 18 || df == 20 || df == 21) {
                uint64_t pos = a->base + (uint64_t)j + PREAMBLE_SAMPLES;
                float level = (s[0] + s[2] + s[7] + s[9]) / 4.0f;
                float rssi = 20.f * log10f(level / 2048.f + 1e-9f);
                /* hora de la trama = hora del bloque + posición de la muestra */
                ok = handle_frame(a, msg, tj, rssi, conf, false);
                if (ok >= 0 && slide)
                    recent_add(a, pos);
            }
        }
        if (ok >= 0) {
            /* Con el entramado CRC no se salta la trama aceptada: otra puede
             * estar solapada dentro de ella. Sin él, se salta (como siempre). */
            if (!slide)
                j += PREAMBLE_SAMPLES + 2 * msg_bits_for_df(ok) - 1;
        } else if (slide) {
            slide_try(a, s, j, tj);
        }
    }
    /* Conservar la cola sin examinar para el siguiente bloque */
    int keep = total - j;
    if (keep > 0) {
        memmove(m, m + j, sizeof(float) * (size_t)keep);
        a->base += (uint64_t)j;
    } else {
        a->base += (uint64_t)total;
    }
    a->tail = keep > 0 ? keep : 0;

    if (now - a->rate_t0 >= 2.0) {
        a->msg_rate = (float)((a->total - a->total_prev) / (now - a->rate_t0));
        a->total_prev = a->total;
        a->rate_t0 = now;
        expire(a, now);
    }
}

void adsb_set_emit(adsb_t *a, dec_emit_fn fn, void *ctx)
{
    a->emit = fn;
    a->emit_ctx = ctx;
}

/* Cuerpo común: "msgs", "rate", "rx" y "ac" (el llamador abre la llave). */
static void write_state_body(adsb_t *a, sb_t *sb, double now);

void adsb_json(adsb_t *a, sb_t *sb, double now)
{
    sb_puts(sb, "{\"type\":\"adsb\",");
    write_state_body(a, sb, now);
}

void adsb_state_json(adsb_t *a, sb_t *sb, double now)
{
    sb_puts(sb, "{");
    write_state_body(a, sb, now);
}

static void write_state_body(adsb_t *a, sb_t *sb, double now)
{
    expire(a, now);
    /* Métricas: fps = tramas válidas/s (ventana de 2 s); fix1/fix2 = corregidas de
     * 1 y 2 bits; crc = recuperadas por el entramado CRC; rej = candidatos
     * DF17/18 descartados por CRC incorregible. */
    sb_printf(sb, "\"msgs\":%lu,\"rate\":%.1f,\"fps\":%.1f,\"fix1\":%lu,\"fix2\":%lu,\"crc\":%lu,\"rej\":%lu,\"rx\":%s,\"ac\":[",
              a->total, a->msg_rate, a->msg_rate, a->st.fix1, a->st.fix2, a->st.crc_rec, a->st.rejected,
              a->rx_valid ? "true" : "false");
    bool first = true;
    for (int i = 0; i < a->nac; i++) {
        const aircraft_t *p = &a->ac[i];
        if (p->msgs < 2 && !p->adsb)
            continue; /* una sola trama Mode S suelta no basta */
        if (!first)
            sb_puts(sb, ",");
        first = false;
        sb_printf(sb, "{\"icao\":\"%06X\"", p->icao);
        if (p->callsign[0]) {
            sb_puts(sb, ",\"cs\":");
            sb_json_str(sb, p->callsign);
        }
        if (p->category[0])
            sb_printf(sb, ",\"cat\":\"%s\"", p->category);
        if (p->alt_valid)
            sb_printf(sb, ",\"alt\":%d", p->altitude_ft);
        if (p->gnss_valid)
            sb_printf(sb, ",\"galt\":%d", p->altitude_gnss_ft);
        if (p->vel_valid)
            sb_printf(sb, ",\"gs\":%.1f,\"trk\":%.1f", p->gs_kt, p->track_deg);
        if (p->air_valid)
            sb_printf(sb, ",\"ias\":%.0f,\"hdg\":%.1f", p->ias_kt, p->heading_deg);
        if (p->vr_valid)
            sb_printf(sb, ",\"vr\":%d", p->vrate_fpm);
        if (p->squawk_valid)
            sb_printf(sb, ",\"sq\":\"%04d\"", p->squawk);
        if (p->emergency)
            sb_printf(sb, ",\"emerg\":%d", p->emergency);
        if (p->pos_valid) {
            sb_printf(sb, ",\"lat\":%.5f,\"lon\":%.5f,\"seen_pos\":%.1f", p->lat, p->lon, fmax(0.0, now - p->pos_time));
            if (a->rx_valid)
                sb_printf(sb, ",\"dist\":%.1f", distance_km(a->rx_lat, a->rx_lon, p->lat, p->lon));
        }
        sb_printf(sb, ",\"gnd\":%s,\"msgs\":%u,\"seen\":%.1f,\"rssi\":%.1f,\"adsb\":%s}", p->on_ground ? "true" : "false",
                  p->msgs, fmax(0.0, now - p->last_seen), p->rssi_db, p->adsb ? "true" : "false");
    }
    sb_puts(sb, "]}");
}

/* ======================================================================= */
/*  Envoltorio para el marco de decodificadores (decoder.h)                */
/* ======================================================================= */
/* ADS-B es WIDE: el hilo de la instancia recibe las muestras Q11 crudas a
 * 2 Msps exactos (1 µs = 2 muestras: el demodulador PPM lo exige). */
static void *dec_adsb_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    adsb_t *a = adsb_create();
    if (!a) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    adsb_set_receiver(a, cfg->loc_valid, cfg->lat, cfg->lon);
    /* opts: {"raw":bool, "crcframe":bool, "fix":0|1|2} */
    if (cfg->opts && cfg->opts[0]) {
        jtok_t t[16];
        int nt = json_parse(cfg->opts, strlen(cfg->opts), t, 16);
        if (nt >= 1 && t[0].type == J_OBJ) {
            adsb_opts_t o = a->opt;
            int i;
            double v;
            bool b;
            if ((i = json_obj_get(cfg->opts, t, 0, "raw")) >= 0 && json_bool(cfg->opts, t, i, &b))
                o.raw = b;
            if ((i = json_obj_get(cfg->opts, t, 0, "crcframe")) >= 0 && json_bool(cfg->opts, t, i, &b))
                o.crcframe = b;
            if ((i = json_obj_get(cfg->opts, t, 0, "fix")) >= 0 && json_num(cfg->opts, t, i, &v))
                o.fix_max = (int)v;
            adsb_set_options(a, &o);
        }
    }
    return a;
}

static void dec_adsb_destroy(void *st) { adsb_destroy(st); }

static void dec_adsb_process_raw(void *st, const int16_t *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    adsb_t *a = st;
    adsb_set_emit(a, emit, ctx); /* solo vale durante esta llamada */
    adsb_process(a, iq, n, now);
    adsb_set_emit(a, NULL, NULL);
}

static void dec_adsb_state(void *st, sb_t *sb, double now) { adsb_state_json(st, sb, now); }

static void dec_adsb_set_location(void *st, bool valid, double lat, double lon)
{
    adsb_set_receiver(st, valid, lat, lon);
}

const decoder_desc_t DEC_ADSB = {
    .id = "adsb",
    .name = "ADS-B / Mode S",
    .group = "Aviación",
    .desc = "Aeronaves a 1090 MHz: identificación, posición, altitud y velocidad (PPM, 2 Msps).",
    .kind = DEC_WIDE,
    .wide_rate = ADSB_RATE,
    .freqs = {1090000000LL, 0, 0, 0},
    .create = dec_adsb_create,
    .destroy = dec_adsb_destroy,
    .process_raw = dec_adsb_process_raw,
    .state_json = dec_adsb_state,
    .set_location = dec_adsb_set_location,
};
