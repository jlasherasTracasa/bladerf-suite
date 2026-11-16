/*
 * json.h — Analizador JSON minimalista sin reservas de memoria.
 *
 * Convierte un texto JSON en un vector plano de "tokens". Cada token describe
 * un valor (objeto, array, cadena, número, booleano o null) mediante su tipo y
 * su posición [start, end) dentro del texto original: no se copia nada.
 *
 * Los tokens se guardan en orden de recorrido en profundidad (preorden):
 *
 *     {"cmd":"tune","freq":145500000}
 *     tok0 = OBJ (size=2)
 *     tok1 = STR "cmd"      tok2 = STR "tune"
 *     tok3 = STR "freq"     tok4 = NUM 145500000
 *
 * - Los hijos de un objeto son pares clave/valor consecutivos.
 * - El primer hijo de un contenedor i es siempre el token i+1.
 * - tok[i].next es el índice del primer token que ya NO pertenece al
 *   subárbol de i; así se salta un valor entero de un golpe.
 *
 * Uso típico:
 *     jtok_t t[64];
 *     int n = json_parse(texto, len, t, 64);
 *     int v = json_obj_get(texto, t, 0, "freq");
 *     double f; if (v >= 0 && json_num(texto, t, v, &f)) ...
 */
#ifndef BLADERF_LAB_JSON_H
#define BLADERF_LAB_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct {
    jtype_t type;
    int start;  /* primer byte del valor (para cadenas: tras la comilla inicial) */
    int end;    /* un byte después del final (para cadenas: la comilla final) */
    int size;   /* objetos: nº de claves; arrays: nº de elementos */
    int next;   /* índice del siguiente token fuera de este subárbol */
} jtok_t;

/* Devuelve el nº de tokens usados, o -1 si el JSON no es válido o no caben. */
int json_parse(const char *js, size_t len, jtok_t *toks, int max_toks);

/* Busca "key" en el objeto obj. Devuelve el índice del valor o -1. */
int json_obj_get(const char *js, const jtok_t *t, int obj, const char *key);

/* Índice del elemento i-ésimo de un array, o -1. */
int json_arr_get(const jtok_t *t, int arr, int index);

bool json_num(const char *js, const jtok_t *t, int i, double *out);
bool json_bool(const char *js, const jtok_t *t, int i, bool *out);
bool json_is_null(const jtok_t *t, int i);
/* Copia la cadena (decodificando escapes \n, \", \uXXXX...) en buf. */
bool json_str(const char *js, const jtok_t *t, int i, char *buf, size_t bufsize);
/* Compara una cadena JSON con un literal C (sin decodificar escapes). */
bool json_str_eq(const char *js, const jtok_t *t, int i, const char *s);

#endif
