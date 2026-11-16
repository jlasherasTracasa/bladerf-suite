/*
 * common.h — Tipos y utilidades compartidas por todo el servidor.
 *
 * BladeRF Lab está escrito en C11 sin dependencias de terceros, salvo el
 * driver del hardware (libbladeRF). Este fichero reúne lo que casi todos los
 * módulos necesitan:
 *
 *   - cf32:      número complejo en coma flotante (una muestra IQ).
 *   - LOG_*:     macros de registro con nivel y marca de tiempo.
 *   - utilidades pequeñas: CLAMP, MIN/MAX, conversiones a dB, reloj monotónico.
 *
 * Convenciones del proyecto:
 *   - Todas las frecuencias van en hercios (Hz) y en enteros de 64 bits cuando
 *     son absolutas (p. ej. 1 090 000 000 Hz no cabe en 32 bits con signo).
 *   - Las muestras IQ normalizadas usan escala ±1.0 = fondo de escala del ADC.
 *   - Los niveles se expresan en dBFS (dB respecto al fondo de escala): un tono
 *     complejo de amplitud 1.0 mide 0 dBFS.
 */
#ifndef BLADERF_LAB_COMMON_H
#define BLADERF_LAB_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <time.h>

#define APP_NAME    "BladeRF Lab"
#define APP_VERSION "2.0.0"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Muestra IQ compleja. Se usa una estructura propia (y no <complex.h>) para que
 * la disposición en memoria sea explícita y el compilador vectorice bien los
 * bucles de filtrado: re, im, re, im... */
typedef struct {
    float re;
    float im;
} cf32;

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Potencia lineal -> dB. El 1e-20 evita log10(0) = -inf con señales nulas. */
static inline float power_to_db(float p) { return 10.0f * log10f(p + 1e-20f); }

/* Reloj monotónico en segundos (no salta si cambia la hora del sistema). */
static inline double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Hora real (Unix epoch) en segundos, para marcas de tiempo visibles. */
static inline double now_wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---------------------------------------------------------------------------
 * Registro (log). Implementado en util/log.c.
 * Niveles: 0 = error, 1 = aviso, 2 = info, 3 = depuración.
 * ------------------------------------------------------------------------- */
enum { LOG_LVL_ERROR = 0, LOG_LVL_WARN, LOG_LVL_INFO, LOG_LVL_DEBUG };

extern int g_log_level;
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_E(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)
#define LOG_W(...) log_msg(LOG_LVL_WARN, __VA_ARGS__)
#define LOG_I(...) log_msg(LOG_LVL_INFO, __VA_ARGS__)
#define LOG_D(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)

#endif /* BLADERF_LAB_COMMON_H */
