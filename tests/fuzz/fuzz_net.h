/*
 * fuzz_net.h — Cliente TCP/WebSocket mínimo para los arneses http y ws_cmd.
 *
 * Solo lo imprescindible: conectar a 127.0.0.1, enviar bytes, leer con tiempo
 * límite, hacer el apretón de manos WebSocket (RFC 6455 §4) y construir tramas
 * enmascaradas de cliente (§5.2-5.3). El enmascarado es obligatorio en las tramas
 * cliente→servidor; el servidor las rechaza sin él, y queremos poder generar tanto
 * tramas correctas (para llegar a las órdenes) como defectuosas (para el análisis).
 */
#ifndef BLADERF_LAB_FUZZ_NET_H
#define BLADERF_LAB_FUZZ_NET_H

#include "fuzz_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

static inline int net_connect(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Envía todo (false si la conexión se cerró: el servidor puede cortar a mitad). */
static inline int net_send(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);
        if (w <= 0)
            return 0;
        p += w;
        len -= (size_t)w;
    }
    return 1;
}

/* Lee lo que haya durante hasta ms milisegundos (devuelve bytes leídos, 0 = nada, −1 = cerrada). */
static inline long net_recv(int fd, void *buf, size_t cap, int ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, ms) <= 0)
        return 0;
    ssize_t r = recv(fd, buf, cap, 0);
    return r > 0 ? r : -1;
}

/* Apretón de manos WebSocket válido. Devuelve el socket o −1. */
static inline int ws_connect(int port)
{
    int fd = net_connect(port);
    if (fd < 0)
        return -1;
    char req[256];
    int n = snprintf(req, sizeof req,
                     "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     port);
    if (!net_send(fd, req, (size_t)n)) {
        close(fd);
        return -1;
    }
    char resp[4096];
    long r = net_recv(fd, resp, sizeof resp - 1, 3000);
    if (r <= 0 || !memmem(resp, (size_t)r, " 101 ", 5)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Trama de cliente enmascarada. Devuelve la longitud escrita en out (cap ≥ len + 14). */
static inline size_t ws_frame_build(uint8_t *out, int op, int fin, const void *pl, size_t len, fz_rng_t *r)
{
    size_t o = 0;
    out[o++] = (uint8_t)((fin ? 0x80 : 0) | (op & 15));
    if (len < 126) {
        out[o++] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        out[o++] = 0x80 | 126;
        out[o++] = (uint8_t)(len >> 8);
        out[o++] = (uint8_t)len;
    } else {
        out[o++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            out[o++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint8_t m[4];
    for (int i = 0; i < 4; i++)
        m[i] = (uint8_t)fz_next(r);
    memcpy(out + o, m, 4);
    o += 4;
    for (size_t i = 0; i < len; i++)
        out[o + i] = ((const uint8_t *)pl)[i] ^ m[i & 3];
    return o + len;
}

#endif
