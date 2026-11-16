/*
 * strbuf.c — Implementación de la cadena dinámica. Ver strbuf.h.
 */
#include "strbuf.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_reserve(sb_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1; /* +1 para el '\0' final */
    if (need <= sb->cap)
        return;
    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < need)
        cap *= 2;
    char *p = realloc(sb->data, cap);
    if (!p)
        abort(); /* sin memoria: no hay recuperación razonable en un servidor pequeño */
    sb->data = p;
    sb->cap = cap;
}

void sb_append(sb_t *sb, const void *data, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, data, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(sb_t *sb, const char *s) { sb_append(sb, s, strlen(s)); }

void sb_printf(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof tmp) {
        sb_append(sb, tmp, (size_t)n);
        return;
    }
    /* No cabía en el búfer temporal: se formatea directamente en el destino. */
    sb_reserve(sb, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void sb_json_str(sb_t *sb, const char *s)
{
    sb_append(sb, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '"': sb_append(sb, "\\\"", 2); break;
        case '\\': sb_append(sb, "\\\\", 2); break;
        case '\n': sb_append(sb, "\\n", 2); break;
        case '\r': sb_append(sb, "\\r", 2); break;
        case '\t': sb_append(sb, "\\t", 2); break;
        default:
            if (*p < 0x20) /* otros caracteres de control: \u00XX */
                sb_printf(sb, "\\u%04x", *p);
            else
                sb_append(sb, p, 1); /* UTF-8 pasa tal cual: JSON lo admite */
        }
    }
    sb_append(sb, "\"", 1);
}

void sb_json_num(sb_t *sb, double v, int decimals)
{
    if (isnan(v) || isinf(v))
        sb_puts(sb, "null");
    else
        sb_printf(sb, "%.*f", decimals, v);
}

void sb_clear(sb_t *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

void sb_free(sb_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}
