/*
 * vdl2_frame.c — Cabecera, aleatorizador, FEC Reed-Solomon entrelazado y HDLC de
 * VDL2. Ver vdl2_frame.h y docs/protocolos/vdl2.md.
 */
#include "vdl2_frame.h"
#include "../dsp/crc.h"
#include "../dsp/fec.h"

#include <stdlib.h>
#include <string.h>

/* 16 tripletes de la palabra única */
const uint8_t VDL2_UW_BITS[48] = {
    0, 0, 0, /**/ 0, 1, 0, /**/ 0, 1, 1, /**/ 1, 1, 0, /**/ 0, 0, 0, /**/ 0, 0, 1, /**/ 1, 0, 1, /**/ 1, 1, 0,
    0, 0, 1, /**/ 1, 0, 0, /**/ 0, 1, 1, /**/ 1, 1, 1, /**/ 1, 0, 1, /**/ 1, 1, 1, /**/ 1, 0, 0, /**/ 0, 1, 0};

/* Gray → binario (Tabla 5 del modem de Kim et al.): 000→0 001→1 011→2 010→3 110→4 111→5 101→6 100→7 */
const uint8_t VDL2_GRAY_TO_INC[8] = {0, 1, 3, 2, 7, 6, 4, 5};
const uint8_t VDL2_INC_TO_GRAY[8] = {0, 1, 3, 2, 6, 7, 5, 4};

/* ---- Cabecera: código lineal (25,20) ---------------------------------------- */
/* La cabecera son 25 bits recibidos en este orden: 3 reservados (a 0) + longitud de 17 bits (LSB
 * primero) + 5 de comprobación. Se trata como una palabra con el primer bit recibido en la posición
 * 24. Matriz de comprobación H = [P | I5]: las 5 últimas columnas son la identidad, así que la
 * comprobación de la fila i es la paridad de los bits de información que cubre su máscara.
 * Matriz contrastada con el comportamiento de dumpvdl2 sobre una grabación real (sigidwiki "VDL-M2
 * IQ", ver docs/protocolos/vdl2.md); la norma pública (EN 301 841-1 V1.1.1) no la reproduce. */
#define HDR_ROWS 5
static const uint32_t HDR_H[HDR_ROWS] = {0x001FFF0u, 0x07E1FE8u, 0x18E61E4u, 0x1B6A662u, 0x0D3CAA1u};

static uint32_t hdr_par(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

static uint32_t hdr_syndrome(uint32_t w)
{
    uint32_t s = 0;
    for (int i = 0; i < HDR_ROWS; i++)
        s |= hdr_par(w & HDR_H[i]) << (HDR_ROWS - 1 - i);
    return s;
}

void vdl2_hdr_encode(uint32_t len, uint8_t bits[VDL2_HDR_BITS])
{
    uint32_t w = 0;
    bits[0] = bits[1] = bits[2] = 0; /* reservados */
    for (int i = 0; i < 17; i++)
        bits[3 + i] = (uint8_t)((len >> i) & 1); /* LSB primero */
    for (int i = 0; i < 20; i++)
        w |= (uint32_t)bits[i] << (24 - i);
    uint32_t syn = hdr_syndrome(w); /* con las 5 posiciones de comprobación a 0, el síndrome ES la paridad */
    for (int i = 0; i < 5; i++)
        bits[20 + i] = (uint8_t)((syn >> (4 - i)) & 1);
}

int vdl2_hdr_decode(const uint8_t bits[VDL2_HDR_BITS], uint32_t *len)
{
    uint32_t w = 0;
    for (int i = 0; i < VDL2_HDR_BITS; i++)
        w |= (uint32_t)(bits[i] & 1) << (24 - i);
    w &= 0x3FFFFFu; /* los 3 bits reservados valen 0: se fuerzan (mejora la corrección) */
    uint32_t syn = hdr_syndrome(w);
    int rc = 0;
    if (syn) {
        rc = -1;
        /* corrección de 1 o 2 bits: el síndrome es la suma de las columnas de H en los bits erróneos */
        uint32_t col[22];
        for (int i = 0; i < 22; i++)
            col[i] = hdr_syndrome(1u << i);
        for (int i = 0; i < 22 && rc < 0; i++)
            if (col[i] == syn) {
                w ^= 1u << i;
                rc = 1;
            }
        for (int i = 0; i < 22 && rc < 0; i++)
            for (int j = i + 1; j < 22; j++)
                if ((col[i] ^ col[j]) == syn) {
                    w ^= (1u << i) | (1u << j);
                    rc = 1;
                    break;
                }
    }
    uint32_t v = 0;
    for (int i = 0; i < 17; i++) /* el primer bit recibido (posición 21) es el LSB */
        v |= ((w >> (21 - i)) & 1u) << i;
    *len = v;
    return rc;
}

/* ---- Aleatorizador ------------------------------------------------------- */
/* Aditivo, registro de 15 bits con estado inicial 0x6959 y realimentación
 * b = s[0] ^ s[14] (x^15 + x + 1 en forma de Fibonacci); la salida es el bit realimentado. Actúa sobre
 * TODO lo que sigue a la palabra única: cabecera (25 bits) y datos, en un flujo continuo. Es el
 * esquema que aplica dumpvdl2 y con el que se decodifica la grabación real (sin él, ninguna trama
 * pasa el FCS). La misma función aleatoriza y desaleatoriza. */
void vdl2_scramble(uint8_t *bits, int nbits)
{
    uint32_t r = 0x6959;
    for (int i = 0; i < nbits; i++) {
        uint32_t b = (r ^ (r >> 14)) & 1;
        r = (r >> 1) | (b << 14);
        bits[i] ^= (uint8_t)b;
    }
}

/* ---- FEC ------------------------------------------------------------------ */
static int ecc_for(int r) { return r < 3 ? 0 : r < 31 ? 2 : r < 68 ? 4 : 6; }

/* Reparto: nb bloques, el último con ld bytes de datos y le bytes de paridad. */
static void layout(int D, int *nb, int *ld, int *le)
{
    int full = D / VDL2_BLOCK_DATA, r = D % VDL2_BLOCK_DATA;
    if (r == 0) {
        *nb = full;
        *ld = VDL2_BLOCK_DATA;
        *le = 6;
    } else {
        *nb = full + 1;
        *ld = r;
        *le = ecc_for(r);
    }
}

int vdl2_encoded_bytes(int D)
{
    int nb, ld, le;
    layout(D, &nb, &ld, &le);
    if (nb == 0)
        return 0;
    return (nb - 1) * (VDL2_BLOCK_DATA + 6) + ld + le;
}

/* Orden de transmisión (como dumpvdl2): primero TODOS los bytes de datos, entrelazados por columnas
 * (byte c de cada bloque, c = 0…248, saltando los bloques que no lo tienen), y después TODA la paridad
 * entrelazada igual (6 columnas). Devuelve, para cada posición del flujo, (bloque, índice en el bloque
 * de 255). 'idx' y 'blk' deben tener vdl2_encoded_bytes() elementos. */
static void order_map(int nb, int ld, int le, int *blk, int *idx)
{
    int o = 0;
    for (int c = 0; c < VDL2_BLOCK_DATA; c++)
        for (int j = 0; j < nb; j++)
            if (c < (j == nb - 1 ? ld : VDL2_BLOCK_DATA)) {
                blk[o] = j;
                idx[o++] = c;
            }
    for (int c = 0; c < 6; c++)
        for (int j = 0; j < nb; j++)
            if (c < (j == nb - 1 ? le : 6)) {
                blk[o] = j;
                idx[o++] = VDL2_BLOCK_DATA + c;
            }
}

void vdl2_fec_encode(const uint8_t *data, int D, uint8_t *out)
{
    int nb, ld, le;
    layout(D, &nb, &ld, &le);
    int E = vdl2_encoded_bytes(D);
    rs_t rs;
    rs_init_vdl2(&rs);
    uint8_t (*cw)[255] = calloc((size_t)nb, 255);
    int *map = malloc(sizeof(int) * 2 * (size_t)E);
    for (int j = 0; j < nb; j++) {
        int dj = j == nb - 1 ? ld : VDL2_BLOCK_DATA;
        memcpy(cw[j], data + (size_t)j * VDL2_BLOCK_DATA, (size_t)dj);
        rs_encode(&rs, cw[j], VDL2_BLOCK_DATA, cw[j] + VDL2_BLOCK_DATA);
    }
    order_map(nb, ld, le, map, map + E);
    for (int o = 0; o < E; o++)
        out[o] = cw[map[o]][map[E + o]];
    free(map);
    free(cw);
}

int vdl2_fec_decode_era(const uint8_t *enc, int D, uint8_t *data, const uint8_t *eflag)
{
    int nb, ld, le;
    layout(D, &nb, &ld, &le);
    int E = vdl2_encoded_bytes(D);
    rs_t rs;
    rs_init_vdl2(&rs);
    uint8_t (*cw)[255] = calloc((size_t)nb, 255);
    int *map = malloc(sizeof(int) * 2 * (size_t)E);
    order_map(nb, ld, le, map, map + E);
    uint8_t (*ef)[255] = calloc((size_t)nb, 255); /* borrados extra por bloque (octetos poco fiables) */
    for (int o = 0; o < E; o++) {
        cw[map[o]][map[E + o]] = enc[o];
        if (eflag && eflag[o])
            ef[map[o]][map[E + o]] = 1;
    }
    free(map);
    int total = 0, rc = 0;
    for (int j = 0; j < nb; j++) {
        int dj = j == nb - 1 ? ld : VDL2_BLOCK_DATA;
        int ej = j == nb - 1 ? le : 6;
        if (ej > 0) {
            int era[255], ne = 0;
            for (int k = ej; k < 6; k++) /* paridad no transmitida = borrado */
                era[ne++] = VDL2_BLOCK_DATA + k;
            for (int q = 0; q < dj && ne < 6; q++)
                if (ef[j][q]) {
                    era[ne++] = q;
                }
            for (int q = VDL2_BLOCK_DATA; q < VDL2_BLOCK_DATA + ej && ne < 6; q++)
                if (ef[j][q]) {
                    era[ne++] = q;
                }
            int c = rs_decode(&rs, cw[j], 255, era, ne);
            if (c < 0)
                rc = -1;
            else
                total += MAX(0, c - ne); /* los borrados (ausentes o dudosos) no cuentan como errores */
        }
        memcpy(data + (size_t)j * VDL2_BLOCK_DATA, cw[j], (size_t)dj);
    }
    free(ef);
    free(cw);
    return rc < 0 ? -1 : total;
}

int vdl2_fec_decode(const uint8_t *enc, int D, uint8_t *data) { return vdl2_fec_decode_era(enc, D, data, NULL); }

/* ---- HDLC ------------------------------------------------------------------ */
int vdl2_add_fcs(uint8_t *frame, int n)
{
    crc_t c;
    crc_init(&c, crc_preset("CRC-16/X-25"));
    uint32_t f = crc_compute(&c, frame, (size_t)n);
    frame[n] = (uint8_t)(f & 0xFF);
    frame[n + 1] = (uint8_t)(f >> 8);
    return n + 2;
}

int vdl2_hdlc_encode(const uint8_t *frame, int n, uint8_t *bits, int cap)
{
    int o = 0, ones = 0;
    static const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    if (cap < 16 + 10 * n)
        return -1;
    memcpy(bits, flag, 8);
    o = 8;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            int v = (frame[i] >> b) & 1;
            bits[o++] = (uint8_t)v;
            ones = v ? ones + 1 : 0;
            if (ones == 5) {
                bits[o++] = 0;
                ones = 0;
            }
        }
    memcpy(bits + o, flag, 8);
    return o + 8;
}

int vdl2_hdlc_decode(const uint8_t *bits, int nbits, vdl2_frame_cb cb, void *ctx)
{
    crc_t crc;
    crc_init(&crc, crc_preset("CRC-16/X-25"));
    uint8_t *fb = malloc((size_t)nbits + 16);
    if (!fb)
        return 0;
    int n = 0, ones = 0, in_frame = 0, found = 0;
    for (int i = 0; i < nbits; i++) {
        int b = bits[i] & 1;
        if (b) {
            ones++;
            if (ones >= 7) { /* aborto */
                in_frame = 0;
                n = 0;
                continue;
            }
            if (in_frame)
                fb[n++] = 1;
            continue;
        }
        if (ones == 6) { /* flag: 0 111111 0; los 7 bits previos (0 + 6 unos) ya están en fb */
            if (in_frame && n >= 7) {
                n -= 7;
                if (n % 8 == 0 && n >= 8 * 11) {
                    int nb = n / 8;
                    uint8_t *f = malloc((size_t)nb);
                    for (int k = 0; k < nb; k++) {
                        uint8_t v = 0;
                        for (int q = 0; q < 8; q++)
                            v |= (uint8_t)(fb[k * 8 + q] << q);
                        f[k] = v;
                    }
                    uint32_t calc = crc_compute(&crc, f, (size_t)nb - 2);
                    uint32_t rx = (uint32_t)(f[nb - 2] | (f[nb - 1] << 8));
                    if (calc == rx) {
                        cb(ctx, f, nb - 2);
                        found++;
                    }
                    free(f);
                }
            }
            in_frame = 1;
            n = 0;
            ones = 0;
            continue;
        }
        if (ones == 5) { /* cero de relleno */
            ones = 0;
            continue;
        }
        ones = 0;
        if (in_frame)
            fb[n++] = 0;
    }
    free(fb);
    return found;
}
