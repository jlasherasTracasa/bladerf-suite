/*
 * aes.h — AES (FIPS-197) de 128/192/256 bits y modo CTR (NIST SP 800-38A).
 *
 * Meshtastic cifra la carga de cada paquete con AES-CTR; la única clave que se
 * usa en este proyecto es la pública por defecto del canal primario
 * (AGENTS.md §0.12). Se implementa aquí desde cero (sin OpenSSL).
 *
 * Teoría: AES opera sobre un estado de 4×4 bytes en GF(2^8) con el polinomio
 * irreducible x^8+x^4+x^3+x+1 (0x11B). Cada ronda aplica SubBytes (inverso
 * multiplicativo en GF(2^8) + transformación afín), ShiftRows, MixColumns
 * (multiplicar cada columna por {03}x^3+{01}x^2+{01}x+{02}) y AddRoundKey.
 * Nr = 10/12/14 rondas para claves de 16/24/32 bytes. La S-box se calcula en
 * la primera llamada a partir de la definición (no se teclea una tabla de 256
 * valores, que es fácil de equivocar) y se comprueba con los vectores de la
 * norma en tests/test_aes.c.
 *
 * CTR: keystream_i = AES_K(contador_i); contador = nonce de 16 bytes cuyos
 * últimos 'ctr_bytes' bytes se incrementan en gran-endian (Arduino Crypto
 * "CTR" con setCounterSize(4), que es lo que usa Meshtastic).
 */
#ifndef BLADERF_LAB_AES_H
#define BLADERF_LAB_AES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int nr;             /* rondas */
    uint8_t rk[15 * 16];/* claves de ronda expandidas */
} aes_ctx_t;

/* keylen: 16, 24 o 32. Devuelve 0 o −1 si la longitud no es válida. */
int aes_set_key(aes_ctx_t *c, const uint8_t *key, int keylen);
void aes_encrypt_block(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt_block(const aes_ctx_t *c, const uint8_t in[16], uint8_t out[16]);
/* CTR: out = in XOR keystream (in y out pueden coincidir). nonce: 16 bytes
 * (contador inicial); ctr_bytes: cuántos bytes finales cuentan (1..16). */
void aes_ctr_xor(const aes_ctx_t *c, const uint8_t nonce[16], int ctr_bytes, const uint8_t *in, uint8_t *out,
                 size_t n);

#endif
