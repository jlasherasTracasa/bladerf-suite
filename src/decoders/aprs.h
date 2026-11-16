/*
 * aprs.h — Capa AX.25 / APRS (APRS Protocol Reference 1.0.1, AX.25 v2.2),
 * independiente del DSP para poder probarla con paquetes publicados.
 */
#ifndef BLADERF_LAB_APRS_H
#define BLADERF_LAB_APRS_H

#include "../common.h"

typedef struct {
    char src[16], dst[16], path[120];
    char kind[16];        /* position, mic-e, status, message, object, item, weather, telemetry, other */
    char info[260];       /* campo de información con los no imprimibles como '.' */
    bool pos_valid;
    double lat, lon;
    char symtab, symcode;
    bool has_speed, has_course, has_alt;
    double speed_kn, course_deg, alt_m;
    char comment[200];
    char obj_name[12];
    bool obj_live;
    char to[12], msg[100], msg_id[10];   /* mensajes */
    char status[100];
    char mice_msg[20];                    /* Mic-E: "En Route", "Emergency"... */
    /* meteorología (NAN = ausente) */
    bool wx;
    double wind_dir, wind_mph, gust_mph, temp_f, rain1h_in, rain24h_in, rain_mid_in, hum_pct, baro_hpa;
} aprs_pkt_t;

/* Trama AX.25 (sin FCS) → paquete. 0 si es una trama UI/APRS válida, −1 si no. */
int aprs_parse_frame(const uint8_t *f, int n, aprs_pkt_t *p);
/* Solo la parte APRS: destino + campo de información (para pruebas). */
void aprs_parse_info(const char *dst, const uint8_t *info, int n, aprs_pkt_t *p);
void aprs_text(const aprs_pkt_t *p, char *out, size_t cap);

/* Construcción de una trama AX.25 UI: devuelve el nº de bytes (sin FCS) o −1.
 * path: "WIDE1-1,WIDE2-2" o NULL. */
int ax25_build_ui(const char *src, const char *dst, const char *path, const uint8_t *info, int ninfo, uint8_t *out, int cap);
/* Indicativo válido: 1-6 caracteres A-Z/0-9 y -SSID opcional (0-15). */
bool ax25_callsign_ok(const char *cs);

#endif
