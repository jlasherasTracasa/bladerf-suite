/*
 * ais.c — Decodificador AIS (Automatic Identification System), ITU-R M.1371.
 *
 * Capa física (M.1371 anexo 2 §3): GMSK con BT = 0,4 a 9600 bit/s en 161,975 y
 * 162,025 MHz con canales de 25 kHz, índice de modulación 0,5 (desviación de
 * frecuencia ±2400 Hz: un símbolo gira la fase ±π/2). Datos NRZI + HDLC
 * (hdlc.h): bandera 0x7E, inserción de ceros y FCS CRC-16/X-25. La ráfaga
 * lleva rampa, 24 bits de secuencia de entrenamiento y bandera de inicio.
 *
 * Cadena DSP (48 kHz = 5 muestras por símbolo, entrada compleja del DDC):
 *   1. Discriminador FM: Δfase/muestra → ±1 para ±2400 Hz (sync.h, fmdisc).
 *      La señal GMSK es FM de envolvente constante, así que el discriminador
 *      es casi el detector óptimo y no necesita conocer la fase de portadora.
 *   2. Filtro paso bajo FIR (corte 0,7·Rb) que elimina el ruido de FM fuera
 *      de la banda del símbolo (el ruido del discriminador crece con f).
 *   3. El desvío de portadora (CFO) aparece como una constante sumada al
 *      discriminador. Se resta su media móvil de 48 símbolos ponderada por la
 *      potencia instantánea |x|² (el HDLC con relleno de ceros y la secuencia de
 *      entrenamiento están equilibrados; la ponderación evita que el ruido
 *      anterior a la ráfaga desplace la estimación).
 *   4. Recuperación de reloj por MÚLTIPLES FASES: con 5 muestras/símbolo se
 *      muestrean los 5 desfases en paralelo, cada uno con su propio NRZI+HDLC.
 *      Gana el que tiene el ojo abierto y el CRC valida: sin lazo que enganchar
 *      en una ráfaga de 27 ms. A ±100 ppm la deriva en 1000 bits es 0,1 símbolo
 *      (< 0,2 de paso entre fases). Los duplicados (misma trama en fases
 *      vecinas) se descartan por huella.
 *
 * No implementada: la mejora PLVA (Viterbi paralelo de lista) de Kanaan et al.
 * arXiv:2503.01744; en tierra las colisiones son poco frecuentes.
 *
 * Constante NO verificada contra la norma: si la secuencia de entrenamiento va
 * también codificada en NRZI (este decodificador no depende de ello: solo usa
 * las banderas HDLC).
 *
 * Mensajes (campos como en la tabla de gpsd, "AIVDM/AIVDO protocol decoding",
 * bits numerados desde 0): 1/2/3 informe de posición clase A, 4/11 estación
 * base/hora UTC, 5 datos estáticos y de viaje, 9 aeronave SAR (solo posición),
 * 18 clase B, 19 clase B ampliado, 21 ayuda a la navegación, 24 clase B
 * estáticos (partes A y B), 27 posición de largo alcance (satélite).
 * Cada evento incluye las sentencias NMEA !AIVDM equivalentes.
 */
#include "ais.h"

#include "decoder.h"
#include "hdlc.h"
#include "../dsp/filter.h"
#include "../dsp/pulse.h"
#include "../dsp/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPS 5
#define DC_WIN (48 * SPS) /* ventana de la media: 48 símbolos */
#define MAX_BITS 1100
#define SHIP_TTL 600.0
#define MAX_SHIPS 1024
#define DEDUP_N 16
#define DEDUP_SAMPLES 12000 /* 0,25 s */
#define AIS_DELAY 12        /* retardo de fmdisc + FIR de 25 coef. (12 muestras de grupo), medido en tests/test_labels.c */

/* ======================================================================= */
/*  Campos de bits                                                         */
/* ======================================================================= */
static uint32_t getu(const uint8_t *b, int start, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++)
        v = (v << 1) | (b[start + i] & 1);
    return v;
}

static int32_t gets(const uint8_t *b, int start, int len)
{
    uint32_t v = getu(b, start, len);
    if (v & (1u << (len - 1)))
        return (int32_t)(v | (len == 32 ? 0 : ~0u << len));
    return (int32_t)v;
}

/* ASCII de 6 bits (Tabla 47): 0..31 → '@'..'_', 32..63 → ' '..'?'.
 * Se recortan los '@' de relleno y los espacios finales. */
static void gettext(const uint8_t *b, int start, int nchars, char *out)
{
    int n = 0;
    for (int i = 0; i < nchars; i++) {
        uint32_t v = getu(b, start + 6 * i, 6);
        out[n++] = (char)(v < 32 ? v + 64 : v);
    }
    while (n > 0 && (out[n - 1] == '@' || out[n - 1] == ' '))
        n--;
    out[n] = '\0';
    char *at = strchr(out, '@'); /* '@' interior = fin de cadena */
    if (at)
        *at = '\0';
}

/* ======================================================================= */
/*  Mensajes                                                               */
/* ======================================================================= */
const char *ais_navstat_name(int s)
{
    static const char *n[16] = {"en marcha con motor", "fondeado", "sin gobierno", "maniobra restringida",
                                "restringido por calado", "amarrado", "varado", "pescando",
                                "navegando a vela", "reservado (HSC)", "reservado (WIG)",
                                "remolcando a popa", "remolcando/empujando", "reservado",
                                "AIS-SART activo", "no definido"};
    return (s >= 0 && s < 16) ? n[s] : "";
}

const char *ais_shiptype_name(int t)
{
    switch (t / 10) {
    case 2: return "WIG";
    case 4: return "alta velocidad";
    case 6: return "pasaje";
    case 7: return "carga";
    case 8: return "petrolero";
    case 9: return "otro";
    case 3:
        switch (t) {
        case 30: return "pesquero";
        case 31: case 32: return "remolcador";
        case 33: return "dragado";
        case 34: return "buceo";
        case 35: return "militar";
        case 36: return "vela";
        case 37: return "recreo";
        }
        return "";
    case 5:
        switch (t) {
        case 50: return "práctico";
        case 51: return "búsqueda y rescate";
        case 52: return "remolcador";
        case 53: return "tender de puerto";
        case 54: return "antipolución";
        case 55: return "autoridad";
        case 58: return "sanitario";
        case 59: return "no combatiente";
        }
        return "";
    }
    return "";
}

static void parse_pos27(const uint8_t *b, ais_msg_t *m)
{
    m->navstat = (int)getu(b, 40, 4);
    int32_t lon = gets(b, 44, 18), lat = gets(b, 62, 17);
    m->pos_valid = lon != 181 * 600 && lat != 91 * 600;
    m->lon = lon / 600.0;
    m->lat = lat / 600.0;
    int sp = (int)getu(b, 79, 6), co = (int)getu(b, 85, 9);
    m->sog_kn = sp == 63 ? -1 : sp;
    m->cog_deg = co == 511 ? -1 : co;
    m->cls = 'A';
}

static void parse_pos(const uint8_t *b, ais_msg_t *m, int s_lon, int s_lat, bool cls_b)
{
    int32_t lon = gets(b, s_lon, 28), lat = gets(b, s_lat, 27);
    m->pos_valid = lon != 181 * 600000 && lat != 91 * 600000;
    m->lon = lon / 600000.0;
    m->lat = lat / 600000.0;
    (void)cls_b;
}

int ais_parse(const uint8_t *b, int n, ais_msg_t *m)
{
    memset(m, 0, sizeof *m);
    m->sog_kn = m->cog_deg = -1;
    m->hdg = m->navstat = -1;
    m->cls = '?';
    if (n < 38)
        return -1;
    m->type = (int)getu(b, 0, 6);
    m->repeat = (int)getu(b, 6, 2);
    m->mmsi = getu(b, 8, 30);
    if (m->mmsi == 0 || m->mmsi > 999999999u || m->type < 1 || m->type > 27)
        return -1;
    switch (m->type) {
    case 1: case 2: case 3: {
        if (n != 168)
            return -1;
        m->cls = 'A';
        m->navstat = (int)getu(b, 38, 4);
        int sp = (int)getu(b, 50, 10), co = (int)getu(b, 116, 12), hd = (int)getu(b, 128, 9);
        m->sog_kn = sp == 1023 ? -1 : sp / 10.0;
        m->cog_deg = co == 3600 ? -1 : co / 10.0;
        m->hdg = hd == 511 ? -1 : hd;
        parse_pos(b, m, 61, 89, false);
        m->second = (int)getu(b, 137, 6);
        break;
    }
    case 4: case 11:
        if (n != 168)
            return -1;
        m->year = (int)getu(b, 38, 14);
        m->month = (int)getu(b, 52, 4);
        m->day = (int)getu(b, 56, 5);
        m->hour = (int)getu(b, 61, 5);
        m->minute = (int)getu(b, 66, 6);
        m->second = (int)getu(b, 72, 6);
        parse_pos(b, m, 79, 107, false);
        m->cls = 'S'; /* estación base fija */
        break;
    case 5: {
        if (n < 420 || n > 430)
            return -1;
        m->cls = 'A';
        m->imo = (int)getu(b, 40, 30);
        gettext(b, 70, 7, m->callsign);
        gettext(b, 112, 20, m->name);
        m->shiptype = (int)getu(b, 232, 8);
        m->bow = (int)getu(b, 240, 9);
        m->stern = (int)getu(b, 249, 9);
        m->port = (int)getu(b, 258, 6);
        m->stbd = (int)getu(b, 264, 6);
        m->eta_mo = (int)getu(b, 274, 4);
        m->eta_d = (int)getu(b, 278, 5);
        m->eta_h = (int)getu(b, 283, 5);
        m->eta_mi = (int)getu(b, 288, 6);
        m->draught_m = getu(b, 294, 8) / 10.0;
        gettext(b, 302, 20, m->dest);
        break;
    }
    case 9: {
        if (n != 168)
            return -1;
        m->cls = 'A';
        int sp = (int)getu(b, 50, 10), co = (int)getu(b, 116, 12);
        m->sog_kn = sp == 1023 ? -1 : sp;
        m->cog_deg = co == 3600 ? -1 : co / 10.0;
        parse_pos(b, m, 61, 89, false);
        break;
    }
    case 18: {
        if (n != 168)
            return -1;
        m->cls = 'B';
        int sp = (int)getu(b, 46, 10), co = (int)getu(b, 112, 12), hd = (int)getu(b, 124, 9);
        m->sog_kn = sp == 1023 ? -1 : sp / 10.0;
        m->cog_deg = co == 3600 ? -1 : co / 10.0;
        m->hdg = hd == 511 ? -1 : hd;
        parse_pos(b, m, 57, 85, true);
        m->second = (int)getu(b, 133, 6);
        break;
    }
    case 19: {
        if (n != 312)
            return -1;
        m->cls = 'B';
        int sp = (int)getu(b, 46, 10), co = (int)getu(b, 112, 12), hd = (int)getu(b, 124, 9);
        m->sog_kn = sp == 1023 ? -1 : sp / 10.0;
        m->cog_deg = co == 3600 ? -1 : co / 10.0;
        m->hdg = hd == 511 ? -1 : hd;
        parse_pos(b, m, 57, 85, true);
        m->second = (int)getu(b, 133, 6);
        gettext(b, 143, 20, m->name);
        m->shiptype = (int)getu(b, 263, 8);
        m->bow = (int)getu(b, 271, 9);
        m->stern = (int)getu(b, 280, 9);
        m->port = (int)getu(b, 289, 6);
        m->stbd = (int)getu(b, 295, 6);
        break;
    }
    case 21: {
        if (n < 272 || n > 360)
            return -1;
        m->cls = 'N';
        m->aid_type = (int)getu(b, 38, 5);
        gettext(b, 43, 20, m->name);
        parse_pos(b, m, 164, 192, false);
        m->bow = (int)getu(b, 219, 9);
        m->stern = (int)getu(b, 228, 9);
        m->port = (int)getu(b, 237, 6);
        m->stbd = (int)getu(b, 243, 6);
        m->second = (int)getu(b, 253, 6);
        break;
    }
    case 24: {
        if (n != 168)
            return -1;
        m->cls = 'B';
        m->part = (int)getu(b, 38, 2);
        if (m->part == 0) {
            gettext(b, 40, 20, m->name);
        } else if (m->part == 1) {
            m->shiptype = (int)getu(b, 40, 8);
            gettext(b, 48, 3, m->vendor);
            gettext(b, 90, 7, m->callsign);
            m->bow = (int)getu(b, 132, 9);
            m->stern = (int)getu(b, 141, 9);
            m->port = (int)getu(b, 150, 6);
            m->stbd = (int)getu(b, 156, 6);
        } else {
            return -1;
        }
        break;
    }
    case 27:
        if (n != 96)
            return -1;
        parse_pos27(b, m);
        break;
    default:
        break; /* tipos 6-8, 10, 12-17, 20, 22, 23, 25, 26: solo cabecera */
    }
    return 0;
}

void ais_text(const ais_msg_t *m, char *out, size_t cap)
{
    char pos[64] = "", spd[48] = "", nm[64] = "";
    if (m->pos_valid)
        snprintf(pos, sizeof pos, " %.4f,%.4f", m->lat, m->lon);
    if (m->sog_kn >= 0)
        snprintf(spd, sizeof spd, " %.1f kn", m->sog_kn);
    if (m->name[0])
        snprintf(nm, sizeof nm, " %s", m->name);
    snprintf(out, cap, "AIS %d MMSI %u%s%s%s", m->type, (unsigned)m->mmsi, nm, pos, spd);
}

/* ======================================================================= */
/*  NMEA                                                                   */
/* ======================================================================= */
static unsigned nmea_cs(const char *s)
{
    unsigned c = 0;
    for (s++; *s && *s != '*'; s++)
        c ^= (unsigned char)*s;
    return c;
}

bool ais_nmea_check(const char *s)
{
    const char *star = strchr(s, '*');
    if (s[0] != '!' && s[0] != '$')
        return false;
    if (!star)
        return false;
    return (unsigned)strtoul(star + 1, NULL, 16) == nmea_cs(s);
}

/* Armado de 6 bits (Tabla 'Payload Armoring'): v+48, y +8 si pasa de 'W'. */
static char armor(unsigned v)
{
    v += 48;
    if (v > 87)
        v += 8;
    return (char)v;
}

static int dearmor(char c)
{
    int v = (unsigned char)c - 48;
    if (v > 40)
        v -= 8;
    return (v < 0 || v > 63) ? -1 : v;
}

int ais_payload_to_bits(const char *p, int fill, uint8_t *bits, int cap)
{
    int n = 0;
    for (; *p && *p != ','; p++) {
        int v = dearmor(*p);
        if (v < 0 || n + 6 > cap)
            return -1;
        for (int k = 5; k >= 0; k--)
            bits[n++] = (uint8_t)((v >> k) & 1);
    }
    return n - fill;
}

void ais_nmea(const uint8_t *b, int n, char chan, int seq, sb_t *out)
{
    int nch = (n + 5) / 6, fill = nch * 6 - n;
    int nsent = (nch + 59) / 60;
    for (int s = 0; s < nsent; s++) {
        char line[160], pay[64];
        int a = s * 60, e = MIN(nch, a + 60), k = 0;
        for (int c = a; c < e; c++) {
            unsigned v = 0;
            for (int j = 0; j < 6; j++) {
                int i = c * 6 + j;
                v = (v << 1) | (i < n ? (unsigned)(b[i] & 1) : 0u);
            }
            pay[k++] = armor(v);
        }
        pay[k] = '\0';
        int len = snprintf(line, sizeof line, "!AIVDM,%d,%d,", nsent, s + 1);
        if (nsent > 1)
            len += snprintf(line + len, sizeof line - (size_t)len, "%d", seq % 10);
        len += snprintf(line + len, sizeof line - (size_t)len, ",%c,%s,%d", chan, pay, s == nsent - 1 ? fill : 0);
        unsigned cs = nmea_cs(line);
        snprintf(line + len, sizeof line - (size_t)len, "*%02X", cs);
        if (s)
            sb_puts(out, "\n");
        sb_puts(out, line);
    }
}

/* ======================================================================= */
/*  Tabla de barcos                                                        */
/* ======================================================================= */
typedef struct {
    uint32_t mmsi;
    char name[21], callsign[8], dest[21];
    int shiptype, imo, navstat, hdg;
    int bow, stern, port, stbd;
    double draught, lat, lon, sog, cog;
    bool pos;
    char cls;
    double first, last;
    unsigned msgs;
} ship_t;

typedef struct {
    uint32_t h;
    long at;
} dedup_t;

typedef struct {
    hdlc_rx_t hdlc;
} phase_t;

typedef struct {
    fmdisc_t disc;
    fir_f_t lpf;
    float *tmp, *tmp2;
    int tmp_cap;
    float ring[DC_WIN], ringw[DC_WIN];
    double ring_sum, ring_w;
    int ring_pos;
    long n;                 /* muestras procesadas */
    long blk_n0;            /* valor de n al empezar la llamada en curso a process() */
    int64_t mk0, mk1;       /* inicio y fin de la trama en curso (muestras, índice absoluto en n) */
    phase_t ph[SPS];
    dedup_t dd[DEDUP_N];
    int dd_pos;
    ship_t *ships;
    int nships;
    unsigned long total, bad_len;
    char chan;
    int seq;
    bool loc_valid;
    double lat, lon;
    sb_t ev;
    sb_t nm;
    uint8_t *bitbuf;
    dec_emit_fn emit;
    void *emit_ctx;
    double blk_now;
    int blk_i;
} ais_t;

static ship_t *get_ship(ais_t *a, uint32_t mmsi, double now)
{
    for (int i = 0; i < a->nships; i++)
        if (a->ships[i].mmsi == mmsi)
            return &a->ships[i];
    if (a->nships >= MAX_SHIPS) { /* se desplaza el más antiguo */
        int old = 0;
        for (int i = 1; i < a->nships; i++)
            if (a->ships[i].last < a->ships[old].last)
                old = i;
        a->ships[old] = a->ships[--a->nships];
    }
    ship_t *s = &a->ships[a->nships++];
    memset(s, 0, sizeof *s);
    s->mmsi = mmsi;
    s->navstat = s->hdg = -1;
    s->sog = s->cog = -1;
    s->first = now;
    return s;
}

static void update_ship(ais_t *a, const ais_msg_t *m, double now)
{
    ship_t *s = get_ship(a, m->mmsi, now);
    s->last = now;
    s->msgs++;
    if (m->cls != '?')
        s->cls = m->cls;
    if (m->pos_valid) {
        s->pos = true;
        s->lat = m->lat;
        s->lon = m->lon;
    }
    if (m->sog_kn >= 0)
        s->sog = m->sog_kn;
    if (m->cog_deg >= 0)
        s->cog = m->cog_deg;
    if (m->hdg >= 0)
        s->hdg = m->hdg;
    if (m->navstat >= 0)
        s->navstat = m->navstat;
    if (m->name[0])
        snprintf(s->name, sizeof s->name, "%s", m->name);
    if (m->callsign[0])
        snprintf(s->callsign, sizeof s->callsign, "%s", m->callsign);
    if (m->dest[0])
        snprintf(s->dest, sizeof s->dest, "%s", m->dest);
    if (m->shiptype)
        s->shiptype = m->shiptype;
    if (m->imo)
        s->imo = m->imo;
    if (m->bow || m->stern || m->port || m->stbd) {
        s->bow = m->bow;
        s->stern = m->stern;
        s->port = m->port;
        s->stbd = m->stbd;
    }
    if (m->draught_m > 0)
        s->draught = m->draught_m;
}

static void expire(ais_t *a, double now)
{
    int w = 0;
    for (int i = 0; i < a->nships; i++)
        if (now - a->ships[i].last < SHIP_TTL)
            a->ships[w++] = a->ships[i];
    a->nships = w;
}

static double dist_km(double la1, double lo1, double la2, double lo2, double *brg)
{
    double p1 = la1 * M_PI / 180, p2 = la2 * M_PI / 180, dl = (lo2 - lo1) * M_PI / 180;
    double h = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (brg) {
        double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
        *brg = fmod(atan2(y, x) * 180 / M_PI + 360.0, 360.0);
    }
    return 2 * 6371.0 * asin(sqrt(h));
}

static void ais_state_json(void *st, sb_t *sb, double now)
{
    ais_t *a = st;
    expire(a, now);
    sb_printf(sb, "{\"msgs\":%lu,\"count\":%d,\"ships\":[", a->total, a->nships);
    for (int i = 0; i < a->nships; i++) {
        const ship_t *s = &a->ships[i];
        if (i)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"mmsi\":%u,\"name\":", (unsigned)s->mmsi);
        sb_json_str(sb, s->name);
        sb_puts(sb, ",\"callsign\":");
        sb_json_str(sb, s->callsign);
        sb_printf(sb, ",\"shiptype\":%d,\"shiptype_name\":", s->shiptype);
        sb_json_str(sb, ais_shiptype_name(s->shiptype));
        if (s->imo)
            sb_printf(sb, ",\"imo\":%d", s->imo);
        if (s->pos) {
            sb_puts(sb, ",\"lat\":");
            sb_json_num(sb, s->lat, 5);
            sb_puts(sb, ",\"lon\":");
            sb_json_num(sb, s->lon, 5);
            if (a->loc_valid) {
                double brg;
                double d = dist_km(a->lat, a->lon, s->lat, s->lon, &brg);
                sb_puts(sb, ",\"dist_km\":");
                sb_json_num(sb, d, 1);
                sb_puts(sb, ",\"bearing\":");
                sb_json_num(sb, brg, 0);
            }
        }
        if (s->sog >= 0) {
            sb_puts(sb, ",\"sog\":");
            sb_json_num(sb, s->sog, 1);
        }
        if (s->cog >= 0) {
            sb_puts(sb, ",\"cog\":");
            sb_json_num(sb, s->cog, 1);
        }
        if (s->hdg >= 0)
            sb_printf(sb, ",\"hdg\":%d", s->hdg);
        if (s->navstat >= 0) {
            sb_printf(sb, ",\"navstat\":%d,\"navstat_name\":", s->navstat);
            sb_json_str(sb, ais_navstat_name(s->navstat));
        }
        sb_puts(sb, ",\"dest\":");
        sb_json_str(sb, s->dest);
        sb_printf(sb, ",\"bow\":%d,\"stern\":%d,\"port\":%d,\"stbd\":%d", s->bow, s->stern, s->port, s->stbd);
        sb_puts(sb, ",\"draught\":");
        sb_json_num(sb, s->draught, 1);
        sb_printf(sb, ",\"class\":\"%c\",\"msgs\":%u,\"age\":", s->cls ? s->cls : '?', s->msgs);
        sb_json_num(sb, now - s->last, 0);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

/* ======================================================================= */
/*  Evento                                                                 */
/* ======================================================================= */
static void emit_msg(ais_t *a, const ais_msg_t *m, const uint8_t *bits, int nbits, double now)
{
    char text[200];
    ais_text(m, text, sizeof text);
    sb_t *e = &a->ev;
    sb_clear(e);
    sb_printf(e, "{\"msg_type\":%d,\"repeat\":%d,\"mmsi\":%u,\"class\":\"%c\"", m->type, m->repeat,
              (unsigned)m->mmsi, m->cls ? m->cls : '?');
    if (m->pos_valid) {
        sb_puts(e, ",\"lat\":");
        sb_json_num(e, m->lat, 6);
        sb_puts(e, ",\"lon\":");
        sb_json_num(e, m->lon, 6);
    }
    if (m->sog_kn >= 0) {
        sb_puts(e, ",\"sog\":");
        sb_json_num(e, m->sog_kn, 1);
    }
    if (m->cog_deg >= 0) {
        sb_puts(e, ",\"cog\":");
        sb_json_num(e, m->cog_deg, 1);
    }
    if (m->hdg >= 0)
        sb_printf(e, ",\"hdg\":%d", m->hdg);
    if (m->navstat >= 0) {
        sb_printf(e, ",\"navstat\":%d,\"navstat_name\":", m->navstat);
        sb_json_str(e, ais_navstat_name(m->navstat));
    }
    if (m->type <= 3 || m->type == 18 || m->type == 19)
        sb_printf(e, ",\"second\":%d", m->second);
    if (m->type == 4 || m->type == 11)
        sb_printf(e, ",\"utc\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"", m->year, m->month, m->day, m->hour, m->minute,
                  m->second);
    if (m->name[0]) {
        sb_puts(e, ",\"name\":");
        sb_json_str(e, m->name);
    }
    if (m->callsign[0]) {
        sb_puts(e, ",\"callsign\":");
        sb_json_str(e, m->callsign);
    }
    if (m->type == 5 || m->type == 19 || (m->type == 24 && m->part == 1)) {
        sb_printf(e, ",\"shiptype\":%d,\"shiptype_name\":", m->shiptype);
        sb_json_str(e, ais_shiptype_name(m->shiptype));
        sb_printf(e, ",\"bow\":%d,\"stern\":%d,\"port\":%d,\"stbd\":%d", m->bow, m->stern, m->port, m->stbd);
    }
    if (m->type == 5) {
        sb_printf(e, ",\"imo\":%d,\"eta\":\"%02d-%02d %02d:%02d\",\"draught\":", m->imo, m->eta_mo, m->eta_d,
                  m->eta_h, m->eta_mi);
        sb_json_num(e, m->draught_m, 1);
        sb_puts(e, ",\"dest\":");
        sb_json_str(e, m->dest);
    }
    if (m->type == 21)
        sb_printf(e, ",\"aid_type\":%d", m->aid_type);
    if (m->type == 24)
        sb_printf(e, ",\"part\":\"%c\"", m->part ? 'B' : 'A');
    sb_clear(&a->nm);
    ais_nmea(bits, nbits, a->chan, a->seq++, &a->nm);
    sb_puts(e, ",\"nmea\":");
    sb_json_str(e, a->nm.data);
    sb_puts(e, ",\"text\":");
    sb_json_str(e, text);
    sb_puts(e, "}");
    update_ship(a, m, now);
    a->total++;
    if (a->emit)
        dec_emit_span(a->emit, a->emit_ctx, a->mk0 - a->blk_n0, a->mk1 - a->blk_n0, e->data, e->len);
}

/* FNV-1a de la carga para descartar la misma trama vista desde fases vecinas. */
static uint32_t hash_bits(const uint8_t *b, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h ^ (uint32_t)n;
}

/* Orden de bits real (validado con grabaciones públicas, ver docs/calidad/VALIDACION.md):
 * el HDLC envía cada octeto con el bit menos significativo primero y los campos
 * de ITU-R M.1371 se definen sobre los octetos ya reordenados, es decir, el
 * bit k del flujo recibido es el bit (7 - k%8) del octeto k/8. Sin esta inversion
 * el tipo de un mensaje "1" sale como 8 y los MMSI como 98xxxxxxx. */
void ais_unscramble_bytes(const uint8_t *in, int nbits, uint8_t *out)
{
    for (int g = 0; g < nbits; g += 8) {
        int r = nbits - g < 8 ? nbits - g : 8;
        for (int i = 0; i < r; i++)
            out[g + i] = in[g + r - 1 - i];
    }
}

static void on_frame(ais_t *a, const uint8_t *raw, int nbits, const hdlc_rx_t *hd)
{
    uint8_t bits[MAX_BITS];
    if (nbits > MAX_BITS)
        return;
    ais_unscramble_bytes(raw, nbits, bits);
    uint32_t h = hash_bits(bits, nbits);
    for (int i = 0; i < DEDUP_N; i++)
        if (a->dd[i].h == h && a->n - a->dd[i].at < DEDUP_SAMPLES)
            return;
    ais_msg_t m;
    if (ais_parse(bits, nbits, &m) < 0) {
        a->bad_len++;
        return;
    }
    /* La bandera detectada en el bit con muestra 'pos' empieza 7,5 símbolos antes (8 bits, el
     * último centrado en pos) y la de cierre acaba medio símbolo después. El retardo del
     * discriminador y del FIR de 25 coeficientes (AIS_DELAY muestras) se resta para que el
     * índice sea el de la señal recibida y no el del flujo demodulado. */
    a->mk0 = hd->last_start - (int64_t)(7.5 * SPS) - AIS_DELAY;
    a->mk1 = hd->last_end + SPS / 2 - AIS_DELAY;
    a->dd[a->dd_pos] = (dedup_t){h, a->n};
    a->dd_pos = (a->dd_pos + 1) % DEDUP_N;
    emit_msg(a, &m, bits, nbits, a->blk_now + (double)a->blk_i / 48000.0);
}

/* ======================================================================= */
/*  DSP                                                                    */
/* ======================================================================= */
static void *ais_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    ais_t *a = calloc(1, sizeof *a);
    if (!a) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    fmdisc_init(&a->disc, 48000.0, 2400.0);
    float h[25];
    fir_design_lowpass(h, 25, 6500.0 / 48000.0, 5.0);
    if (fir_f_init(&a->lpf, h, 25, 1) < 0)
        goto fail;
    for (int p = 0; p < SPS; p++)
        if (hdlc_rx_init(&a->ph[p].hdlc, MAX_BITS) < 0)
            goto fail;
    a->ships = calloc(MAX_SHIPS, sizeof(ship_t));
    a->bitbuf = malloc(MAX_BITS);
    if (!a->ships || !a->bitbuf)
        goto fail;
    a->chan = (cfg && cfg->freq_hz >= 162000000LL && cfg->freq_hz < 162100000LL) ? 'B' : 'A';
    if (cfg && cfg->loc_valid) {
        a->loc_valid = true;
        a->lat = cfg->lat;
        a->lon = cfg->lon;
    }
    return a;
fail:
    snprintf(err, errlen, "sin memoria");
    free(a->ships);
    free(a->bitbuf);
    free(a);
    return NULL;
}

static void ais_destroy(void *st)
{
    ais_t *a = st;
    if (!a)
        return;
    for (int p = 0; p < SPS; p++)
        hdlc_rx_free(&a->ph[p].hdlc);
    fir_f_free(&a->lpf);
    free(a->tmp);
    free(a->tmp2);
    free(a->ships);
    free(a->bitbuf);
    sb_free(&a->ev);
    sb_free(&a->nm);
    free(a);
}

static void ais_set_location(void *st, bool valid, double lat, double lon)
{
    ais_t *a = st;
    a->loc_valid = valid;
    a->lat = lat;
    a->lon = lon;
}

static void ais_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    ais_t *a = st;
    if (n > a->tmp_cap) {
        a->tmp_cap = n + 4096;
        a->tmp = realloc(a->tmp, sizeof(float) * (size_t)a->tmp_cap);
        a->tmp2 = realloc(a->tmp2, sizeof(float) * (size_t)a->tmp_cap + 64);
    }
    a->emit = emit;
    a->emit_ctx = ctx;
    a->blk_now = now;
    a->blk_n0 = a->n;
    fmdisc_process(&a->disc, iq, n, a->tmp);
    int m = fir_f_process(&a->lpf, a->tmp, n, a->tmp2);
    for (int i = 0; i < m; i++) {
        float y = a->tmp2[i];
        /* Media ponderada por la potencia instantánea |x|²: el ruido previo a la
         * ráfaga (potencia baja) apenas contamina la estimación del desvío. */
        float pw = iq[i].re * iq[i].re + iq[i].im * iq[i].im;
        float yc = CLAMP(y, -4.0f, 4.0f);
        a->ring_sum += (double)yc * pw - a->ring[a->ring_pos];
        a->ring_w += pw - a->ringw[a->ring_pos];
        a->ring[a->ring_pos] = yc * pw;
        a->ringw[a->ring_pos] = pw;
        if (++a->ring_pos == DC_WIN)
            a->ring_pos = 0;
        float dc = a->ring_w > 1e-12 ? (float)(a->ring_sum / a->ring_w) : 0.0f;
        hdlc_rx_t *h = &a->ph[a->n % SPS].hdlc;
        h->pos = a->n;
        a->n++;
        a->blk_i = i;
        int nb = hdlc_rx_push_level(h, y > dc);
        if (nb > 0) {
            memcpy(a->bitbuf, h->bits, (size_t)nb);
            on_frame(a, a->bitbuf, nb, h);
        }
        if (a->n % (1L << 22) == 0) { /* limita la deriva de las sumas móviles */
            double sy = 0, sw = 0;
            for (int k = 0; k < DC_WIN; k++) {
                sy += a->ring[k];
                sw += a->ringw[k];
            }
            a->ring_sum = sy;
            a->ring_w = sw;
        }
    }
}

const decoder_desc_t DEC_AIS = {
    .id = "ais",
    .name = "AIS",
    .group = "Marítimo",
    .desc = "Sistema de identificación automática de buques (GMSK 9600).",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 25000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {161975000LL, 162025000LL, 0LL, 0LL},
    .create = ais_create,
    .destroy = ais_destroy,
    .process = ais_process,
    .state_json = ais_state_json,
    .set_location = ais_set_location,
};
