/*
 * test_auto.c — Instancias automáticas del host (perfiles con auto:true) y
 * regresión de chan_drift (desbordamiento con ppm > 0 y señales largas: se
 * ejerce bajo ASan con `make SAN=1 test`).
 *
 * El reloj es el que pasa el test (parámetro 'now'), así que los 60 s de
 * caducidad se simulan sin esperar.
 */
#include "test_util.h"

#include "decoders/host.h"
#include "util/strbuf.h"

/* Instancias con "owner":1 y "proto":proto en la lista del host; freq_out = la primera */
static int count_owner1(dec_host_t *h, const char *proto)
{
    sb_t sb = {0};
    dec_host_list_json(h, &sb);
    int n = 0;
    char key[40];
    snprintf(key, sizeof key, "\"proto\":\"%s\"", proto);
    for (char *p = sb.data; (p = strstr(p, key)); p++) {
        char *end = strchr(p, '}');
        char *o = strstr(p, "\"owner\":1");
        if (o && end && o < end)
            n++;
    }
    sb_free(&sb);
    return n;
}

static int count_all(dec_host_t *h)
{
    sb_t sb = {0};
    dec_host_list_json(h, &sb);
    int n = 0;
    for (char *p = sb.data; (p = strstr(p, "\"inst\":")); p++)
        n++;
    sb_free(&sb);
    return n;
}

static void feed(dec_host_t *h, const dec_sig_t *s, int n, double now)
{
    dec_host_auto_signals(h, s, n, now);
    CHECK(dec_host_auto_sync(h, 5.0));
}

static void test_auto(void)
{
    printf("auto: altas, duplicados, banda, máximo 8 y caducidad a 60 s\n");
    dec_host_t *h = dec_host_create(NULL);
    CHECK(h != NULL);
    CHECK(dec_host_auto_add_rule(h, "pocsag", 150000000, 160000000) == 0);
    CHECK(dec_host_auto_add_rule(h, "adsb", 150000000, 160000000) < 0); /* wide: se rechaza */
    CHECK(dec_host_auto_add_rule(h, "noexiste", 1, 2) < 0);

    double t = 1000.0;
    dec_sig_t s1[] = {{153000000, 12500, 20}, {153003000, 12500, 15} /* duplicada (3 kHz) */,
                      {155000000, 12500, 25}, {170000000, 12500, 30} /* fuera de banda */};
    feed(h, s1, 4, t);
    CHECK_EQ(count_owner1(h, "pocsag"), 2);

    /* Misma señal otra vez, con deriva de 4 kHz: no crea otra */
    dec_sig_t s2[] = {{153004000, 12500, 20}, {155000000, 12500, 25}};
    feed(h, s2, 2, t + 10);
    CHECK_EQ(count_owner1(h, "pocsag"), 2);

    /* Una instancia manual del mismo protocolo evita el duplicado automático */
    char err[160];
    CHECK(dec_host_add(h, "pocsag", 157000000, NULL, 0, err, sizeof err) > 0);
    dec_sig_t s3[] = {{157002000, 12500, 20}};
    feed(h, s3, 1, t + 11);
    CHECK_EQ(count_owner1(h, "pocsag"), 2);

    /* La de 155 MHz se sigue viendo; la de 153 MHz desaparece. A los 59 s sigue, a los 61 s no. */
    dec_sig_t only155[] = {{155000000, 12500, 25}};
    feed(h, only155, 1, t + 59);
    CHECK_EQ(count_owner1(h, "pocsag"), 2); /* 153 MHz se vio en t+10: lleva 49 s */
    feed(h, only155, 1, t + 69);
    CHECK_EQ(count_owner1(h, "pocsag"), 2); /* 59 s: aún no */
    feed(h, only155, 1, t + 71);
    CHECK_EQ(count_owner1(h, "pocsag"), 1); /* 61 s */
    CHECK_EQ(count_all(h), 2);              /* + la manual */

    /* Máximo 8: 12 señales distintas; si faltan sitios gana la de mayor SNR */
    dec_sig_t many[12];
    for (int i = 0; i < 12; i++)
        many[i] = (dec_sig_t){151000000 + 100000LL * i, 12500, (float)(10 + i)};
    feed(h, many, 12, t + 80);
    CHECK_EQ(count_owner1(h, "pocsag"), DEC_AUTO_MAX);
    CHECK_EQ(count_all(h), DEC_AUTO_MAX + 1);

    /* Cambio de perfil: reglas fuera y sin repoblar */
    dec_host_auto_clear(h);
    dec_host_remove_owner(h, 1);
    feed(h, many, 12, t + 90);
    CHECK_EQ(count_owner1(h, "pocsag"), 0);
    CHECK_EQ(count_all(h), 1);
    dec_host_destroy(h);
}

static void test_drift_overflow(void)
{
    printf("chan_drift: n grande con ppm > 0 (ASan detecta el desbordamiento antiguo)\n");
    const int n = 3000000; /* n·ppm·1e-6 = 300 ≫ 8 muestras de margen antiguo */
    cf32 *x = calloc((size_t)n, sizeof *x), *y = calloc((size_t)n, sizeof *y);
    CHECK(x && y);
    for (int i = 0; i < n; i++)
        x[i] = (cf32){cosf(0.01f * (float)i), sinf(0.01f * (float)i)};
    chan_drift(x, n, +100.0, y);
    chan_drift(x, n, -100.0, y);
    chan_drift(x, 4096, +100.0, y);
    CHECK(fabsf(y[100].re) <= 1.01f);
    free(x);
    free(y);
}

int main(void)
{
    test_auto();
    test_drift_overflow();
    return TEST_SUMMARY();
}
