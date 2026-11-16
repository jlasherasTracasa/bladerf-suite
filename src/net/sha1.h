/*
 * sha1.h — SHA-1 (FIPS 180-4) y base64 (RFC 4648), implementados aquí.
 *
 * Solo se usan para el saludo (handshake) de WebSocket, que exige calcular
 *     base64( SHA1( Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
 * SHA-1 ya no se considera seguro para firmas, pero aquí no protege nada:
 * el protocolo lo usa solo para demostrar que el servidor entiende WebSocket.
 */
#ifndef BLADERF_LAB_SHA1_H
#define BLADERF_LAB_SHA1_H

#include <stddef.h>
#include <stdint.h>

void sha1(const void *data, size_t len, uint8_t out[20]);
/* Codifica n bytes en base64. out debe tener sitio para 4·ceil(n/3)+1. */
void base64_encode(const uint8_t *in, size_t n, char *out);

#endif
