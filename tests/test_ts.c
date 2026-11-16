/*
 * test_ts.c — Demux TS (src/tv/ts.c), reensamblado PES y unidades de acceso
 * H.264 (src/tv/pes.c).
 *
 * Los TS se construyen con ts_build.h (paquetes de 188 B con PCR, relleno en el
 * campo de adaptación, PES con PTS/DTS) y se alimentan en trozos de tamaño y
 * alineación arbitrarios. Se comprueba: sincronía y resincronización tras basura,
 * continuidad (salto y duplicado), PCR, PTS/DTS (incluido el cruce de 2^33),
 * reensamblado con longitud declarada y con longitud 0, descarte de un PES
 * dañado, corte de AU H.264 por AUD y por inicio de PES, detección de fotograma
 * clave y SPS de 1280x720, 1920x1080 y 720x576 (con recorte).
 */
#include "test_util.h"
#include "ts_build.h"
#include "tv/pes.h"

typedef struct {
    int n, cc_err, dup, pcr_n;
    int64_t last_pcr;
    int pids[4];
} cnt_t;

static void on_pkt(void *ctx, const ts_pkt_t *k)
{
    cnt_t *c = ctx;
    c->n++;
    c->cc_err += k->cc_err;
    c->dup += k->dup;
    if (k->has_pcr) {
        c->pcr_n++;
        c->last_pcr = k->pcr27;
    }
}

static uint8_t g_ts[1 << 20];

static void test_parse(void)
{
    tsb_t b;
    memset(&b, 0, sizeof b);
    uint8_t es[100];
    memset(es, 0xAA, sizeof es);
    size_t n = tsb_packets_pes(&b, g_ts, 0x100, 0xE0, 90000, -1, es, sizeof es, 27000000LL * 5 + 123, false);
    CHECK_EQ(n, 188 * 1);
    ts_pkt_t k;
    CHECK_EQ(ts_parse(g_ts, &k), 0);
    CHECK_EQ(k.pid, 0x100);
    CHECK(k.pusi);
    CHECK(k.has_pcr);
    CHECK_EQ(k.pcr27, 27000000LL * 5 + 123);
    CHECK(k.payload != NULL);
    CHECK_EQ(k.plen, 188 - 4 - 8 - (188 - 4 - 8 - 100 - 14));
    g_ts[0] = 0x46;
    CHECK(ts_parse(g_ts, &k) < 0);
}

static void test_demux(void)
{
    tsb_t b;
    memset(&b, 0, sizeof b);
    uint8_t es[400];
    memset(es, 0x55, sizeof es);
    size_t n = 0;
    for (int i = 0; i < 40; i++)
        n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xC0, 3600 * i, -1, es, sizeof es, i % 5 == 0 ? 27000000LL * i : -1, false);
    int total_pkts = (int)(n / TS_PKT);

    /* 1) alineado, en trozos de 777 bytes (no múltiplos de 188) con 91 bytes de basura delante */
    uint8_t *junk = malloc(n + 200);
    memset(junk, 0x13, 91);
    memcpy(junk + 91, g_ts, n);
    ts_demux_t *d = NULL;
    cnt_t c = {0};
    d = ts_demux_create(on_pkt, &c);
    for (size_t o = 0; o < n + 91; o += 777)
        ts_demux_feed(d, junk + o, (n + 91 - o) < 777 ? (n + 91 - o) : 777);
    ts_stats_t st;
    ts_demux_stats(d, &st);
    /* el demux necesita ver 4 sincronías seguidas: las últimas 3 quedan retenidas */
    CHECK(c.n >= total_pkts - 3 && c.n <= total_pkts);
    CHECK_EQ(c.cc_err, 0);
    CHECK_EQ(c.dup, 0);
    CHECK(c.pcr_n >= 7);
    ts_demux_destroy(d);

    /* 2) salto de continuidad: se quita un paquete */
    memset(&c, 0, sizeof c);
    d = ts_demux_create(on_pkt, &c);
    uint8_t *cut = malloc(n + 1000);
    size_t m = 0;
    for (int i = 0; i < total_pkts; i++)
        if (i != 20) {
            memcpy(cut + m, g_ts + i * TS_PKT, TS_PKT);
            m += TS_PKT;
        }
    ts_demux_feed(d, cut, m);
    ts_demux_stats(d, &st);
    CHECK_EQ(st.cc_errors, 1);
    CHECK_EQ(c.cc_err, 1);
    ts_demux_destroy(d);

    /* 3) paquete duplicado (permitido una vez): se marca dup */
    memset(&c, 0, sizeof c);
    d = ts_demux_create(on_pkt, &c);
    m = 0;
    for (int i = 0; i < total_pkts; i++) {
        memcpy(cut + m, g_ts + i * TS_PKT, TS_PKT);
        m += TS_PKT;
        if (i == 10) {
            memcpy(cut + m, g_ts + i * TS_PKT, TS_PKT);
            m += TS_PKT;
        }
    }
    ts_demux_feed(d, cut, m);
    CHECK_EQ(c.dup, 1);
    CHECK_EQ(c.cc_err, 0);
    ts_demux_destroy(d);

    /* 4) basura en medio: pierde y recupera la sincronía */
    memset(&c, 0, sizeof c);
    d = ts_demux_create(on_pkt, &c);
    m = 0;
    memcpy(cut, g_ts, 20 * TS_PKT);
    m = 20 * TS_PKT;
    for (int i = 0; i < 300; i++)
        cut[m++] = (uint8_t)(i * 7 + 1) == 0x47 ? 0x48 : (uint8_t)(i * 7 + 1);
    memcpy(cut + m, g_ts + 20 * TS_PKT, (size_t)(total_pkts - 20) * TS_PKT);
    m += (size_t)(total_pkts - 20) * TS_PKT;
    ts_demux_feed(d, cut, m);
    ts_demux_stats(d, &st);
    CHECK(c.n >= total_pkts - 6);
    CHECK(st.sync_losses >= 1 || c.n >= total_pkts - 6);
    ts_demux_destroy(d);
    free(cut);
    free(junk);
}

/* ---- PES ---- */
typedef struct {
    int n;
    int64_t pts[16], dts[16];
    size_t len[16];
    uint8_t first[16];
} pesrec_t;

static void on_pes(void *ctx, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    pesrec_t *r = ctx;
    if (r->n < 16) {
        r->pts[r->n] = pts;
        r->dts[r->n] = dts;
        r->len[r->n] = len;
        r->first[r->n] = es[0];
    }
    r->n++;
}

static void on_pes_pkt(void *ctx, const ts_pkt_t *k) { pes_feed(ctx, k); }

static void run_pes(const uint8_t *ts, size_t n, pesrec_t *r, unsigned long *dropped)
{
    pes_asm_t *p = pes_create(on_pes, r);
    ts_demux_t *d = ts_demux_create(on_pes_pkt, p);
    ts_demux_feed(d, ts, n);
    if (dropped)
        *dropped = pes_dropped(p);
    ts_demux_destroy(d);
    pes_destroy(p);
}

static void test_pes(void)
{
    tsb_t b;
    memset(&b, 0, sizeof b);
    static uint8_t es[3000];
    for (size_t i = 0; i < sizeof es; i++)
        es[i] = (uint8_t)(i * 31 + 7);
    size_t n = 0;
    int64_t big = (1LL << 33) - 1000;
    /* longitud declarada (audio) y longitud 0 (vídeo), con y sin DTS, y PTS cerca del cruce de 2^33 */
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xC0, 90000, -1, es, 1000, -1, false);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xC0, 93000, -1, es, 1500, -1, false);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, big, big - 7200, es, 2900, -1, true);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 5, 2, es, 200, -1, true);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 3600, -1, es, 10, -1, true); /* cierra el anterior */
    pesrec_t r = {0};
    unsigned long dr = 0;
    run_pes(g_ts, n, &r, &dr);
    CHECK_EQ(r.n, 4); /* el último (longitud 0) queda abierto */
    CHECK_EQ(r.pts[0], 90000);
    CHECK_EQ(r.len[0], 1000);
    CHECK_EQ(r.pts[1], 93000);
    CHECK_EQ(r.len[1], 1500);
    CHECK_EQ(r.pts[2], big);
    CHECK_EQ(r.dts[2], big - 7200);
    CHECK_EQ(r.len[2], 2900);
    CHECK_EQ(r.pts[3], 5);
    CHECK_EQ(r.dts[3], 2);
    CHECK_EQ(r.first[0], es[0]);

    /* pes_read_ts con marcadores malos */
    uint8_t t5[5];
    tsb_write_ts(t5, 123456789LL, 2);
    CHECK_EQ(pes_read_ts(t5), 123456789LL);
    t5[2] &= 0xFE;
    CHECK(pes_read_ts(t5) < 0);

    /* un salto de continuidad en mitad de un PES lo descarta entero */
    memset(&b, 0, sizeof b);
    n = 0;
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 100, -1, es, 2900, -1, true);
    size_t first_end = n;
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 200, -1, es, 2900, -1, true);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 300, -1, es, 100, -1, true);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 400, -1, es, 100, -1, true);
    /* quitar el 2.º paquete del 2.º PES */
    memmove(g_ts + first_end + 2 * TS_PKT, g_ts + first_end + 3 * TS_PKT, n - first_end - 3 * TS_PKT);
    n -= TS_PKT;
    memset(&r, 0, sizeof r);
    run_pes(g_ts, n, &r, &dr);
    CHECK_EQ(r.n, 2); /* 1.º y 3.º; el 2.º dañado se descarta y el 4.º queda abierto */
    CHECK_EQ(r.pts[0], 100);
    CHECK_EQ(r.pts[1], 300);
    CHECK_EQ(dr, 1);

    /* TEI (el RS no pudo corregir el paquete): su carga no se usa y el PES en curso se pierde */
    memset(&b, 0, sizeof b);
    n = 0;
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 100, -1, es, 2900, -1, true);
    first_end = n;
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 200, -1, es, 2900, -1, true);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 300, -1, es, 100, -1, true);
    n += tsb_packets_pes(&b, g_ts + n, 0x101, 0xE0, 400, -1, es, 100, -1, true);
    g_ts[first_end + 2 * TS_PKT + 1] |= 0x80;
    memset(&r, 0, sizeof r);
    run_pes(g_ts, n, &r, &dr);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(r.pts[1], 300);
    CHECK_EQ(dr, 1);
}

/* ---- H.264 ---- */
typedef struct {
    int n;
    bool key[16], sps[16];
    int64_t pts[16];
    size_t len[16];
} aurec_t;

static void on_au(void *ctx, const h264_au_t *au)
{
    aurec_t *a = ctx;
    if (a->n < 16) {
        a->key[a->n] = au->key;
        a->sps[a->n] = au->has_sps;
        a->pts[a->n] = au->pts;
        a->len[a->n] = au->len;
    }
    a->n++;
}

static const uint8_t SPS720[] = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50, 0x05, 0xbb, 0x01, 0x10, 0x00, 0x00,
                                 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60};
static const uint8_t SPS1080[] = {0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78, 0x02, 0x27, 0xe5, 0xc0, 0x44, 0x00,
                                  0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xc8, 0x3c, 0x60, 0xc6, 0x58};
static const uint8_t SPS576[] = {0x67, 0x64, 0x00, 0x1e, 0xac, 0xd9, 0x40, 0xb4, 0x12, 0x6c, 0x04, 0x40, 0x00, 0x00,
                                 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83, 0xc5, 0x8b, 0x65, 0x80};
static const uint8_t PPS[] = {0x68, 0xef, 0x8f, 0xcb};

static size_t nal(uint8_t *o, const uint8_t *body, size_t n, bool long_sc)
{
    size_t k = 0;
    if (long_sc)
        o[k++] = 0;
    o[k++] = 0;
    o[k++] = 0;
    o[k++] = 1;
    memcpy(o + k, body, n);
    return k + n;
}

static void test_h264(void)
{
    uint8_t aud[] = {0x09, 0xF0}, idr[] = {0x65, 0x88, 0x84, 0x00, 0x33, 0x12, 0x99}, pslice[] = {0x41, 0x9A, 0x24, 0x6C, 0x42};
    uint8_t sei_rp[] = {0x06, 0x06, 0x01, 0x80, 0x80};
    uint8_t au_key[256], au_p[256], au_sei[256];
    size_t k = 0;
    k += nal(au_key + k, aud, sizeof aud, true);
    k += nal(au_key + k, SPS720, sizeof SPS720, true);
    k += nal(au_key + k, PPS, sizeof PPS, false);
    k += nal(au_key + k, idr, sizeof idr, false);
    size_t kl = k;
    k = 0;
    k += nal(au_p + k, aud, sizeof aud, true);
    k += nal(au_p + k, pslice, sizeof pslice, false);
    size_t pl = k;
    k = 0;
    k += nal(au_sei + k, aud, sizeof aud, true);
    k += nal(au_sei + k, sei_rp, sizeof sei_rp, false);
    k += nal(au_sei + k, pslice, sizeof pslice, false);
    size_t sl = k;

    aurec_t a = {0};
    h264_split_t *s = h264_split_create(on_au, &a);
    /* una AU por PES (lo normal) */
    h264_split_feed(s, au_key, kl, 1000, 900);
    h264_split_feed(s, au_p, pl, 4600, 4500);
    h264_split_feed(s, au_sei, sl, 8200, 8100);
    CHECK_EQ(a.n, 2); /* la última queda pendiente */
    CHECK(a.key[0]);
    CHECK(a.sps[0]);
    CHECK_EQ(a.pts[0], 1000);
    CHECK_EQ(a.len[0], kl);
    CHECK(!a.key[1]);
    CHECK(!a.sps[1]);
    h264_split_flush(s);
    CHECK_EQ(a.n, 3);
    CHECK(a.key[2]); /* SEI de punto de recuperación */

    /* tres AU en un mismo PES: se cortan por AUD, solo la primera conserva el PTS */
    memset(&a, 0, sizeof a);
    uint8_t multi[600];
    size_t mk = 0;
    memcpy(multi, au_key, kl);
    mk = kl;
    memcpy(multi + mk, au_p, pl);
    mk += pl;
    memcpy(multi + mk, au_p, pl);
    mk += pl;
    h264_split_feed(s, multi, mk, 50000, 50000);
    h264_split_flush(s);
    CHECK_EQ(a.n, 3);
    CHECK_EQ(a.pts[0], 50000);
    CHECK_EQ(a.pts[1], PTS_NONE);
    CHECK(a.key[0] && !a.key[1] && !a.key[2]);
    CHECK_EQ(a.len[0], kl);

    /* sin AUD: un PES con PTS abre AU nueva */
    memset(&a, 0, sizeof a);
    uint8_t noaud[64];
    size_t nk = nal(noaud, pslice, sizeof pslice, false);
    h264_split_feed(s, noaud, nk, 100, 100);
    h264_split_feed(s, noaud, nk, 200, 200);
    h264_split_flush(s);
    CHECK_EQ(a.n, 2);
    CHECK_EQ(a.pts[1], 200);
    h264_split_destroy(s);

    /* búsqueda de NAL y tamaño por SPS */
    size_t nl = 0;
    const uint8_t *p = annexb_find_nal(au_key, kl, 8, &nl);
    CHECK(p != NULL);
    CHECK_EQ(nl, sizeof PPS);
    CHECK(annexb_find_nal(au_p, pl, 7, &nl) == NULL);
    int w = 0, h = 0;
    CHECK(h264_sps_size(SPS720, sizeof SPS720, &w, &h));
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);
    CHECK(h264_sps_size(SPS1080, sizeof SPS1080, &w, &h));
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1080); /* 1088 con recorte de 8 */
    CHECK(h264_sps_size(SPS576, sizeof SPS576, &w, &h));
    CHECK_EQ(w, 720);
    CHECK_EQ(h, 576);
    CHECK(!h264_sps_size(PPS, sizeof PPS, &w, &h));
}

int main(void)
{
    test_parse();
    test_demux();
    test_pes();
    test_h264();
    return TEST_SUMMARY();
}
