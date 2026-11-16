/*
 * test_tv.c — Gestor de televisión (src/tv/tv.c) de extremo a extremo, sin radio.
 *
 * Se alimenta con un TS sintético (PAT, PMT, SDT, NIT con LCN, PES de vídeo H.264 con
 * SPS real de 1280×720 y PES de audio MP2 silencioso) y se examinan los mensajes que
 * se enviarían al navegador (JSON «tv»/«tv_scan», binarios tipo 5 y 6, ver
 * docs/PROTOCOLO.md):
 *   - lista de servicios con nombre, proveedor, LCN, códecs y tamaño del vídeo;
 *   - nada de vídeo hasta el primer fotograma clave; SPS/PPS antepuestos si faltan;
 *   - PTS/DTS en la cabecera; audio PCM estéreo a 48 kHz con PTS espaciados 2160 ticks;
 *   - cruce de 2^33 en el PTS y saltos del reloj: la salida nunca retrocede;
 *   - servicio solo con AC-3: sin audio y se avisa; servicio inexistente: error;
 *   - exploración de canales con un receptor simulado y reproducción en bucle de un .ts.
 * Si hay ffmpeg con libx264 se repite con un TS real codificado por él (datos de prueba).
 */
#include "test_util.h"
#include "ts_build.h"
#include "tv/tv.h"

#include <pthread.h>
#include <unistd.h>

#define MAXMSG 400
typedef struct {
    int kind[MAXMSG];
    size_t len[MAXMSG];
    uint8_t *data[MAXMSG];
    int n;
    char last_tv[16384], last_scan[16384];
    pthread_mutex_t mu;
} rec_t;

static void sink(void *ctx, int kind, const void *d, size_t n)
{
    rec_t *r = ctx;
    pthread_mutex_lock(&r->mu);
    if (kind == 0) {
        const char *s = d;
        if (n > 10 && !strncmp(s, "{\"type\":\"tv\",", 13) && n < sizeof r->last_tv) {
            memcpy(r->last_tv, d, n);
            r->last_tv[n] = 0;
        } else if (n > 10 && !strncmp(s, "{\"type\":\"tv_scan\"", 17) && n < sizeof r->last_scan) {
            memcpy(r->last_scan, d, n);
            r->last_scan[n] = 0;
        }
    } else if (r->n < MAXMSG) {
        r->kind[r->n] = kind;
        r->len[r->n] = n;
        r->data[r->n] = malloc(n);
        memcpy(r->data[r->n], d, n);
        r->n++;
    }
    pthread_mutex_unlock(&r->mu);
}

static void rec_clear(rec_t *r)
{
    pthread_mutex_lock(&r->mu);
    for (int i = 0; i < r->n; i++)
        free(r->data[i]);
    r->n = 0;
    pthread_mutex_unlock(&r->mu);
}

static int64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return (int64_t)v;
}

/* ---- generador de TS sintético ---- */
static const uint8_t SPS720[] = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xbb, 0x01, 0x10, 0x00, 0x00,
                                 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60};
static const uint8_t PPS[] = {0x68, 0xef, 0x8f, 0xcb};

static size_t make_au(uint8_t *o, bool key, bool with_sps, int fill)
{
    size_t k = 0;
    static const uint8_t sc[] = {0, 0, 0, 1};
    memcpy(o + k, sc, 4);
    k += 4;
    o[k++] = 0x09;
    o[k++] = 0xF0;
    if (with_sps) {
        memcpy(o + k, sc, 4);
        k += 4;
        memcpy(o + k, SPS720, sizeof SPS720);
        k += sizeof SPS720;
        memcpy(o + k, sc, 4);
        k += 4;
        memcpy(o + k, PPS, sizeof PPS);
        k += sizeof PPS;
    }
    memcpy(o + k, sc, 4);
    k += 4;
    o[k++] = key ? 0x65 : 0x41;
    for (int i = 0; i < fill; i++)
        o[k++] = 0x55;
    return k;
}

/* trama MP2 estéreo 48 kHz 192 kbit/s en silencio (asignación 0: solo cabecera) */
static size_t silent_mp2(uint8_t *o)
{
    memset(o, 0, 576);
    o[0] = 0xFF;
    o[1] = 0xFD;
    o[2] = 0xA4;
    o[3] = 0x00;
    return 576;
}

typedef struct {
    tsb_t b;
    uint8_t buf[1 << 18];
} gen_t;

static void put_psi(gen_t *g, tv_t *tv, bool ac3_only)
{
    uint8_t sec[1024];
    size_t l, n;
    int sid[] = {0x0101, 0x0102, 0x0103}, pid[] = {0x100, 0x200, 0x300};
    l = tsb_pat(sec, 1, 0, sid, pid, ac3_only ? 3 : 2);
    n = tsb_packets_section(&g->b, g->buf, TS_PID_PAT, sec, l);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
    uint8_t lang[] = {0x0A, 4, 's', 'p', 'a', 0};
    tsb_es_t es1[] = {{0x1B, 0x101, NULL, 0}, {0x03, 0x102, lang, sizeof lang}};
    l = tsb_pmt(sec, 0x0101, 0, 0x101, es1, 2);
    n = tsb_packets_section(&g->b, g->buf, 0x100, sec, l);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
    uint8_t ac3d[] = {0x6A, 1, 0};
    tsb_es_t es2[] = {{0x1B, 0x201, NULL, 0}, {0x06, 0x202, ac3d, sizeof ac3d}};
    tsb_es_t es3[] = {{0x1B, 0x201, NULL, 0}, {0x04, 0x202, NULL, 0}};
    l = tsb_pmt(sec, 0x0102, 0, 0x201, es3, 2);
    n = tsb_packets_section(&g->b, g->buf, 0x200, sec, l);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
    if (ac3_only) {
        l = tsb_pmt(sec, 0x0103, 0, 0x201, es2, 2);
        n = tsb_packets_section(&g->b, g->buf, 0x300, sec, l);
        tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
    }
    size_t o = tsb_sdt_begin(sec, 1, 0x22D4, 0);
    o = tsb_sdt_service(sec, o, 0x0101, 0x19, (const uint8_t *)"RTVE", 4, (const uint8_t *)"La 1", 4);
    o = tsb_sdt_service(sec, o, 0x0102, 0x19, (const uint8_t *)"Atresmedia", 10, (const uint8_t *)"Antena 3", 8);
    if (ac3_only)
        o = tsb_sdt_service(sec, o, 0x0103, 0x19, (const uint8_t *)"Prueba", 6, (const uint8_t *)"Solo AC3", 8);
    l = tsb_finish_section(sec, o - 3);
    n = tsb_packets_section(&g->b, g->buf, TS_PID_SDT, sec, l);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
    int lsid[] = {0x0101, 0x0102}, lcn[] = {1, 3};
    l = tsb_nit(sec, 0x3001, 0, "TDT", 1, 0x22D4, 578000000LL, lsid, lcn, 2);
    n = tsb_packets_section(&g->b, g->buf, TS_PID_NIT, sec, l);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
}

/* un fotograma: PES de vídeo en pid vp con PTS pts */
static void put_video(gen_t *g, tv_t *tv, int vp, int64_t pts, bool key, bool sps, int64_t pcr)
{
    uint8_t au[4096];
    size_t l = make_au(au, key, sps, key ? 1500 : 300);
    size_t n = tsb_packets_pes(&g->b, g->buf, vp, 0xE0, pts, pts - 7200, au, l, pcr, true);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
}
static void put_audio(gen_t *g, tv_t *tv, int ap, int64_t pts, int nframes)
{
    uint8_t es[4 * 576];
    size_t l = 0;
    for (int i = 0; i < nframes; i++)
        l += silent_mp2(es + l);
    size_t n = tsb_packets_pes(&g->b, g->buf, ap, 0xC0, pts, -1, es, l, -1, false);
    tv_ts_in(tv, g->buf, (int)(n / TS_PKT));
}

static int count_kind(rec_t *r, int kind)
{
    int c = 0;
    for (int i = 0; i < r->n; i++)
        c += r->kind[i] == kind;
    return c;
}

static void test_synthetic(void)
{
    static rec_t r;
    memset(&r, 0, sizeof r);
    pthread_mutex_init(&r.mu, NULL);
    tv_t *tv = tv_create(NULL, NULL);
    CHECK(tv != NULL);
    tv_set_sink(tv, sink, &r);
    gen_t *g = calloc(1, sizeof *g);

    put_psi(g, tv, true);
    tv_send_snapshot(tv, -1);
    CHECK(strstr(r.last_tv, "\"name\":\"La 1\"") != NULL);
    CHECK(strstr(r.last_tv, "\"name\":\"Antena 3\"") != NULL);
    CHECK(strstr(r.last_tv, "\"provider\":\"Atresmedia\"") != NULL);
    CHECK(strstr(r.last_tv, "\"lcn\":3") != NULL);
    CHECK(strstr(r.last_tv, "\"codec\":\"h264\"") != NULL);
    CHECK(strstr(r.last_tv, "\"lang\":\"spa\"") != NULL);
    CHECK(strstr(r.last_tv, "\"current\":null") != NULL);
    CHECK(strstr(r.last_tv, "\"network\":\"TDT\"") != NULL);

    char err[200];
    CHECK(tv_cmd_service(tv, 0x9999, err, sizeof err) < 0);
    CHECK(tv_cmd_service(tv, 0x0101, err, sizeof err) == 0);

    /* empieza por fotogramas P (sin clave): no debe salir nada hasta la clave */
    int64_t base = 900000;
    put_video(g, tv, 0x101, base, false, false, 27000000LL);
    put_video(g, tv, 0x101, base + 3600, false, false, -1);
    CHECK_EQ(count_kind(&r, 5), 0);
    /* clave sin SPS/PPS y aún no se han visto: tampoco */
    put_video(g, tv, 0x101, base + 7200, true, false, -1);
    CHECK_EQ(count_kind(&r, 5), 0);
    /* clave con SPS/PPS → sale; el siguiente PES cierra la AU anterior */
    put_video(g, tv, 0x101, base + 10800, true, true, -1);
    put_video(g, tv, 0x101, base + 14400, false, false, -1);
    put_video(g, tv, 0x101, base + 18000, false, false, -1);
    /* el PES de vídeo (longitud 0) se cierra con el siguiente y la AU espera a ese cierre: la clave sale al llegar
     * el 2.º fotograma posterior (2 fotogramas de latencia, 80 ms a 25 fps) */
    CHECK_EQ(count_kind(&r, 5), 1);
    int i0 = 0;
    while (i0 < r.n && r.kind[i0] != 5)
        i0++;
    const uint8_t *m = r.data[i0];
    CHECK_EQ(m[0], 5);
    CHECK_EQ(m[1] & 1, 1);
    CHECK_EQ(m[2], 1); /* H.264 */
    int64_t first_pts = rd64(m + 4);
    CHECK_EQ(rd64(m + 4) - rd64(m + 12), 7200);
    /* la AU empieza por AUD y lleva SPS (0x67) y PPS (0x68) */
    CHECK(!memcmp(m + 20, "\x00\x00\x00\x01\x09", 5));
    bool has_sps = false, has_pps = false;
    for (size_t i = 20; i + 5 < r.len[i0]; i++) {
        if (!memcmp(m + i, "\x00\x00\x00\x01\x67", 5)) has_sps = true;
        if (!memcmp(m + i, "\x00\x00\x00\x01\x68", 5)) has_pps = true;
    }
    CHECK(has_sps && has_pps);
    /* siguiente GOP: clave SIN SPS/PPS en el flujo → el gestor los antepone */
    rec_clear(&r);
    for (int k = 0; k < 6; k++)
        put_video(g, tv, 0x101, base + 21600 + 3600 * k, k == 0, false, -1);
    CHECK(count_kind(&r, 5) >= 5);
    int ik = 0;
    while (ik < r.n && !(r.kind[ik] == 5 && (r.data[ik][1] & 1)))
        ik++; /* los P pendientes del bloque anterior salen primero */
    CHECK(ik < r.n);
    m = r.data[ik];
    CHECK(!memcmp(m + 20, "\x00\x00\x00\x01\x67", 5)); /* SPS delante (guardado) */
    int64_t prev = rd64(m + 4);
    CHECK(prev > first_pts);
    for (int i = ik + 1; i < r.n; i++)
        if (r.kind[i] == 5) {
            int64_t p = rd64(r.data[i] + 4);
            CHECK_EQ(p - prev, 3600);
            CHECK_EQ(r.data[i][1] & 1, 0);
            prev = p;
        }
    /* nº de secuencia (byte 3) consecutivo */
    for (int i = ik + 1; i < r.n; i++)
        if (r.kind[i] == 5 && r.kind[i - 1] == 5)
            CHECK_EQ((r.data[i][3] - r.data[i - 1][3]) & 255, 1);

    /* tamaño del vídeo detectado por el SPS aparece en el JSON */
    tv_send_snapshot(tv, -1);
    CHECK(strstr(r.last_tv, "\"w\":1280,\"h\":720") != NULL);
    CHECK(strstr(r.last_tv, "\"current\":257") != NULL);

    /* audio: 3 PES de 2 tramas */
    rec_clear(&r);
    int64_t apts = base + 20000;
    for (int k = 0; k < 3; k++)
        put_audio(g, tv, 0x102, apts + k * 4320, 2);
    CHECK_EQ(count_kind(&r, 6), 5); /* la 6.ª trama espera a ver la sincronía de la siguiente */
    m = r.data[0];
    CHECK_EQ(m[0], 6);
    CHECK_EQ(m[1], 2);
    CHECK_EQ(m[4] | (m[5] << 8), 48000);
    CHECK_EQ(r.len[0], 16 + 1152 * 2 * 2);
    /* mismo reloj que el vídeo: la diferencia de PTS de origen se conserva */
    CHECK_EQ(rd64(m + 8) - first_pts, apts - (base + 10800));
    for (int i = 1; i < r.n; i++)
        CHECK_EQ(rd64(r.data[i] + 8) - rd64(r.data[i - 1] + 8), 2160);

    /* cruce de 2^33 en el PTS (vídeo): la salida sigue creciendo sin saltos */
    tv_cmd_service(tv, 0x0101, err, sizeof err);
    rec_clear(&r);
    int64_t w0 = (1LL << 33) - 3 * 3600;
    put_video(g, tv, 0x101, w0, true, true, -1);
    for (int k = 1; k < 8; k++)
        put_video(g, tv, 0x101, (w0 + 3600 * k) & ((1LL << 33) - 1), false, false, -1);
    prev = -1;
    int nv = 0;
    for (int i = 0; i < r.n; i++)
        if (r.kind[i] == 5) {
            int64_t p = rd64(r.data[i] + 4);
            if (prev >= 0)
                CHECK_EQ(p - prev, 3600);
            prev = p;
            nv++;
        }
    CHECK(nv >= 6);

    /* salto real del reloj (10 s hacia atrás): se abre época nueva y el tiempo no retrocede */
    int64_t last = prev;
    rec_clear(&r);
    int64_t j0 = 100000;
    put_video(g, tv, 0x101, j0, true, true, -1);
    for (int k = 1; k < 5; k++)
        put_video(g, tv, 0x101, j0 + 3600 * k, false, false, -1);
    prev = last;
    for (int i = 0; i < r.n; i++)
        if (r.kind[i] == 5) {
            int64_t p = rd64(r.data[i] + 4);
            CHECK(p > prev);
            prev = p;
        }
    CHECK(count_kind(&r, 5) >= 4);

    /* servicio con AC-3: vídeo sí, audio no (solo MP2 se decodifica) */
    CHECK(tv_cmd_service(tv, 0x0103, err, sizeof err) == 0);
    tv_send_snapshot(tv, -1);
    CHECK(strstr(r.last_tv, "\"audio_ok\":false") != NULL);
    CHECK(strstr(r.last_tv, "\"codec\":\"ac3\"") != NULL);
    tv_cmd_stop(tv);
    tv_send_snapshot(tv, -1);
    CHECK(strstr(r.last_tv, "\"current\":null") != NULL);

    tv_counters_t c;
    tv_counters(tv, &c);
    CHECK(c.video_frames > 10);
    CHECK_EQ(c.audio_frames, 5);
    rec_clear(&r);
    free(g);
    tv_destroy(tv);
}

/* ---- exploración con un receptor simulado ---- */
typedef struct {
    tv_t *tv;
    gen_t *g;
    int tuned[64];
    int ntuned, untuned;
} fake_t;

static fake_t F;

static int fake_tune(void *app, int64_t f, char *err, size_t el)
{
    (void)app;
    (void)err;
    (void)el;
    F.tuned[F.ntuned++ % 64] = tv_freq_channel(f);
    int ch = tv_freq_channel(f);
    if (ch == 22 || ch == 24) {
        tv_rx_stats_t st = {.lock = true, .mode_k = 8, .guard_den = 4, .qam = 64, .cr_num = 2, .cr_den = 3, .mer_db = 20.5f};
        tv_set_rx_stats(F.tv, &st);
        put_psi(F.g, F.tv, false);
    }
    return 7;
}
static void fake_untune(void *app, int inst)
{
    (void)app;
    (void)inst;
    F.untuned++;
}

static void test_scan(void)
{
    static rec_t r;
    memset(&r, 0, sizeof r);
    pthread_mutex_init(&r.mu, NULL);
    tv_ops_t ops = {.app = NULL, .tune = fake_tune, .untune = fake_untune, .lock_wait_s = 0.4, .psi_wait_s = 0.5};
    tv_t *tv = tv_create(NULL, &ops);
    tv_set_sink(tv, sink, &r);
    memset(&F, 0, sizeof F);
    F.tv = tv;
    F.g = calloc(1, sizeof(gen_t));
    CHECK_EQ(tv_channel_freq(21), 474000000LL);
    CHECK_EQ(tv_channel_freq(48), 690000000LL);
    CHECK_EQ(tv_freq_channel(578000000LL), 34);
    CHECK_EQ(tv_freq_channel(100000000LL), 0);
    char err[200];
    CHECK(tv_cmd_scan(tv, tv_channel_freq(21), tv_channel_freq(25), err, sizeof err) == 0);
    CHECK(tv_cmd_scan(tv, 0, 0, err, sizeof err) < 0); /* ya hay una en curso */
    for (int i = 0; i < 100; i++) {
        usleep(100000);
        pthread_mutex_lock(&r.mu);
        bool done = strstr(r.last_scan, "\"done\":true") != NULL;
        pthread_mutex_unlock(&r.mu);
        if (done)
            break;
    }
    pthread_mutex_lock(&r.mu);
    CHECK(strstr(r.last_scan, "\"done\":true") != NULL);
    CHECK(strstr(r.last_scan, "\"running\":false") != NULL);
    CHECK(strstr(r.last_scan, "\"ch\":22,") != NULL);
    CHECK(strstr(r.last_scan, "\"ch\":24,") != NULL);
    CHECK(strstr(r.last_scan, "\"ch\":23,\"mer") == NULL);
    CHECK(strstr(r.last_scan, "\"names\":[\"La 1\",\"Antena 3\"]") != NULL);
    pthread_mutex_unlock(&r.mu);
    CHECK_EQ(F.ntuned, 5);
    CHECK(F.untuned >= 5);
    /* tras tv_cmd_tune, un fichero no sintoniza; el servicio no existe */
    tv_destroy(tv);
    free(F.g);
}

/* ---- fichero .ts en bucle ---- */
static void test_file(void)
{
    static rec_t r;
    memset(&r, 0, sizeof r);
    pthread_mutex_init(&r.mu, NULL);
    /* TS de ~0,4 s: PSI, 10 fotogramas a 25 fps y 17 tramas de audio, con PCR en el vídeo */
    char path[200];
    snprintf(path, sizeof path, "/tmp/test_tv_%d.ts", (int)getpid());
    tv_t *tv0 = tv_create(NULL, NULL); /* solo se usa como destino para generar los paquetes */
    (void)tv0;
    FILE *f = fopen(path, "wb");
    gen_t *g = calloc(1, sizeof *g);
    uint8_t sec[1024];
    size_t l, n;
    int sid[] = {0x0101}, pid[] = {0x100};
    l = tsb_pat(sec, 1, 0, sid, pid, 1);
    n = tsb_packets_section(&g->b, g->buf, TS_PID_PAT, sec, l);
    fwrite(g->buf, 1, n, f);
    uint8_t lang[] = {0x0A, 4, 's', 'p', 'a', 0};
    tsb_es_t es1[] = {{0x1B, 0x101, NULL, 0}, {0x03, 0x102, lang, sizeof lang}};
    l = tsb_pmt(sec, 0x0101, 0, 0x101, es1, 2);
    n = tsb_packets_section(&g->b, g->buf, 0x100, sec, l);
    fwrite(g->buf, 1, n, f);
    size_t o = tsb_sdt_begin(sec, 1, 0x22D4, 0);
    o = tsb_sdt_service(sec, o, 0x0101, 0x19, (const uint8_t *)"RTVE", 4, (const uint8_t *)"La 1", 4);
    l = tsb_finish_section(sec, o - 3);
    n = tsb_packets_section(&g->b, g->buf, TS_PID_SDT, sec, l);
    fwrite(g->buf, 1, n, f);
    for (int k = 0; k < 10; k++) {
        uint8_t au[4096];
        size_t al = make_au(au, k == 0, k == 0, 800);
        int64_t pts = 90000 + 3600LL * k;
        n = tsb_packets_pes(&g->b, g->buf, 0x101, 0xE0, pts, pts, au, al, 27000000LL / 25 * k, true);
        fwrite(g->buf, 1, n, f);
        uint8_t es[576 * 2];
        size_t el = silent_mp2(es);
        el += silent_mp2(es + el);
        if (k < 8) {
            n = tsb_packets_pes(&g->b, g->buf, 0x102, 0xC0, pts + 1000, -1, es, el, -1, false);
            fwrite(g->buf, 1, n, f);
        }
    }
    fclose(f);
    free(g);
    tv_destroy(tv0);

    tv_t *tv = tv_create(NULL, NULL);
    tv_set_sink(tv, sink, &r);
    char err[200];
    CHECK(tv_play_file(tv, "/nonexistent.ts", err, sizeof err) < 0);
    CHECK(tv_play_file(tv, path, err, sizeof err) == 0);
    usleep(300000);
    CHECK(tv_cmd_service(tv, 0x0101, err, sizeof err) == 0);
    CHECK(tv_cmd_tune(tv, 578000000LL, 0, err, sizeof err) < 0); /* en modo fichero no se sintoniza */
    usleep(1500000); /* >2 vueltas del bucle */
    tv_cmd_stop(tv);
    tv_counters_t c;
    tv_counters(tv, &c);
    CHECK(c.video_frames >= 10);
    CHECK(c.key_frames >= 2); /* ≥ 2 vueltas */
    /* el tiempo sigue creciendo al volver al principio */
    int64_t prev = -1;
    int back = 0, nv = 0;
    pthread_mutex_lock(&r.mu);
    for (int i = 0; i < r.n; i++)
        if (r.kind[i] == 5) {
            int64_t p = rd64(r.data[i] + 4);
            if (prev >= 0 && p <= prev)
                back++;
            prev = p;
            nv++;
        }
    pthread_mutex_unlock(&r.mu);
    CHECK(nv >= 10);
    CHECK_EQ(back, 0);
    rec_clear(&r);
    tv_destroy(tv);
    unlink(path);
}

/* ---- TS real codificado por ffmpeg (si está) ---- */
static void test_ffmpeg_ts(void)
{
    if (system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264") != 0) {
        printf("  (sin ffmpeg/libx264: se omite el TS real)\n");
        return;
    }
    char path[200], cmd[1500];
    snprintf(path, sizeof path, "/tmp/test_tv_ff_%d.ts", (int)getpid());
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25:duration=2 -f lavfi -i sine=frequency=440:duration=2"
             " -c:v libx264 -preset ultrafast -g 25 -bf 2 -b:v 800k -c:a mp2 -b:a 192k -ar 48000 -ac 2"
             " -metadata service_name='Canal Ñandú' -metadata service_provider='Ñoño TV' -mpegts_service_id 0x2001 %s",
             path);
    if (system(cmd) != 0) {
        printf("  (ffmpeg no pudo generar el TS; se omite)\n");
        return;
    }
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    size_t got = fread(d, 1, (size_t)sz, f);
    fclose(f);
    unlink(path);
    static rec_t r;
    memset(&r, 0, sizeof r);
    pthread_mutex_init(&r.mu, NULL);
    tv_t *tv = tv_create(NULL, NULL);
    tv_set_sink(tv, sink, &r);
    size_t pk = got / TS_PKT;
    tv_ts_in(tv, d, (int)(pk / 5)); /* primero PSI y los primeros fotogramas */
    tv_send_snapshot(tv, -1);
    CHECK(strstr(r.last_tv, "\"name\":\"Canal Ñandú\"") != NULL);
    CHECK(strstr(r.last_tv, "\"provider\":\"Ñoño TV\"") != NULL);
    CHECK(strstr(r.last_tv, "\"sid\":8193") != NULL);
    char err[200];
    CHECK(tv_cmd_service(tv, 0x2001, err, sizeof err) == 0);
    tv_ts_in(tv, d + (pk / 5) * TS_PKT, (int)(pk - pk / 5));
    tv_counters_t c;
    tv_counters(tv, &c);
    printf("  ffmpeg TS: %lu paquetes, %lu fotogramas (%lu clave), %lu tramas de audio\n", c.ts_packets, c.video_frames,
           c.key_frames, c.audio_frames);
    CHECK(c.video_frames >= 20);
    CHECK(c.key_frames >= 1);
    CHECK(c.audio_frames >= 60);
    tv_send_snapshot(tv, -1);
    CHECK(strstr(r.last_tv, "\"w\":640,\"h\":360") != NULL);
    /* PTS de vídeo a 25 fps: los AU salen en orden de DTS (B-frames), el PTS no es monótono pero el DTS sí */
    int64_t pd = -1;
    int ok = 1;
    for (int i = 0; i < r.n; i++)
        if (r.kind[i] == 5) {
            int64_t dts = rd64(r.data[i] + 12);
            if (pd >= 0 && dts <= pd)
                ok = 0;
            pd = dts;
        }
    CHECK(ok);
    free(d);
    rec_clear(&r);
    tv_destroy(tv);
}

int main(void)
{
    test_synthetic();
    test_scan();
    test_file();
    test_ffmpeg_ts();
    return TEST_SUMMARY();
}
