/*
 * sha1.c — SHA-1 según FIPS 180-4, sección 6.1, y codificación base64.
 *
 * SHA-1 procesa el mensaje en bloques de 512 bits. Antes se rellena con un
 * bit '1', ceros y la longitud original en bits (64 bits big-endian) hasta un
 * múltiplo de 512. Cada bloque se expande a 80 palabras de 32 bits y se pasa
 * por 80 rondas que actualizan cinco registros (a..e).
 */
#include "sha1.h"

#include <string.h>

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);     /* "Ch": elige c o d según b */
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;              /* paridad */
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d); /* "Maj": mayoría */
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint8_t *p = data;
    size_t full = len / 64;
    for (size_t i = 0; i < full; i++)
        sha1_block(h, p + 64 * i);

    /* Último bloque (o dos) con el relleno */
    uint8_t tail[128] = {0};
    size_t rem = len - full * 64;
    memcpy(tail, p + full * 64, rem);
    tail[rem] = 0x80;
    size_t tlen = (rem + 1 + 8 <= 64) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, tail);
    if (tlen == 128)
        sha1_block(h, tail + 64);

    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

void base64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        /* 3 bytes (24 bits) → 4 símbolos de 6 bits; '=' rellena el final */
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = '\0';
}
