/*
 * recorder.h — Grabación de IQ crudo en formato SigMF.
 *
 * SigMF (Signal Metadata Format, https://sigmf.org) es el estándar abierto
 * para guardar grabaciones de radio: un fichero binario con las muestras
 * (.sigmf-data) y otro JSON con los metadatos (.sigmf-meta): tipo de dato,
 * tasa de muestreo, frecuencia, fecha, equipo… Cualquier herramienta que
 * entienda SigMF puede abrir las grabaciones, y esta misma aplicación las
 * reproduce con --replay.
 *
 * Arquitectura: el hilo DSP no puede esperar al disco (perdería muestras),
 * así que copia los datos a un búfer circular grande y un hilo escritor
 * independiente los vuelca al fichero. Si el disco no da abasto, se
 * descartan bloques y se contabilizan en "dropped".
 */
#ifndef BLADERF_LAB_RECORDER_H
#define BLADERF_LAB_RECORDER_H

#include "../common.h"

typedef struct recorder recorder_t;

typedef struct {
    bool active;
    char name[160];        /* nombre base del fichero (sin extensión) */
    unsigned long long bytes;
    double seconds;
    unsigned long long dropped; /* muestras perdidas por disco lento */
} recorder_status_t;

recorder_t *recorder_create(const char *dir);
void recorder_destroy(recorder_t *r);
/* Empieza a grabar. hw: descripción del equipo para los metadatos. */
int recorder_start(recorder_t *r, uint32_t rate, int64_t freq_hz, const char *hw, uint64_t max_bytes, char *err,
                   size_t errlen);
void recorder_stop(recorder_t *r);
/* Llamado desde el hilo DSP: encola n muestras IQ (Q11). No bloquea. */
void recorder_push(recorder_t *r, const int16_t *iq, unsigned n);
void recorder_status(recorder_t *r, recorder_status_t *st);
/* Borra una grabación (los dos ficheros SigMF o el fichero dado). */
int recorder_delete(recorder_t *r, const char *name);

#endif
