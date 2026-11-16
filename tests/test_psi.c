/*
 * test_psi.c — PSI/SI (src/tv/psi.c): CRC-32/MPEG-2, texto DVB → UTF-8, PAT, PMT
 * (tipos de flujo y descriptores), SDT (nombre y proveedor, descriptor 0x48), NIT
 * (nombre de red, frecuencia y LCN), versiones, secciones partidas en varios
 * paquetes, varias secciones en un paquete y descarte por CRC erróneo.
 *
 * Vector del CRC: CRC-32/MPEG-2 de "123456789" = 0x0376E6E7 (catálogo de reveng).
 */
#include "test_util.h"
#include "ts_build.h"

/* bytes DVB (con \x…) → UTF-8 esperado */
static bool txtn(const char *dvb, size_t n, const char *want)
{
    char out[128];
    dvb_text_to_utf8((const uint8_t *)dvb, n, out, sizeof out);
    if (strcmp(out, want)) {
        printf("  texto: obtenido '%s', esperado '%s'\n", out, want);
        return false;
    }
    return true;
}

#define txt(dvb, want) txtn(dvb, sizeof(dvb) - 1, want)

static void test_text(void)
{
    /* ISO 6937 por omisión: el acento va DELANTE de la letra (EN 300 468 Tabla A.1) */
    CHECK(txt("La 1", "La 1"));
    CHECK(txt("Antena 3", "Antena 3"));
    CHECK(txt("A\xC4" "no", "Año"));
    CHECK(txt("Canci\xC2" "on", "Canción"));
    CHECK(txt("\xC4" "Nand\xC2" "u", "Ñandú"));
    CHECK(txt("\xC8" "u\xC1" "a", "üà"));
    CHECK(txt("\xE9" "e \xA4 \xE1\xFA", "Øe € Æœ"));
    CHECK(txt("\xCB" "c \xCA" "A", "ç Å"));
    CHECK(txt("\xC4" "z", "z\xCC\x83")); /* sin precompuesto: letra + marca combinante */
    /* controles: 0x8A = salto → espacio; 0x86/0x87 = énfasis, se ignoran */
    CHECK(txt("Tele\x8A" "cinco\x86" "2\x87", "Tele cinco2"));
    /* tablas de la norma: 0x10 0x00 0x01 = 8859-1; 0x10 0x00 0x0F = 8859-15; 0x0B = 8859-15 */
    CHECK(txt("\x10\x00\x01" "A\xF1" "o \xA4", "Año ¤"));
    CHECK(txt("\x10\x00\x0F" "A\xF1" "o \xA4", "Año €"));
    CHECK(txt("\x0B" "\xA6\xBD", "Š\xC5\x93"));
    CHECK(txt("\x05" "\xD0\xFD", "Ğı")); /* 8859-9 */
    /* UTF-8 (0x15) y UCS-2 (0x11) */
    CHECK(txt("\x15" "M\xC3\xBAsica", "Música"));
    CHECK(txt("\x11\x00" "A\x00\xF1\x20\xAC", "Añ€"));
    /* nunca escribe más de cap */
    char small[6];
    size_t n = dvb_text_to_utf8((const uint8_t *)"Antena 3 es larga", 17, small, sizeof small);
    CHECK(n < sizeof small);
    CHECK_EQ(small[n], 0);
}

static void test_crc(void)
{
    CHECK_EQ(crc32_mpeg2((const uint8_t *)"123456789", 9), 0x0376E6E7u);
    uint8_t sec[64];
    int sid[] = {1}, pid[] = {0x100};
    size_t l = tsb_pat(sec, 1, 0, sid, pid, 1);
    CHECK_EQ(crc32_mpeg2(sec, l), 0); /* con el CRC incluido da 0 */
    sec[9] ^= 0x10;
    CHECK(crc32_mpeg2(sec, l) != 0);
}

typedef struct {
    psi_t *p;
    tsb_t b;
    uint8_t buf[1 << 16];
} rig_t;

static void feed_pkt(void *ctx, const ts_pkt_t *k)
{
    rig_t *r = ctx;
    if (psi_wants_pid(r->p, k->pid))
        psi_feed(r->p, k);
}

static void push(rig_t *r, ts_demux_t *d, int pid, const uint8_t *sec, size_t len)
{
    size_t n = tsb_packets_section(&r->b, r->buf, pid, sec, len);
    /* se necesitan 4 paquetes más para que el demux dé por buena la sincronía */
    ts_demux_feed(d, r->buf, n);
}

static void flush_sync(rig_t *r, ts_demux_t *d)
{
    uint8_t nullp[TS_PKT * 4];
    memset(nullp, 0xFF, sizeof nullp);
    for (int i = 0; i < 4; i++) {
        nullp[i * TS_PKT] = 0x47;
        nullp[i * TS_PKT + 1] = 0x1F;
        nullp[i * TS_PKT + 2] = 0xFF;
        nullp[i * TS_PKT + 3] = 0x10;
    }
    ts_demux_feed(d, nullp, sizeof nullp);
    (void)r;
}

static void test_tables(void)
{
    rig_t *r = calloc(1, sizeof *r);
    r->p = psi_create();
    ts_demux_t *d = ts_demux_create(feed_pkt, r);
    uint8_t sec[1024];
    size_t l;

    int sid[] = {0x0101, 0x0102, 0x0103}, pmt[] = {0x100, 0x200, 0x300};
    l = tsb_pat(sec, 0x0001, 3, sid, pmt, 3);
    push(r, d, TS_PID_PAT, sec, l);

    /* PMT 1: H.264 + MP2 (es) + AC-3 por descriptor 0x6A */
    uint8_t lang_es[] = {0x0A, 4, 's', 'p', 'a', 0};
    uint8_t ac3d[] = {0x6A, 1, 0x00, 0x0A, 4, 'e', 'n', 'g', 0};
    tsb_es_t es1[] = {{0x1B, 0x101, NULL, 0}, {0x03, 0x102, lang_es, sizeof lang_es}, {0x06, 0x103, ac3d, sizeof ac3d}};
    l = tsb_pmt(sec, 0x0101, 1, 0x101, es1, 3);
    push(r, d, 0x100, sec, l);
    /* PMT 2: HEVC + AAC + E-AC-3 (0x7A) + subtítulos (0x59) */
    uint8_t eac3d[] = {0x7A, 0};
    uint8_t subd[] = {0x59, 8, 's', 'p', 'a', 0x10, 0, 1, 0, 1};
    tsb_es_t es2[] = {{0x24, 0x201, NULL, 0}, {0x0F, 0x202, NULL, 0}, {0x06, 0x203, eac3d, sizeof eac3d},
                      {0x06, 0x204, subd, sizeof subd}, {0x04, 0x205, NULL, 0}};
    l = tsb_pmt(sec, 0x0102, 1, 0x201, es2, 5);
    push(r, d, 0x200, sec, l);
    /* PMT 3: MPEG-2 vídeo 0x02 y descriptor CA (0x09) → cifrado */
    uint8_t cad[] = {0x09, 4, 0x06, 0x04, 0xE0, 0x10};
    tsb_es_t es3[] = {{0x02, 0x301, cad, sizeof cad}};
    l = tsb_pmt(sec, 0x0103, 1, 0x301, es3, 1);
    push(r, d, 0x300, sec, l);

    /* SDT: nombres en ISO 6937, Latin-9 y UTF-8; dos secciones en el mismo paquete */
    size_t o = tsb_sdt_begin(sec, 0x0001, 0x22D4, 2);
    o = tsb_sdt_service(sec, o, 0x0101, 0x19, (const uint8_t *)"Atresmedia", 10, (const uint8_t *)"Antena 3", 8);
    o = tsb_sdt_service(sec, o, 0x0102, 0x19, (const uint8_t *)"\x10\x00\x0F" "RTVE", 8, (const uint8_t *)"\x10\x00\x0F" "La 1 \xA4", 9);
    size_t l1 = tsb_finish_section(sec, o - 3);
    uint8_t sec2[300];
    o = tsb_sdt_begin(sec2, 0x0001, 0x22D4, 2);
    sec2[6] = 1;
    sec2[7] = 1; /* section_number 1 de 1 */
    o = tsb_sdt_service(sec2, o, 0x0103, 0x01, (const uint8_t *)"\x15" "Ñoño", 7, (const uint8_t *)"\xC4" "Nand\xC2" "u", 7);
    size_t l2 = tsb_finish_section(sec2, o - 3);
    memcpy(sec + l1, sec2, l2);
    push(r, d, TS_PID_SDT, sec, l1 + l2);

    /* NIT */
    int lsid[] = {0x0101, 0x0102}, llcn[] = {3, 1};
    l = tsb_nit(sec, 0x3001, 0, "TDT Espa\xC4" "na", 0x0001, 0x22D4, 570000000LL, lsid, llcn, 2);
    push(r, d, TS_PID_NIT, sec, l);
    flush_sync(r, d);

    CHECK_EQ(psi_ts_id(r->p), 1);
    CHECK_EQ(psi_nservices(r->p), 3);
    const psi_service_t *s = psi_find(r->p, 0x0101);
    CHECK(s && s->have_pmt && s->have_sdt);
    if (s) {
        CHECK(!strcmp(s->name, "Antena 3"));
        CHECK(!strcmp(s->provider, "Atresmedia"));
        CHECK_EQ(s->svc_type, 0x19);
        CHECK_EQ(s->pmt_pid, 0x100);
        CHECK_EQ(s->pcr_pid, 0x101);
        CHECK_EQ(s->lcn, 3);
        CHECK_EQ(s->nes, 3);
        CHECK_EQ(s->es[0].kind, ES_VIDEO);
        CHECK_EQ(s->es[0].codec, CODEC_H264);
        CHECK_EQ(s->es[1].codec, CODEC_MP2);
        CHECK(!strcmp(s->es[1].lang, "spa"));
        CHECK_EQ(s->es[2].codec, CODEC_AC3);
        CHECK_EQ(s->es[2].kind, ES_AUDIO);
        CHECK(!strcmp(s->es[2].lang, "eng"));
        CHECK(!s->ca);
    }
    s = psi_find(r->p, 0x0102);
    CHECK(s != NULL);
    if (s) {
        CHECK(!strcmp(s->name, "La 1 €"));
        CHECK(!strcmp(s->provider, "RTVE"));
        CHECK_EQ(s->lcn, 1);
        CHECK_EQ(s->nes, 5);
        CHECK_EQ(s->es[0].codec, CODEC_HEVC);
        CHECK_EQ(s->es[1].codec, CODEC_AAC);
        CHECK_EQ(s->es[2].codec, CODEC_EAC3);
        CHECK_EQ(s->es[3].codec, CODEC_SUB);
        CHECK_EQ(s->es[3].kind, ES_OTHER);
        CHECK_EQ(s->es[4].codec, CODEC_MP2);
    }
    s = psi_find(r->p, 0x0103); /* segunda sección del SDT en el mismo paquete */
    CHECK(s != NULL);
    if (s) {
        CHECK(!strcmp(s->name, "Ñandú"));
        CHECK(!strcmp(s->provider, "Ñoño"));
        CHECK(s->ca);
        CHECK_EQ(s->es[0].codec, CODEC_MPEG2V);
        CHECK_EQ(s->svc_type, 1);
    }
    CHECK(!strcmp(psi_network_name(r->p), "TDT España"));
    int64_t fr[4];
    CHECK_EQ(psi_nit_freqs(r->p, fr, 4), 1);
    CHECK_EQ(fr[0], 570000000LL);

    /* una sección con el CRC roto no cambia nada */
    unsigned g0 = psi_generation(r->p);
    o = tsb_sdt_begin(sec, 0x0001, 0x22D4, 3);
    o = tsb_sdt_service(sec, o, 0x0101, 0x19, (const uint8_t *)"X", 1, (const uint8_t *)"Hackeado", 8);
    l = tsb_finish_section(sec, o - 3);
    sec[l - 1] ^= 0xFF;
    push(r, d, TS_PID_SDT, sec, l);
    flush_sync(r, d);
    CHECK_EQ(psi_generation(r->p), g0);
    CHECK(!strcmp(psi_find(r->p, 0x0101)->name, "Antena 3"));
    /* con CRC bueno sí se actualiza (y sube la generación) */
    sec[l - 1] ^= 0xFF;
    l = tsb_finish_section(sec, o - 3);
    push(r, d, TS_PID_SDT, sec, l);
    flush_sync(r, d);
    CHECK(psi_generation(r->p) > g0);
    CHECK(!strcmp(psi_find(r->p, 0x0101)->name, "Hackeado"));

    /* SDT grande: 20 servicios repartidos en varios paquetes (la sección cruza límites) */
    o = tsb_sdt_begin(sec, 0x0001, 0x22D4, 4);
    for (int i = 0; i < 20; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "Canal numero %02d", i);
        o = tsb_sdt_service(sec, o, 0x1000 + i, 1, (const uint8_t *)"Prov", 4, (const uint8_t *)nm, (int)strlen(nm));
    }
    l = tsb_finish_section(sec, o - 3);
    CHECK(l > 3 * 184);
    push(r, d, TS_PID_SDT, sec, l);
    flush_sync(r, d);
    CHECK(psi_find(r->p, 0x1013) != NULL);
    CHECK(psi_find(r->p, 0x1013) && !strcmp(psi_find(r->p, 0x1013)->name, "Canal numero 19"));

    /* un salto de continuidad en medio de una sección larga la descarta */
    o = tsb_sdt_begin(sec, 0x0001, 0x22D4, 5);
    for (int i = 0; i < 20; i++)
        o = tsb_sdt_service(sec, o, 0x2000 + i, 1, (const uint8_t *)"Prov", 4, (const uint8_t *)"Perdido", 7);
    l = tsb_finish_section(sec, o - 3);
    size_t n = tsb_packets_section(&r->b, r->buf, TS_PID_SDT, sec, l);
    memmove(r->buf + TS_PKT, r->buf + 2 * TS_PKT, n - 2 * TS_PKT);
    ts_demux_feed(d, r->buf, n - TS_PKT);
    flush_sync(r, d);
    CHECK(psi_find(r->p, 0x2000) == NULL);

    ts_demux_destroy(d);
    psi_destroy(r->p);
    free(r);
}

int main(void)
{
    test_crc();
    test_text();
    test_tables();
    return TEST_SUMMARY();
}
