/*
 * test_host.c — Marco de decodificadores de extremo a extremo (sin servidor):
 *   mod_adsb → canal con ruido → dec_host_feed → hilo de la instancia → evento JSON.
 * También: estados "waiting"/"error", catálogo, retune, eliminación y límite de eventos.
 */
#include "test_util.h"

#include "decoders/host.h"
#include "tx/mod.h"
#include "util/json.h"

#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static evs_t g_ev;

static void on_event(void *ctx, const char *line)
{
    (void)ctx;
    pthread_mutex_lock(&lock);
    tu_collect(&g_ev, line, strlen(line));
    pthread_mutex_unlock(&lock);
}

static void feed_all(dec_host_t *h, const cf32 *x, const int16_t *q, int n, double fs, int64_t center)
{
    const int B = 40000; /* 20 ms a 2 Msps */
    for (int off = 0; off < n; off += B) {
        int c = MIN(B, n - off);
        dec_host_feed(h, q + 2 * (size_t)off, x + off, (unsigned)c, fs, center, now_wall());
        dec_host_flush(h, 5.0); /* el test alimenta más rápido que el tiempo real */
    }
}

static void test_adsb_via_host(void)
{
    printf("ADS-B: modulador → ruido → host → evento y estado\n");
    const modulator_desc_t *m = modulator_find("adsb");
    CHECK(m != NULL);
    iqbuf_t iq = {0};
    char err[200] = "";
    CHECK(m->modulate(NULL, &iq, err, sizeof err) == 0);
    CHECK_NEAR(iq.rate, 2e6, 0.5);
    CHECK(!m->air_ok);

    uint32_t seed = 12345;
    chan_awgn(iq.s, iq.n, 18.0, &seed);
    chan_cfo(iq.s, iq.n, iq.rate, 30e3); /* un desvío no afecta: el decodificador usa el módulo */
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)iq.n);
    cf32_to_q11(iq.s, iq.n, 1.0, q);

    dec_host_t *h = dec_host_create(NULL);
    dec_host_on_event(h, on_event, NULL);
    dec_host_set_location(h, true, 52.0, 4.0);
    int id = dec_host_add(h, "adsb", 1090000000, NULL, 0, err, sizeof err);
    CHECK(id > 0);
    feed_all(h, iq.s, q, iq.n, 2e6, 1090000000);

    pthread_mutex_lock(&lock);
    CHECK(ev_has(&g_ev, "\"type\":\"dec\""));
    CHECK(ev_has(&g_ev, "\"proto\":\"adsb\""));
    CHECK(ev_has(&g_ev, "\"icao\":\"4840D6\""));
    CHECK(ev_has(&g_ev, "\"cs\":\"KLM1023\""));
    CHECK(ev_has(&g_ev, "\"text\":\"Nuevo 4840D6 KLM1023\""));
    CHECK(ev_has(&g_ev, "\"kind\":\"new\""));
    CHECK(!ev_has(&g_ev, "\"hex\":"));
    /* Por defecto un evento por aeronave nueva / cambio, no uno por trama (8 tramas) */
    printf("  eventos recibidos: %d\n", evs_count(&g_ev));
    CHECK_EQ(evs_count(&g_ev), 1);
    /* cada línea debe ser JSON válido */
    jtok_t *t = malloc(sizeof(jtok_t) * 256);
    char *p = g_ev.data;
    int bad = 0;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (json_parse(p, len, t, 256) < 1)
            bad++;
        p = nl ? nl + 1 : NULL;
    }
    CHECK_EQ(bad, 0);
    free(t);
    pthread_mutex_unlock(&lock);

    dec_host_request_state(h);
    dec_host_flush(h, 5.0);
    sb_t sb = {0};
    CHECK(dec_host_inst_state(h, id, &sb));
    printf("  estado: %.300s\n", sb.data ? sb.data : "(vacío)");
    CHECK(strstr(sb.data, "\"ac\":[") && strstr(sb.data, "\"icao\":\"4840D6\"") && strstr(sb.data, "\"rx\":true"));
    CHECK(strstr(sb.data, "\"cs\":\"KLM1023\"") != NULL);
    sb_free(&sb);

    char st[16], e2[160];
    CHECK(dec_host_inst_info(h, id, st, sizeof st, e2, sizeof e2) && strcmp(st, "ok") == 0);

    /* lista y catálogo */
    dec_host_list_json(h, &sb);
    CHECK(strstr(sb.data, "\"type\":\"dec_list\"") && strstr(sb.data, "\"proto\":\"adsb\"") &&
          strstr(sb.data, "\"state\":\"ok\""));
    sb_free(&sb);

    /* tasa incompatible → error con mensaje claro */
    evs_free(&g_ev);
    cf32 *x2 = calloc(40000, sizeof(cf32));
    int16_t *q2 = calloc(80000, sizeof(int16_t));
    feed_all(h, x2, q2, 40000, 2.4e6, 1090000000);
    CHECK(dec_host_inst_info(h, id, st, sizeof st, e2, sizeof e2));
    CHECK(strcmp(st, "error") == 0);
    CHECK(strstr(e2, "exactamente 2 Msps") != NULL);
    printf("  con 2,4 Msps: %s — %s\n", st, e2);
    /* vuelve a 2 Msps y se recupera */
    feed_all(h, x2, q2, 40000, 2e6, 1090000000);
    CHECK(dec_host_inst_info(h, id, st, sizeof st, e2, sizeof e2) && strcmp(st, "ok") == 0);
    free(x2);
    free(q2);

    CHECK(dec_host_remove(h, id) == 0);
    CHECK(dec_host_remove(h, id) < 0);
    dec_host_destroy(h);

    /* Con opts {"raw":true}: un evento por trama, con hex, posición y velocidad */
    evs_free(&g_ev);
    h = dec_host_create(NULL);
    dec_host_on_event(h, on_event, NULL);
    dec_host_set_location(h, true, 52.0, 4.0);
    id = dec_host_add(h, "adsb", 1090000000, "{\"raw\":true}", 0, err, sizeof err);
    CHECK(id > 0);
    feed_all(h, iq.s, q, iq.n, 2e6, 1090000000);
    pthread_mutex_lock(&lock);
    printf("  eventos con raw: %d\n", evs_count(&g_ev));
    CHECK(evs_count(&g_ev) >= 8);
    CHECK(ev_has(&g_ev, "\"hex\":\"8D4840D6"));
    CHECK(ev_has(&g_ev, "\"lat\":52.2"));
    pthread_mutex_unlock(&lock);
    dec_host_request_state(h);
    dec_host_flush(h, 5.0);
    CHECK(dec_host_inst_state(h, id, &sb));
    CHECK(strstr(sb.data, "\"fps\":") && strstr(sb.data, "\"fix1\":") && strstr(sb.data, "\"fix2\":") &&
          strstr(sb.data, "\"crc\":"));
    sb_free(&sb);
    dec_host_destroy(h);
    free(q);
    iqbuf_free(&iq);
    evs_free(&g_ev);
}

static void test_states(void)
{
    printf("Estados: waiting / error (stub) y retune\n");
    dec_host_t *h = dec_host_create(NULL);
    char err[200] = "";
    CHECK(dec_host_add(h, "noexiste", 1, NULL, 0, err, sizeof err) < 0);
    CHECK(strstr(err, "desconocido") != NULL);
    CHECK(dec_host_add(h, "ais", 0, NULL, 0, err, sizeof err) > 0); /* usa la frecuencia típica */
    CHECK(dec_host_add(h, "ais", 162025000, "{\"x\":1", 0, err, sizeof err) < 0); /* opts inválidas */
    int in_band = dec_host_add(h, "ais", 161975000, NULL, 0, err, sizeof err);
    int out_band = dec_host_add(h, "ais", 150000000, "{\"a\":1}", 1, err, sizeof err);
    CHECK(in_band > 0 && out_band > 0);

    const int n = 40000;
    cf32 *x = calloc((size_t)n, sizeof(cf32));
    int16_t *q = calloc((size_t)n * 2, sizeof(int16_t));
    feed_all(h, x, q, n, 1e6, 162000000);
    char st[16], e2[160];
    CHECK(dec_host_inst_info(h, in_band, st, sizeof st, e2, sizeof e2));
    /* en banda: procesa (ok si el decodificador existe, error si aún es un esqueleto) */
    CHECK(strcmp(st, "waiting") != 0);
    CHECK(dec_host_inst_info(h, out_band, st, sizeof st, e2, sizeof e2));
    CHECK(strcmp(st, "waiting") == 0);

    /* retune del que estaba fuera de banda: pasa a procesar (deja de estar en espera) */
    CHECK(dec_host_retune(h, out_band, 162025000) == 0);
    feed_all(h, x, q, n, 1e6, 162000000);
    CHECK(dec_host_inst_info(h, out_band, st, sizeof st, e2, sizeof e2) && strcmp(st, "waiting") != 0);

    /* narrow con captura menor que su tasa de canal: error claro */
    int vdl = dec_host_add(h, "dvbt", 474000000, NULL, 0, err, sizeof err);
    feed_all(h, x, q, n, 1e6, 474000000);
    CHECK(dec_host_inst_info(h, vdl, st, sizeof st, e2, sizeof e2) && strcmp(st, "error") == 0);
    printf("  dvbt a 1 Msps: %s\n", e2);

    dec_host_remove_owner(h, 1);
    sb_t sb = {0};
    dec_host_list_json(h, &sb);
    CHECK(strstr(sb.data, "\"owner\":1") == NULL);
    CHECK(strstr(sb.data, "\"owner\":0") != NULL);
    sb_free(&sb);

    dec_host_catalog_json(&sb);
    jtok_t *t = malloc(sizeof(jtok_t) * 8192);
    int nt = json_parse(sb.data, sb.len, t, 8192);
    CHECK(nt > 0);
    int l = json_obj_get(sb.data, t, 0, "list");
    int ndec = 0;
    while (DECODERS[ndec])
        ndec++;
    CHECK(l >= 0 && t[l].size == ndec); /* el catálogo lista todos los registrados, sin número fijo */
    CHECK(strstr(sb.data, "\"id\":\"tdt\"") != NULL);
    CHECK(strstr(sb.data, "\"id\":\"adsb\"") && strstr(sb.data, "\"kind\":\"wide\"") &&
          strstr(sb.data, "\"wide_rate\":2000000"));
    free(t);
    sb_free(&sb);
    free(x);
    free(q);
    dec_host_destroy(h);
}

static void test_limit(void)
{
    printf("Límite de eventos por instancia (200/s)\n");
    const modulator_desc_t *m = modulator_find("adsb");
    iqbuf_t iq = {0};
    char err[200] = "";
    /* 100 repeticiones × 4 tramas = 400 eventos casi instantáneos al alimentar sin tiempo real */
    CHECK(m->modulate("{\"repeat\":100}", &iq, err, sizeof err) == 0);
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)iq.n);
    cf32_to_q11(iq.s, iq.n, 1.0, q);
    evs_free(&g_ev);
    dec_host_t *h = dec_host_create(NULL);
    dec_host_on_event(h, on_event, NULL);
    int id = dec_host_add(h, "adsb", 1090000000, "{\"raw\":true}", 0, err, sizeof err);
    CHECK(id > 0);
    feed_all(h, iq.s, q, iq.n, 2e6, 1090000000);
    sb_t sb = {0};
    dec_host_list_json(h, &sb);
    pthread_mutex_lock(&lock);
    int n = evs_count(&g_ev);
    pthread_mutex_unlock(&lock);
    printf("  400 tramas → %d eventos; lista: %.200s\n", n, sb.data);
    CHECK(n >= 190 && n < 400);
    CHECK(strstr(sb.data, "\"drops\":0,") == NULL);
    sb_free(&sb);
    dec_host_destroy(h);
    free(q);
    iqbuf_free(&iq);
    evs_free(&g_ev);
}

int main(void)
{
    test_adsb_via_host();
    test_states();
    test_limit();
    return TEST_SUMMARY();
}
