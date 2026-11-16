/*
 * json.c — Analizador JSON por descenso recursivo. Ver json.h.
 *
 * Gramática admitida (RFC 8259):
 *   valor  := objeto | array | cadena | número | true | false | null
 *   objeto := '{' [ cadena ':' valor { ',' cadena ':' valor } ] '}'
 *   array  := '[' [ valor { ',' valor } ] ']'
 *
 * La profundidad de anidamiento se limita a 32 niveles para que un mensaje
 * malicioso no pueda agotar la pila con "[[[[[[...".
 */
#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *js;
    size_t len;
    size_t pos;
    jtok_t *toks;
    int max;
    int n;
} parser_t;

#define MAX_DEPTH 32

static void skip_ws(parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        p->pos++;
    }
}

static int new_tok(parser_t *p, jtype_t type, int start)
{
    if (p->n >= p->max)
        return -1;
    jtok_t *t = &p->toks[p->n];
    t->type = type;
    t->start = start;
    t->end = start;
    t->size = 0;
    t->next = 0;
    return p->n++;
}

static int parse_value(parser_t *p, int depth);

static int parse_string(parser_t *p)
{
    /* p->pos apunta a la comilla de apertura */
    int idx = new_tok(p, J_STR, (int)p->pos + 1);
    if (idx < 0)
        return -1;
    p->pos++;
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == '"') {
            p->toks[idx].end = (int)p->pos;
            p->pos++;
            p->toks[idx].next = p->n;
            return idx;
        }
        if (c == '\\') {
            p->pos++; /* el carácter escapado se valida al decodificar */
            if (p->pos >= p->len)
                return -1;
        } else if ((unsigned char)c < 0x20) {
            return -1; /* caracteres de control sin escapar no son válidos */
        }
        p->pos++;
    }
    return -1; /* cadena sin cerrar */
}

static int parse_literal(parser_t *p)
{
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->js[p->pos];
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        p->pos++;
    }
    size_t n = p->pos - start;
    const char *s = p->js + start;
    jtype_t type;
    if (n == 4 && memcmp(s, "true", 4) == 0)
        type = J_BOOL;
    else if (n == 5 && memcmp(s, "false", 5) == 0)
        type = J_BOOL;
    else if (n == 4 && memcmp(s, "null", 4) == 0)
        type = J_NULL;
    else if (n > 0 && (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')))
        type = J_NUM;
    else
        return -1;
    int idx = new_tok(p, type, (int)start);
    if (idx < 0)
        return -1;
    p->toks[idx].end = (int)p->pos;
    p->toks[idx].next = p->n;
    return idx;
}

static int parse_container(parser_t *p, int depth, bool is_obj)
{
    int idx = new_tok(p, is_obj ? J_OBJ : J_ARR, (int)p->pos);
    if (idx < 0)
        return -1;
    char close = is_obj ? '}' : ']';
    p->pos++; /* salta '{' o '[' */
    skip_ws(p);
    if (p->pos < p->len && p->js[p->pos] == close) {
        p->pos++;
        p->toks[idx].end = (int)p->pos;
        p->toks[idx].next = p->n;
        return idx;
    }
    for (;;) {
        skip_ws(p);
        if (is_obj) {
            if (p->pos >= p->len || p->js[p->pos] != '"')
                return -1; /* las claves deben ser cadenas */
            if (parse_string(p) < 0)
                return -1;
            skip_ws(p);
            if (p->pos >= p->len || p->js[p->pos] != ':')
                return -1;
            p->pos++;
        }
        if (parse_value(p, depth + 1) < 0)
            return -1;
        p->toks[idx].size++;
        skip_ws(p);
        if (p->pos >= p->len)
            return -1;
        char c = p->js[p->pos++];
        if (c == ',')
            continue;
        if (c == close)
            break;
        return -1;
    }
    p->toks[idx].end = (int)p->pos;
    p->toks[idx].next = p->n;
    return idx;
}

static int parse_value(parser_t *p, int depth)
{
    if (depth > MAX_DEPTH)
        return -1;
    skip_ws(p);
    if (p->pos >= p->len)
        return -1;
    char c = p->js[p->pos];
    if (c == '{')
        return parse_container(p, depth, true);
    if (c == '[')
        return parse_container(p, depth, false);
    if (c == '"')
        return parse_string(p);
    return parse_literal(p);
}

int json_parse(const char *js, size_t len, jtok_t *toks, int max_toks)
{
    parser_t p = {js, len, 0, toks, max_toks, 0};
    if (parse_value(&p, 0) < 0)
        return -1;
    skip_ws(&p);
    if (p.pos != len)
        return -1; /* basura tras el valor raíz */
    return p.n;
}

int json_obj_get(const char *js, const jtok_t *t, int obj, const char *key)
{
    if (obj < 0 || t[obj].type != J_OBJ)
        return -1;
    size_t klen = strlen(key);
    int k = obj + 1;
    for (int i = 0; i < t[obj].size; i++) {
        int v = k + 1;
        if ((size_t)(t[k].end - t[k].start) == klen && memcmp(js + t[k].start, key, klen) == 0)
            return v;
        k = t[v].next;
    }
    return -1;
}

int json_arr_get(const jtok_t *t, int arr, int index)
{
    if (arr < 0 || t[arr].type != J_ARR || index < 0 || index >= t[arr].size)
        return -1;
    int k = arr + 1;
    for (int i = 0; i < index; i++)
        k = t[k].next;
    return k;
}

bool json_num(const char *js, const jtok_t *t, int i, double *out)
{
    if (i < 0 || t[i].type != J_NUM)
        return false;
    char buf[64];
    int n = t[i].end - t[i].start;
    if (n <= 0 || n >= (int)sizeof buf)
        return false;
    memcpy(buf, js + t[i].start, (size_t)n);
    buf[n] = '\0';
    char *endp;
    *out = strtod(buf, &endp);
    return *endp == '\0';
}

bool json_bool(const char *js, const jtok_t *t, int i, bool *out)
{
    if (i < 0 || t[i].type != J_BOOL)
        return false;
    *out = js[t[i].start] == 't';
    return true;
}

bool json_is_null(const jtok_t *t, int i) { return i >= 0 && t[i].type == J_NULL; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Escribe un punto de código Unicode en UTF-8. Devuelve los bytes escritos. */
static size_t utf8_put(char *dst, size_t room, unsigned cp)
{
    if (cp < 0x80 && room >= 1) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800 && room >= 2) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (room >= 3) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

bool json_str(const char *js, const jtok_t *t, int i, char *buf, size_t bufsize)
{
    if (i < 0 || t[i].type != J_STR || bufsize == 0)
        return false;
    size_t o = 0;
    for (int p = t[i].start; p < t[i].end; p++) {
        if (o + 4 >= bufsize)
            return false; /* no cabe: mejor fallar que truncar en silencio */
        char c = js[p];
        if (c != '\\') {
            buf[o++] = c;
            continue;
        }
        c = js[++p];
        switch (c) {
        case 'n': buf[o++] = '\n'; break;
        case 't': buf[o++] = '\t'; break;
        case 'r': buf[o++] = '\r'; break;
        case 'b': buf[o++] = '\b'; break;
        case 'f': buf[o++] = '\f'; break;
        case 'u': {
            if (p + 4 >= t[i].end)
                return false;
            unsigned cp = 0;
            for (int k = 1; k <= 4; k++) {
                int h = hexval(js[p + k]);
                if (h < 0)
                    return false;
                cp = cp * 16 + (unsigned)h;
            }
            p += 4;
            o += utf8_put(buf + o, bufsize - o - 1, cp);
            break;
        }
        default: buf[o++] = c; /* \" \\ \/ */
        }
    }
    buf[o] = '\0';
    return true;
}

bool json_str_eq(const char *js, const jtok_t *t, int i, const char *s)
{
    if (i < 0 || t[i].type != J_STR)
        return false;
    size_t n = strlen(s);
    return (size_t)(t[i].end - t[i].start) == n && memcmp(js + t[i].start, s, n) == 0;
}
