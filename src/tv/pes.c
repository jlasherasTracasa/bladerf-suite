/*
 * pes.c — Reensamblador PES y separador de unidades de acceso H.264 (ver pes.h).
 */
#include "pes.h"

#include <stdlib.h>
#include <string.h>

int64_t pes_read_ts(const uint8_t *p)
{
    if (!(p[0] & 1) || !(p[2] & 1) || !(p[4] & 1))
        return -1;
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) |
           ((int64_t)p[3] << 7) | (p[4] >> 1);
}

struct pes_asm {
    pes_fn fn;
    void *ctx;
    uint8_t *buf;
    size_t len, cap;
    bool active;
    unsigned long dropped;
};

pes_asm_t *pes_create(pes_fn fn, void *ctx)
{
    pes_asm_t *p = calloc(1, sizeof *p);
    if (p) {
        p->fn = fn;
        p->ctx = ctx;
    }
    return p;
}
void pes_destroy(pes_asm_t *p)
{
    if (p) {
        free(p->buf);
        free(p);
    }
}
void pes_reset(pes_asm_t *p)
{
    p->len = 0;
    p->active = false;
}
unsigned long pes_dropped(const pes_asm_t *p) { return p->dropped; }

/* Interpreta p->buf como PES completo y lo entrega. */
static void finish(pes_asm_t *p)
{
    const uint8_t *b = p->buf;
    size_t n = p->len;
    p->len = 0;
    p->active = false;
    if (n < 9 || b[0] || b[1] || b[2] != 1)
        return;
    int sid = b[3];
    /* streams sin la cabecera «larga» (padding, mapas…): no son de interés */
    if (sid == 0xBE || sid == 0xBC || sid == 0xBF || sid == 0xF0 || sid == 0xF1 || sid == 0xFF || sid == 0xF2 || sid == 0xF8)
        return;
    size_t hl = b[8];
    if (9 + hl > n)
        return;
    int64_t pts = PTS_NONE, dts = PTS_NONE;
    int pd = (b[7] >> 6) & 3;
    if (pd >= 2 && hl >= 5) {
        int64_t v = pes_read_ts(b + 9);
        if (v >= 0)
            pts = v;
    }
    if (pd == 3 && hl >= 10) {
        int64_t v = pes_read_ts(b + 14);
        if (v >= 0)
            dts = v;
    }
    if (dts == PTS_NONE)
        dts = pts;
    size_t es0 = 9 + hl;
    /* PES_packet_length != 0 limita los datos (puede haber relleno detrás) */
    size_t pl = (b[4] << 8) | b[5];
    size_t end = n;
    if (pl && 6 + pl < n)
        end = 6 + pl;
    if (p->fn && end > es0)
        p->fn(p->ctx, b + es0, end - es0, pts, dts);
}

void pes_feed(pes_asm_t *p, const ts_pkt_t *k)
{
    if (!k->payload || k->dup || k->scrambled)
        return;
    if (k->tei) { /* paquete que el RS no pudo corregir: su carga no vale; la imagen en curso queda rota */
        if (p->active)
            p->dropped++;
        p->len = 0;
        p->active = false;
        return;
    }
    if (k->cc_err && p->active) {
        /* se perdió algo en medio: la imagen estaría rota; se descarta entera */
        p->len = 0;
        p->active = false;
        p->dropped++;
    }
    if (k->pusi) {
        if (p->active && p->len)
            finish(p);
        p->len = 0;
        p->active = true;
    } else if (!p->active) {
        return;
    }
    if (p->len + (size_t)k->plen > p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 65536;
        while (nc < p->len + (size_t)k->plen)
            nc *= 2;
        if (nc > (size_t)16 << 20) { /* PES absurdo */
            p->len = 0;
            p->active = false;
            return;
        }
        uint8_t *nb = realloc(p->buf, nc);
        if (!nb)
            return;
        p->buf = nb;
        p->cap = nc;
    }
    /* Q3 (fuzz): un inicio de PES con plen == 0 (campo de adaptación que ocupa todo el
     * paquete) llegaba aquí con p->buf == NULL y memcpy(NULL, …, 0) es indefinido. */
    if (k->plen > 0)
        memcpy(p->buf + p->len, k->payload, (size_t)k->plen);
    p->len += (size_t)k->plen;
    /* PES con longitud declarada: se cierra en cuanto está completo (audio) */
    if (p->len >= 6) {
        size_t pl = (p->buf[4] << 8) | p->buf[5];
        if (pl && p->len >= 6 + pl)
            finish(p);
    }
}

/* ======================================================================= */
/*  H.264 Annex B                                                          */
/* ======================================================================= */
/* Posición del próximo código de inicio 00 00 01 a partir de i (apunta al primer 00 de
 * los dos últimos ceros), o len si no hay. */
static size_t next_start(const uint8_t *d, size_t len, size_t i)
{
    for (; i + 3 <= len; i++)
        if (d[i + 2] > 1)
            i += 2; /* salto rápido: ningún 00 00 01 puede empezar antes */
        else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
            return i;
    return len;
}

const uint8_t *annexb_find_nal(const uint8_t *d, size_t len, int type, size_t *nal_len)
{
    size_t i = next_start(d, len, 0);
    while (i < len) {
        size_t s = i + 3;
        size_t e = next_start(d, len, s);
        size_t ne = e;
        while (ne > s && d[ne - 1] == 0) /* ceros de relleno / 4.º byte del siguiente código */
            ne--;
        if (s < len && (d[s] & 0x1F) == type) {
            if (nal_len)
                *nal_len = ne - s;
            return d + s;
        }
        i = e;
    }
    return NULL;
}

/* Lector de bits con eliminación de emulation_prevention (00 00 03). */
typedef struct { const uint8_t *d; size_t n, pos; int zeros; } br_t;
static int br_bit(br_t *b)
{
    size_t byte = b->pos >> 3;
    if (byte >= b->n)
        return 0;
    int v = (b->d[byte] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static uint32_t br_bits(br_t *b, int n)
{
    uint32_t v = 0;
    while (n--)
        v = (v << 1) | br_bit(b);
    return v;
}
static uint32_t br_ue(br_t *b)
{
    int z = 0;
    while (!br_bit(b) && z < 32 && b->pos < b->n * 8)
        z++;
    return z ? ((1u << z) - 1 + br_bits(b, z)) : 0;
}
static int32_t br_se(br_t *b)
{
    uint32_t k = br_ue(b);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

int h264_sps_size(const uint8_t *nal, size_t len, int *w, int *h)
{
    if (len < 4 || (nal[0] & 0x1F) != 7)
        return 0;
    /* quitar emulation prevention */
    uint8_t rbsp[256];
    size_t n = 0;
    for (size_t i = 1; i < len && n < sizeof rbsp; i++) {
        if (i >= 3 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0)
            continue;
        rbsp[n++] = nal[i];
    }
    br_t b = {rbsp, n, 0, 0};
    int profile = (int)br_bits(&b, 8);
    br_bits(&b, 8);
    br_bits(&b, 8);
    br_ue(&b); /* sps_id */
    int chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        chroma = (int)br_ue(&b);
        if (chroma == 3)
            br_bit(&b);
        br_ue(&b);
        br_ue(&b);
        br_bit(&b);
        if (br_bit(&b)) { /* seq_scaling_matrix */
            int cnt = chroma != 3 ? 8 : 12;
            for (int i = 0; i < cnt; i++)
                if (br_bit(&b)) {
                    int sz = i < 6 ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < sz; j++) {
                        if (next) {
                            next = (last + br_se(&b) + 256) % 256;
                        }
                        last = next ? next : last;
                    }
                }
        }
    }
    br_ue(&b); /* log2_max_frame_num */
    uint32_t poc = br_ue(&b);
    if (poc == 0)
        br_ue(&b);
    else if (poc == 1) {
        br_bit(&b);
        br_se(&b);
        br_se(&b);
        uint32_t c = br_ue(&b);
        for (uint32_t i = 0; i < c && i < 256; i++)
            br_se(&b);
    }
    br_ue(&b); /* max_num_ref_frames */
    br_bit(&b);
    uint32_t mbw = br_ue(&b) + 1, mbh = br_ue(&b) + 1;
    int frame_mbs_only = br_bit(&b);
    if (!frame_mbs_only)
        br_bit(&b);
    br_bit(&b); /* direct_8x8 */
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (br_bit(&b)) {
        cl = br_ue(&b);
        cr = br_ue(&b);
        ct = br_ue(&b);
        cb = br_ue(&b);
    }
    int cux = (chroma == 1 || chroma == 2) ? 2 : 1;
    int cuy = (chroma == 1 ? 2 : 1) * (2 - frame_mbs_only);
    if (chroma == 0) { cux = 1; cuy = 2 - frame_mbs_only; }
    int W = (int)(mbw * 16) - (int)(cux * (cl + cr));
    int H = (int)((2 - frame_mbs_only) * mbh * 16) - (int)(cuy * (ct + cb));
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192)
        return 0;
    *w = W;
    *h = H;
    return 1;
}

struct h264_split {
    h264_au_fn fn;
    void *ctx;
    uint8_t *buf;
    size_t len, cap;
    int64_t pts, dts;
};

h264_split_t *h264_split_create(h264_au_fn fn, void *ctx)
{
    h264_split_t *s = calloc(1, sizeof *s);
    if (s) {
        s->fn = fn;
        s->ctx = ctx;
        s->pts = s->dts = PTS_NONE;
    }
    return s;
}
void h264_split_destroy(h264_split_t *s)
{
    if (s) {
        free(s->buf);
        free(s);
    }
}
void h264_split_reset(h264_split_t *s)
{
    s->len = 0;
    s->pts = s->dts = PTS_NONE;
}

void h264_split_flush(h264_split_t *s)
{
    if (s->len == 0)
        return;
    h264_au_t au = {.data = s->buf, .len = s->len, .pts = s->pts, .dts = s->dts};
    bool sps = false, pps = false;
    size_t i = next_start(s->buf, s->len, 0);
    while (i < s->len) {
        size_t st = i + 3;
        if (st >= s->len)
            break;
        int t = s->buf[st] & 0x1F;
        if (t == 5)
            au.key = true;
        else if (t == 7)
            sps = true;
        else if (t == 8)
            pps = true;
        else if (t == 6 && st + 2 < s->len && s->buf[st + 1] == 6)
            au.key = true; /* SEI payloadType 6: recovery point */
        i = next_start(s->buf, s->len, st);
    }
    au.has_sps = sps && pps;
    if (s->fn)
        s->fn(s->ctx, &au);
    s->len = 0;
    s->pts = s->dts = PTS_NONE;
}

static void append(h264_split_t *s, const uint8_t *d, size_t n)
{
    if (s->len + n > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1 << 18;
        while (nc < s->len + n)
            nc *= 2;
        uint8_t *nb = realloc(s->buf, nc);
        if (!nb)
            return;
        s->buf = nb;
        s->cap = nc;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
}

void h264_split_feed(h264_split_t *s, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    size_t seg = 0; /* inicio del trozo aún no copiado */
    /* PES con PTS ⇒ empieza AU nueva (con o sin AUD) */
    if (pts != PTS_NONE) {
        h264_split_flush(s);
        s->pts = pts;
        s->dts = dts;
    }
    size_t i = next_start(es, len, 0);
    while (i < len) {
        size_t st = i + 3;
        if (st >= len)
            break;
        if ((es[st] & 0x1F) == 9) {
            /* AUD: empieza AU nueva. El 4.º byte 00 del código de inicio largo es suyo. */
            size_t cut = (i > 0 && es[i - 1] == 0) ? i - 1 : i;
            if (cut > seg)
                append(s, es + seg, cut - seg);
            if (s->len)
                h264_split_flush(s);
            seg = cut;
        }
        i = next_start(es, len, st);
    }
    append(s, es + seg, len - seg);
}
