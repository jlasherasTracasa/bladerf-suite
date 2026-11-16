/*
 * test_json.c — Analizador JSON (src/util/json.*) y strbuf.
 */
#include "test_util.h"

#include "util/json.h"
#include "util/strbuf.h"

int main(void)
{
    const char *js = "{\"cmd\":\"dec_add\",\"proto\":\"ais\",\"freq\":161975000,\"on\":true,\"x\":null,"
                     "\"opts\":{\"a\":[1,2,{\"b\":\"c\"}],\"d\":-1.5e2},\"s\":\"h\\u00f3la\\n\\\"q\\\"\"}";
    jtok_t t[64];
    int n = json_parse(js, strlen(js), t, 64);
    CHECK(n > 0);
    CHECK(t[0].type == J_OBJ);
    CHECK_EQ(t[0].size, 7);

    char buf[64];
    int i = json_obj_get(js, t, 0, "cmd");
    CHECK(i >= 0 && json_str(js, t, i, buf, sizeof buf) && strcmp(buf, "dec_add") == 0);
    CHECK(json_str_eq(js, t, i, "dec_add"));
    double v = 0;
    i = json_obj_get(js, t, 0, "freq");
    CHECK(i >= 0 && json_num(js, t, i, &v));
    CHECK_NEAR(v, 161975000.0, 0.5);
    bool b = false;
    i = json_obj_get(js, t, 0, "on");
    CHECK(i >= 0 && json_bool(js, t, i, &b) && b);
    i = json_obj_get(js, t, 0, "x");
    CHECK(i >= 0 && json_is_null(t, i));
    CHECK(json_obj_get(js, t, 0, "no_existe") < 0);

    /* objeto anidado: start apunta a '{' y [start,end) es el objeto entero */
    i = json_obj_get(js, t, 0, "opts");
    CHECK(i >= 0 && t[i].type == J_OBJ);
    CHECK(js[t[i].start] == '{' && js[t[i].end - 1] == '}');
    int arr = json_obj_get(js, t, i, "a");
    CHECK(arr >= 0 && t[arr].type == J_ARR && t[arr].size == 3);
    int el = json_arr_get(t, arr, 2);
    CHECK(el >= 0 && t[el].type == J_OBJ);
    int d = json_obj_get(js, t, i, "d");
    CHECK(d >= 0 && json_num(js, t, d, &v));
    CHECK_NEAR(v, -150.0, 1e-9);
    /* tras un valor anidado, 'next' salta el subárbol: la clave siguiente se encuentra */
    i = json_obj_get(js, t, 0, "s");
    CHECK(i >= 0 && json_str(js, t, i, buf, sizeof buf));
    CHECK(strcmp(buf, "h\xc3\xb3la\n\"q\"") == 0);

    /* JSON inválidos */
    const char *bad[] = {"{", "{\"a\":}", "{\"a\" 1}", "[1,2", "{\"a\":1,}", "", "nul"};
    for (size_t k = 0; k < ARRAY_LEN(bad); k++)
        CHECK(json_parse(bad[k], strlen(bad[k]), t, 64) < 0);
    /* no caben los tokens */
    CHECK(json_parse(js, strlen(js), t, 3) < 0);

    /* strbuf: escapado correcto */
    sb_t sb = {0};
    sb_puts(&sb, "{\"t\":");
    sb_json_str(&sb, "a\"b\\c\nd\x01");
    sb_puts(&sb, ",\"n\":");
    sb_json_num(&sb, NAN, 2);
    sb_puts(&sb, "}");
    n = json_parse(sb.data, sb.len, t, 64);
    CHECK(n > 0);
    i = json_obj_get(sb.data, t, 0, "t");
    CHECK(i >= 0 && json_str(sb.data, t, i, buf, sizeof buf) && strcmp(buf, "a\"b\\c\nd\x01") == 0);
    i = json_obj_get(sb.data, t, 0, "n");
    CHECK(i >= 0 && json_is_null(t, i));
    sb_free(&sb);
    return TEST_SUMMARY();
}
