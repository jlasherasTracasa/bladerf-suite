/*
 * fuzz_sigmf.c — Fuzzing de la lectura de ficheros IQ (source_open_file en
 * src/sdr/source.c): metadatos .sigmf-meta corruptos y datos truncados.
 *
 * Por iteración se escribe en /tmp/q3 un par .sigmf-meta/.sigmf-data (o un fichero
 * suelto .cu8/.cs8/.cs16) y se abre con source_open_file(); si abre, se leen varios
 * bloques con source_read() (incluida la vuelta al principio del fichero, que es
 * donde un fichero truncado o de tamaño impar podría desalinear las muestras).
 *
 * Entradas:
 *   - meta: JSON SigMF válido (el que genera --gen) mutado a nivel de bytes, o
 *     con campos extremos (tasa 0/negativa/1e300/NaN, frecuencia 1e300, datatype
 *     desconocido o larguísimo, "global"/"captures" de tipo equivocado, anidamiento
 *     profundo, fichero de 2 MiB que supera el búfer de lectura);
 *   - datos: longitud 0, 1, 2, 3, 5, impar, y aleatoria hasta 20 kB con el
 *     contenido aleatorio; ficheros que no existen; rutas largas.
 * Los bloques solo se leen si la tasa declarada es ≥ 100 kHz: con una tasa mínima
 * (1 Hz) el reproductor, por diseño, duerme n/rate segundos para marcar el ritmo.
 *
 * Casos de regresión: ficheros de tests/fuzz/casos/sigmf/ con el contenido del
 * .sigmf-meta; se acompañan de un .sigmf-data de 1001 bytes (tamaño impar).
 */
#include "fuzz_util.h"
#include "sdr/source.h"

#define DIR "/tmp/q3"

static const char *META =
    "{\"global\":{\"core:datatype\":\"ci16_le\",\"core:sample_rate\":1000000,\"core:version\":\"1.0.0\","
    "\"bladerf_lab:full_scale\":2048},\"captures\":[{\"core:sample_start\":0,\"core:frequency\":162000000,"
    "\"core:datetime\":\"2026-11-20T15:18:46Z\"}],\"annotations\":[]}";

static void write_file(const char *path, const void *b, size_t l)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(b, 1, l, f);
    fclose(f);
}

static void try_open(const char *path, uint32_t rate_hint)
{
    char err[300] = "";
    source_t *s = source_open_file(path, rate_hint, 100000000, err, sizeof err);
    if (!s)
        return;
    const source_info_t *in = source_info(s);
    (void)in->board;
    (void)source_frequency(s);
    uint32_t r = source_rate(s);
    if (r >= 100000 && source_start(s) == 0) {
        static int16_t iq[2 * 4096];
        for (int i = 0; i < 4; i++)
            if (source_read(s, iq, i == 3 ? 1 : 1024 * (unsigned)(i + 1), NULL) < 0)
                break;
        source_stop(s);
    }
    source_close(s);
}

static const char *DT[] = {"ci16_le", "ci16", "ci8", "cu8", "cf32_le", "", "ci16_leXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"};
static const char *NUMS[] = {"0", "-1", "1", "1e300", "-1e300", "1e999", "4294967295", "4294967296", "1.5", "\"1000000\"",
                             "null", "[]", "{}", "9223372036854775807", "1e-300", "0.0001", "100000", "99999999999"};
static const char *TYPES[] = {"null", "[]", "{}", "1", "\"x\"", "true", "[[[[[[[[[[1]]]]]]]]]]", "[{}]", "[1]"};

static size_t gen_meta(fz_rng_t *r, char *o, size_t cap)
{
    switch (fz_below(r, 3)) {
    case 0: { /* campos extremos */
        char gl[400];
        snprintf(gl, sizeof gl, "{\"core:datatype\":\"%s\",\"core:sample_rate\":%s,\"bladerf_lab:full_scale\":%s}",
                 DT[fz_below(r, sizeof DT / sizeof *DT)], NUMS[fz_below(r, sizeof NUMS / sizeof *NUMS)],
                 NUMS[fz_below(r, sizeof NUMS / sizeof *NUMS)]);
        const char *g = fz_below(r, 6) ? gl : TYPES[fz_below(r, sizeof TYPES / sizeof *TYPES)];
        char cp[200];
        snprintf(cp, sizeof cp, "[{\"core:frequency\":%s}]", NUMS[fz_below(r, sizeof NUMS / sizeof *NUMS)]);
        const char *c = fz_below(r, 6) ? cp : TYPES[fz_below(r, sizeof TYPES / sizeof *TYPES)];
        return (size_t)snprintf(o, cap, "{\"global\":%s,\"captures\":%s}", g, c);
    }
    case 1: { /* fichero enorme (> 1 MiB: más que el búfer de lectura) */
        size_t n = (size_t)snprintf(o, cap, "{\"global\":{\"core:datatype\":\"ci8\",\"core:sample_rate\":1000000,\"x\":\"");
        size_t big = (1u << 20) + fz_below(r, 1000);
        if (big > cap - 100)
            big = cap - 100;
        memset(o + n, 'a', big);
        n += big;
        n += (size_t)snprintf(o + n, cap - n, "\"}}");
        return n;
    }
    default:
        break;
    }
    size_t l = strlen(META);
    memcpy(o, META, l);
    return fz_mutate(r, (uint8_t *)o, l, cap);
}

typedef struct {
    fz_rng_t *r;
} ctx_t;

static void one_meta(const uint8_t *meta, size_t ml, fz_rng_t *r, size_t data_len, const char *ext)
{
    static uint8_t data[20000];
    for (size_t i = 0; i < data_len; i++)
        data[i] = (uint8_t)fz_next(r);
    char mp[128], dp[128];
    snprintf(mp, sizeof mp, DIR "/fz.sigmf-%s", ext ? ext : "meta");
    snprintf(dp, sizeof dp, DIR "/fz.sigmf-data");
    write_file(mp, meta, ml);
    write_file(dp, data, data_len);
    try_open(mp, 0);
    try_open(dp, 0); /* también admite abrir por el .sigmf-data */
}

static void regression_case(const uint8_t *b, size_t l, void *ctx)
{
    one_meta(b, l, ((ctx_t *)ctx)->r, 1001, NULL);
}

int main(int argc, char **argv)
{
    fz_t z;
    fz_init(&z, "sigmf", argc, argv, 60);
    mkdir(DIR, 0755);
    ctx_t cx = {&z.rng};
    fz_run_cases("sigmf", regression_case, &cx);
    static char meta[(1 << 21) + 4096];
    unsigned long it = 0;
    while (fz_alive(&z)) {
        it++;
        size_t ml = gen_meta(&z.rng, meta, sizeof meta);
        fz_current(meta, ml);
        static const size_t LENS[] = {0, 1, 2, 3, 4, 5, 7, 1001, 4096, 4097};
        size_t dl = fz_below(&z.rng, 3) == 0 ? fz_below(&z.rng, 20000) : LENS[fz_below(&z.rng, 10)];
        one_meta((uint8_t *)meta, ml, &z.rng, dl, fz_below(&z.rng, 8) ? NULL : "data");
        if (it % 8 == 0) { /* ficheros sueltos sin SigMF */
            static const char *EXT[] = {"cu8", "cs8", "cs16", "CU8", "iq", "sigmf-data"};
            char p[128];
            snprintf(p, sizeof p, DIR "/fz_raw.%s", EXT[fz_below(&z.rng, 6)]);
            uint8_t d[9000];
            size_t n = fz_below(&z.rng, sizeof d);
            for (size_t i = 0; i < n; i++)
                d[i] = (uint8_t)fz_next(&z.rng);
            write_file(p, d, n);
            try_open(p, fz_below(&z.rng, 4) ? 1000000 : 0);
        }
        if (it % 50 == 0) { /* ruta inexistente o demasiado larga */
            char p[1400];
            memset(p, 'a', sizeof p);
            memcpy(p + fz_below(&z.rng, 1100), ".sigmf-meta", 12);
            p[fz_below(&z.rng, 2) ? 1200 : 1010] = 0;
            try_open(p, 1000000);
            try_open(DIR "/no_existe.sigmf-meta", 0);
        }
    }
    printf("[fuzz:sigmf] %lu iteraciones, sin fallos\n", it);
    return 0;
}
