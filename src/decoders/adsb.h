/*
 * adsb.h — Decodificador ADS-B / Mode S a 1090 MHz, escrito desde cero.
 *
 * Los aviones (y algunos vehículos de aeropuerto) emiten en 1090 MHz tramas
 * de 56 o 112 bits con modulación por posición de pulso (PPM): cada bit dura
 * 1 µs y vale 1 si el pulso está en la primera mitad, 0 si está en la segunda.
 * Cada trama empieza con un preámbulo de 4 pulsos (0, 1, 3,5 y 4,5 µs).
 *
 * Con el bladeRF a 2 Msps cada medio microsegundo es exactamente una muestra,
 * así que:
 *   - el preámbulo son las muestras 0, 2, 7 y 9 "altas";
 *   - el bit i se decide comparando las muestras 16+2i y 17+2i.
 *
 * Tras detectar una trama se comprueba su CRC-24 (polinomio 0xFFF409). Las
 * tramas DF17/18 con 1 o 2 bits erróneos se corrigen buscando el síndrome
 * solo entre los bits MENOS FIABLES (confianza por bit de la demodulación
 * PPM, Landry et al., Applied Sciences 13(1):493), y solo si la aeronave ya
 * es conocida. Opcionalmente (opts {"crcframe":true}) se busca además por
 * CRC deslizante, sin preámbulo (Stream1090), para recuperar tramas solapadas.
 *
 * Se decodifican:
 *   DF17/18 ADS-B: identificación (indicativo, categoría), posición en vuelo
 *                  y en superficie (CPR), altitud barométrica y GNSS,
 *                  velocidad sobre el suelo / aérea, régimen vertical,
 *                  estado de emergencia y código transpondedor.
 *   DF11:          respuestas "all-call" (descubre direcciones ICAO).
 *   DF0/4/16/20:   altitud (dirección ICAO recuperada del CRC).
 *   DF5/21:        código transpondedor (squawk).
 *
 * Referencia principal: Junzi Sun, "The 1090 Megahertz Riddle: A Guide to
 * Decoding Mode S and ADS-B Signals", 2ª ed., TU Delft OPEN Publishing, 2021.
 * Ver docs/protocolos/adsb.md para la explicación detallada.
 */
#ifndef BLADERF_LAB_ADSB_H
#define BLADERF_LAB_ADSB_H

#include "../common.h"
#include "../util/strbuf.h"
#include "decoder.h"

#define ADSB_RATE 2000000 /* el decodificador necesita exactamente 2 Msps */

#define ADSB_TRAIL_N 24    /* puntos de pista por aeronave (acotado en memoria) */
#define ADSB_TRAIL_DT 8.0  /* separación mínima entre puntos (s) */
enum { ADSB_POS_NONE = 0, ADSB_POS_GLOBAL = 1, ADSB_POS_LOCAL = 2 };

typedef struct {
    uint32_t icao;
    char callsign[9];
    char category[3];       /* "A3", "B1"... o "" */
    int altitude_ft;        /* barométrica */
    int altitude_gnss_ft;
    int squawk;             /* en decimal leyendo los 4 dígitos octales: 7700 → 7700 */
    int vrate_fpm;
    float gs_kt;            /* velocidad sobre el suelo */
    float track_deg;        /* rumbo sobre el suelo */
    float ias_kt;           /* velocidad aérea indicada/verdadera */
    float heading_deg;      /* rumbo magnético */
    double lat, lon;
    int emergency;          /* 0 = ninguna; 1..6 según DO-260B */
    bool alt_valid, gnss_valid, squawk_valid, vr_valid, vel_valid, air_valid, pos_valid, on_ground;
    /* Último par de tramas CPR (0 = par, 1 = impar) */
    int cpr_lat[2], cpr_lon[2];
    double cpr_time[2];
    bool cpr_surface[2];
    double pos_time;        /* instante de la última posición válida */
    int pos_src;            /* origen de la posición: ADSB_POS_NONE/GLOBAL/LOCAL */
    /* Pista: últimos puntos aceptados (anillo, espaciados >= ADSB_TRAIL_DT s) */
    struct { double t, lat, lon; int alt_ft; bool alt_ok; } trail[ADSB_TRAIL_N];
    int trail_n, trail_head;
    double first_seen, last_seen;
    unsigned msgs;
    float rssi_db;          /* nivel medio suavizado de las tramas (dBFS) */
    bool adsb;              /* visto en tramas ADS-B (no solo Mode S) */
    /* Seguimiento de lo ya anunciado como evento (cambios de estado) */
    bool announced;
    char ev_callsign[9];
    int ev_squawk, ev_emerg;
} aircraft_t;

/* Opciones del decodificador (JSON de dec_cfg_t.opts: "raw", "crcframe", "fix"). */
typedef struct {
    int fix_max;     /* bits corregibles en DF17/18: 0, 1 o 2 (por defecto 2) */
    bool crcframe;   /* entramado por CRC deslizante (por defecto no) */
    bool raw;        /* un evento por trama con "hex" (por defecto: solo cambios) */
} adsb_opts_t;

/* Contadores acumulados desde adsb_create(). */
typedef struct {
    unsigned long frames;   /* tramas válidas aceptadas */
    unsigned long fix1;     /* de ellas, corregidas de 1 bit */
    unsigned long fix2;     /* corregidas de 2 bits */
    unsigned long crc_rec;  /* recuperadas por el entramado CRC deslizante */
    unsigned long rejected; /* candidatos DF17/18 con CRC erróneo e incorregible */
    float fps;              /* tramas válidas por segundo (ventana de 2 s) */
} adsb_stats_t;

typedef struct adsb adsb_t;

adsb_t *adsb_create(void);
void adsb_destroy(adsb_t *a);
void adsb_reset(adsb_t *a);
/* Posición del receptor: mejora el decodificado y permite calcular distancias. */
void adsb_set_receiver(adsb_t *a, bool valid, double lat, double lon);
void adsb_set_options(adsb_t *a, const adsb_opts_t *o);
void adsb_get_stats(const adsb_t *a, adsb_stats_t *st);
/* Procesa muestras IQ crudas (Q11) a 2 Msps. now: hora Unix en segundos. */
void adsb_process(adsb_t *a, const int16_t *iq, int n, double now);
/* Decodifica una trama dada en hexadecimal (pruebas y depuración).
 * Devuelve el DF si el CRC es válido, −1 si no. */
int adsb_decode_hex(adsb_t *a, const char *hex, double now);
/* Serializa la lista de aeronaves como JSON (mensaje "adsb", formato antiguo). */
void adsb_json(adsb_t *a, sb_t *sb, double now);
/* Igual pero sin el campo "type": es el objeto que devuelve state_json() del
 * decodificador {"msgs","rate","rx","ac":[...]} (mensaje "dec_state"). */
void adsb_state_json(adsb_t *a, sb_t *sb, double now);
/* Función a la que se notifica cada trama DF17/18 válida (JSON de evento). Se
 * invoca desde dentro de adsb_process()/adsb_decode_hex(). NULL la desactiva. */
void adsb_set_emit(adsb_t *a, dec_emit_fn fn, void *ctx);

/* Descriptor del marco de decodificadores (decoder.h): WIDE, 2 Msps exactos. */
extern const decoder_desc_t DEC_ADSB;
const aircraft_t *adsb_find(adsb_t *a, uint32_t icao);
unsigned long adsb_total_msgs(const adsb_t *a);

/* Expuestas para las pruebas unitarias. */
uint32_t modes_crc24(const uint8_t *msg, int nbits_data);
int cpr_nl(double lat);

#endif
