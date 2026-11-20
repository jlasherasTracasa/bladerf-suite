/*
 * dvbt_vit.h — Decodificador de Viterbi K=7 (171,133 octal), tasa 1/2, de decisión
 * blanda, continuo (sin terminar) y rápido: mariposa con AVX2 (16 estados por registro,
 * métricas de 16 bit saturadas) y recorrido hacia atrás por bloques.
 *
 * Teoría. Con el registro de 7 bits r = (u_t, u_{t−1}, …, u_{t−6}) y estado s = los 6
 * bits más antiguos (el nuevo bit entra por el bit 5), las dos ramas que llegan al
 * estado j + 32·b salen de 2j y 2j+1. Como los dos polinomios (0x79, 0x5B) tienen
 * tomas en el bit más reciente y en el más antiguo, cambiar uno de esos bits invierte
 * las dos salidas: la métrica de rama de (2j+1) es la opuesta de la de 2j, y la de
 * b = 1 también. Por eso una mariposa solo necesita M_j = ±s0 ± s1:
 *     new[j]    = máx(pm[2j] + M_j, pm[2j+1] − M_j)
 *     new[j+32] = máx(pm[2j] − M_j, pm[2j+1] + M_j)
 * Convención soft: positivo = más probable el bit 0; 0 = borrado (perforado).
 *
 * Tasa medida: ver docs/protocolos/dvbt.md.
 */
#ifndef BLADERF_LAB_DVBT_VIT_H
#define BLADERF_LAB_DVBT_VIT_H

#include <stdint.h>

typedef struct dvbt_vit dvbt_vit_t;

dvbt_vit_t *dvbt_vit_create(void);
void dvbt_vit_destroy(dvbt_vit_t *v);
void dvbt_vit_reset(dvbt_vit_t *v);

/* Procesa nsteps pasos (2 valores blandos int8 por paso: X, Y). Escribe en 'out' los
 * bytes (MSB primero) de los bloques de 128 bits que ya se pueden decidir y devuelve
 * cuántos bytes (múltiplo de 16). 'out' debe tener sitio para nsteps/8 + 32 bytes. */
int dvbt_vit_push(dvbt_vit_t *v, const int8_t *soft, int nsteps, uint8_t *out);

#endif
