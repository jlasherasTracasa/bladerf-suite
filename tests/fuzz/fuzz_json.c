/*
 * fuzz_json.c — Fuzzing del analizador JSON (src/util/json.*).
 *
 * Entradas: órdenes reales del protocolo WebSocket mutadas, más generadores
 * específicos de casos hostiles: anidamiento muy profundo, cadenas enormes,
 * escapes inválidos (\u truncado, \u con no-hex, barra al final), números
 * absurdos, claves repetidas y basura binaria.
 *
 * Tras json_parse se comprueban los invariantes que el resto del programa da por
 * ciertos (todo token dentro del texto, start<=end, next dentro de rango y
 * posterior al propio token, size coherente) y se ejercitan todas las funciones de
 * acceso con buffers pequeños y grandes, para que ASan vigile cada lectura.
 */
#include "fuzz_util.h"
#include "util/json.h"

#include <assert.h>

static const char *SEEDS[] = {
    "{\"cmd\":\"config\",\"center\":100000000,\"rate\":2400000,\"rf_bw\":1500000,\"gain_mode\":\"manual\","
    "\"gain\":30,\"biastee\":false,\"fft\":4096,\"fps\":30,\"det_snr\":8,\"loc\":{\"lat\":40.4,\"lon\":-3.7}}",
    "{\"cmd\":\"channel\",\"freq\":100300000,\"mode\":\"wfm\",\"bw\":200000,\"squelch\":null,\"deemph\":50,"
    "\"stereo\":true,\"audio\":true}",
    "{\"cmd\":\"dec_add\",\"proto\":\"ais\",\"freq\":161975000,\"opts\":{\"a\":[1,2,3,{\"b\":\"c\\u00e9\\n\"}]}}",
    "{\"cmd\":\"tx\",\"proto\":\"pocsag\",\"mode\":\"file\",\"freq\":466075000,\"gain\":-10,\"center\":466300000,"
    "\"rate\":1000000,\"repeat\":2,\"params\":{\"text\":\"hola \\\"mundo\\\"\",\"addr\":1234567}}",
    "[1,2.5e3,-0.0,true,false,null,\"\\\\\\/\\b\\f\\r\\t\",[],{}]",
    "{\"a\":{\"b\":{\"c\":{\"d\":[[[[]]]]}}}}",
    "\"\\ud83d\\ude00 emoji\"",
};

static size_t gen_hostile(fz_rng_t *r, uint8_t *b, size_t cap)
{
    size_t n = 0;
    switch (fz_below(r, 7)) {
    case 0: { /* anidamiento profundo (corte exacto en el límite 32 y muy por encima) */
        size_t d = 1 + fz_below(r, 5000);
        char open = fz_below(r, 2) ? '[' : '{';
        for (size_t i = 0; i < d && n + 8 < cap; i++) {
            b[n++] = (uint8_t)open;
            if (open == '{') {
                memcpy(b + n, "\"k\":", 4);
                n += 4;
            }
        }
        if (fz_below(r, 2))
            b[n++] = '1';
        for (size_t i = 0; i < d && n + 1 < cap && fz_below(r, 20); i++)
            b[n++] = open == '[' ? ']' : '}';
        break;
    }
    case 1: { /* cadena enorme */
        size_t l = fz_below(r, (uint32_t)cap - 8);
        b[n++] = '"';
        while (n < l + 1)
            b[n++] = (uint8_t)('a' + fz_below(r, 26));
        if (fz_below(r, 4))
            b[n++] = '"';
        break;
    }
    case 2: { /* escapes inválidos */
        static const char *E[] = {"\\u", "\\u12", "\\uZZZZ", "\\", "\\x", "\\u00", "\\ud800", "\\uDC00", "\\u0000", "\\/"};
        b[n++] = '"';
        for (int i = 0, k = 1 + (int)fz_below(r, 8); i < k && n + 16 < cap; i++) {
            const char *e = E[fz_below(r, 10)];
            size_t l = strlen(e);
            memcpy(b + n, e, l);
            n += l;
            if (fz_below(r, 2))
                b[n++] = 'a';
        }
        if (fz_below(r, 2))
            b[n++] = '"';
        break;
    }
    case 3: { /* números extraños */
        static const char *N[] = {"-", "--1", "1e", "1e999", "-1e999", "0x10", "1.", ".5", "00", "+1", "1e-999",
                                  "123456789012345678901234567890123456789012345678901234567890123456789", "nan", "Infinity"};
        const char *s = N[fz_below(r, 14)];
        n = (size_t)snprintf((char *)b, cap, "{\"v\":%s}", s);
        break;
    }
    case 4: /* muchas claves / elementos */
        b[n++] = '[';
        for (int i = 0; i < 3000 && n + 4 < cap; i++) {
            b[n++] = (uint8_t)('0' + i % 10);
            b[n++] = ',';
        }
        b[n - 1] = fz_below(r, 4) ? ']' : ' ';
        break;
    default: /* basura binaria */
        n = fz_below(r, 200);
        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)fz_next(r);
    }
    return n;
}

static void walk(const char *js, size_t len, const jtok_t *t, int n)
{
    for (int i = 0; i < n; i++) {
        assert(t[i].start >= 0 && t[i].start <= t[i].end && (size_t)t[i].end <= len);
        assert(t[i].next > i && t[i].next <= n);
        assert(t[i].size >= 0);
        char small[8], big[4096];
        double d;
        bool bo;
        (void)json_str(js, t, i, small, sizeof small);
        (void)json_str(js, t, i, big, sizeof big);
        (void)json_num(js, t, i, &d);
        (void)json_bool(js, t, i, &bo);
        (void)json_is_null(t, i);
        (void)json_str_eq(js, t, i, "cmd");
        if (t[i].type == J_OBJ) {
            (void)json_obj_get(js, t, i, "cmd");
            (void)json_obj_get(js, t, i, "");
            int c = i + 1;
            for (int k = 0; k < t[i].size; k++) {
                assert(c < n && t[c].type == J_STR && c + 1 < n);
                int v = c + 1;
                assert(json_obj_get(js, t, i, "k") < 0 || 1);
                c = t[v].next;
            }
            assert(c == t[i].next);
        } else if (t[i].type == J_ARR) {
            for (int k = 0; k < t[i].size && k < 8; k++)
                assert(json_arr_get(t, i, k) > i);
            (void)json_arr_get(t, i, t[i].size);
            (void)json_arr_get(t, i, -1);
        }
    }
}

static void one(const uint8_t *in, size_t len, void *ctx)
{
    (void)ctx;
    /* copia exacta en el montón: ASan detecta cualquier lectura más allá de len */
    char *js = malloc(len ? len : 1);
    memcpy(js, in, len);
    static jtok_t big[4096];
    int caps[] = {4096, 1, 3, 16};
    for (int c = 0; c < 4; c++) {
        jtok_t *t = c == 0 ? big : malloc(sizeof(jtok_t) * (size_t)caps[c]); /* exacto, para ASan */
        int n = json_parse(js, len, t, caps[c]);
        if (n > 0)
            walk(js, len, t, n);
        if (c)
            free(t);
    }
    free(js);
}

int main(int argc, char **argv)
{
    fz_t z;
    fz_init(&z, "json", argc, argv, 60);
    fz_run_cases("json", one, NULL);
    static uint8_t buf[1 << 16];
    unsigned long it = 0;
    while (fz_alive(&z)) {
        for (int k = 0; k < 200; k++, it++) {
            size_t len;
            if (fz_below(&z.rng, 4) == 0) {
                len = gen_hostile(&z.rng, buf, sizeof buf);
            } else {
                const char *s = SEEDS[fz_below(&z.rng, sizeof SEEDS / sizeof *SEEDS)];
                len = strlen(s);
                memcpy(buf, s, len);
                len = fz_mutate(&z.rng, buf, len, sizeof buf);
            }
            fz_current(buf, len);
            one(buf, len, NULL);
        }
    }
    printf("[fuzz:json] %lu entradas, sin fallos\n", it);
    return 0;
}
