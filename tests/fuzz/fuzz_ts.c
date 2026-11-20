/*
 * fuzz_ts.c — Fuzzing del demux TS, PSI/SI, PES, cortador H.264 y decodificador
 * MP2 (src/tv/ts.c, psi.c, pes.c, mp2.c).
 *
 * Tres frentes:
 *  1. Flujo TS completo: se construyen con ts_build.h secciones PAT/PMT/SDT/NIT y
 *     PES con PTS/DTS (válidos), se mutan ANTES de calcular el CRC-32 (si no, el
 *     CRC los descartaría casi todos y no se ejercitarían los analizadores de
 *     descriptores) y también DESPUÉS (paquetes corruptos), y se alimentan al
 *     demux en trozos de tamaño y alineación arbitrarios. Del demux cuelgan psi,
 *     un ensamblador PES por PID, el cortador H.264 y el decodificador MP2: la misma
 *     cadena que usa tv.c.
 *  2. API directa con estructuras imposibles: ts_pkt_t con plen/pointer_field/
 *     cc/pid aleatorios para psi_feed y pes_feed; PES con longitudes
 *     declaradas imposibles (0, 65535, menor que la cabecera, cabecera PTS
 *     truncada); ES H.264 con códigos de inicio y SPS aleatorios (exp-Golomb
 *     que se sale del buffer); dvb_text_to_utf8 con tablas de caracteres
 *     inválidas y buffers de salida diminutos.
 *  3. MP2: cabeceras de trama válidas (MPEG-1/2, capas I/II/III, todas las tasas y
 *     modos) con carga aleatoria o mutada, concatenadas, troceadas y truncadas;
 *     mp2_decode_one con longitudes 0..N.
 *
 * Invariantes comprobados en las llamadas de retorno: plen ∈ [0,184], PID de 13 bits,
 * muestras de MP2 ≤ 1152 por canal y 1–2 canales, AU H.264 con data/len coherentes.
 * Casos de regresión (tests/fuzz/casos/ts/): primer byte = modo
 * ('T' flujo TS, 'M' bytes para mp2_decode, 'H' ES H.264, 'S' texto DVB,
 * 'P' carga útil para pes_feed directo), el
 * resto son los bytes de entrada.
 */
#include "fuzz_util.h"
#include "ts_build.h"
#include "tv/mp2.h"
#include "tv/pes.h"
#include "tv/psi.h"
#include "tv/ts.h"

#include <assert.h>

typedef struct {
    psi_t *psi;
    pes_asm_t *pes[3];
    h264_split_t *h264;
    mp2_t *mp2;
    unsigned long n_pkt, n_pes, n_au, n_pcm;
} rig_t;

static void on_pcm(void *ctx, const int16_t *pcm, int n, int ch, int rate)
{
    rig_t *r = ctx;
    assert(n >= 0 && n <= 1152 && ch >= 1 && ch <= 2 && rate > 0);
    if (n)
        (void)*(volatile const int16_t *)(pcm + (size_t)n * (size_t)ch - 1); /* lectura de la última muestra */
    r->n_pcm += (unsigned)n;
}

static void on_au(void *ctx, const h264_au_t *au)
{
    rig_t *r = ctx;
    r->n_au++;
    if (au->len)
        (void)*(volatile const uint8_t *)(au->data + au->len - 1);
}

static void on_pes(void *ctx, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    rig_t *r = ctx;
    (void)pts;
    (void)dts;
    r->n_pes++;
    if (len)
        (void)*(volatile const uint8_t *)(es + len - 1);
    h264_split_feed(r->h264, es, len, pts, dts);
    mp2_decode(r->mp2, es, len, on_pcm, r);
}

static void on_pkt(void *ctx, const ts_pkt_t *k)
{
    rig_t *r = ctx;
    r->n_pkt++;
    assert(k->pid >= 0 && k->pid <= 0x1FFF && k->plen >= 0 && k->plen <= 184);
    if (k->payload && k->plen)
        (void)*(volatile const uint8_t *)(k->payload + k->plen - 1);
    if (psi_wants_pid(r->psi, k->pid))
        psi_feed(r->psi, k);
    for (int i = 0; i < 3; i++)
        if (k->pid == 0x101 + i)
            pes_feed(r->pes[i], k);
}

static rig_t *rig_new(void)
{
    rig_t *r = calloc(1, sizeof *r);
    r->psi = psi_create();
    for (int i = 0; i < 3; i++)
        r->pes[i] = pes_create(on_pes, r);
    r->h264 = h264_split_create(on_au, r);
    r->mp2 = mp2_create();
    return r;
}

static void rig_free(rig_t *r)
{
    /* consulta final de todo lo que expone psi (nombres, ES, NIT…) */
    int n = psi_nservices(r->psi);
    for (int i = 0; i < n; i++) {
        const psi_service_t *s = psi_service(r->psi, i);
        assert(s && s->nes >= 0 && s->nes <= PSI_MAX_ES);
        assert(strlen(s->name) < sizeof s->name && strlen(s->provider) < sizeof s->provider);
        (void)psi_find(r->psi, s->sid);
    }
    int64_t fr[8];
    int nf = psi_nit_freqs(r->psi, fr, 8);
    assert(nf >= 0 && nf <= 8);
    (void)psi_network_name(r->psi);
    h264_split_flush(r->h264);
    psi_destroy(r->psi);
    for (int i = 0; i < 3; i++)
        pes_destroy(r->pes[i]);
    h264_split_destroy(r->h264);
    mp2_destroy(r->mp2);
    free(r);
}

/* ---- Semillas de secciones PSI/SI ----------------------------------------- */
typedef struct {
    uint8_t d[1024];
    size_t len;
    int pid;
} sec_t;
static sec_t SEC[8];
static int NSEC;

static void build_seeds(void)
{
    uint8_t *s;
    int sid[] = {0x0101, 0x0102, 0x0103}, pmt[] = {0x100, 0x200, 0x300};
    s = SEC[NSEC].d;
    SEC[NSEC].len = tsb_pat(s, 1, 3, sid, pmt, 3);
    SEC[NSEC++].pid = 0;
    uint8_t lang_es[] = {0x0A, 4, 's', 'p', 'a', 0}, ac3d[] = {0x6A, 1, 0, 0x0A, 4, 'e', 'n', 'g', 0};
    tsb_es_t es1[] = {{0x1B, 0x101, NULL, 0}, {0x03, 0x102, lang_es, sizeof lang_es}, {0x06, 0x103, ac3d, sizeof ac3d}};
    s = SEC[NSEC].d;
    SEC[NSEC].len = tsb_pmt(s, 0x0101, 1, 0x101, es1, 3);
    SEC[NSEC++].pid = 0x100;
    uint8_t subd[] = {0x59, 8, 's', 'p', 'a', 0x10, 0, 1, 0, 1}, cad[] = {0x09, 4, 0x06, 0x04, 0xE0, 0x10};
    tsb_es_t es2[] = {{0x24, 0x201, NULL, 0}, {0x0F, 0x202, NULL, 0}, {0x06, 0x204, subd, sizeof subd},
                      {0x02, 0x301, cad, sizeof cad}};
    s = SEC[NSEC].d;
    SEC[NSEC].len = tsb_pmt(s, 0x0102, 1, 0x201, es2, 4);
    SEC[NSEC++].pid = 0x200;
    s = SEC[NSEC].d;
    size_t o = tsb_sdt_begin(s, 1, 0x22D4, 2);
    o = tsb_sdt_service(s, o, 0x0101, 0x19, (const uint8_t *)"Atresmedia", 10, (const uint8_t *)"Antena 3", 8);
    o = tsb_sdt_service(s, o, 0x0102, 0x19, (const uint8_t *)"\x10\x00\x0F" "RTVE", 8,
                        (const uint8_t *)"\x10\x00\x0F" "La 1 \xA4", 9);
    SEC[NSEC].len = tsb_finish_section(s, o - 3);
    SEC[NSEC++].pid = TS_PID_SDT;
    int lsid[] = {0x0101, 0x0102}, llcn[] = {3, 1};
    s = SEC[NSEC].d;
    SEC[NSEC].len = tsb_nit(s, 0x3001, 0, "TDT Espa\xC4" "na", 1, 0x22D4, 570000000LL, lsid, llcn, 2);
    SEC[NSEC++].pid = TS_PID_NIT;
}

/* recalcula el CRC-32 al final de la sección (o lo deja roto si fix = 0) */
static void sec_fix_crc(uint8_t *d, size_t len)
{
    if (len < 8)
        return;
    uint32_t c = crc32_mpeg2(d, len - 4);
    d[len - 4] = (uint8_t)(c >> 24);
    d[len - 3] = (uint8_t)(c >> 16);
    d[len - 2] = (uint8_t)(c >> 8);
    d[len - 1] = (uint8_t)c;
}

/* ---- ES de relleno para PES ------------------------------------------------ */
static size_t gen_es(fz_rng_t *r, uint8_t *o, size_t cap)
{
    size_t n = 0, want = fz_below(r, 3000);
    if (want > cap - 16)
        want = cap - 16;
    switch (fz_below(r, 3)) {
    case 0: /* H.264 Annex B con SPS/PPS/AUD/slices de contenido aleatorio */
        while (n < want) {
            static const uint8_t T[] = {0x67, 0x68, 0x09, 0x65, 0x41, 0x06, 0x01, 0x0C, 0x1F};
            if (fz_below(r, 2))
                memcpy(o + n, "\x00\x00\x00\x01", 4), n += 4;
            else
                memcpy(o + n, "\x00\x00\x01", 3), n += 3;
            o[n++] = T[fz_below(r, sizeof T)];
            size_t l = fz_below(r, 60);
            for (size_t i = 0; i < l && n < want + 8; i++)
                o[n++] = fz_below(r, 5) ? (uint8_t)fz_next(r) : 0;
        }
        break;
    case 1: /* tramas MP2 */
        for (; n + 1500 < want + 1500 && n < cap - 1500 && fz_below(r, 4); ) {
            o[n++] = 0xFF;
            o[n++] = (uint8_t)(0xF0 | (fz_below(r, 4) << 1) | fz_below(r, 2));
            o[n++] = (uint8_t)fz_next(r);
            o[n++] = (uint8_t)fz_next(r);
            size_t l = fz_below(r, 700);
            for (size_t i = 0; i < l; i++)
                o[n++] = (uint8_t)fz_next(r);
        }
        break;
    default:
        for (; n < want; n++)
            o[n] = (uint8_t)fz_next(r);
    }
    return n;
}

static size_t gen_stream(fz_rng_t *r, uint8_t *out, size_t cap)
{
    static tsb_t b;
    static uint8_t tmp[1 << 14], es[1 << 13];
    size_t n = 0;
    int items = 1 + (int)fz_below(r, 10);
    for (int i = 0; i < items && n + 20000 < cap; i++) {
        uint32_t kind = fz_below(r, 8);
        if (kind < 4) {
            sec_t *s = &SEC[fz_below(r, NSEC)];
            size_t l = s->len;
            memcpy(tmp, s->d, l);
            if (fz_below(r, 3)) {
                l = fz_mutate(r, tmp + 3, l - 7, sizeof tmp - 8) + 7; /* cuerpo, sin cabecera de 3 B ni CRC */
                memmove(tmp + l - 4, tmp + 3 + (l - 7), 4); /* hueco para el CRC */
                if (fz_below(r, 2)) { /* longitud de sección coherente con el nuevo tamaño */
                    tmp[1] = (uint8_t)((tmp[1] & 0xF0) | (((l - 3) >> 8) & 0x0F));
                    tmp[2] = (uint8_t)(l - 3);
                }
                if (fz_below(r, 8))
                    sec_fix_crc(tmp, l);
            }
            int pid = fz_below(r, 10) ? s->pid : (int)fz_below(r, 0x2000);
            n += tsb_packets_section(&b, out + n, pid, tmp, l);
        } else if (kind < 6) {
            size_t el = gen_es(r, es, sizeof es);
            int pid = 0x101 + (int)fz_below(r, 3);
            int64_t pts = (int64_t)(fz_next(r) & 0x1FFFFFFFFLL);
            n += tsb_packets_pes(&b, out + n, pid, fz_below(r, 2) ? 0xE0 : 0xC0, pts, fz_below(r, 2) ? pts - 3000 : -1, es,
                                 el, fz_below(r, 2) ? (int64_t)(fz_next(r) & 0xFFFFFFFFFLL) : -1, fz_below(r, 4) == 0);
        } else if (kind < 7) { /* paquete aleatorio con sincronía válida */
            uint8_t *p = out + n;
            for (int k = 0; k < TS_PKT; k++)
                p[k] = (uint8_t)fz_next(r);
            p[0] = 0x47;
            if (fz_below(r, 2))
                p[1] = (uint8_t)((p[1] & 0xE0) | 1), p[2] = (uint8_t)(0x01 + fz_below(r, 3));
            n += TS_PKT;
        } else { /* paquete nulo / de relleno */
            uint8_t *p = out + n;
            memset(p, 0xFF, TS_PKT);
            p[0] = 0x47, p[1] = 0x1F, p[2] = 0xFF, p[3] = 0x10;
            n += TS_PKT;
        }
    }
    if (fz_below(r, 3) == 0)
        n = fz_mutate(r, out, n, cap);
    return n;
}

static void feed_stream(const uint8_t *d, size_t n, fz_rng_t *r)
{
    rig_t *g = rig_new();
    ts_demux_t *dm = ts_demux_create(on_pkt, g);
    /* seis paquetes nulos de arranque para que el demux dé por buena la sincronía */
    uint8_t nul[TS_PKT * 6];
    memset(nul, 0xFF, sizeof nul);
    for (int i = 0; i < 6; i++)
        nul[i * TS_PKT] = 0x47, nul[i * TS_PKT + 1] = 0x1F, nul[i * TS_PKT + 2] = 0xFF, nul[i * TS_PKT + 3] = 0x10;
    ts_demux_feed(dm, nul, sizeof nul);
    /* copia exacta por trozo en el montón: ASan vigila los límites de cada llamada */
    for (size_t o = 0; o < n;) {
        size_t c = 1 + fz_below(r, fz_below(r, 4) ? 2000 : 20);
        if (c > n - o)
            c = n - o;
        uint8_t *chunk = malloc(c);
        memcpy(chunk, d + o, c);
        ts_demux_feed(dm, chunk, c);
        free(chunk);
        o += c;
    }
    if (fz_below(r, 4) == 0)
        ts_demux_reset(dm);
    ts_stats_t st;
    ts_demux_stats(dm, &st);
    ts_demux_destroy(dm);
    rig_free(g);
}

/* ---- Frente 2: API directa ------------------------------------------------- */
static void direct_api(fz_rng_t *r)
{
    rig_t *g = rig_new();
    uint8_t buf[512];
    for (int i = 0; i < 60; i++) {
        for (size_t k = 0; k < sizeof buf; k++)
            buf[k] = (uint8_t)fz_next(r);
        if (fz_below(r, 2)) { /* pointer_field y cabeceras de sección plausibles */
            buf[0] = (uint8_t)fz_below(r, 190);
            buf[1 + fz_below(r, 8)] = (uint8_t)(fz_below(r, 2) ? 0x00 : 0x02);
        }
        ts_pkt_t k = {0};
        k.pid = (int)fz_below(r, 6) ? (int[]){0, 0x10, 0x11, 0x100, 0x101, 0x102}[fz_below(r, 6)] : (int)fz_below(r, 0x2000);
        k.pusi = fz_below(r, 2);
        k.cc = (int)fz_below(r, 16);
        k.tei = fz_below(r, 20) == 0;
        k.scrambled = fz_below(r, 20) == 0;
        k.cc_err = fz_below(r, 6) == 0;
        k.dup = fz_below(r, 20) == 0;
        k.plen = (int)fz_below(r, 185);
        k.payload = fz_below(r, 30) ? buf : NULL;
        if (!k.payload)
            k.plen = 0;
        if (fz_below(r, 2) && k.payload) { /* inicio de PES con longitud imposible */
            static const uint16_t PL[] = {0, 1, 3, 6, 8, 9, 65535, 183, 184};
            uint8_t *p = buf;
            p[0] = 0, p[1] = 0, p[2] = 1, p[3] = (uint8_t)(fz_below(r, 2) ? 0xE0 : 0xC0);
            uint16_t l = PL[fz_below(r, 9)];
            p[4] = (uint8_t)(l >> 8), p[5] = (uint8_t)l;
            p[6] = 0x80;
            p[7] = (uint8_t)(fz_below(r, 4) << 6);
            p[8] = (uint8_t)fz_below(r, 255); /* header_data_length mayor que el paquete */
            k.pusi = true;
        }
        int pid = 0x101 + (int)fz_below(r, 3);
        k.pid = fz_below(r, 3) ? pid : k.pid;
        pes_feed(g->pes[pid - 0x101], &k);
        if (psi_wants_pid(g->psi, k.pid) || fz_below(r, 3) == 0)
            psi_feed(g->psi, &k);
    }
    rig_free(g);

    /* pes_read_ts, annexb_find_nal, SPS y texto DVB sobre bytes aleatorios */
    for (int i = 0; i < 40; i++) {
        size_t l = fz_below(r, 80);
        uint8_t *b = malloc(l ? l : 1);
        for (size_t k = 0; k < l; k++)
            b[k] = (uint8_t)fz_next(r);
        if (l >= 5)
            (void)pes_read_ts(b);
        size_t nl;
        (void)annexb_find_nal(b, l, (int)fz_below(r, 32), &nl);
        int w, h;
        (void)h264_sps_size(b, l, &w, &h);
        char out[48];
        size_t cap = fz_below(r, 4) ? sizeof out : fz_below(r, 4);
        size_t ol = dvb_text_to_utf8(b, l, out, cap);
        assert(cap == 0 ? ol == 0 : ol < cap);
        free(b);
    }
}

/* ---- Frente 3: MP2 --------------------------------------------------------- */
static size_t gen_mp2(fz_rng_t *r, uint8_t *o, size_t cap)
{
    size_t n = 0;
    int frames = 1 + (int)fz_below(r, 5);
    for (int f = 0; f < frames && n + 2000 < cap; f++) {
        int id = (int)fz_below(r, 2), layer = 1 + (int)fz_below(r, 3), prot = (int)fz_below(r, 2);
        int br = 1 + (int)fz_below(r, 14), sr = (int)fz_below(r, 3), pad = (int)fz_below(r, 2);
        uint32_t h = 0xFFE00000u | ((uint32_t)id << 19) | ((uint32_t)layer << 17) | ((uint32_t)prot << 16) |
                     ((uint32_t)br << 12) | ((uint32_t)sr << 10) | ((uint32_t)pad << 9) | (fz_next(r) & 0x1FF);
        if (fz_below(r, 10) == 0)
            h ^= (uint32_t)fz_next(r); /* cabecera dañada */
        for (int i = 3; i >= 0; i--)
            o[n++] = (uint8_t)(h >> (8 * i));
        size_t l = 100 + fz_below(r, 1000);
        for (size_t i = 0; i < l; i++)
            o[n++] = fz_below(r, 6) ? (uint8_t)fz_next(r) : 0;
    }
    if (fz_below(r, 3) == 0)
        n = fz_mutate(r, o, n, cap);
    return n;
}

static void run_mp2(const uint8_t *d, size_t n, fz_rng_t *r)
{
    rig_t *g = rig_new();
    mp2_t *m = g->mp2;
    for (size_t o = 0; o < n;) {
        size_t c = 1 + fz_below(r, fz_below(r, 2) ? 1500 : 30);
        if (c > n - o)
            c = n - o;
        uint8_t *ch = malloc(c);
        memcpy(ch, d + o, c);
        mp2_decode(m, ch, c, on_pcm, g);
        free(ch);
        o += c;
    }
    int16_t pcm[1152 * 2];
    int chn, rate;
    for (size_t l = 0; l < 40 && l <= n; l += 1 + fz_below(r, 7)) {
        uint8_t *ch = malloc(l ? l : 1);
        memcpy(ch, d, l);
        int rc = mp2_decode_one(m, ch, l, pcm, &chn, &rate);
        (void)rc;
        free(ch);
    }
    (void)mp2_bad_frames(m);
    mp2_reset(m);
    rig_free(g);
}

static void one_case(const uint8_t *b, size_t l, void *ctx)
{
    fz_rng_t *r = ctx;
    if (l < 1)
        return;
    switch (b[0]) {
    case 'T': feed_stream(b + 1, l - 1, r); break;
    case 'M': run_mp2(b + 1, l - 1, r); break;
    case 'H': {
        rig_t *g = rig_new();
        h264_split_feed(g->h264, b + 1, l - 1, 0, 0);
        h264_split_feed(g->h264, b + 1, l - 1, 1, 1);
        rig_free(g);
        break;
    }
    case 'P': { /* pes_feed directo: carga útil de l-1 bytes (0 = plen 0 con payload no nulo) */
        rig_t *g = rig_new();
        ts_pkt_t k = {.pid = 0x101, .pusi = true, .payload = b + 1, .plen = (int)(l - 1) > 184 ? 184 : (int)(l - 1)};
        pes_feed(g->pes[0], &k);
        k.pusi = false;
        pes_feed(g->pes[0], &k);
        rig_free(g);
        break;
    }
    case 'S': {
        char out[64];
        (void)dvb_text_to_utf8(b + 1, l - 1, out, sizeof out);
        break;
    }
    default: break;
    }
}

int main(int argc, char **argv)
{
    fz_t z;
    fz_init(&z, "ts", argc, argv, 60);
    build_seeds();
    fz_run_cases("ts", one_case, &z.rng);
    static uint8_t buf[1 << 18];
    unsigned long it = 0;
    while (fz_alive(&z)) {
        it++;
        size_t n;
        switch (fz_below(&z.rng, 5)) {
        case 0:
            direct_api(&z.rng);
            break;
        case 1:
            n = gen_mp2(&z.rng, buf, sizeof buf);
            fz_current(buf, n);
            run_mp2(buf, n, &z.rng);
            break;
        default:
            n = gen_stream(&z.rng, buf, sizeof buf);
            fz_current(buf, n);
            feed_stream(buf, n, &z.rng);
        }
    }
    printf("[fuzz:ts] %lu iteraciones, sin fallos\n", it);
    return 0;
}
