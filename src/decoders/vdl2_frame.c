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

/* ---- Cabecera: código cíclico (25,20), g(x) = x^5 + x^2 + 1 ---------------- */
/* Registro de división: alimentar los bits en orden (el primero = grado más alto).
 * Tras los 20 bits de información el registro es la paridad; tras los 25 es el
 * síndrome (0 si no hay errores). */
static uint32_t hdr_lfsr(const uint8_t *bits, int n)
{
    uint32_t reg = 0;
    for (int i = 0; i < n; i++) {
        uint32_t fb = ((reg >> 4) & 1) ^ (bits[i] & 1);
        reg = (reg << 1) & 0x1F;
        if (fb)
            reg ^= 0x05;
    }
    return reg;
}

void vdl2_hdr_encode(uint32_t len, uint8_t bits[VDL2_HDR_BITS])
{
    bits[0] = bits[1] = bits[2] = 0; /* reservados */
    for (int i = 0; i < 17; i++)
        bits[3 + i] = (uint8_t)((len >> i) & 1); /* LSB primero */
    uint32_t p = hdr_lfsr(bits, 20);
    for (int i = 0; i < 5; i++)
        bits[20 + i] = (uint8_t)((p >> (4 - i)) & 1);
}

int vdl2_hdr_decode(const uint8_t bits[VDL2_HDR_BITS], uint32_t *len)
{
    uint8_t b[VDL2_HDR_BITS];
    memcpy(b, bits, sizeof b);
    int rc = 0;
    uint32_t syn = hdr_lfsr(b, VDL2_HDR_BITS);
    if (syn) {
        rc = -1;
        for (int i = 0; i < VDL2_HDR_BITS; i++) {
            uint8_t e[VDL2_HDR_BITS] = {0};
            e[i] = 1;
            if (hdr_lfsr(e, VDL2_HDR_BITS) == syn) {
                b[i] ^= 1;
                rc = 1;
                break;
            }
        }
    }
    uint32_t v = 0;
    for (int i = 0; i < 17; i++)
        v |= (uint32_t)(b[3 + i] & 1) << i;
    *len = v;
    return rc;
}

/* ---- Aleatorizador ------------------------------------------------------- */
/* Aleatorizador aditivo. Registro de 15 bits con x^15 + x^14 + 1: la secuencia cumple
 * g[n] = g[n-1] ^ g[n-15]. CONTRASTADO con grabación real (sigidwiki "VDL-M2 IQ", ver
 * docs/calidad/VALIDACION.md): se actúa solo sobre los datos (el primer bit tras la cabecera de 25
 * bits; la cabecera NO se aleatoriza) y los 15 primeros bits de la secuencia son
 * 010011100011111 (obtenidos de bursts cuyo primer octeto es la bandera 0x7E y la dirección
 * 0x14; coinciden en varias ráfagas y la recurrencia predice los bits siguientes). */
static const uint8_t SCR_SEED[15] = {0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1};

void vdl2_scramble(uint8_t *bits, int nbits)
{
    uint8_t g[15];
    memcpy(g, SCR_SEED, sizeof g);
    int pos = 0; /* g es un anillo: g[pos] = g[n-15] */
    for (int i = 0; i < nbits; i++) {
        uint8_t out = g[pos];
        bits[i] ^= out;
        /* g[n] = g[n-1] ^ g[n-15]; g[n-1] está en pos-1 (mod 15) */
        g[pos] = (uint8_t)(g[(pos + 14) % 15] ^ out);
        pos = (pos + 1) % 15;
    }
}

/* ---- FEC ------------------------------------------------------------------ */
static int ecc_for(int r) { return r <= 2 ? 0 : r <= 30 ? 2 : r <= 67 ? 4 : 6; }

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
    return (nb - 1) * 255 + ld + le;
}

/* Orden de transmisión: para cada posición p = 0..254, los bloques en orden
 * (j = 0..nb−1) que tengan byte en esa posición. Los bloques completos miden 255. */
static int blk_len(int j, int nb, int ld, int le) { return j == nb - 1 ? ld + le : 255; }

void vdl2_fec_encode(const uint8_t *data, int D, uint8_t *out)
{
    int nb, ld, le;
    layout(D, &nb, &ld, &le);
    rs_t rs;
    rs_init_vdl2(&rs);
    uint8_t (*cw)[255] = calloc((size_t)nb, 255);
    for (int j = 0; j < nb; j++) {
        int dj = j == nb - 1 ? ld : VDL2_BLOCK_DATA;
        memcpy(cw[j], data + (size_t)j * VDL2_BLOCK_DATA, (size_t)dj);
        rs_encode(&rs, cw[j], VDL2_BLOCK_DATA, cw[j] + VDL2_BLOCK_DATA);
    }
    /* secuencia enviada por bloque: datos 0..dj-1 y luego paridad 249.. (solo 'le' de la última) */
    int o = 0;
    for (int p = 0; p < 255; p++)
        for (int j = 0; j < nb; j++) {
            int L = blk_len(j, nb, ld, le);
            if (p >= L)
                continue;
            int dj = j == nb - 1 ? ld : VDL2_BLOCK_DATA;
            int src = p < dj ? p : VDL2_BLOCK_DATA + (p - dj);
            out[o++] = cw[j][src];
        }
    free(cw);
}

int vdl2_fec_decode(const uint8_t *enc, int D, uint8_t *data)
{
    int nb, ld, le;
    layout(D, &nb, &ld, &le);
    rs_t rs;
    rs_init_vdl2(&rs);
    uint8_t (*cw)[255] = calloc((size_t)nb, 255);
    int o = 0;
    for (int p = 0; p < 255; p++)
        for (int j = 0; j < nb; j++) {
            int L = blk_len(j, nb, ld, le);
            if (p >= L)
                continue;
            int dj = j == nb - 1 ? ld : VDL2_BLOCK_DATA;
            int dst = p < dj ? p : VDL2_BLOCK_DATA + (p - dj);
            cw[j][dst] = enc[o++];
        }
    int total = 0, rc = 0;
    for (int j = 0; j < nb; j++) {
        int dj = j == nb - 1 ? ld : VDL2_BLOCK_DATA;
        int ej = j == nb - 1 ? le : 6;
        if (ej > 0) {
            int era[6], ne = 0;
            for (int k = ej; k < 6; k++) /* paridad no transmitida = borrado */
                era[ne++] = VDL2_BLOCK_DATA + k;
            int c = rs_decode(&rs, cw[j], 255, era, ne);
            if (c < 0)
                rc = -1;
            else
                total += c - ne; /* los borrados no son errores del canal */
        }
        memcpy(data + (size_t)j * VDL2_BLOCK_DATA, cw[j], (size_t)dj);
    }
    free(cw);
    return rc < 0 ? -1 : total;
}

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
