/*
 * ts.c — Demultiplexor de Transport Stream (ver ts.h).
 */
#include "ts.h"

#include <stdlib.h>
#include <string.h>

int ts_parse(const uint8_t *p, ts_pkt_t *o)
{
    if (p[0] != 0x47)
        return -1;
    memset(o, 0, sizeof *o);
    o->tei = (p[1] & 0x80) != 0;
    o->pusi = (p[1] & 0x40) != 0;
    o->pid = ((p[1] & 0x1F) << 8) | p[2];
    o->scrambled = (p[3] & 0xC0) != 0;
    int afc = (p[3] >> 4) & 3;
    o->cc = p[3] & 15;
    int off = 4;
    if (afc & 2) {
        /* adaptation_field_length no incluye su propio byte; máx. 183 con carga
         * y 183 sin ella (relleno hasta 188) */
        int al = p[4];
        if (al > 183)
            return -1;
        off = 5 + al;
        if (al >= 1) {
            uint8_t fl = p[5];
            o->discont = (fl & 0x80) != 0;
            if ((fl & 0x10) && al >= 7) {
                /* PCR_base 33 bits, 6 reservados, PCR_ext 9 bits */
                uint64_t base = ((uint64_t)p[6] << 25) | ((uint64_t)p[7] << 17) | ((uint64_t)p[8] << 9) |
                                ((uint64_t)p[9] << 1) | (p[10] >> 7);
                int ext = ((p[10] & 1) << 8) | p[11];
                o->has_pcr = true;
                o->pcr27 = (int64_t)(base * 300 + ext);
            }
        }
    }
    if ((afc & 1) && off < TS_PKT) {
        o->payload = p + off;
        o->plen = TS_PKT - off;
    }
    return 0;
}

struct ts_demux {
    ts_pkt_fn fn;
    void *ctx;
    uint8_t buf[TS_PKT * 4]; /* resto sin procesar entre llamadas */
    size_t nbuf;
    bool synced;
    int miss;
    uint8_t last_cc[8192]; /* 0xFF = desconocido */
    ts_stats_t st;
};

ts_demux_t *ts_demux_create(ts_pkt_fn fn, void *ctx)
{
    ts_demux_t *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->fn = fn;
    d->ctx = ctx;
    ts_demux_reset(d);
    return d;
}

void ts_demux_destroy(ts_demux_t *d) { free(d); }

void ts_demux_reset(ts_demux_t *d)
{
    d->nbuf = 0;
    d->synced = false;
    d->miss = 0;
    memset(d->last_cc, 0xFF, sizeof d->last_cc);
}

void ts_demux_stats(const ts_demux_t *d, ts_stats_t *st) { *st = d->st; }

/* Reglas de continuidad (13818-1 §2.4.3.3): con carga útil el contador sube 1; sin
 * carga se repite el del paquete anterior; un paquete idéntico puede duplicarse una vez. */
static void handle(ts_demux_t *d, const uint8_t *p)
{
    ts_pkt_t k;
    if (ts_parse(p, &k) < 0)
        return;
    d->st.packets++;
    if (k.tei)
        d->st.tei++;
    if (k.pid == TS_PID_NULL) {
        d->st.null_pkts++;
        return;
    }
    int afc = (p[3] >> 4) & 3;
    uint8_t last = d->last_cc[k.pid];
    if (afc & 1) {
        if (last != 0xFF) {
            if (k.cc == last && !k.discont)
                k.dup = true;
            else if (k.cc != ((last + 1) & 15) && !k.discont) {
                k.cc_err = true;
                d->st.cc_errors++;
            }
        }
        d->last_cc[k.pid] = (uint8_t)k.cc;
    }
    /* un paquete con TEI no es fiable: tratarlo como pérdida */
    if (k.tei)
        k.cc_err = true;
    if (d->fn)
        d->fn(d->ctx, &k);
}

void ts_demux_feed(ts_demux_t *d, const uint8_t *data, size_t len)
{
    while (len) {
        size_t room = sizeof d->buf - d->nbuf, n = len < room ? len : room;
        memcpy(d->buf + d->nbuf, data, n);
        d->nbuf += n;
        data += n;
        len -= n;
        size_t pos = 0;
        for (;;) {
            if (!d->synced) {
                /* buscar 0x47 repetido a 188 B (hasta 3 veces) */
                size_t i = pos;
                bool found = false;
                while (i + TS_PKT * 3 + 1 <= d->nbuf) {
                    if (d->buf[i] == 0x47 && d->buf[i + TS_PKT] == 0x47 && d->buf[i + 2 * TS_PKT] == 0x47 &&
                        d->buf[i + 3 * TS_PKT] == 0x47) {
                        found = true;
                        break;
                    }
                    i++;
                }
                if (!found) {
                    pos = i; /* lo anterior no sirve */
                    break;
                }
                pos = i;
                d->synced = true;
                d->miss = 0;
            }
            if (pos + TS_PKT > d->nbuf)
                break;
            if (d->buf[pos] == 0x47) {
                d->miss = 0;
                handle(d, d->buf + pos);
                pos += TS_PKT;
            } else {
                if (++d->miss >= 3) {
                    d->synced = false;
                    d->st.sync_losses++;
                    pos++;
                } else {
                    pos += TS_PKT; /* paquete perdido: seguir en la rejilla */
                }
            }
        }
        memmove(d->buf, d->buf + pos, d->nbuf - pos);
        d->nbuf -= pos;
    }
}
