/*
 * vec_real.h — Carga de vectores IQ reales (fragmentos pequeños de grabaciones
 * públicas, guardados en tests/data/) para los tests de los decodificadores.
 *
 *   vr_load_cu8(ruta, &n)    IQ de 8 bits sin signo (RTL-SDR) → cf32 normalizado
 *   vr_load_cs8(ruta, &n)    IQ de 8 bit con signo → cf32 (±1 = 127)
 *   vr_load_ci16(ruta, &n)   IQ int16 little-endian → cf32 (±1 = 32768) 
 *   vr_load_q11(ruta, &n)    IQ int16 little-endian tal cual (Q11 crudo de los WIDE)
 *   VR_SAN                   1 si se compila con AddressSanitizer/UBSan
 *   VR_CPU(x)                umbral de coste de CPU: ×8 bajo sanitizer, porque la
 *                            instrumentación multiplica el tiempo de las FFT y los
 *                            bucles estrechos sin que cambie el algoritmo.
 *
 * Si el fichero no existe (p. ej. se ejecuta el test desde otro directorio) las
 * funciones devuelven NULL y el test lo anota sin fallar: los vectores se
 * buscan en tests/data/ relativo al directorio del repositorio.
 */
#ifndef BLADERF_LAB_VEC_REAL_H
#define BLADERF_LAB_VEC_REAL_H

#include "common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__SANITIZE_ADDRESS__)
#define VR_SAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define VR_SAN 1
#endif
#endif
#ifndef VR_SAN
#define VR_SAN 0
#endif
#define VR_CPU(x) ((x) * (VR_SAN ? 8.0 : 1.0))

static inline uint8_t *vr_read(const char *path, long *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *bytes = sz;
    return b;
}

static inline cf32 *vr_load_cu8(const char *path, int *n)
{
    long sz;
    uint8_t *b = vr_read(path, &sz);
    if (!b)
        return NULL;
    *n = (int)(sz / 2);
    cf32 *x = malloc(sizeof(cf32) * (size_t)(*n + 1));
    for (int i = 0; i < *n; i++) {
        x[i].re = ((float)b[2 * i] - 127.5f) / 127.5f;
        x[i].im = ((float)b[2 * i + 1] - 127.5f) / 127.5f;
    }
    free(b);
    return x;
}

/* IQ de 8 bit con signo (cs8) → cf32 (±1 = 127) */
static inline cf32 *vr_load_cs8(const char *path, int *n)
{
    long sz;
    uint8_t *b = vr_read(path, &sz);
    if (!b)
        return NULL;
    *n = (int)(sz / 2);
    cf32 *x = malloc(sizeof(cf32) * (size_t)(*n + 1));
    for (int i = 0; i < *n; i++) {
        x[i].re = (float)(int8_t)b[2 * i] / 127.0f;
        x[i].im = (float)(int8_t)b[2 * i + 1] / 127.0f;
    }
    free(b);
    return x;
}

static inline int16_t *vr_load_q11(const char *path, int *n)
{
    long sz;
    uint8_t *b = vr_read(path, &sz);
    if (!b)
        return NULL;
    *n = (int)(sz / 4);
    return (int16_t *)b;   /* little-endian nativo (x86) */
}

static inline cf32 *vr_load_ci16(const char *path, int *n)
{
    int16_t *q = vr_load_q11(path, n);
    if (!q)
        return NULL;
    cf32 *x = malloc(sizeof(cf32) * (size_t)(*n + 1));
    for (int i = 0; i < *n; i++) {
        x[i].re = (float)q[2 * i] / 32768.0f;
        x[i].im = (float)q[2 * i + 1] / 32768.0f;
    }
    free(q);
    return x;
}

#endif
