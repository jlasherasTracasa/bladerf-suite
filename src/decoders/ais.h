/*
 * ais.h — Capa de carga útil de AIS (ITU-R M.1371): independiente del DSP para
 * poder probarla con sentencias AIVDM publicadas.
 *
 * La carga es un array de bits (uno por byte, 0/1) en orden de transmisión:
 * cada campo se transmite con su bit más significativo primero.
 */
#ifndef BLADERF_LAB_AIS_H
#define BLADERF_LAB_AIS_H

#include "../common.h"
#include "../util/strbuf.h"

typedef struct {
    int type, repeat;
    uint32_t mmsi;
    /* posición (tipos 1-3, 4, 9, 18, 19, 21, 27) */
    bool pos_valid;
    double lat, lon;
    double sog_kn, cog_deg;     /* −1 si no disponible */
    int hdg;                    /* −1 si no disponible */
    int navstat;                /* −1 si no aplica */
    int second;
    /* estáticos (5, 19, 21, 24) */
    char name[21], callsign[8], dest[21], vendor[8];
    int shiptype, imo;
    int bow, stern, port, stbd;
    double draught_m;
    int eta_mo, eta_d, eta_h, eta_mi;
    int aid_type;               /* tipo 21 */
    int part;                   /* tipo 24: 0 = A, 1 = B */
    int year, month, day, hour, minute;   /* tipo 4 */
    char cls;                   /* 'A', 'B' o '?' */
} ais_msg_t;

/* Decodifica; devuelve 0 si el tipo/longitud son válidos y rellena m, −1 si no. */
int ais_parse(const uint8_t *bits, int nbits, ais_msg_t *m);
/* Texto de una sola línea para la interfaz. */
void ais_text(const ais_msg_t *m, char *out, size_t cap);
const char *ais_shiptype_name(int t);
const char *ais_navstat_name(int s);
/* Carga → sentencias !AIVDM separadas por '\n' (con checksum). seq: nº de mensaje
 * secuencial para las multiparte (0..9), chan: 'A' o 'B'. */
void ais_nmea(const uint8_t *bits, int nbits, char chan, int seq, sb_t *out);
/* Inverso: una sentencia !AIVDM/!AIVDO (o el campo de carga) → bits. Devuelve nbits o −1. */
/* Reordena los bits de cada octeto (flujo HDLC <-> campos M.1371). Es su propia inversa. */
void ais_unscramble_bytes(const uint8_t *in, int nbits, uint8_t *out);
int ais_payload_to_bits(const char *payload, int fill, uint8_t *bits, int cap);
/* Comprueba el checksum NMEA de una sentencia completa. */
bool ais_nmea_check(const char *sentence);

#endif
