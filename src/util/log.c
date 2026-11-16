/*
 * log.c — Registro de mensajes con nivel y hora.
 *
 * Escribe en stderr con el formato:
 *     2026-11-15 20:31:07.123 [INFO ] mensaje
 *
 * Es seguro llamarlo desde varios hilos: cada línea se compone entera en un
 * búfer local y se emite con una sola llamada a fputs (POSIX garantiza que
 * stdio bloquea el FILE durante la llamada), así las líneas no se entremezclan.
 */
#include "../common.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

int g_log_level = LOG_LVL_INFO;

static const char *LEVEL_NAMES[] = {"ERROR", "AVISO", "INFO ", "DEBUG"};

void log_msg(int level, const char *fmt, ...)
{
    if (level > g_log_level)
        return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    char line[1024];
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec, (int)(tv.tv_usec / 1000), LEVEL_NAMES[CLAMP(level, 0, 3)]);

    va_list ap;
    va_start(ap, fmt);
    if (n > 0 && n < (int)sizeof line)
        n += vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);

    if (n >= (int)sizeof line - 1)
        n = (int)sizeof line - 2;
    line[n] = '\n';
    line[n + 1] = '\0';
    fputs(line, stderr);
}
