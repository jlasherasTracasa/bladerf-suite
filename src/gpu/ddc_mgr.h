/*
 * ddc_mgr.h — Gestor del DDC en GPU para el host de decodificadores.
 *
 * Un hilo propio recibe bloques IQ crudos (Q11) del hilo DSP y los pasa por el
 * banco de canales de ddc_gpu. Cada canal entrega sus muestras a la tasa
 * intermedia fs/D mediante un callback; la etapa fina (filtro de canal y
 * remuestreo exacto) la hace después cada instancia en su propio hilo.
 *
 *   hilo DSP ── ddc_mgr_feed() ──► anillo de 4 bloques (memcpy, NO bloquea;
 *                                   si está lleno se descarta y se cuenta)
 *                                      │
 *                          hilo GPU: ddc_gpu_process() ──► callback por canal
 *
 * ddc_mgr_add/remove los llaman los hilos de las instancias y pueden esperar
 * (milisegundos) a que termine el bloque en curso; el hilo DSP nunca espera.
 * Sin CUDA, ddc_mgr_create() devuelve NULL.
 */
#ifndef BLADERF_LAB_DDC_MGR_H
#define BLADERF_LAB_DDC_MGR_H

#include "ddc_gpu.h"

typedef struct ddc_mgr ddc_mgr_t;

/* Salidas de un canal (tasa intermedia). now = hora Unix de la primera. */
typedef void (*ddc_mgr_out_fn)(void *ctx, const cf32 *y, int n, double now, double fs_capture, int64_t center);

ddc_mgr_t *ddc_mgr_create(int max_block, char *err, size_t errlen);
void ddc_mgr_destroy(ddc_mgr_t *m);
const char *ddc_mgr_device_name(ddc_mgr_t *m);
/* Crea un canal. Devuelve su manejador (≥ 0) o −1 con err; *fs_mid = tasa de
 * las salidas. fine: etapa fina ya creada por el llamador (ddc_fine_create). */
int ddc_mgr_add(ddc_mgr_t *m, double fs, int64_t center, int64_t freq, double fs_out, double bw_hz,
                ddc_mgr_out_fn fn, void *ctx, double *fs_mid, char *err, size_t errlen);
void ddc_mgr_remove(ddc_mgr_t *m, int h);
/* Hilo DSP: copia el bloque y vuelve. false si se descartó. */
bool ddc_mgr_feed(ddc_mgr_t *m, const int16_t *iq, unsigned n, double fs, int64_t center, double now);
typedef struct {
    unsigned long blocks, drops;
    double gpu_s;      /* tiempo de GPU acumulado (s) */
    double thread_s;   /* tiempo de CPU del hilo (s) */
    int channels;
} ddc_mgr_stats_t;
void ddc_mgr_stats(ddc_mgr_t *m, ddc_mgr_stats_t *st);

#endif
