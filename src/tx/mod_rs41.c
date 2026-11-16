/*
 * mod_rs41.c — Modulador de la radiosonda RS41 (GFSK 4800 Bd) a 48 kHz.
 *
 * Genera una trama RS41 completa (preámbulo de 320 bits 0101…, cabecera, paridad
 * Reed-Solomon, bloques de estado/PTU/tiempo GPS/posición) con rs41_build() y la
 * modula: bits NRZ (LSB primero por octeto), mantenidos 10 muestras, suavizados
 * con un filtro gaussiano de BT = 0,5 (Σh = 1) e integrados en fase con
 * desviación ±2,4 kHz (índice h = 1).
 *
 * air_ok = false: la meteorología de radiosondas está en 400,15-406 MHz, fuera
 * de las bandas permitidas (AGENTS.md §6). Solo modos file/loopback.
 *
 * Parámetros JSON (opcionales):
 *   {"serial":"N1234567","frame":120,"lat":40.4,"lon":-3.7,"alt":12500,
 *    "vn":2.0,"ve":-8.0,"vu":5.0,"batt":2.7,"sats":9,"week":2400,
 *    "tow_ms":345600000,"errors":0,"repeat":2}
 * "errors" corrompe ese nº de octetos tras calcular la paridad (prueba de FEC).
 * Cada repetición incrementa el nº de trama y el tiempo GPS en 1 s.
 */
#include "mod.h"

#include "../decoders/rs41_frame.h"
#include "../dsp/pulse.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000.0
#define SPS 10
#define DEV_HZ 2400.0

static int rs41_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    rs41_params_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.serial, sizeof p.serial, "N1234567");
    p.frame_no = 120;
    p.batt_v = 2.7;
    p.gps_week = 2400;
    p.gps_tow_ms = 345600000u;
    p.lat = 40.4168;
    p.lon = -3.7038;
    p.alt_m = 12500;
    p.vn_ms = 2.0;
    p.ve_ms = -8.0;
    p.vu_ms = 5.0;
    p.sats = 9;
    for (int i = 0; i < 6; i++)
        p.ptu_raw[i] = 0x1000u * (uint32_t)(i + 1) + 0x123u;
    int repeat = 2;
    if (params && params[0]) {
        jtok_t t[64];
        int nt = json_parse(params, strlen(params), t, 64);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        double v;
        int i;
        char sn[40];
        if ((i = json_obj_get(params, t, 0, "serial")) >= 0 && json_str(params, t, i, sn, sizeof sn))
            snprintf(p.serial, sizeof p.serial, "%.8s", sn);
        if ((i = json_obj_get(params, t, 0, "frame")) >= 0 && json_num(params, t, i, &v))
            p.frame_no = (int)CLAMP(v, 0, 65535);
        if ((i = json_obj_get(params, t, 0, "lat")) >= 0 && json_num(params, t, i, &v))
            p.lat = CLAMP(v, -85, 85);
        if ((i = json_obj_get(params, t, 0, "lon")) >= 0 && json_num(params, t, i, &v))
            p.lon = CLAMP(v, -180, 180);
        if ((i = json_obj_get(params, t, 0, "alt")) >= 0 && json_num(params, t, i, &v))
            p.alt_m = CLAMP(v, -400, 40000);
        if ((i = json_obj_get(params, t, 0, "vn")) >= 0 && json_num(params, t, i, &v))
            p.vn_ms = CLAMP(v, -300, 300);
        if ((i = json_obj_get(params, t, 0, "ve")) >= 0 && json_num(params, t, i, &v))
            p.ve_ms = CLAMP(v, -300, 300);
        if ((i = json_obj_get(params, t, 0, "vu")) >= 0 && json_num(params, t, i, &v))
            p.vu_ms = CLAMP(v, -300, 300);
        if ((i = json_obj_get(params, t, 0, "batt")) >= 0 && json_num(params, t, i, &v))
            p.batt_v = CLAMP(v, 0, 25);
        if ((i = json_obj_get(params, t, 0, "sats")) >= 0 && json_num(params, t, i, &v))
            p.sats = (int)CLAMP(v, 0, 24);
        if ((i = json_obj_get(params, t, 0, "week")) >= 0 && json_num(params, t, i, &v))
            p.gps_week = (int)CLAMP(v, 0, 65535);
        if ((i = json_obj_get(params, t, 0, "tow_ms")) >= 0 && json_num(params, t, i, &v))
            p.gps_tow_ms = (uint32_t)CLAMP(v, 0, 604800000.0);
        if ((i = json_obj_get(params, t, 0, "errors")) >= 0 && json_num(params, t, i, &v))
            p.inject_errors = (int)CLAMP(v, 0, 40);
        if ((i = json_obj_get(params, t, 0, "repeat")) >= 0 && json_num(params, t, i, &v))
            repeat = (int)CLAMP(v, 1, 20);
    }
    out->rate = RATE;
    const int span = 3, nh = pulse_ntaps(SPS, span);
    float *h = malloc(sizeof(float) * (size_t)nh);
    if (!h) {
        snprintf(err, errlen, "sin memoria");
        return -1;
    }
    pulse_gaussian(h, 0.5, SPS, span, PULSE_NORM_DC);
    double phase = 0.7;
    iqbuf_reserve(out, 2400);
    memset(out->s + out->n, 0, sizeof(cf32) * 2400);
    out->n += 2400;
    for (int r = 0; r < repeat; r++) {
        uint8_t plain[RS41_FRAME_LEN], tx[RS41_FRAME_LEN];
        rs41_build(&p, plain);
        memcpy(tx, plain, sizeof tx);
        rs41_scramble(tx, RS41_FRAME_LEN);   /* para el aire se aleatoriza */
        for (int i = 0; i < 8; i++)
            tx[i] = RS41_HEADER_TX[i];
        int nbits = RS41_PREAMBLE_BITS + RS41_FRAME_LEN * 8;
        float *nrz = malloc(sizeof(float) * (size_t)(nbits * SPS + 2 * nh));
        if (!nrz) {
            free(h);
            snprintf(err, errlen, "sin memoria");
            return -1;
        }
        int ns = 0;
        for (int b = 0; b < nbits; b++) {
            int bit;
            if (b < RS41_PREAMBLE_BITS)
                bit = b & 1;   /* 0101…01: acaba en 1 */
            else {
                int q = b - RS41_PREAMBLE_BITS;
                bit = (tx[q >> 3] >> (q & 7)) & 1;
            }
            for (int k = 0; k < SPS; k++)
                nrz[ns++] = bit ? 1.f : -1.f;
        }
        int half = nh / 2;
        iqbuf_reserve(out, ns + 2400);
        for (int i = 0; i < ns; i++) {
            float f = 0;   /* convolución con el pulso gaussiano (Σh = 1) */
            for (int k = 0; k < nh; k++) {
                int j = i + half - k;
                f += h[k] * nrz[j < 0 ? 0 : (j >= ns ? ns - 1 : j)];
            }
            phase += 2.0 * M_PI * DEV_HZ * f / RATE;
            iqbuf_push(out, (cf32){(float)(0.8 * cos(phase)), (float)(0.8 * sin(phase))});
        }
        free(nrz);
        /* hueco hasta completar 1 s */
        int gap = (int)RATE - ns;
        iqbuf_reserve(out, gap > 0 ? gap : 0);
        for (int i = 0; i < gap; i++)
            iqbuf_push(out, (cf32){0, 0});
        p.frame_no++;
        p.gps_tow_ms += 1000;
        p.alt_m += p.vu_ms;
    }
    free(h);
    return 0;
}

const modulator_desc_t MOD_RS41 = {
    .id = "rs41",
    .name = "Radiosonda RS41",
    .air_ok = false,
    .modulate = rs41_modulate,
};
