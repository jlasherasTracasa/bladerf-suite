/*
 * dvbt_outer.c — ver dvbt_outer.h.
 */
#include "dvbt_outer.h"

#include "../dsp/fec.h"
#include "dvbt_tab.h"

#include <stdlib.h>
#include <string.h>

#define SRCH_PKTS 8            /* paquetes de la ventana de búsqueda de sincronismo */
#define BUFCAP (204 * 40)

struct dvbt_outer {
    rs_t rs;
    uint8_t buf[BUFCAP + 256];  /* bytes sin alinear (búsqueda) */
    int nb;
    int bitoff;                 /* desfase de bits detectado (0..7) */
    uint8_t carry;              /* byte anterior para aplicar el desfase de bits */
    bool have_carry;
    int synced;
    /* desentrelazador: rama j, FIFO de (11−j)·17 bytes */
    uint8_t *fifo[12];
    int fpos[12];
    int branch;                 /* rama del próximo byte */
    uint8_t pkt[204];
    int pn;                     /* bytes acumulados del paquete en curso */
    int warm;                   /* paquetes de latencia por descartar */
    int grp;                    /* posición del paquete en el grupo de 8 (−1 desconocida) */
    int bad_sync;               /* sincronías erróneas consecutivas del flujo entrelazado */
    uint8_t hist[64];           /* 1 = fallo, últimos 64 paquetes */
    int hpos, hcount;
    uint8_t outbuf[188 * 32];
    int outn;
    dvbt_outer_stats_t st;
};

dvbt_outer_t *dvbt_outer_create(void)
{
    dvbt_outer_t *o = calloc(1, sizeof *o);
    if (!o)
        return NULL;
    rs_init_dvb(&o->rs);
    for (int j = 0; j < 12; j++)
        if (11 - j)
            o->fifo[j] = calloc((size_t)(11 - j) * 17, 1);
    dvbt_outer_reset(o);
    return o;
}

void dvbt_outer_destroy(dvbt_outer_t *o)
{
    if (!o)
        return;
    for (int j = 0; j < 12; j++)
        free(o->fifo[j]);
    free(o);
}

void dvbt_outer_reset(dvbt_outer_t *o)
{
    o->nb = 0;
    o->synced = 0;
    o->bitoff = 0;
    o->have_carry = false;
    for (int j = 0; j < 12; j++) {
        if (o->fifo[j])
            memset(o->fifo[j], 0, (size_t)(11 - j) * 17);
        o->fpos[j] = 0;
    }
    o->branch = 0;
    o->pn = 0;
    o->warm = 11;
    o->grp = -1;
    o->bad_sync = 0;
    o->outn = 0;
    memset(o->hist, 1, sizeof o->hist);
    o->hcount = 64;
    o->hpos = 0;
    o->st.synced = 0;
}

void dvbt_outer_stats(const dvbt_outer_t *o, dvbt_outer_stats_t *st)
{
    *st = o->st;
    st->synced = o->synced;
}

int dvbt_outer_fail_pct(const dvbt_outer_t *o)
{
    if (!o->synced)
        return 100;
    int f = 0;
    for (int i = 0; i < 64; i++)
        f += o->hist[i];
    return f * 100 / 64;
}

/* Byte i del flujo con desfase de bits 'off' (0..7) sobre b[] (hace falta b[i+1]). */
static inline uint8_t shifted(const uint8_t *b, int i, int off)
{
    return off ? (uint8_t)((b[i] << off) | (b[i + 1] >> (8 - off))) : b[i];
}

static inline bool is_sync(uint8_t v) { return v == 0x47 || v == 0xB8; }

/* Busca el desfase periódico. Devuelve true y fija *pos (bytes) y *off (bits). */
static bool find_sync(const uint8_t *b, int n, int *pos, int *off)
{
    int best = 0, bp = 0, bo = 0;
    for (int o = 0; o < 8; o++)
        for (int j = 0; j < 204; j++) {
            int sc = 0, tot = 0;
            for (int m = 0; j + 204 * m + 1 < n && m < SRCH_PKTS; m++, tot++)
                sc += is_sync(shifted(b, j + 204 * m, o));
            if (tot >= SRCH_PKTS - 1 && sc > best) {
                best = sc;
                bp = j;
                bo = o;
            }
        }
    if (best >= SRCH_PKTS - 2) {
        *pos = bp;
        *off = bo;
        return true;
    }
    return false;
}

static void flush_out(dvbt_outer_t *o, dvbt_outer_sink_fn sink, void *ctx)
{
    if (o->outn && sink)
        sink(ctx, o->outbuf, o->outn);
    o->st.out_pkts += (unsigned long)o->outn;
    o->outn = 0;
}

/* Un paquete de 204 B desentrelazado completo. */
static void handle_packet(dvbt_outer_t *o, uint8_t *p, dvbt_outer_sink_fn sink, void *ctx)
{
    if (o->warm > 0) {
        o->warm--;
        return;
    }
    uint8_t orig[188];
    memcpy(orig, p, 188);
    int r = rs_decode(&o->rs, p, 204, NULL, 0);
    bool ok = r >= 0 && (p[0] == 0x47 || p[0] == 0xB8);
    if (r >= 0 && !ok)
        r = -1; /* "corregido" pero sin sincronía válida: falso positivo del RS */
    if (ok) {
        if (r == 0)
            o->st.rs_ok++;
        else {
            o->st.rs_corrected++;
            for (int i = 0; i < 188; i++)
                o->st.bit_errors += (unsigned long)__builtin_popcount((unsigned)(orig[i] ^ p[i]));
        }
        o->st.bits += 188 * 8;
        if (p[0] == 0xB8)
            o->grp = 0;
        else if (o->grp == 0 && o->st.rs_ok + o->st.rs_corrected > 16 && 0) {
            /* reservado */
        }
    } else {
        o->st.rs_failed++;
        memcpy(p, orig, 188); /* RS no corrigió: conserva los datos recibidos */
    }
    o->hist[o->hpos] = ok ? 0 : 1;
    o->hpos = (o->hpos + 1) & 63;
    if (o->grp < 0) {
        /* sin grupo no se puede desaleatorizar; espera al primer 0xB8 */
        return;
    }
    /* desaleatoriza (la PRBS corre también sobre los sincronismos 1…7 sin aplicarse) */
    const uint8_t *prbs = dvbt_prbs1503();
    uint8_t *out = o->outbuf + (size_t)o->outn * 188;
    out[0] = 0x47;
    int base = o->grp * 188;
    for (int i = 1; i < 188; i++)
        out[i] = (uint8_t)(p[i] ^ prbs[base + i - 1]);
    if (!ok)
        out[1] |= 0x80; /* transport_error_indicator */
    o->grp = (o->grp + 1) & 7;
    if (++o->outn == 32)
        flush_out(o, sink, ctx);
}

/* Procesa un byte alineado del flujo entrelazado. */
static void feed_aligned(dvbt_outer_t *o, uint8_t byte, dvbt_outer_sink_fn sink, void *ctx)
{
    int j = o->branch;
    uint8_t out = byte;
    if (11 - j) {
        int len = (11 - j) * 17;
        out = o->fifo[j][o->fpos[j]];
        o->fifo[j][o->fpos[j]] = byte;
        o->fpos[j] = o->fpos[j] + 1 == len ? 0 : o->fpos[j] + 1;
    }
    o->branch = j == 11 ? 0 : j + 1;
    /* comprobación de sincronía del flujo entrelazado (primer byte de cada paquete) */
    if (o->pn == 0) {
        if (is_sync(byte))
            o->bad_sync = 0;
        else if (++o->bad_sync > 8) {
            o->synced = 0;
            o->st.synced = 0;
            o->nb = 0;
            return;
        }
    }
    o->pkt[o->pn++] = out;
    if (o->pn == 204) {
        o->pn = 0;
        handle_packet(o, o->pkt, sink, ctx);
    }
}

void dvbt_outer_push(dvbt_outer_t *o, const uint8_t *bytes, int nbytes, dvbt_outer_sink_fn sink, void *ctx)
{
    int i = 0;
    while (i < nbytes) {
        if (o->synced) {
            /* alinea los bytes con el desfase de bits; carry guarda el anterior */
            for (; i < nbytes && o->synced; i++) {
                uint8_t cur = bytes[i], v;
                if (!o->have_carry) {
                    o->carry = cur;
                    o->have_carry = true;
                    continue;
                }
                v = o->bitoff ? (uint8_t)((o->carry << o->bitoff) | (cur >> (8 - o->bitoff))) : o->carry;
                o->carry = cur;
                feed_aligned(o, v, sink, ctx);
            }
            continue;
        }
        /* búsqueda */
        int take = MIN(nbytes - i, BUFCAP - o->nb);
        memcpy(o->buf + o->nb, bytes + i, (size_t)take);
        o->nb += take;
        i += take;
        if (o->nb >= 204 * (SRCH_PKTS + 1) + 2) {
            int pos, off;
            if (find_sync(o->buf, o->nb, &pos, &off)) {
                /* reinicia el desentrelazador y reprocesa lo almacenado ya alineado */
                for (int j = 0; j < 12; j++) {
                    if (o->fifo[j])
                        memset(o->fifo[j], 0, (size_t)(11 - j) * 17);
                    o->fpos[j] = 0;
                }
                o->branch = 0;
                o->pn = 0;
                o->warm = 11;
                o->grp = -1;
                o->bad_sync = 0;
                o->bitoff = off;
                o->have_carry = false;
                o->synced = 1;
                o->st.synced = 1;
                int n = o->nb;
                o->nb = 0;
                /* los bytes a partir de pos (con desfase de bits) */
                o->carry = o->buf[pos];
                o->have_carry = true;
                for (int k = pos + 1; k < n; k++) {
                    uint8_t cur = o->buf[k];
                    uint8_t v = off ? (uint8_t)((o->carry << off) | (cur >> (8 - off))) : o->carry;
                    o->carry = cur;
                    if (o->synced)
                        feed_aligned(o, v, sink, ctx);
                }
            } else {
                /* descarta la mitad más antigua y sigue buscando */
                int keep = 204 * 4;
                memmove(o->buf, o->buf + o->nb - keep, (size_t)keep);
                o->nb = keep;
            }
        }
    }
    flush_out(o, sink, ctx);
}
