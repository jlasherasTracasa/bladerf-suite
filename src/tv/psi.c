/*
 * psi.c — PAT, PMT, SDT, NIT y texto DVB (ver psi.h).
 */
#include "psi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/*  CRC-32/MPEG-2                                                          */
/* ======================================================================= */
static uint32_t crc_tab[256];
static bool crc_ready;

static void crc_init_tab(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        crc_tab[i] = c;
    }
    crc_ready = true;
}

uint32_t crc32_mpeg2(const uint8_t *d, size_t n)
{
    if (!crc_ready)
        crc_init_tab(); /* idempotente: carrera benigna (mismo contenido) */
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = (c << 8) ^ crc_tab[(c >> 24) ^ d[i]];
    return c;
}

/* ======================================================================= */
/*  Texto DVB → UTF-8                                                      */
/* ======================================================================= */
static size_t put_utf8(char *out, size_t cap, size_t n, uint32_t cp)
{
    uint8_t b[4];
    int l;
    if (cp < 0x80) { b[0] = (uint8_t)cp; l = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 63); l = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); l = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); l = 4; }
    if (n + (size_t)l >= cap)
        return n;
    for (int i = 0; i < l; i++)
        out[n++] = (char)b[i];
    return n;
}

/* ISO 6937 (EN 300 468 Tabla A.1, variante DVB): 0xA0–0xFF. 0 = sin asignar.
 * 0xC1–0xCF son acentos «delante» (combinan con la letra siguiente). */
static const uint16_t iso6937_hi[96] = {
    /* A0 */ 0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x20AC, 0x00A5, 0, 0x00A7, 0x00A4, 0x2018, 0x201C, 0x00AB, 0x2190, 0x2191, 0x2192, 0x2193,
    /* B0 */ 0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00D7, 0x00B5, 0x00B6, 0x00B7, 0x00F7, 0x2019, 0x201D, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    /* C0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* D0 */ 0x2015, 0x00B9, 0x00AE, 0x00A9, 0x2122, 0x266A, 0x00AC, 0x00A6, 0, 0, 0, 0, 0x215B, 0x215C, 0x215D, 0x215E,
    /* E0 */ 0x2126, 0x00C6, 0x0110, 0x00AA, 0x0126, 0, 0x0132, 0x013F, 0x0141, 0x00D8, 0x0152, 0x00BA, 0x00DE, 0x0166, 0x014A, 0x0149,
    /* F0 */ 0x0138, 0x00E6, 0x0111, 0x00F0, 0x0127, 0x0131, 0x0133, 0x0140, 0x0142, 0x00F8, 0x0153, 0x00DF, 0x00FE, 0x0167, 0x014B, 0x00AD,
};

/* Composición de acento + letra → carácter precompuesto (los que usan las lenguas
 * de España y Europa occidental). Si no está, se emite letra + marca combinante. */
/* Q3 (fuzz): res[] tenía 16 huecos pero la fila del acento agudo lista 24 letras, y
 * compose() leía res[16..23] fuera del array (y devolvía basura) con "ś", "ź", "ĺ"… */
typedef struct { uint16_t mark; const char *base; uint16_t res[24]; } comp_t;
static const comp_t comps[] = {
    {0x0300, "AEIOUaeiou", {0xC0, 0xC8, 0xCC, 0xD2, 0xD9, 0xE0, 0xE8, 0xEC, 0xF2, 0xF9}},
    {0x0301, "AEIOUYaeiouyCcNnSsZzLlRr", {0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xDD, 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFD, 0x106, 0x107, 0x143, 0x144,
                          0x15A, 0x15B, 0x179, 0x17A, 0x139, 0x13A, 0x154, 0x155}},
    {0x0302, "AEIOUaeiou", {0xC2, 0xCA, 0xCE, 0xD4, 0xDB, 0xE2, 0xEA, 0xEE, 0xF4, 0xFB}},
    {0x0303, "ANOanoIiUu", {0xC3, 0xD1, 0xD5, 0xE3, 0xF1, 0xF5, 0x128, 0x129, 0x168, 0x169}},
    {0x0308, "AEIOUaeiouy", {0xC4, 0xCB, 0xCF, 0xD6, 0xDC, 0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xFF}},
    {0x030A, "AaUu", {0xC5, 0xE5, 0x16E, 0x16F}},
    {0x0327, "Cc", {0xC7, 0xE7}},
    {0x030C, "CcEeSsZzRrNn", {0x10C, 0x10D, 0x11A, 0x11B, 0x160, 0x161, 0x17D, 0x17E, 0x158, 0x159, 0x147, 0x148}},
};

static const uint16_t comb_marks[16] = {
    /* C0..CF */ 0, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0306, 0x0307, 0x0308, 0, 0x030A, 0x0327, 0, 0x030B, 0x0328, 0x030C,
};

static uint32_t compose(uint16_t mark, uint8_t base)
{
    for (size_t i = 0; i < sizeof comps / sizeof comps[0]; i++) {
        if (comps[i].mark != mark)
            continue;
        const char *p = strchr(comps[i].base, base);
        if (p && base)
            return comps[i].res[p - comps[i].base];
    }
    return 0;
}

/* 8859-15 difiere de Latin-1 en 8 posiciones; 8859-9 en 6 */
static uint32_t latin_x(int part, uint8_t c)
{
    if (c < 0xA0)
        return c;
    if (part == 15) {
        switch (c) {
        case 0xA4: return 0x20AC; case 0xA6: return 0x160; case 0xA8: return 0x161; case 0xB4: return 0x17D;
        case 0xB8: return 0x17E; case 0xBC: return 0x152; case 0xBD: return 0x153; case 0xBE: return 0x178;
        }
    } else if (part == 9) {
        switch (c) {
        case 0xD0: return 0x11E; case 0xDD: return 0x130; case 0xDE: return 0x15E;
        case 0xF0: return 0x11F; case 0xFD: return 0x131; case 0xFE: return 0x15F;
        }
    }
    return c;
}

size_t dvb_text_to_utf8(const uint8_t *s, size_t len, char *out, size_t cap)
{
    size_t n = 0;
    if (cap == 0)
        return 0;
    enum { T6937, LATIN, UTF8, UCS2 } mode = T6937;
    int part = 1;
    size_t i = 0;
    if (len > 0 && s[0] < 0x20) {
        uint8_t c = s[0];
        if (c >= 0x01 && c <= 0x0B) { mode = LATIN; part = c == 0x0B ? 15 : c == 0x05 ? 9 : 0; i = 1; }
        else if (c == 0x10 && len >= 3) { mode = LATIN; part = s[2]; i = 3; }
        else if (c == 0x11) { mode = UCS2; i = 1; }
        else if (c == 0x15) { mode = UTF8; i = 1; }
        else i = 1; /* 0x12–0x14 (GB, Big5, KSC): no soportados, se leen como ASCII */
        if (mode == LATIN && part != 9 && part != 15)
            part = 1; /* 5–8 y 10–14 caen en Latin-1: solo difieren fuera de las lenguas habituales */
    }
    while (i < len) {
        uint8_t c = s[i];
        if (mode == UTF8) {
            if (c == 0xC2 && i + 1 < len && s[i + 1] >= 0x80 && s[i + 1] <= 0x9F) {
                if (s[i + 1] == 0x8A && n + 1 < cap) out[n++] = ' ';
                i += 2;
                continue;
            }
            if (n + 1 < cap) out[n++] = (char)c;
            i++;
            continue;
        }
        if (mode == UCS2) {
            if (i + 1 >= len)
                break;
            uint32_t cp = (uint32_t)(s[i] << 8) | s[i + 1];
            i += 2;
            if (cp >= 0xE080 && cp <= 0xE09F) {
                if (cp == 0xE08A) n = put_utf8(out, cap, n, ' ');
                continue;
            }
            if (cp >= 0x80 && cp <= 0x9F) continue;
            n = put_utf8(out, cap, n, cp);
            continue;
        }
        i++;
        if (c >= 0x80 && c <= 0x9F) {
            if (c == 0x8A)
                n = put_utf8(out, cap, n, ' ');
            continue; /* 0x86/0x87 énfasis y demás: se ignoran */
        }
        if (c < 0xA0) {
            if (c >= 0x20 || c == '\t')
                n = put_utf8(out, cap, n, c);
            continue;
        }
        if (mode == LATIN) {
            n = put_utf8(out, cap, n, latin_x(part, c));
            continue;
        }
        /* ISO 6937 */
        if (c >= 0xC1 && c <= 0xCF) {
            uint16_t mark = comb_marks[c - 0xC0];
            if (i < len && mark) {
                uint8_t b = s[i];
                uint32_t pre = compose(mark, b);
                if (pre) { n = put_utf8(out, cap, n, pre); i++; continue; }
                if (b >= 0x20 && b < 0x7F) { n = put_utf8(out, cap, n, b); n = put_utf8(out, cap, n, mark); i++; continue; }
                if (b == ' ') { i++; continue; }
            }
            continue;
        }
        uint16_t cp = iso6937_hi[c - 0xA0];
        if (cp)
            n = put_utf8(out, cap, n, cp);
    }
    out[n] = '\0';
    return n;
}

/* ======================================================================= */
/*  Reensamblado de secciones                                              */
/* ======================================================================= */
#define SEC_MAX 4096
#define MAX_SLOTS 48

typedef struct {
    int pid;
    uint8_t buf[SEC_MAX + 188];
    int len;
    bool active;
} sec_asm_t;

typedef struct { int sid; int tsid; int lcn; } lcn_t;

struct psi {
    sec_asm_t slots[MAX_SLOTS];
    int nslots;
    psi_service_t svc[PSI_MAX_SVC];
    int nsvc;
    int ts_id;
    int pat_ver, net_id;
    char net_name[96];
    lcn_t lcn[128];
    int nlcn;
    int64_t nit_freq[16];
    int nfreq;
    int pmt_ver[PSI_MAX_SVC];
    unsigned gen;
    bool pat_ok;
};

psi_t *psi_create(void)
{
    psi_t *p = calloc(1, sizeof *p);
    if (p)
        psi_reset(p);
    return p;
}
void psi_destroy(psi_t *p) { free(p); }

void psi_reset(psi_t *p)
{
    unsigned g = p->gen;
    memset(p, 0, sizeof *p);
    p->gen = g + 1;
    p->pat_ver = -1;
    p->ts_id = -1;
    for (int i = 0; i < PSI_MAX_SVC; i++)
        p->pmt_ver[i] = -1;
}

unsigned psi_generation(const psi_t *p) { return p->gen; }
int psi_ts_id(const psi_t *p) { return p->ts_id; }
const char *psi_network_name(const psi_t *p) { return p->net_name; }
int psi_nservices(const psi_t *p) { return p->nsvc; }
const psi_service_t *psi_service(const psi_t *p, int i) { return i >= 0 && i < p->nsvc ? &p->svc[i] : NULL; }

psi_service_t *psi_find_mut(psi_t *p, int sid)
{
    for (int i = 0; i < p->nsvc; i++)
        if (p->svc[i].sid == sid)
            return &p->svc[i];
    return NULL;
}
const psi_service_t *psi_find(const psi_t *p, int sid) { return psi_find_mut((psi_t *)p, sid); }

int psi_nit_freqs(const psi_t *p, int64_t *out, int cap)
{
    int n = p->nfreq < cap ? p->nfreq : cap;
    for (int i = 0; i < n; i++)
        out[i] = p->nit_freq[i];
    return n;
}

static psi_service_t *svc_get(psi_t *p, int sid)
{
    psi_service_t *s = psi_find_mut(p, sid);
    if (s || p->nsvc >= PSI_MAX_SVC)
        return s;
    s = &p->svc[p->nsvc++];
    memset(s, 0, sizeof *s);
    s->sid = sid;
    p->gen++;
    return s;
}

/* ---- PAT ---- */
static void parse_pat(psi_t *p, const uint8_t *s, int len)
{
    int ver = (s[5] >> 1) & 31;
    int tsid = (s[3] << 8) | s[4];
    if (p->pat_ok && ver == p->pat_ver && tsid == p->ts_id)
        return;
    p->ts_id = tsid;
    p->pat_ver = ver;
    p->pat_ok = true;
    for (int i = 8; i + 4 <= len - 4; i += 4) {
        int sid = (s[i] << 8) | s[i + 1];
        int pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
        if (sid == 0)
            continue; /* PID de la NIT */
        psi_service_t *v = svc_get(p, sid);
        if (v && v->pmt_pid != pid) {
            v->pmt_pid = pid;
            p->gen++;
        }
    }
    p->gen++;
}

/* ---- PMT ---- */
static void es_from_type(psi_es_t *e, const uint8_t *desc, int dl, bool *ca)
{
    bool ac3 = false, eac3 = false, sub = false, txt = false, aac = false;
    for (int i = 0; i + 2 <= dl;) {
        int tag = desc[i], l = desc[i + 1];
        if (i + 2 + l > dl)
            break;
        const uint8_t *d = desc + i + 2;
        switch (tag) {
        case 0x09: *ca = true; break;
        case 0x0A:
            if (l >= 3) { memcpy(e->lang, d, 3); e->lang[3] = 0; }
            break;
        case 0x6A: ac3 = true; break;
        case 0x7A: eac3 = true; break;
        case 0x7C: aac = true; break;
        case 0x59: sub = true; break;
        case 0x56: case 0x46: txt = true; break;
        }
        i += 2 + l;
    }
    switch (e->stream_type) {
    case 0x01: case 0x02: e->kind = ES_VIDEO; e->codec = CODEC_MPEG2V; break;
    case 0x1B: e->kind = ES_VIDEO; e->codec = CODEC_H264; break;
    case 0x24: e->kind = ES_VIDEO; e->codec = CODEC_HEVC; break;
    case 0x03: case 0x04: e->kind = ES_AUDIO; e->codec = CODEC_MP2; break;
    case 0x0F: case 0x11: e->kind = ES_AUDIO; e->codec = CODEC_AAC; break;
    case 0x81: e->kind = ES_AUDIO; e->codec = CODEC_AC3; break;
    case 0x87: e->kind = ES_AUDIO; e->codec = CODEC_EAC3; break;
    case 0x06:
        if (eac3) { e->kind = ES_AUDIO; e->codec = CODEC_EAC3; }
        else if (ac3) { e->kind = ES_AUDIO; e->codec = CODEC_AC3; }
        else if (aac) { e->kind = ES_AUDIO; e->codec = CODEC_AAC; }
        else if (sub) e->codec = CODEC_SUB;
        else if (txt) e->codec = CODEC_TXT;
        else e->codec = CODEC_OTHER;
        break;
    default: e->codec = CODEC_OTHER; break;
    }
}

static void parse_pmt(psi_t *p, const uint8_t *s, int len)
{
    int sid = (s[3] << 8) | s[4];
    int ver = (s[5] >> 1) & 31;
    psi_service_t *v = psi_find_mut(p, sid);
    if (!v)
        return; /* PMT de un servicio que la PAT no anunció */
    int idx = (int)(v - p->svc);
    if (v->have_pmt && p->pmt_ver[idx] == ver)
        return;
    p->pmt_ver[idx] = ver;
    int pcr = ((s[8] & 0x1F) << 8) | s[9];
    int pil = ((s[10] & 0x0F) << 8) | s[11];
    bool ca = false;
    for (int i = 12; i + 2 <= 12 + pil && i + 2 <= len - 4;) {
        if (s[i] == 0x09)
            ca = true;
        i += 2 + s[i + 1];
    }
    int pos = 12 + pil, n = 0;
    psi_es_t es[PSI_MAX_ES];
    memset(es, 0, sizeof es);
    while (pos + 5 <= len - 4 && n < PSI_MAX_ES) {
        psi_es_t *e = &es[n];
        e->stream_type = s[pos];
        e->pid = ((s[pos + 1] & 0x1F) << 8) | s[pos + 2];
        int el = ((s[pos + 3] & 0x0F) << 8) | s[pos + 4];
        if (pos + 5 + el > len - 4)
            break;
        es_from_type(e, s + pos + 5, el, &ca);
        pos += 5 + el;
        n++;
    }
    /* conservar w/h ya medidos si el flujo es el mismo */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < v->nes; j++)
            if (v->es[j].pid == es[i].pid && v->es[j].codec == es[i].codec) {
                es[i].w = v->es[j].w;
                es[i].h = v->es[j].h;
            }
    memcpy(v->es, es, sizeof es);
    v->nes = n;
    v->pcr_pid = pcr;
    v->ca = ca;
    v->have_pmt = true;
    p->gen++;
}

/* ---- SDT ---- */
static void parse_sdt(psi_t *p, const uint8_t *s, int len)
{
    int pos = 11;
    while (pos + 5 <= len - 4) {
        int sid = (s[pos] << 8) | s[pos + 1];
        int dl = ((s[pos + 3] & 0x0F) << 8) | s[pos + 4];
        bool ca = (s[pos + 3] & 0x10) != 0;
        pos += 5;
        if (pos + dl > len - 4)
            break;
        psi_service_t *v = svc_get(p, sid);
        for (int i = pos; v && i + 2 <= pos + dl;) {
            int tag = s[i], l = s[i + 1];
            if (i + 2 + l > pos + dl)
                break;
            if (tag == 0x48 && l >= 3) {
                const uint8_t *d = s + i + 2;
                int pl = d[1];
                if (2 + pl < l) {
                    int nl = d[2 + pl];
                    if (3 + pl + nl <= l) {
                        char prov[96], name[96];
                        dvb_text_to_utf8(d + 2, pl, prov, sizeof prov);
                        dvb_text_to_utf8(d + 3 + pl, nl, name, sizeof name);
                        if (strcmp(prov, v->provider) || strcmp(name, v->name) || v->svc_type != d[0] || !v->have_sdt) {
                            snprintf(v->provider, sizeof v->provider, "%s", prov);
                            snprintf(v->name, sizeof v->name, "%s", name);
                            v->svc_type = d[0];
                            p->gen++;
                        }
                        v->have_sdt = true;
                    }
                }
            }
            i += 2 + l;
        }
        if (v && ca && !v->ca) {
            v->ca = true;
            p->gen++;
        }
        pos += dl;
    }
}

/* ---- NIT ---- */
static void add_lcn(psi_t *p, int sid, int tsid, int lcn)
{
    for (int i = 0; i < p->nlcn; i++)
        if (p->lcn[i].sid == sid && p->lcn[i].tsid == tsid) {
            if (p->lcn[i].lcn != lcn) { p->lcn[i].lcn = lcn; p->gen++; }
            return;
        }
    if (p->nlcn < 128) {
        p->lcn[p->nlcn++] = (lcn_t){sid, tsid, lcn};
        p->gen++;
    }
}

static void parse_nit(psi_t *p, const uint8_t *s, int len)
{
    int nid = (s[3] << 8) | s[4];
    int nl = ((s[8] & 0x0F) << 8) | s[9];
    if (10 + nl + 2 > len - 4)
        return;
    p->net_id = nid;
    for (int i = 10; i + 2 <= 10 + nl;) {
        int tag = s[i], l = s[i + 1];
        if (tag == 0x40 && i + 2 + l <= 10 + nl) {
            char nm[96];
            dvb_text_to_utf8(s + i + 2, l, nm, sizeof nm);
            if (strcmp(nm, p->net_name)) { snprintf(p->net_name, sizeof p->net_name, "%s", nm); p->gen++; }
        }
        i += 2 + l;
    }
    int pos = 10 + nl;
    int tl = ((s[pos] & 0x0F) << 8) | s[pos + 1];
    pos += 2;
    int end = pos + tl;
    if (end > len - 4)
        end = len - 4;
    while (pos + 6 <= end) {
        int tsid = (s[pos] << 8) | s[pos + 1];
        int dl = ((s[pos + 4] & 0x0F) << 8) | s[pos + 5];
        pos += 6;
        if (pos + dl > end)
            break;
        for (int i = pos; i + 2 <= pos + dl;) {
            int tag = s[i], l = s[i + 1];
            if (i + 2 + l > pos + dl)
                break;
            const uint8_t *d = s + i + 2;
            if (tag == 0x5A && l >= 4) {
                /* terrestrial_delivery_system: frecuencia en unidades de 10 Hz */
                int64_t f = (int64_t)(((uint32_t)d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]) * 10;
                bool have = false;
                for (int k = 0; k < p->nfreq; k++)
                    have |= p->nit_freq[k] == f;
                if (!have && p->nfreq < 16)
                    p->nit_freq[p->nfreq++] = f;
            } else if (tag == 0x83) {
                for (int k = 0; k + 4 <= l; k += 4)
                    add_lcn(p, (d[k] << 8) | d[k + 1], tsid, ((d[k + 2] & 0x03) << 8) | d[k + 3]);
            }
            i += 2 + l;
        }
        pos += dl;
    }
    for (int i = 0; i < p->nsvc; i++)
        for (int k = 0; k < p->nlcn; k++)
            if (p->lcn[k].sid == p->svc[i].sid && (p->ts_id < 0 || p->lcn[k].tsid == p->ts_id))
                p->svc[i].lcn = p->lcn[k].lcn;
}

static void on_section(psi_t *p, int pid, const uint8_t *s, int len)
{
    if (len < 12 || !(s[1] & 0x80) || !(s[5] & 1))
        return; /* sin sintaxis larga o con current_next = 0 */
    if (crc32_mpeg2(s, len) != 0)
        return;
    switch (s[0]) {
    case 0x00: if (pid == TS_PID_PAT) parse_pat(p, s, len); break;
    case 0x02: parse_pmt(p, s, len); break;
    case 0x42: if (pid == TS_PID_SDT) parse_sdt(p, s, len); break;
    case 0x40: if (pid == TS_PID_NIT) parse_nit(p, s, len); break;
    }
}

bool psi_wants_pid(const psi_t *p, int pid)
{
    if (pid == TS_PID_PAT || pid == TS_PID_NIT || pid == TS_PID_SDT)
        return true;
    for (int i = 0; i < p->nsvc; i++)
        if (p->svc[i].pmt_pid == pid)
            return true;
    return false;
}

static sec_asm_t *slot_get(psi_t *p, int pid)
{
    for (int i = 0; i < p->nslots; i++)
        if (p->slots[i].pid == pid)
            return &p->slots[i];
    if (p->nslots >= MAX_SLOTS)
        return NULL;
    sec_asm_t *a = &p->slots[p->nslots++];
    memset(a, 0, sizeof *a);
    a->pid = pid;
    return a;
}

/* Saca todas las secciones completas del buffer; deja el resto. */
static void drain(psi_t *p, sec_asm_t *a)
{
    for (;;) {
        if (a->len < 3)
            return;
        if (a->buf[0] == 0xFF) { /* relleno */
            a->len = 0;
            a->active = false;
            return;
        }
        int sl = 3 + (((a->buf[1] & 0x0F) << 8) | a->buf[2]);
        if (sl > SEC_MAX) {
            a->len = 0;
            a->active = false;
            return;
        }
        if (a->len < sl)
            return;
        on_section(p, a->pid, a->buf, sl);
        memmove(a->buf, a->buf + sl, a->len - sl);
        a->len -= sl;
        if (a->len == 0)
            a->active = false;
    }
}

void psi_feed(psi_t *p, const ts_pkt_t *k)
{
    if (!k->payload || k->dup || k->scrambled)
        return;
    sec_asm_t *a = slot_get(p, k->pid);
    if (!a)
        return;
    if (k->tei) { /* no corregible por el RS: se pierde la sección en curso */
        a->len = 0;
        a->active = false;
        return;
    }
    if (k->cc_err) {
        a->len = 0;
        a->active = false;
    }
    const uint8_t *d = k->payload;
    int n = k->plen;
    if (k->pusi) {
        int ptr = d[0];
        if (1 + ptr > n)
            return;
        if (a->active && ptr > 0 && a->len + ptr <= (int)sizeof a->buf) { /* cola de la sección anterior */
            memcpy(a->buf + a->len, d + 1, ptr);
            a->len += ptr;
            drain(p, a);
        }
        a->len = 0;
        a->active = true;
        d += 1 + ptr;
        n -= 1 + ptr;
    } else if (!a->active) {
        return;
    }
    if (n <= 0 || a->len + n > (int)sizeof a->buf) {
        a->len = 0;
        a->active = false;
        return;
    }
    memcpy(a->buf + a->len, d, n);
    a->len += n;
    drain(p, a);
}
