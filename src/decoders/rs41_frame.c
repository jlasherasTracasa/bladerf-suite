/*
 * rs41_frame.c — Trama RS41: CRC, máscara, bloques, Reed-Solomon, ECEF.
 * Ver rs41_frame.h y docs/decoders/rs41.md.
 *
 * Reed-Solomon. Dos palabras RS(255,231) acortadas, entrelazadas octeto a
 * octeto: la palabra k (0 o 1) lleva los datos de las posiciones 0x38+k+2i
 * (i < 132) y su paridad (24 octetos) en 0x08+k+2i (i < 24). Se codifica con
 * el cuerpo de Galois de polinomio 0x11D, primera raíz α^0 y 24 raíces
 * (rs_init_rs41). RIESGO: ni el orden de la paridad ni (gfpoly, fcr) están
 * confirmados con una trama real; la corrección solo se intenta cuando los CRC
 * fallan y su resultado se vuelve a validar con los CRC, de modo que un error
 * aquí no produce datos falsos, solo pierde la corrección.
 *
 * ECEF → geodésicas (WGS-84) por iteración de Bowring:
 *   N = a/√(1 − e² sin²φ);  h = p/cosφ − N;  φ = atan2(z, p(1 − e²N/(N+h)))
 * con p = √(x²+y²), e² = f(2−f), f = 1/298,257223563, a = 6 378 137 m.
 */
#include "rs41_frame.h"

#include "../dsp/crc.h"
#include "../dsp/fec.h"

#include <string.h>

/* Máscara de 64 octetos y cabecera: bazjo/RS41_Decoding (documentación pública). */
const uint8_t RS41_MASK[64] = {
    0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
    0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1, 0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
    0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C, 0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
    0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23, 0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};
const uint8_t RS41_HEADER_TX[8] = {0x10, 0xB6, 0xCA, 0x11, 0x22, 0x96, 0x12, 0xF8};

#define WGS_A 6378137.0
#define WGS_F (1.0 / 298.257223563)

uint16_t rs41_crc16(const uint8_t *p, int n)
{
    static crc_t c;
    static bool ok = false;
    if (!ok) {
        const crc_params_t *pp = crc_preset("CRC-16/CCITT-FALSE");
        if (pp)
            crc_init(&c, pp);
        ok = pp != NULL;
    }
    return (uint16_t)crc_compute(&c, p, (size_t)n);
}

void rs41_scramble(uint8_t *frame, int n)
{
    for (int i = 0; i < n; i++)
        frame[i] ^= RS41_MASK[i & 63];
}

void rs41_ecef_to_lla(double x, double y, double z, double *lat_deg, double *lon_deg, double *alt_m)
{
    const double e2 = WGS_F * (2.0 - WGS_F);
    double p = sqrt(x * x + y * y), lon = atan2(y, x);
    double lat = atan2(z, p * (1.0 - e2)), h = 0;
    for (int i = 0; i < 6; i++) {
        double s = sin(lat), n = WGS_A / sqrt(1.0 - e2 * s * s);
        h = p / cos(lat) - n;
        lat = atan2(z, p * (1.0 - e2 * n / (n + h)));
    }
    *lat_deg = lat * 180.0 / M_PI;
    *lon_deg = lon * 180.0 / M_PI;
    *alt_m = h;
}

void rs41_lla_to_ecef(double lat_deg, double lon_deg, double alt_m, double *x, double *y, double *z)
{
    const double e2 = WGS_F * (2.0 - WGS_F);
    double la = lat_deg * M_PI / 180.0, lo = lon_deg * M_PI / 180.0;
    double s = sin(la), n = WGS_A / sqrt(1.0 - e2 * s * s);
    *x = (n + alt_m) * cos(la) * cos(lo);
    *y = (n + alt_m) * cos(la) * sin(lo);
    *z = (n * (1.0 - e2) + alt_m) * s;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int le16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

/* Bloque 0x7B: ECEF en cm (3×int32 LE), velocidad ECEF en cm/s (3×int16 LE),
 * nº de satélites (u8). Se pasa a ENU para dar rumbo, velocidad y ascenso. */
static void pos_block(const uint8_t *d, rs41_info_t *in)
{
    double x = (int32_t)le32(d) / 100.0, y = (int32_t)le32(d + 4) / 100.0, z = (int32_t)le32(d + 8) / 100.0;
    double vx = le16s(d + 12) / 100.0, vy = le16s(d + 14) / 100.0, vz = le16s(d + 16) / 100.0;
    rs41_ecef_to_lla(x, y, z, &in->lat, &in->lon, &in->alt_m);
    double la = in->lat * M_PI / 180.0, lo = in->lon * M_PI / 180.0;
    double ve = -sin(lo) * vx + cos(lo) * vy;
    double vn = -sin(la) * cos(lo) * vx - sin(la) * sin(lo) * vy + cos(la) * vz;
    double vu = cos(la) * cos(lo) * vx + cos(la) * sin(lo) * vy + sin(la) * vz;
    in->speed_ms = sqrt(ve * ve + vn * vn);
    in->heading_deg = fmod(atan2(ve, vn) * 180.0 / M_PI + 360.0, 360.0);
    in->climb_ms = vu;
    in->sats = d[18];
}

int rs41_parse(const uint8_t *p, int len, rs41_info_t *in)
{
    memset(in, 0, sizeof *in);
    int pos = 0x39, nok = 0;
    while (pos + 4 <= len) {
        int id = p[pos], l = p[pos + 1];
        if (pos + 2 + l + 2 > len)
            break;
        const uint8_t *d = p + pos + 2;
        uint16_t crc = (uint16_t)(d[l] | d[l + 1] << 8);
        if (rs41_crc16(d, l) == crc) {
            bool used = true;
            if (id == 0x79 && l >= 11) {
                in->frame_no = d[0] | d[1] << 8;
                memcpy(in->serial, d + 2, 8);
                in->serial[8] = 0;
                for (int i = 0; i < 8; i++)
                    if (in->serial[i] < 0x20 || in->serial[i] > 0x7E)
                        in->serial[i] = '?';
                in->batt_v = d[10] / 10.0;
                in->status_ok = true;
            } else if (id == 0x7A && l >= 20) {
                for (int i = 0; i < 6; i++)
                    in->ptu_raw[i] = d[2 + 3 * i] | (uint32_t)d[3 + 3 * i] << 8 | (uint32_t)d[4 + 3 * i] << 16;
                in->ptu_ok = true;
            } else if (id == 0x7C && l >= 6) {
                in->gps_week = d[0] | d[1] << 8;
                in->gps_tow_ms = le32(d + 2);
                in->time_ok = true;
            } else if (id == 0x7B && l >= 19) {
                pos_block(d, in);
                in->pos_ok = true;
            } else {
                used = id != 0x76;   /* el relleno no cuenta como bloque informativo */
            }
            if (used)
                nok++;
        }
        pos += 2 + l + 2;
    }
    in->nblocks_ok = nok;
    return nok;
}

#define RS_NDATA 132
#define RS_NPAR 24

int rs41_rs_correct(uint8_t *plain, int len)
{
    if (len != RS41_FRAME_LEN)
        return -1;
    static rs_t rs;
    static bool ok = false;
    if (!ok)
        ok = rs_init_rs41(&rs) == 0;
    if (!ok)
        return -1;
    int total = 0;
    for (int k = 0; k < 2; k++) {
        uint8_t cw[RS_NDATA + RS_NPAR];
        /* Orden CONTRASTADO con grabaciones reales (docs/decoders/rs41.md): los datos
         * de la palabra k son los octetos 0x38+k+2i (entrelazados) y su paridad son 24
         * octetos CONSECUTIVOS 0x08+24k+j (sin entrelazar); el polinomio se lee con el
         * coeficiente de mayor grado primero: d[131]…d[0], p[23]…p[0]. */
        for (int i = 0; i < RS_NDATA; i++)
            cw[i] = plain[0x38 + k + 2 * (RS_NDATA - 1 - i)];
        for (int j = 0; j < RS_NPAR; j++)
            cw[RS_NDATA + j] = plain[0x08 + 24 * k + (RS_NPAR - 1 - j)];
        int r = rs_decode(&rs, cw, RS_NDATA + RS_NPAR, NULL, 0);
        if (r < 0)
            return -1;
        if (r > 0) {
            for (int i = 0; i < RS_NDATA; i++)
                plain[0x38 + k + 2 * (RS_NDATA - 1 - i)] = cw[i];
            for (int j = 0; j < RS_NPAR; j++)
                plain[0x08 + 24 * k + (RS_NPAR - 1 - j)] = cw[RS_NDATA + j];
            total += r;
        }
    }
    return total;
}

/* ---- Construcción ------------------------------------------------------ */
static int put_block(uint8_t *f, int pos, int id, const uint8_t *data, int l)
{
    f[pos] = (uint8_t)id;
    f[pos + 1] = (uint8_t)l;
    memcpy(f + pos + 2, data, (size_t)l);
    uint16_t c = rs41_crc16(data, l);
    f[pos + 2 + l] = (uint8_t)c;
    f[pos + 3 + l] = (uint8_t)(c >> 8);
    return pos + 4 + l;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put16(uint8_t *p, int v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void rs41_build(const rs41_params_t *p, uint8_t f[RS41_FRAME_LEN])
{
    memset(f, 0, RS41_FRAME_LEN);
    for (int i = 0; i < 8; i++)
        f[i] = RS41_HEADER_TX[i] ^ RS41_MASK[i];
    f[0x38] = 0x0F;
    int pos = 0x39;
    uint8_t d[64];
    /* 0x79 estado */
    memset(d, 0, sizeof d);
    put16(d, p->frame_no);
    memcpy(d + 2, p->serial, strnlen(p->serial, 8));
    d[10] = (uint8_t)lrint(p->batt_v * 10.0);
    pos = put_block(f, pos, 0x79, d, 0x28);
    /* 0x7A PTU (crudo) */
    memset(d, 0, sizeof d);
    put16(d, p->frame_no);
    for (int i = 0; i < 6; i++) {
        d[2 + 3 * i] = (uint8_t)p->ptu_raw[i];
        d[3 + 3 * i] = (uint8_t)(p->ptu_raw[i] >> 8);
        d[4 + 3 * i] = (uint8_t)(p->ptu_raw[i] >> 16);
    }
    pos = put_block(f, pos, 0x7A, d, 0x2A);
    /* 0x7C tiempo GPS */
    memset(d, 0, sizeof d);
    put16(d, p->gps_week);
    put32(d + 2, p->gps_tow_ms);
    pos = put_block(f, pos, 0x7C, d, 0x15);
    /* 0x7B posición/velocidad ECEF */
    memset(d, 0, sizeof d);
    double x, y, z;
    rs41_lla_to_ecef(p->lat, p->lon, p->alt_m, &x, &y, &z);
    put32(d, (uint32_t)(int32_t)lrint(x * 100.0));
    put32(d + 4, (uint32_t)(int32_t)lrint(y * 100.0));
    put32(d + 8, (uint32_t)(int32_t)lrint(z * 100.0));
    double la = p->lat * M_PI / 180.0, lo = p->lon * M_PI / 180.0;
    /* ENU → ECEF: x = −sinλ·e − sinφcosλ·n + cosφcosλ·u, etc. */
    double vx = -sin(lo) * p->ve_ms - sin(la) * cos(lo) * p->vn_ms + cos(la) * cos(lo) * p->vu_ms;
    double vy = cos(lo) * p->ve_ms - sin(la) * sin(lo) * p->vn_ms + cos(la) * sin(lo) * p->vu_ms;
    double vz = cos(la) * p->vn_ms + sin(la) * p->vu_ms;
    put16(d + 12, (int)lrint(vx * 100.0));
    put16(d + 14, (int)lrint(vy * 100.0));
    put16(d + 16, (int)lrint(vz * 100.0));
    d[18] = (uint8_t)p->sats;
    pos = put_block(f, pos, 0x7B, d, 0x15);
    /* 0x76 relleno hasta el final */
    int rest = RS41_FRAME_LEN - pos - 4;
    memset(d, 0, sizeof d);
    uint8_t pad[160] = {0};
    pos = put_block(f, pos, 0x76, pad, rest);
    (void)pos;
    /* Paridad Reed-Solomon de las dos palabras entrelazadas */
    rs_t rs;
    if (rs_init_rs41(&rs) == 0)
        for (int k = 0; k < 2; k++) {
            uint8_t cw[RS_NDATA], par[RS_NPAR];
            for (int i = 0; i < RS_NDATA; i++)
                cw[i] = f[0x38 + k + 2 * (RS_NDATA - 1 - i)];
            rs_encode(&rs, cw, RS_NDATA, par);
            for (int j = 0; j < RS_NPAR; j++)
                f[0x08 + 24 * k + (RS_NPAR - 1 - j)] = par[j];
        }
    /* Errores de prueba: se corrompen octetos de datos repartidos en las dos palabras */
    for (int e = 0; e < p->inject_errors; e++) {
        int idx = 0x39 + 7 * e;
        if (idx < RS41_FRAME_LEN)
            f[idx] ^= (uint8_t)(0x5A + e);
    }
}
