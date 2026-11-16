/*
 * test_aes.c — AES con los vectores de FIPS-197 (Apéndices B y C) y de CTR con
 * los de NIST SP 800-38A (F.5.1, AES-128 CTR). Más la comprobación del hash de
 * canal por defecto de Meshtastic (clave por defecto + "LongFast" → 8).
 */
#include "test_util.h"

#include "decoders/aes.h"

static int hex2bin(const char *h, uint8_t *o)
{
    int n = 0;
    for (; h[0] && h[1]; h += 2)
        o[n++] = (uint8_t)strtoul((char[]){h[0], h[1], 0}, NULL, 16);
    return n;
}

static void block_test(const char *key, const char *pt, const char *ct)
{
    uint8_t k[32], p[16], c[16], o[16], d[16];
    int kl = hex2bin(key, k);
    hex2bin(pt, p);
    hex2bin(ct, c);
    aes_ctx_t ctx;
    CHECK_EQ(aes_set_key(&ctx, k, kl), 0);
    aes_encrypt_block(&ctx, p, o);
    CHECK(memcmp(o, c, 16) == 0);
    aes_decrypt_block(&ctx, c, d);
    CHECK(memcmp(d, p, 16) == 0);
}

int main(void)
{
    /* FIPS-197 Apéndice B (ejemplo de cifrado) */
    block_test("2b7e151628aed2a6abf7158809cf4f3c", "3243f6a8885a308d313198a2e0370734",
               "3925841d02dc09fbdc118597196a0b32");
    /* Apéndice C.1 / C.2 / C.3 */
    block_test("000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff",
               "69c4e0d86a7b0430d8cdb78070b4c55a");
    block_test("000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff",
               "dda97ca4864cdfe06eaf70a0ec0d7191");
    block_test("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "00112233445566778899aabbccddeeff",
               "8ea2b7ca516745bfeafc49904b496089");

    /* NIST SP 800-38A F.5.1: CTR-AES128, 4 bloques, contador f0f1…feff, +1 en 128 bits */
    uint8_t k[16], nonce[16], pt[64], ct[64], o[64];
    hex2bin("2b7e151628aed2a6abf7158809cf4f3c", k);
    hex2bin("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", nonce);
    hex2bin("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
            "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
            pt);
    hex2bin("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
            "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee",
            ct);
    aes_ctx_t ctx;
    aes_set_key(&ctx, k, 16);
    aes_ctr_xor(&ctx, nonce, 16, pt, o, 64);
    CHECK(memcmp(o, ct, 64) == 0);
    aes_ctr_xor(&ctx, nonce, 16, ct, o, 37); /* longitud no múltiplo de 16 */
    CHECK(memcmp(o, pt, 37) == 0);
    /* Con contador de 4 bytes el acarreo no se propaga más allá: bloque 2 igual aquí */
    aes_ctr_xor(&ctx, nonce, 4, pt, o, 64);
    CHECK(memcmp(o, ct, 64) == 0);

    /* Clave por defecto de Meshtastic (AQ==): hash de canal con "LongFast" = 8 */
    static const uint8_t defkey[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                       0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    uint8_t h = 0;
    for (const char *s = "LongFast"; *s; s++)
        h ^= (uint8_t)*s;
    for (int i = 0; i < 16; i++)
        h ^= defkey[i];
    CHECK_EQ(h, 8);
    return TEST_SUMMARY();
}
