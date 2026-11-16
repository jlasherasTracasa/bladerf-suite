/*
 * iqbuf.c — Búfer dinámico de muestras IQ para los moduladores.
 */
#include "mod.h"

#include <stdio.h>
#include <stdlib.h>

void iqbuf_reserve(iqbuf_t *b, int extra)
{
    if (b->n + extra <= b->cap)
        return;
    int cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    cf32 *p = realloc(b->s, sizeof(cf32) * (size_t)cap);
    if (!p) {
        fprintf(stderr, "iqbuf: sin memoria (%d muestras)\n", cap);
        abort();
    }
    b->s = p;
    b->cap = cap;
}

void iqbuf_push(iqbuf_t *b, cf32 v)
{
    iqbuf_reserve(b, 1);
    b->s[b->n++] = v;
}

void iqbuf_free(iqbuf_t *b)
{
    free(b->s);
    b->s = NULL;
    b->n = b->cap = 0;
}
