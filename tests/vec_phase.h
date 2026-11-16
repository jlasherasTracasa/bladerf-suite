/*
 * vec_phase.h — Vectores reales de FM (grupo A de validación, V1).
 *
 * Una señal FM de envolvente constante se guarda solo con su FASE: un byte por
 * muestra a 48 kHz (256 niveles por vuelta, error de cuantización < 0,7°), de modo
 * que 4 s de señal ocupan < 200 KB. vp_load_phase() reconstruye exp(j·2π·p/256).
 * Los ficheros salen de audio de un receptor FM remodulado en FM (ver cada test
 * para la fuente y la licencia); build/v1/tools/mkphase.py los genera.
 * Si el fichero no existe (test lanzado desde otro directorio) devuelve NULL y el
 * test lo anota sin fallar.
 */
#ifndef BLADERF_LAB_VEC_PHASE_H
#define BLADERF_LAB_VEC_PHASE_H

#include "common.h"

#include <stdio.h>
#include <stdlib.h>

static inline cf32 *vp_load_phase(const char *path, int *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    cf32 *z = malloc(sizeof(cf32) * (size_t)sz);
    if (!b || !z || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        free(z);
        return NULL;
    }
    fclose(f);
    for (long i = 0; i < sz; i++) {
        double a = 2.0 * M_PI * b[i] / 256.0;
        z[i] = (cf32){(float)(0.8 * cos(a)), (float)(0.8 * sin(a))};
    }
    free(b);
    *n = (int)sz;
    return z;
}

#endif
