/*
 * fuzz_http.c — Fuzzing del servidor HTTP/WebSocket de src/net/server.c.
 *
 * server.c no expone su analizador de peticiones sin sockets (handle_http y
 * ws_parse_one son static y están ligados al bucle poll), así que el arnés lanza el
 * servidor real, en proceso y bajo ASan/UBSan, en un puerto 8970–8979, y le envía
 * por TCP peticiones malformadas:
 *   - peticiones HTTP válidas mutadas (línea de petición, cabeceras, rutas con
 *     "..", "%2e", cadenas enormes, \r\n sueltos, bytes NUL);
 *   - apretón WebSocket con cabeceras defectuosas (clave enorme, Origin≠Host);
 *   - tras un apretón válido: tramas con longitudes imposibles (2^63), sin
 *     máscara, fragmentadas a medias, continuaciones huérfanas, pings enormes,
 *     opcodes reservados, y envío por trozos de 1 byte.
 * Cada cierto número de conexiones se comprueba que el servidor sigue atendiendo
 * (/api/status → 200) y al final se comprueba que no pierde memoria al destruirse.
 * Los casos de regresión (ficheros de tests/fuzz/casos/http/) son flujos de bytes
 * que se envían tal cual en una conexión nueva.
 */
#include "fuzz_net.h"
#include "common.h"
#include "net/server.h"

#include <pthread.h>

#define PORT 8970

static unsigned long g_texts, g_bins;

static void on_open(void *app, int c) { (void)app; (void)c; }
static void on_text(void *app, int c, const char *m, size_t l)
{
    (void)app; (void)c;
    /* la cadena debe estar terminada en '\0' y tener l bytes legibles */
    volatile char x = m[l];
    (void)x;
    for (size_t i = 0; i < l; i += 4096)
        (void)*(volatile const char *)(m + i);
    g_texts++;
}
static void on_binary(void *app, int c, const void *d, size_t l)
{
    (void)app; (void)c;
    if (l)
        (void)*(volatile const uint8_t *)((const uint8_t *)d + l - 1);
    g_bins++;
}
static void on_close(void *app, int c) { (void)app; (void)c; }
static void on_status(void *app, sb_t *b) { (void)app; sb_puts(b, "{\"ok\":true}"); }

static void *srv_thread(void *p)
{
    server_run(p);
    return NULL;
}

static const char *HTTP_SEEDS[] = {
    "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /index.html HTTP/1.1\r\nHost: x\r\nAccept: */*\r\n\r\n",
    "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /api/recordings HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /recordings/a.sigmf-meta HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /recordings/../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n",
    "GET /js/../../Makefile HTTP/1.1\r\nHost: x\r\n\r\n",
    "HEAD /ws?x=1 HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /ws HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
    "GET /ws HTTP/1.1\r\nHost: 127.0.0.1:8970\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
    "Origin: http://127.0.0.1:8970\r\n\r\n",
    "GET /ws HTTP/1.1\r\nHost: evil\r\nUpgrade: websocket\r\nSec-WebSocket-Key: a\r\nOrigin: http://other\r\n\r\n",
};

static void send_stream(const uint8_t *buf, size_t len, fz_rng_t *r, bool drip)
{
    int fd = net_connect(PORT);
    if (fd < 0)
        return;
    if (drip) { /* trozos pequeños: el analizador debe aguantar peticiones partidas */
        for (size_t o = 0; o < len;) {
            size_t n = 1 + fz_below(r, 17);
            if (n > len - o)
                n = len - o;
            if (!net_send(fd, buf + o, n))
                break;
            o += n;
        }
    } else {
        net_send(fd, buf, len);
    }
    char rb[2048];
    if (fz_below(r, 4) == 0)
        net_recv(fd, rb, sizeof rb, 2);
    close(fd);
}

static void regression_case(const uint8_t *b, size_t l, void *ctx) { send_stream(b, l, ctx, false); }

static int alive(void)
{
    int fd = net_connect(PORT);
    if (fd < 0)
        return 0;
    const char *rq = "GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n";
    net_send(fd, rq, strlen(rq));
    char rb[1024];
    long n = 0, t = 0;
    while (t < 20 && (n = net_recv(fd, rb, sizeof rb - 1, 100)) == 0)
        t++;
    close(fd);
    return n > 0 && memmem(rb, (size_t)n, " 200 ", 5) != NULL;
}

/* Tras un apretón válido, flujo de tramas de cliente mutadas / imposibles. */
static void ws_session(fz_rng_t *r)
{
    int fd = ws_connect(PORT);
    if (fd < 0)
        return;
    static uint8_t out[(1 << 17) + 64], pl[1 << 16];
    int nf = 1 + (int)fz_below(r, 6);
    for (int i = 0; i < nf; i++) {
        size_t pn = fz_below(r, 4) == 0 ? fz_below(r, sizeof pl) : fz_below(r, 200);
        for (size_t k = 0; k < pn; k++)
            pl[k] = (uint8_t)('a' + fz_below(r, 26));
        int op = (int)fz_below(r, 16);
        if (fz_below(r, 2))
            op = 1 + (int)fz_below(r, 2); /* texto/binario más a menudo */
        size_t n = ws_frame_build(out, op, (int)fz_below(r, 2), pl, pn, r);
        switch (fz_below(r, 8)) {
        case 0: /* sin máscara */
            out[1] &= 0x7F;
            break;
        case 1: /* longitud de 64 bits absurda */
            n = 0;
            out[n++] = 0x81;
            out[n++] = 0xFF;
            for (int k = 0; k < 8; k++)
                out[n++] = (uint8_t)(k == 0 ? 0x80 | fz_below(r, 128) : fz_next(r));
            for (int k = 0; k < 8; k++)
                out[n++] = (uint8_t)fz_next(r);
            break;
        case 2: /* truncar la trama */
            n = 1 + fz_below(r, (uint32_t)n);
            break;
        case 3: /* bytes de cabecera aleatorios */
            out[0] = (uint8_t)fz_next(r);
            out[1] = (uint8_t)fz_next(r);
            break;
        default:
            break;
        }
        if (!net_send(fd, out, n))
            break;
    }
    char rb[4096];
    net_recv(fd, rb, sizeof rb, 1);
    close(fd);
}

int main(int argc, char **argv)
{
    fz_t z;
    fz_init(&z, "http", argc, argv, 60);
    g_log_level = LOG_LVL_ERROR; /* el servidor avisa de cada Origin rechazado: ruido */
    mkdir("/tmp/q3", 0755);
    mkdir("/tmp/q3/rec_http", 0755);
    FILE *f = fopen("/tmp/q3/rec_http/a.sigmf-meta", "w");
    if (f) {
        fputs("{}", f);
        fclose(f);
    }
    server_callbacks_t cb = {on_open, on_text, on_binary, on_close, on_status};
    char err[200];
    server_t *s = server_create("127.0.0.1", PORT, "web", "/tmp/q3/rec_http", &cb, NULL, err, sizeof err);
    if (!s) {
        fprintf(stderr, "no se pudo crear el servidor: %s\n", err);
        return 2;
    }
    pthread_t th;
    pthread_create(&th, NULL, srv_thread, s);

    fz_run_cases("http", regression_case, &z.rng);
    if (!alive()) {
        fprintf(stderr, "el servidor no responde tras los casos de regresión\n");
        return 1;
    }
    static uint8_t buf[1 << 16];
    unsigned long it = 0;
    while (fz_alive(&z)) {
        size_t len;
        uint32_t k = fz_below(&z.rng, 10);
        if (k < 6) {
            const char *sd = HTTP_SEEDS[fz_below(&z.rng, sizeof HTTP_SEEDS / sizeof *HTTP_SEEDS)];
            len = strlen(sd);
            memcpy(buf, sd, len);
            len = fz_mutate(&z.rng, buf, len, sizeof buf);
            fz_current(buf, len);
            send_stream(buf, len, &z.rng, fz_below(&z.rng, 3) == 0);
        } else if (k < 7) { /* cabeceras enormes o muchas */
            len = (size_t)snprintf((char *)buf, 64, "GET /");
            size_t big = fz_below(&z.rng, 40000);
            for (size_t i = 0; i < big && len < sizeof buf - 64; i++)
                buf[len++] = fz_below(&z.rng, 3) ? 'a' : (uint8_t)fz_next(&z.rng);
            memcpy(buf + len, " HTTP/1.1\r\nHost: x\r\n\r\n", 23);
            len += 23;
            fz_current(buf, len);
            send_stream(buf, len, &z.rng, false);
        } else if (k < 8) { /* basura binaria */
            len = fz_below(&z.rng, 300);
            for (size_t i = 0; i < len; i++)
                buf[i] = (uint8_t)fz_next(&z.rng);
            fz_current(buf, len);
            send_stream(buf, len, &z.rng, false);
        } else {
            ws_session(&z.rng);
        }
        if (++it % 300 == 0 && !alive()) {
            fprintf(stderr, "[fuzz:http] el servidor dejó de responder tras %lu conexiones\n", it);
            return 1;
        }
    }
    if (!alive()) {
        fprintf(stderr, "[fuzz:http] el servidor no responde al final\n");
        return 1;
    }
    server_stop(s);
    pthread_join(th, NULL);
    server_destroy(s);
    printf("[fuzz:http] %lu conexiones (%lu textos, %lu binarios WS entregados), sin fallos\n", it, g_texts, g_bins);
    return 0;
}
