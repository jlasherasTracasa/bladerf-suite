/*
 * ts_build.h — Constructor mínimo de Transport Streams para las pruebas de ts, psi
 * y tv: secciones PSI/SI (PAT, PMT, SDT, NIT) con CRC-32/MPEG-2, paquetes PES con
 * PTS/DTS y troceado en paquetes de 188 bytes con contadores de continuidad.
 * Referencias: ISO/IEC 13818-1 §2.4.3–2.4.4 y ETSI EN 300 468.
 */
#ifndef BLADERF_LAB_TS_BUILD_H
#define BLADERF_LAB_TS_BUILD_H

#include "tv/psi.h"
#include "tv/ts.h"

#include <stdint.h>
#include <string.h>

typedef struct { uint8_t cc[8192]; } tsb_t;

/* Cierra una sección larga: cabecera de 3 bytes (la longitud incluye CRC) + CRC. */
static inline size_t tsb_finish_section(uint8_t *s, size_t body_len /* tras los 3 primeros bytes, sin CRC */)
{
    size_t sl = body_len + 4;
    s[1] = (uint8_t)((s[1] & 0xF0) | ((sl >> 8) & 0x0F));
    s[2] = (uint8_t)sl;
    uint32_t c = crc32_mpeg2(s, 3 + body_len);
    s[3 + body_len] = (uint8_t)(c >> 24);
    s[4 + body_len] = (uint8_t)(c >> 16);
    s[5 + body_len] = (uint8_t)(c >> 8);
    s[6 + body_len] = (uint8_t)c;
    return 3 + sl;
}

static inline size_t tsb_long_header(uint8_t *s, int table_id, int ext, int ver)
{
    s[0] = (uint8_t)table_id;
    s[1] = 0xB0;
    s[3] = (uint8_t)(ext >> 8);
    s[4] = (uint8_t)ext;
    s[5] = (uint8_t)(0xC1 | (ver << 1));
    s[6] = 0;
    s[7] = 0;
    return 8;
}

static inline size_t tsb_pat(uint8_t *s, int tsid, int ver, const int *sid, const int *pmt_pid, int n)
{
    size_t o = tsb_long_header(s, 0x00, tsid, ver);
    for (int i = 0; i < n; i++) {
        s[o++] = (uint8_t)(sid[i] >> 8);
        s[o++] = (uint8_t)sid[i];
        s[o++] = (uint8_t)(0xE0 | (pmt_pid[i] >> 8));
        s[o++] = (uint8_t)pmt_pid[i];
    }
    return tsb_finish_section(s, o - 3);
}

typedef struct { int type, pid; const uint8_t *desc; int dlen; } tsb_es_t;

static inline size_t tsb_pmt(uint8_t *s, int sid, int ver, int pcr_pid, const tsb_es_t *es, int n)
{
    size_t o = tsb_long_header(s, 0x02, sid, ver);
    s[o++] = (uint8_t)(0xE0 | (pcr_pid >> 8));
    s[o++] = (uint8_t)pcr_pid;
    s[o++] = 0xF0;
    s[o++] = 0; /* program_info_length */
    for (int i = 0; i < n; i++) {
        s[o++] = (uint8_t)es[i].type;
        s[o++] = (uint8_t)(0xE0 | (es[i].pid >> 8));
        s[o++] = (uint8_t)es[i].pid;
        s[o++] = (uint8_t)(0xF0 | (es[i].dlen >> 8));
        s[o++] = (uint8_t)es[i].dlen;
        if (es[i].dlen)
            memcpy(s + o, es[i].desc, (size_t)es[i].dlen);
        o += (size_t)es[i].dlen;
    }
    return tsb_finish_section(s, o - 3);
}

/* Un servicio del SDT con nombre y proveedor ya codificados en DVB (bytes crudos). */
static inline size_t tsb_sdt_service(uint8_t *s, size_t o, int sid, int svc_type, const uint8_t *prov, int pl,
                                     const uint8_t *name, int nl)
{
    s[o++] = (uint8_t)(sid >> 8);
    s[o++] = (uint8_t)sid;
    s[o++] = 0xFC;
    int dl = 2 + 3 + pl + nl;
    s[o++] = (uint8_t)(0x80 | (dl >> 8)); /* running_status 4, free_CA 0 */
    s[o++] = (uint8_t)dl;
    s[o++] = 0x48;
    s[o++] = (uint8_t)(3 + pl + nl);
    s[o++] = (uint8_t)svc_type;
    s[o++] = (uint8_t)pl;
    memcpy(s + o, prov, (size_t)pl);
    o += (size_t)pl;
    s[o++] = (uint8_t)nl;
    memcpy(s + o, name, (size_t)nl);
    return o + (size_t)nl;
}

static inline size_t tsb_sdt_begin(uint8_t *s, int tsid, int onid, int ver)
{
    size_t o = tsb_long_header(s, 0x42, tsid, ver);
    s[o++] = (uint8_t)(onid >> 8);
    s[o++] = (uint8_t)onid;
    s[o++] = 0xFF;
    return o;
}

/* NIT: nombre de red, un TS con frecuencia (Hz) y LCN para los servicios dados. */
static inline size_t tsb_nit(uint8_t *s, int net_id, int ver, const char *net_name, int tsid, int onid, int64_t freq_hz,
                             const int *sid, const int *lcn, int n)
{
    size_t o = tsb_long_header(s, 0x40, net_id, ver);
    int nl = (int)strlen(net_name);
    s[o++] = (uint8_t)(0xF0 | ((2 + nl) >> 8));
    s[o++] = (uint8_t)(2 + nl);
    s[o++] = 0x40;
    s[o++] = (uint8_t)nl;
    memcpy(s + o, net_name, (size_t)nl);
    o += (size_t)nl;
    size_t ts_len_pos = o;
    o += 2;
    size_t ts0 = o;
    s[o++] = (uint8_t)(tsid >> 8);
    s[o++] = (uint8_t)tsid;
    s[o++] = (uint8_t)(onid >> 8);
    s[o++] = (uint8_t)onid;
    size_t dl_pos = o;
    o += 2;
    size_t d0 = o;
    /* terrestrial_delivery_system_descriptor: frecuencia en pasos de 10 Hz */
    uint32_t f = (uint32_t)(freq_hz / 10);
    s[o++] = 0x5A;
    s[o++] = 11;
    s[o++] = (uint8_t)(f >> 24);
    s[o++] = (uint8_t)(f >> 16);
    s[o++] = (uint8_t)(f >> 8);
    s[o++] = (uint8_t)f;
    memset(s + o, 0, 7);
    o += 7;
    /* descriptor privado EACEM: private_data_specifier 0x28 + logical_channel_descriptor 0x83 */
    s[o++] = 0x5F;
    s[o++] = 4;
    s[o++] = 0;
    s[o++] = 0;
    s[o++] = 0;
    s[o++] = 0x28;
    s[o++] = 0x83;
    s[o++] = (uint8_t)(4 * n);
    for (int i = 0; i < n; i++) {
        s[o++] = (uint8_t)(sid[i] >> 8);
        s[o++] = (uint8_t)sid[i];
        s[o++] = (uint8_t)(0xFC | (lcn[i] >> 8));
        s[o++] = (uint8_t)lcn[i];
    }
    int dl = (int)(o - d0);
    s[dl_pos] = (uint8_t)(0xF0 | (dl >> 8));
    s[dl_pos + 1] = (uint8_t)dl;
    int tl = (int)(o - ts0);
    s[ts_len_pos] = (uint8_t)(0xF0 | (tl >> 8));
    s[ts_len_pos + 1] = (uint8_t)tl;
    return tsb_finish_section(s, o - 3);
}

/* Trocea una sección en paquetes (pointer_field = 0). Devuelve bytes escritos. */
static inline size_t tsb_packets_section(tsb_t *b, uint8_t *out, int pid, const uint8_t *sec, size_t len)
{
    size_t n = 0, pos = 0;
    bool first = true;
    while (pos < len) {
        uint8_t *p = out + n;
        memset(p, 0xFF, TS_PKT);
        p[0] = 0x47;
        p[1] = (uint8_t)((first ? 0x40 : 0) | (pid >> 8));
        p[2] = (uint8_t)pid;
        p[3] = (uint8_t)(0x10 | (b->cc[pid]++ & 15));
        size_t o = 4;
        if (first)
            p[o++] = 0;
        size_t c = len - pos < TS_PKT - o ? len - pos : TS_PKT - o;
        memcpy(p + o, sec + pos, c);
        pos += c;
        first = false;
        n += TS_PKT;
    }
    return n;
}

static inline void tsb_write_ts(uint8_t *p, int64_t v, int marker)
{
    p[0] = (uint8_t)((marker << 4) | (((v >> 30) & 7) << 1) | 1);
    p[1] = (uint8_t)(v >> 22);
    p[2] = (uint8_t)((((v >> 15) & 0x7F) << 1) | 1);
    p[3] = (uint8_t)(v >> 7);
    p[4] = (uint8_t)(((v & 0x7F) << 1) | 1);
}

/* PES completo → paquetes TS. pcr27 ≥ 0 añade PCR en el primer paquete. dts < 0: solo PTS. */
static inline size_t tsb_packets_pes(tsb_t *b, uint8_t *out, int pid, int stream_id, int64_t pts, int64_t dts,
                                     const uint8_t *es, size_t len, int64_t pcr27, bool zero_len)
{
    static uint8_t pes[1 << 20];
    size_t o = 0;
    bool has_dts = dts >= 0 && dts != pts;
    pes[o++] = 0;
    pes[o++] = 0;
    pes[o++] = 1;
    pes[o++] = (uint8_t)stream_id;
    size_t lenpos = o;
    o += 2;
    pes[o++] = 0x80;
    pes[o++] = (uint8_t)(has_dts ? 0xC0 : 0x80);
    pes[o++] = (uint8_t)(has_dts ? 10 : 5);
    tsb_write_ts(pes + o, pts, has_dts ? 3 : 2);
    o += 5;
    if (has_dts) {
        tsb_write_ts(pes + o, dts, 1);
        o += 5;
    }
    memcpy(pes + o, es, len);
    o += len;
    size_t pl = o - 6;
    if (zero_len || pl > 0xFFFF) {
        pes[lenpos] = pes[lenpos + 1] = 0;
    } else {
        pes[lenpos] = (uint8_t)(pl >> 8);
        pes[lenpos + 1] = (uint8_t)pl;
    }
    size_t n = 0, pos = 0;
    bool first = true;
    while (pos < o) {
        uint8_t *p = out + n;
        bool pcr = first && pcr27 >= 0;
        size_t rest = o - pos;
        size_t af = pcr ? 8 : 0; /* bytes totales del campo de adaptación, con su byte de longitud */
        size_t room = TS_PKT - 4 - af;
        if (rest < room) { /* último paquete: relleno dentro del campo de adaptación */
            af += room - rest;
            room = rest;
        }
        p[0] = 0x47;
        p[1] = (uint8_t)((first ? 0x40 : 0) | (pid >> 8));
        p[2] = (uint8_t)pid;
        p[3] = (uint8_t)((af ? 0x30 : 0x10) | (b->cc[pid]++ & 15));
        if (af) {
            p[4] = (uint8_t)(af - 1);
            if (af >= 2) {
                p[5] = pcr ? 0x10 : 0x00;
                memset(p + 6, 0xFF, af - 2);
                if (pcr) {
                    uint64_t base = (uint64_t)(pcr27 / 300);
                    int ext = (int)(pcr27 % 300);
                    p[6] = (uint8_t)(base >> 25);
                    p[7] = (uint8_t)(base >> 17);
                    p[8] = (uint8_t)(base >> 9);
                    p[9] = (uint8_t)(base >> 1);
                    p[10] = (uint8_t)(((base & 1) << 7) | 0x7E | (ext >> 8));
                    p[11] = (uint8_t)ext;
                }
            }
        }
        memcpy(p + 4 + af, pes + pos, room);
        pos += room;
        first = false;
        n += TS_PKT;
    }
    return n;
}

#endif
