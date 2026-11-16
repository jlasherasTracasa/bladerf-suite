/*
 * ism_protocols.h — Trenes de pulsos, protocolos de sensores ISM y analizador
 * genérico (estilo Universal Radio Hacker). Ver docs/decoders/ism.md.
 *
 * Un "tren de pulsos" es la representación común a OOK y FSK: la lista
 * alternada de duraciones (µs) de nivel alto ("marca") y nivel bajo
 * ("espacio"), empezando SIEMPRE por una marca (d[0] puede ser 0 si la señal
 * empezó en bajo). En OOK la marca es portadora presente; en FSK es la
 * frecuencia por encima de la media de la ráfaga.
 *
 * Sobre esa lista trabajan:
 *   - los decodificadores de protocolos concretos (ism_decode_train), que
 *     validan suma de comprobación / CRC antes de dar un resultado;
 *   - el analizador genérico (ism_analyze), que infiere la modulación de
 *     bits (PWM / PPM / Manchester / NRZ) agrupando longitudes de pulso;
 *   - los codificadores (ism_encode, ism_train_*) que usan el modulador y los
 *     tests. Los codificadores de TPMS, mandos, alarmas… NO existen aquí a
 *     propósito (AGENTS.md: el modulador solo emite sensores meteorológicos y
 *     la trama de prueba propia); los tests los construyen localmente.
 */
#ifndef BLADERF_LAB_ISM_PROTOCOLS_H
#define BLADERF_LAB_ISM_PROTOCOLS_H

#include "../common.h"

#define ISM_MAXD 4096   /* duraciones por ráfaga */

typedef struct {
    uint32_t d[ISM_MAXD]; /* µs; índices pares = marca, impares = espacio */
    int n;
} ism_train_t;

enum {
    ISM_HAS_TEMP = 1, ISM_HAS_HUM = 2, ISM_HAS_BATT = 4, ISM_HAS_WIND = 8,
    ISM_HAS_RAIN = 16, ISM_HAS_PRESS = 32, ISM_HAS_DIR = 64
};

typedef struct {
    const char *model;
    uint32_t id;
    int channel;           /* 0 = sin canal */
    unsigned has;          /* máscara ISM_HAS_* */
    double temp_c, humidity, wind_avg_ms, wind_gust_ms, rain_mm, pressure_kpa;
    int wind_dir_deg;
    bool batt_low;
    char hex[48];          /* octetos de la trama validada */
} ism_msg_t;

typedef struct {
    char mod[24];          /* "OOK-PWM", "OOK-PPM", "Manchester", "NRZ/PCM"… */
    double short_us, long_us, gap_us, bit_us;
    int nbits, repeats, nrows;
    char hex[130];         /* bits de la fila más repetida, MSB primero */
} ism_generic_t;

/* ---- Construcción de trenes ------------------------------------------- */
void ism_train_clear(ism_train_t *t);
void ism_train_pair(ism_train_t *t, uint32_t mark_us, uint32_t space_us);
/* Añade (o alarga) un espacio al final. */
void ism_train_gap(ism_train_t *t, uint32_t space_us);
/* Manchester: bit 1 = alto→bajo (inv=0) o al revés (inv=1); half_us = medio bit. */
void ism_train_manch(ism_train_t *t, const uint8_t *bits, int nbits, uint32_t half_us, int inv);
/* Utilidades de bits/CRC compartidas con los tests. */
int ism_put_bits(uint8_t *bits, int pos, uint32_t v, int nb);   /* MSB primero; devuelve pos+nb */
uint8_t ism_crc8(const uint8_t *p, int n, uint8_t poly, uint8_t init);
uint8_t ism_lfsr_digest8(const uint8_t *p, int n, uint8_t gen, uint8_t key);

/* ---- Decodificación ---------------------------------------------------- */
/* Devuelve el nº de mensajes con checksum válido (≤ max). */
int ism_decode_train(const ism_train_t *t, bool fsk, ism_msg_t *out, int max);
bool ism_analyze(const ism_train_t *t, bool fsk, ism_generic_t *g);

/* ---- Codificación (solo meteorología + trama de prueba) --------------- */
extern const char *const ISM_TX_MODELS[];   /* modelos que el modulador puede emitir */
/* m->model selecciona el formato. repeat = nº de filas/paquetes. <0 si no se permite. */
int ism_encode(const ism_msg_t *m, int repeat, ism_train_t *t);
/* Trama de prueba de BladeRF Lab: 'counter' y 4 octetos de carga. */
void ism_encode_test(uint16_t counter, const uint8_t payload[4], int repeat, ism_train_t *t);

/* ---- Renderizado a IQ (banda base) ------------------------------------ */
/* Añade al final de *buf (cf32 malloc'ado, crece) el tren: OOK (amplitud amp) o
 * FSK (±dev_hz, fase continua, *phase se conserva entre llamadas). */
void ism_render(cf32 **buf, int *n, int *cap, const ism_train_t *t, bool fsk, double fs, double amp,
                double dev_hz, double *phase);
void ism_render_silence(cf32 **buf, int *n, int *cap, int nsamples);

#endif
