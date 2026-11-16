/*
 * mod_dvbt.h — Modulador DVB-T completo (TS → señal OFDM) para pruebas de extremo a
 * extremo. La interfaz de catálogo (MOD_DVBT) está en mod_dvbt.c; aquí la de más
 * bajo nivel que usan los tests y el banco de pruebas del receptor.
 */
#ifndef BLADERF_LAB_MOD_DVBT_H
#define BLADERF_LAB_MOD_DVBT_H

#include "mod.h"

typedef struct {
    int n;              /* 2048 | 8192 */
    int guard;          /* 0..3 = 1/32, 1/16, 1/8, 1/4 */
    int constellation;  /* 0 QPSK, 1 16-QAM, 2 64-QAM */
    int hierarchy;      /* campo TPS: 0 ninguna, 1 α=1, 2 α=2, 3 α=4 */
    int hp, lp;         /* tasas 0..4 = 1/2 2/3 3/4 5/6 7/8 */
    int cell_id;        /* < 0: sin cell id (longitud 23) */
    int frames;         /* nº de tramas de 68 símbolos */
    int frame0;         /* nº de trama del primer símbolo (0..3) */
    float peak;         /* pico tras normalizar (0 = 0,85) */
    cf32 *dbg_data;     /* opcional: portadoras de datos de cada símbolo (frames·68·ndata) */
} dvbt_mod_cfg_t;

/* Rellena una configuración por defecto: 8k, guarda 1/4, 64-QAM 2/3, sin jerarquía. */
void dvbt_mod_defaults(dvbt_mod_cfg_t *c);

/* Modula. ts_hp: npkt_hp paquetes de 188 B (con 0x47) que se repiten cíclicamente; ts_lp
 * solo si hay jerarquía (si es NULL, el LP lleva el mismo TS). La salida va a 64/7 MHz con
 * el pico normalizado. Devuelve 0 o <0 con texto en err. */
int dvbt_mod_run(const dvbt_mod_cfg_t *c, const uint8_t *ts_hp, int npkt_hp, const uint8_t *ts_lp, int npkt_lp,
                 iqbuf_t *out, char *err, size_t errlen);

/* TS de demostración: PAT, NIT, SDT (con nombre de servicio), PMT y relleno nulo. Rellena
 * npkt paquetes de 188 B (la tasa de repetición de las tablas es la típica). */
void dvbt_demo_ts(uint8_t *out, int npkt, const char *service_name);

#endif
