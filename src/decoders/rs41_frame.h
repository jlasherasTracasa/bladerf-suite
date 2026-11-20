/*
 * rs41_frame.h — Estructura de la trama de la radiosonda Vaisala RS41 (común
 * al decodificador y al modulador). Ver docs/protocolos/rs41.md.
 *
 * Trama estándar de 320 octetos (4800 Bd → 0,533 s, una por segundo), enviada
 * con cada octeto LSB primero y precedida de un preámbulo de 320 bits 0101…01:
 *
 *   0x000-0x007  cabecera (constante)
 *   0x008-0x037  48 octetos de paridad Reed-Solomon (2 palabras entrelazadas)
 *   0x038        tipo de trama: 0x0F normal, 0xF0 ampliada
 *   0x039-…      bloques: id(1) longitud(1) datos(len) CRC-16(2, LE)
 *
 * Todo lo que va después de la cabecera ya viene aleatorizado: se hace XOR con
 * una máscara pseudoaleatoria de 64 octetos que se repite. La máscara y la
 * cabecera aleatorizada (10 B6 CA 11 22 96 12 F8, que da 86 35 F4 40 93 DF 1A 60
 * al desaleatorizar) son datos públicos (descripción de bazjo/RS41_Decoding).
 *
 * Lo que NO está verificado con una trama real capturada (no hay acceso a una):
 * parámetros y disposición de la paridad Reed-Solomon, y los desplazamientos
 * dentro de los bloques 0x79/0x7A/0x7B/0x7C. Ver docs/protocolos/rs41.md.
 */
#ifndef BLADERF_LAB_RS41_FRAME_H
#define BLADERF_LAB_RS41_FRAME_H

#include "../common.h"

#define RS41_FRAME_LEN 320
#define RS41_HDR_LEN 8
#define RS41_PREAMBLE_BITS 320

extern const uint8_t RS41_MASK[64];
extern const uint8_t RS41_HEADER_TX[8];     /* cabecera tal cual se transmite */

typedef struct {
    bool status_ok, ptu_ok, time_ok, pos_ok;
    int nblocks_ok;
    char serial[9];
    int frame_no;
    double batt_v;
    uint32_t ptu_raw[6];
    int gps_week;
    uint32_t gps_tow_ms;
    double lat, lon, alt_m;     /* WGS84 */
    double speed_ms, heading_deg, climb_ms;
    int sats;
} rs41_info_t;

uint16_t rs41_crc16(const uint8_t *p, int n);   /* CRC-16/CCITT-FALSE */
/* XOR con la máscara (la misma operación aleatoriza y desaleatoriza). */
void rs41_scramble(uint8_t *frame, int n);
/* Recorre los bloques de una trama desaleatorizada y rellena info con los que
 * tienen CRC correcto. Devuelve el nº de bloques válidos. */
int rs41_parse(const uint8_t *plain, int len, rs41_info_t *info);
/* Corrige con Reed-Solomon (2 palabras entrelazadas). Devuelve el nº de octetos
 * corregidos o −1 si alguna palabra no es corregible. Solo tramas de 320. */
int rs41_rs_correct(uint8_t *plain, int len);

void rs41_ecef_to_lla(double x, double y, double z, double *lat_deg, double *lon_deg, double *alt_m);
void rs41_lla_to_ecef(double lat_deg, double lon_deg, double alt_m, double *x, double *y, double *z);

typedef struct {
    char serial[9];
    int frame_no;
    double batt_v;
    uint32_t ptu_raw[6];
    int gps_week;
    uint32_t gps_tow_ms;
    double lat, lon, alt_m;
    double vn_ms, ve_ms, vu_ms;    /* velocidad norte, este y vertical */
    int sats;
    int inject_errors;             /* octetos que se corrompen tras la paridad (prueba de FEC) */
} rs41_params_t;

/* Construye la trama desaleatorizada de 320 octetos. */
void rs41_build(const rs41_params_t *p, uint8_t plain[RS41_FRAME_LEN]);

#endif
