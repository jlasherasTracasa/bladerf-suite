/*
 * dsc.h — Capa de mensaje DSC (ITU-R M.493 / M.541): símbolos de 10 bits y
 * análisis de la secuencia de llamada. Independiente del DSP, para pruebas.
 */
#ifndef BLADERF_LAB_DSC_H
#define BLADERF_LAB_DSC_H

#include "../common.h"

#define DSC_MAX_SYM 96

typedef struct {
    int fs;                       /* especificador de formato (100..127) */
    int category;                 /* −1 si no lleva */
    char to[16], from[16];        /* MMSI (9 dígitos) o dígitos del área/grupo */
    int nature;                   /* naturaleza del socorro, −1 si no */
    bool pos_valid;
    double lat, lon;
    bool time_valid;
    int hour, minute;
    int tc1, tc2;                 /* telemandos, −1 si no */
    int subsequent;               /* tipo de comunicación subsiguiente (socorro) */
    int eos;
    int ext[16], next;            /* símbolos de extensión (canal/frecuencia) */
    char dist_mmsi[16];           /* socorro retransmitido/acusado */
} dsc_msg_t;

/* Símbolo de 10 bits (en orden de transmisión: 7 de información LSB primero y 3
 * de comprobación) → valor 0..127 o −1 si el recuento de ceros no cuadra. */
int dsc_sym_decode(const uint8_t *bits10);
void dsc_sym_encode(int v, uint8_t *bits10);

/* sym[0] = especificador de formato … sym[n−2] = EOS, sym[n−1] = ECC (ya validado).
 * Devuelve 0 si la estructura es coherente. */
int dsc_parse_message(const int *sym, int n, dsc_msg_t *m);
const char *dsc_format_name(int fs);
const char *dsc_category_name(int c);
const char *dsc_nature_name(int n);
const char *dsc_telecommand_name(int t);
/* ECC: XOR de los símbolos desde el especificador de formato hasta el EOS inclusive. */
int dsc_ecc(const int *sym, int n_through_eos);

#endif
