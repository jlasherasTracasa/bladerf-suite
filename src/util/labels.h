/*
 * labels.h — Exportación de etiquetas en tres niveles a SigMF (--replay --labels).
 *
 * Objetivo: que cada región tiempo-frecuencia de una grabación quede clasificada según
 * lo que se sabe de ella con certeza, para construir un conjunto de datos etiquetado
 * (PLAN.md §5: etiquetado en tres niveles calibrado por inyección):
 *
 *   verified  Un decodificador entregó un mensaje con CRC/FEC correctos. Es la única
 *             clase con verdad de terreno: protocolo, frecuencia, muestra de inicio y fin,
 *             SNR y nivel medidos en el canal. Una anotación por evento.
 *   unknown   El detector de energía (dsp/detector.h, OS-CFAR) marca una señal sostenida
 *             que NINGÚN evento verificado explica: puede ser un protocolo no soportado,
 *             un mensaje corrupto o una interferencia. Se anota con su tramo de tiempo y
 *             su banda. Se excluyen los tramos cubiertos (con un margen >= 1 s, la huella del detector tras
 *             una ráfaga: tau·(SNR_pico-6 dB)/8,69 s; tau = 1 s, constante de tiempo del
 *             detector) por eventos verificados cuya banda solape la de la señal.
 *   empty     NO se escribe: es el complemento. Toda región tiempo-frecuencia de la
 *             captura sin anotación `verified` ni `unknown` se considera vacía (solo
 *             ruido a la sensibilidad del detector). Se declara en "global" con
 *             lab:empty_convention para que quien lea el fichero no tenga que adivinarlo.
 *   gap       (extra) Tramo que una instancia de decodificador no llegó a mirar (cola llena
 *             o anillo del motor): ahí un `empty` implícito no es fiable. Con --replay-fast
 *             no debería haber ninguno; si lo hay, el etiquetador los excluye.
 *
 * Resolución de las regiones `unknown`: el detector da una imagen cada 0,5 s de señal y
 * tiene histéresis (DET_N_ON apariciones, DET_M_OFF ausencias), así que el inicio y el fin
 * de una región tienen una incertidumbre de ±1 s; se corrige el sesgo medio de la histéresis
 * pero no es precisión de muestra. Los `verified` sí la tienen (ver dec_emit_span).
 *
 * Anotación SigMF (https://github.com/sigmf/SigMF): core:sample_start, core:sample_count,
 * core:freq_lower_edge, core:freq_upper_edge, core:label y el espacio de nombres "lab:"
 * (lab:level, lab:proto, lab:snr_db, lab:rssi_dbfs, lab:precision, lab:text).
 */
#ifndef BLADERF_LAB_LABELS_H
#define BLADERF_LAB_LABELS_H

#include "../common.h"
#include "../dsp/detector.h"

typedef struct labels labels_t;

/* datatype: "ci16_le", "ci8", "cu8"… dataset: nombre del fichero de datos (puede ser NULL). */
labels_t *labels_create(const char *datatype, const char *dataset);
void labels_destroy(labels_t *l);
/* Tasa y centro de la captura (antes de reproducir). */
void labels_set_source(labels_t *l, double rate, int64_t center_hz);

/* Línea JSONL de dec_host_on_event(): {"type":"dec",...} o {"type":"gap",...}. Seguro entre hilos. */
void labels_on_event(void *l, const char *line);
/* Señales del detector (engine_set_label_sink). Seguro entre hilos. */
void labels_on_signals(void *l, uint64_t sample_end, double rate, int64_t center_hz, const det_signal_t *sigs, int n);

/* Escribe el .sigmf-meta. total_samples: nº de muestras de la fuente (cierra las regiones
 * abiertas). Devuelve 0, o <0 con texto en err. n_* (opcionales) reciben los recuentos. */
int labels_write(labels_t *l, const char *path, uint64_t total_samples, char *err, size_t errlen, int *n_verified,
                 int *n_unknown, int *n_gap);

#endif
