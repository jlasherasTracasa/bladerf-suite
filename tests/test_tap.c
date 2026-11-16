/*
 * test_tap.c — Toma de IQ del motor para tx_cal (engine_capture_iq / engine_wait_tuned).
 * Motor real sobre una fuente de fichero: el hilo DSP rellena la toma mientras el
 * hilo de prueba espera, sin que el hilo DSP deje de producir bloques.
 */
#include "test_util.h"

#include "engine.h"
#include "tx/mod.h"
#include "tx/tx.h"

#include <unistd.h>

int main(void)
{
    /* Un fichero SigMF con un tono a 100 kHz, 1 Msps */
    const char *prefix = "/tmp/test_tap_src";
    iqbuf_t iq = {0};
    iq.rate = 1e6;
    iq.n = 200000;
    iq.s = calloc(iq.n, sizeof(cf32));
    for (int i = 0; i < iq.n; i++)
        iq.s[i] = (cf32){0.5f * cosf(2.f * (float)M_PI * 0.1f * (float)i), 0.5f * sinf(2.f * (float)M_PI * 0.1f * (float)i)};
    char err[300] = "";
    CHECK(tx_write_sigmf(prefix, &iq, 1, 433920000, "tono de prueba", err, sizeof err) == 0);

    source_t *src = source_open_file("/tmp/test_tap_src.sigmf-meta", 0, 0, err, sizeof err);
    CHECK(src != NULL);
    if (!src)
        return TEST_SUMMARY();
    server_callbacks_t cb = {0};
    server_t *srv = server_create("127.0.0.1", 0, "web", "/tmp", &cb, NULL, err, sizeof err);
    CHECK(srv != NULL);
    engine_cfg_t cfg = {.center_hz = 433920000, .rate = 1000000, .fft_size = 1024, .fps = 20, .det_snr = 10};
    engine_t *e = engine_create(src, srv, &cfg, false, "/tmp");
    CHECK(e != NULL);
    CHECK(engine_start(e) == 0);

    CHECK(engine_wait_tuned(e, 433920000, 1000000, 3.0));
    unsigned n = 65536;
    int16_t *buf = malloc(sizeof(int16_t) * 2 * n);
    CHECK(engine_capture_iq(e, 433920000, 1000000, buf, n, 5.0, err, sizeof err) == 0);
    /* El tono llega entero: amplitud ≈ 0,5·2048 */
    int maxv = 0;
    for (unsigned i = 0; i < 2 * n; i++)
        maxv = MAX(maxv, abs(buf[i]));
    CHECK(maxv > 900 && maxv < 1100);
    /* Con otra frecuencia/tasa pide error, no se cuelga */
    CHECK(engine_capture_iq(e, 100000000, 1000000, buf, n, 1.0, err, sizeof err) < 0);
    /* Capturas repetidas y concurrentes con el hilo DSP */
    for (int k = 0; k < 3; k++)
        CHECK(engine_capture_iq(e, 433920000, 1000000, buf, 4096, 5.0, err, sizeof err) == 0);

    free(buf);
    engine_destroy(e);
    server_destroy(srv);
    source_close(src);
    iqbuf_free(&iq);
    return TEST_SUMMARY();
}
