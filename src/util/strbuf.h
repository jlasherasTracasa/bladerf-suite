/*
 * strbuf.h — Cadena de texto dinámica (string builder).
 *
 * Se usa para componer respuestas HTTP y mensajes JSON sin calcular tamaños a
 * mano. Crece por duplicación, así que añadir es O(1) amortizado.
 *
 *   sb_t sb = {0};
 *   sb_printf(&sb, "{\"freq\":%llu,", f);
 *   sb_json_str(&sb, nombre);   // añade "nombre" con comillas y escapes JSON
 *   ...
 *   usar(sb.data, sb.len);
 *   sb_free(&sb);
 */
#ifndef BLADERF_LAB_STRBUF_H
#define BLADERF_LAB_STRBUF_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char *data;   /* siempre terminada en '\0' si len > 0 */
    size_t len;   /* bytes útiles, sin contar el '\0' */
    size_t cap;   /* capacidad reservada */
} sb_t;

void sb_reserve(sb_t *sb, size_t extra);
void sb_append(sb_t *sb, const void *data, size_t n);
void sb_puts(sb_t *sb, const char *s);
void sb_printf(sb_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Añade s entre comillas, escapando lo necesario para que sea JSON válido. */
void sb_json_str(sb_t *sb, const char *s);
/* Añade un número en coma flotante; NaN/Inf se escriben como null (JSON no los admite). */
void sb_json_num(sb_t *sb, double v, int decimals);
void sb_clear(sb_t *sb);
void sb_free(sb_t *sb);

#endif
