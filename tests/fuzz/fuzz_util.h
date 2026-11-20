/*
 * fuzz_util.h — Utilidades comunes de los arneses de fuzzing propio (Q3).
 *
 * No usamos libFuzzer ni AFL (AGENTS.md §0.3): cada arnés es un programa normal
 * que genera entradas con un PRNG con semilla, muta entradas válidas y las
 * entrega al código bajo prueba compilado con ASan + UBSan.
 *
 * Reproducibilidad: la semilla se imprime al arrancar; "--seed N" repite la misma
 * secuencia de entradas. Además, si el proceso muere (ASan/UBSan abortan), el
 * callback de muerte vuelca la última entrada anunciada con fz_current() en
 * /tmp/fuzz_<objetivo>_crash.bin, para copiarla a tests/fuzz/casos/<objetivo>/.
 *
 * Mutaciones (las clásicas de un fuzzer de caja gris sin cobertura):
 *   volteo de bit/byte, byte extremo (0, 0xFF, 0x7F, 0x80), truncado,
 *   duplicado de un trozo, inserción de bytes, borrado de un trozo y
 *   sustitución por un "entero interesante" de 16/32 bits (límites típicos
 *   de desbordamiento: 0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF…).
 *
 * Casos de regresión: fz_run_cases() abre todos los ficheros de un directorio y
 * llama al callback con su contenido; los arneses lo hacen siempre al arrancar.
 */
#ifndef BLADERF_LAB_FUZZ_UTIL_H
#define BLADERF_LAB_FUZZ_UTIL_H

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/common_interface_defs.h>
#endif

typedef struct {
    uint64_t s;
} fz_rng_t;

static inline uint64_t fz_next(fz_rng_t *r)
{
    /* xorshift64* (Vigna): suficiente para generar entradas, nada criptográfico */
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static inline uint32_t fz_below(fz_rng_t *r, uint32_t n) { return n ? (uint32_t)(fz_next(r) >> 33) % n : 0; }
static inline double fz_unit(fz_rng_t *r) { return (double)(fz_next(r) >> 11) / 9007199254740992.0; }

static inline double fz_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct {
    const char *name; /* objetivo, p. ej. "json" */
    uint64_t seed;
    double seconds;
    fz_rng_t rng;
    double t_end;
} fz_t;

static const void *fz_cur_buf;
static size_t fz_cur_len;
static const char *fz_cur_name = "x";
static char fz_note[512]; /* contexto de la entrada actual (configuración, opciones…) */

static void fz_death(void)
{
    if (fz_note[0])
        fprintf(stderr, "[fuzz] contexto: %s\n", fz_note);
    if (!fz_cur_buf)
        return;
    char path[128];
    snprintf(path, sizeof path, "/tmp/fuzz_%s_crash.bin", fz_cur_name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(fz_cur_buf, 1, fz_cur_len, f);
        fclose(f);
        fprintf(stderr, "[fuzz] entrada guardada en %s (%zu bytes)\n", path, fz_cur_len);
    }
}

/* Anuncia la entrada que se va a probar (para volcarla si el proceso muere). */
static inline void fz_current(const void *buf, size_t len)
{
    fz_cur_buf = buf;
    fz_cur_len = len;
}

static inline void fz_init(fz_t *z, const char *name, int argc, char **argv, double default_seconds)
{
    memset(z, 0, sizeof *z);
    z->name = name;
    z->seconds = default_seconds;
    z->seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 20);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            z->seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            z->seconds = atof(argv[++i]);
    }
    z->rng.s = z->seed ? z->seed : 1;
    for (int i = 0; i < 4; i++)
        fz_next(&z->rng);
    z->t_end = fz_now() + z->seconds;
    fz_cur_name = name;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("[fuzz:%s] semilla=%llu segundos=%.0f\n", name, (unsigned long long)z->seed, z->seconds);
#if defined(__SANITIZE_ADDRESS__)
    __sanitizer_set_death_callback(fz_death);
#endif
}

static inline int fz_alive(const fz_t *z) { return fz_now() < z->t_end; }

static const uint32_t FZ_INTERESTING[] = {0,          1,          2,          0x7F,       0x80,       0xFF,
                                          0x100,      0x7FFF,     0x8000,     0xFFFF,     0x10000,    0x7FFFFFFF,
                                          0x80000000u, 0xFFFFFFFFu, 0x1000000, 0xFFFFFF};

/* Muta buf (longitud len, capacidad cap) con 1–4 operaciones; devuelve la nueva longitud. */
static inline size_t fz_mutate(fz_rng_t *r, uint8_t *buf, size_t len, size_t cap)
{
    int ops = 1 + (int)fz_below(r, 4);
    for (int k = 0; k < ops; k++) {
        switch (fz_below(r, 9)) {
        case 0: /* voltear un bit */
            if (len)
                buf[fz_below(r, (uint32_t)len)] ^= (uint8_t)(1u << fz_below(r, 8));
            break;
        case 1: /* byte aleatorio */
            if (len)
                buf[fz_below(r, (uint32_t)len)] = (uint8_t)fz_next(r);
            break;
        case 2: { /* byte extremo */
            static const uint8_t ext[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
            if (len)
                buf[fz_below(r, (uint32_t)len)] = ext[fz_below(r, 5)];
            break;
        }
        case 3: /* truncar */
            if (len > 1)
                len = 1 + fz_below(r, (uint32_t)len - 1);
            break;
        case 4: /* duplicar un trozo */
            if (len > 1 && len < cap) {
                size_t a = fz_below(r, (uint32_t)len), n = 1 + fz_below(r, (uint32_t)(len - a));
                if (n > cap - len)
                    n = cap - len;
                memmove(buf + a + n, buf + a, len - a);
                memcpy(buf + a, buf + a + n, n);
                len += n;
            }
            break;
        case 5: /* insertar bytes aleatorios */
            if (len < cap) {
                size_t a = fz_below(r, (uint32_t)len + 1), n = 1 + fz_below(r, 8);
                if (n > cap - len)
                    n = cap - len;
                memmove(buf + a + n, buf + a, len - a);
                for (size_t i = 0; i < n; i++)
                    buf[a + i] = (uint8_t)fz_next(r);
                len += n;
            }
            break;
        case 6: /* borrar un trozo */
            if (len > 2) {
                size_t a = fz_below(r, (uint32_t)len), n = 1 + fz_below(r, (uint32_t)(len - a));
                memmove(buf + a, buf + a + n, len - a - n);
                len -= n;
            }
            break;
        case 7: /* entero interesante de 1/2/4 bytes */
            if (len >= 4) {
                size_t a = fz_below(r, (uint32_t)len - 3);
                uint32_t v = FZ_INTERESTING[fz_below(r, sizeof FZ_INTERESTING / sizeof *FZ_INTERESTING)];
                size_t w = 1u << fz_below(r, 3);
                for (size_t i = 0; i < w; i++)
                    buf[a + i] = (uint8_t)(v >> (8 * i));
            }
            break;
        default: /* intercambiar dos bytes */
            if (len > 1) {
                size_t a = fz_below(r, (uint32_t)len), b = fz_below(r, (uint32_t)len);
                uint8_t t = buf[a];
                buf[a] = buf[b];
                buf[b] = t;
            }
        }
    }
    return len;
}

/* Lee un fichero completo (NULL si no existe). El llamador libera. */
static inline uint8_t *fz_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)(n > 0 ? n : 0) + 1);
    size_t got = n > 0 ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    *len = got;
    return b;
}

/* Reproduce los casos de regresión de tests/fuzz/casos/<objetivo>/ . Devuelve cuántos. */
static inline int fz_run_cases(const char *name, void (*fn)(const uint8_t *, size_t, void *), void *ctx)
{
    char dir[256];
    const char *base = getenv("FUZZ_CASES");
    snprintf(dir, sizeof dir, "%s/%s", base ? base : "tests/fuzz/casos", name);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[640];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        size_t len;
        uint8_t *b = fz_read_file(path, &len);
        if (!b)
            continue;
        fz_current(b, len);
        fn(b, len, ctx);
        free(b);
        n++;
    }
    closedir(d);
    printf("[fuzz:%s] %d casos de regresión reproducidos\n", name, n);
    return n;
}

#endif
