/*
 * server.c — Servidor HTTP + WebSocket con bucle poll(). Ver server.h.
 *
 * Reglas de concurrencia:
 *   - Solo el hilo de red lee de los sockets y cambia el estado de conexión.
 *   - Las colas de salida y los campos used/id/state de cada cliente se
 *     protegen con s->lock, porque el hilo DSP encola mensajes.
 *   - Las callbacks a la aplicación se llaman SIN tener s->lock cogido, para
 *     no provocar interbloqueos con los cerrojos de la aplicación.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "server.h"

#include "../common.h"
#include "sha1.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define QCAP 2048                 /* mensajes pendientes por cliente */
#define DROP_BYTES (2u << 20)     /* a partir de aquí se descartan tramas */
#define IN_MAX (2u << 20)         /* tamaño máximo de entrada acumulada */
#define HDR_MAX 16384             /* cabeceras HTTP */
#define WS_MAX_PAYLOAD (1u << 20)
#define FILE_CHUNK (256 * 1024)
#define STATIC_MAX (16u << 20)

typedef struct {
    int refs;   /* protegido por s->lock */
    size_t len;
    uint8_t data[];
} msg_t;

enum { C_HTTP, C_WS, C_DRAIN, C_FILE };

typedef struct {
    bool used;
    int fd;
    int id;
    int state;
    bool kill;
    /* entrada (solo hilo de red) */
    uint8_t *in;
    size_t in_len, in_cap;
    uint8_t *frag;      /* mensaje WebSocket fragmentado en curso */
    size_t frag_len;
    int frag_op;
    /* salida (protegida por s->lock) */
    msg_t *q[QCAP];
    int qh, qn;
    size_t qoff, qbytes;
    /* descarga de fichero */
    int file_fd;
    off_t file_left;
} client_t;

struct server {
    int lfd, efd;
    char webroot[512], recdir[512];
    server_callbacks_t cb;
    void *app;
    pthread_mutex_t lock;
    client_t cl[MAX_CLIENTS];
    int next_id;
    volatile bool stop;
    unsigned long long sent;
};

/* ======================================================================= */
/*  Mensajes y colas                                                       */
/* ======================================================================= */
static msg_t *msg_new(size_t len)
{
    msg_t *m = malloc(sizeof *m + len);
    if (!m)
        abort();
    m->refs = 0;
    m->len = len;
    return m;
}

static void msg_unref(msg_t *m)
{
    if (--m->refs <= 0)
        free(m);
}

/* Trama WebSocket de servidor (sin máscara): cabecera + datos. */
static msg_t *ws_frame(int opcode, const void *data, size_t len)
{
    size_t hl = len < 126 ? 2 : (len < 65536 ? 4 : 10);
    msg_t *m = msg_new(hl + len);
    uint8_t *h = m->data;
    h[0] = (uint8_t)(0x80 | opcode); /* FIN + opcode */
    if (len < 126) {
        h[1] = (uint8_t)len;
    } else if (len < 65536) {
        h[1] = 126;
        h[2] = (uint8_t)(len >> 8);
        h[3] = (uint8_t)len;
    } else {
        h[1] = 127;
        for (int i = 0; i < 8; i++)
            h[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
    }
    memcpy(h + hl, data, len);
    return m;
}

/* Encola m en el cliente c. Requiere s->lock. Devuelve false si se descartó. */
static bool enqueue(client_t *c, msg_t *m, bool droppable)
{
    if (droppable && (c->qbytes > DROP_BYTES || c->qn >= QCAP / 2))
        return false;
    if (c->qn >= QCAP) {
        c->kill = true; /* cliente atascado: mejor desconectarlo */
        return false;
    }
    c->q[(c->qh + c->qn) % QCAP] = m;
    c->qn++;
    c->qbytes += m->len;
    m->refs++;
    return true;
}

static void wake(server_t *s)
{
    uint64_t one = 1;
    ssize_t r = write(s->efd, &one, sizeof one);
    (void)r;
}

static void send_ws(server_t *s, int client, int opcode, const void *data, size_t len, bool droppable)
{
    msg_t *m = ws_frame(opcode, data, len);
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &s->cl[i];
        if (c->used && c->state == C_WS && !c->kill && (client < 0 || c->id == client))
            enqueue(c, m, droppable);
    }
    if (m->refs == 0)
        free(m);
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

void server_send_text(server_t *s, int client, const char *text, size_t len) { send_ws(s, client, 1, text, len, false); }

void server_send_binary(server_t *s, int client, const void *data, size_t len, bool droppable)
{
    send_ws(s, client, 2, data, len, droppable);
}

void server_kick(server_t *s, int client)
{
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s->cl[i].used && s->cl[i].id == client)
            s->cl[i].kill = true;
    pthread_mutex_unlock(&s->lock);
    wake(s);
}

int server_client_count(server_t *s)
{
    int n = 0;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s->cl[i].used && s->cl[i].state == C_WS)
            n++;
    pthread_mutex_unlock(&s->lock);
    return n;
}

unsigned long long server_bytes_sent(server_t *s)
{
    pthread_mutex_lock(&s->lock);
    unsigned long long v = s->sent;
    pthread_mutex_unlock(&s->lock);
    return v;
}

/* ======================================================================= */
/*  Creación / cierre                                                      */
/* ======================================================================= */
server_t *server_create(const char *host, int port, const char *webroot, const char *recdir,
                        const server_callbacks_t *cb, void *app, char *err, size_t errlen)
{
    server_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    snprintf(s->webroot, sizeof s->webroot, "%s", webroot);
    snprintf(s->recdir, sizeof s->recdir, "%s", recdir);
    s->cb = *cb;
    s->app = app;
    s->next_id = 1;
    pthread_mutex_init(&s->lock, NULL);

    s->lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        snprintf(err, errlen, "dirección no válida: %s", host);
        goto fail;
    }
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) < 0) {
        snprintf(err, errlen, "no se puede escuchar en %s:%d: %s%s", host, port, strerror(errno),
                 errno == EADDRINUSE ? " (¿ya hay otra instancia en marcha?)" : "");
        goto fail;
    }
    if (listen(s->lfd, 16) < 0) {
        snprintf(err, errlen, "listen: %s", strerror(errno));
        goto fail;
    }
    s->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return s;
fail:
    close(s->lfd);
    free(s);
    return NULL;
}

void server_stop(server_t *s)
{
    s->stop = true;
    wake(s);
}

static void client_free_queue(client_t *c)
{
    while (c->qn > 0) {
        msg_unref(c->q[c->qh]);
        c->qh = (c->qh + 1) % QCAP;
        c->qn--;
    }
    c->qbytes = c->qoff = 0;
}

static void client_close(server_t *s, client_t *c)
{
    pthread_mutex_lock(&s->lock);
    bool was_ws = c->state == C_WS;
    int id = c->id;
    client_free_queue(c);
    c->used = false;
    pthread_mutex_unlock(&s->lock);
    close(c->fd);
    if (c->file_fd > 0)
        close(c->file_fd);
    c->file_fd = 0;
    free(c->in);
    free(c->frag);
    c->in = c->frag = NULL;
    c->in_len = c->in_cap = c->frag_len = 0;
    if (was_ws && s->cb.on_close)
        s->cb.on_close(s->app, id);
}

void server_destroy(server_t *s)
{
    if (!s)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s->cl[i].used)
            client_close(s, &s->cl[i]);
    close(s->lfd);
    close(s->efd);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

/* ======================================================================= */
/*  HTTP                                                                   */
/* ======================================================================= */
static const char *SEC_HEADERS =
    "X-Content-Type-Options: nosniff\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Content-Security-Policy: default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; "
    "connect-src 'self' ws: wss:; style-src 'self' 'unsafe-inline'; worker-src 'self' blob:\r\n";

/* Encola una respuesta HTTP completa y deja el cliente en modo "vaciar y cerrar". */
static void http_respond(server_t *s, client_t *c, int code, const char *status, const char *ctype,
                         const void *body, size_t blen, const char *extra)
{
    sb_t h = {0};
    sb_printf(&h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\n", code,
              status, ctype, blen);
    sb_puts(&h, SEC_HEADERS);
    if (extra)
        sb_puts(&h, extra);
    sb_puts(&h, "Connection: close\r\n\r\n");
    msg_t *m = msg_new(h.len + blen);
    memcpy(m->data, h.data, h.len);
    if (blen)
        memcpy(m->data + h.len, body, blen);
    sb_free(&h);
    pthread_mutex_lock(&s->lock);
    enqueue(c, m, false);
    if (m->refs == 0)
        free(m);
    c->state = C_DRAIN;
    pthread_mutex_unlock(&s->lock);
}

static void http_error(server_t *s, client_t *c, int code, const char *status, const char *text)
{
    http_respond(s, c, code, status, "text/plain; charset=utf-8", text, strlen(text), NULL);
}

static const char *mime_for(const char *path)
{
    static const struct { const char *ext, *type; } T[] = {
        {".html", "text/html; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},   {".svg", "image/svg+xml"},
        {".png", "image/png"},                  {".ico", "image/x-icon"},
        {".json", "application/json"},          {".webmanifest", "application/manifest+json"},
        {".wav", "audio/wav"},                  {".md", "text/plain; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},
    };
    const char *dot = strrchr(path, '.');
    if (dot)
        for (size_t i = 0; i < ARRAY_LEN(T); i++)
            if (strcasecmp(dot, T[i].ext) == 0)
                return T[i].type;
    return "application/octet-stream";
}

/* Acepta solo rutas formadas por segmentos "normales": letras, dígitos, . _ -
 * y sin segmentos que empiecen por '.' (impide "..", ocultos, etc.). */
static bool path_safe(const char *p)
{
    if (*p == '/')
        p++;
    bool seg_start = true;
    for (; *p; p++) {
        char ch = *p;
        if (ch == '/') {
            if (seg_start)
                return false; /* "//" */
            seg_start = true;
            continue;
        }
        if (seg_start && ch == '.')
            return false;
        seg_start = false;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' ||
              ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

/* Une dir + sep + name en out sin truncar en silencio: false si no cabe (el llamador responde 404). */
static bool join_path(char *out, size_t n, const char *dir, const char *sep, const char *name)
{
    size_t a = strlen(dir), b = strlen(sep), c = strlen(name);
    if (a + b + c + 1 > n)
        return false;
    memcpy(out, dir, a);
    memcpy(out + a, sep, b);
    memcpy(out + a + b, name, c + 1);
    return true;
}

static void serve_static(server_t *s, client_t *c, const char *path)
{
    if (strcmp(path, "/") == 0)
        path = "/index.html";
    if (!path_safe(path)) {
        http_error(s, c, 400, "Bad Request", "Ruta no permitida");
        return;
    }
    char full[1024];
    FILE *f = join_path(full, sizeof full, s->webroot, "", path) ? fopen(full, "rb") : NULL;
    struct stat st;
    if (!f || fstat(fileno(f), &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > STATIC_MAX) {
        if (f)
            fclose(f);
        http_error(s, c, 404, "Not Found", "No encontrado");
        return;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    http_respond(s, c, 200, "OK", mime_for(path), buf, n, NULL);
    free(buf);
}

static int cmp_mtime_desc(const void *a, const void *b)
{
    const struct { char name[256]; long long size; long long mtime; } *x = a, *y = b;
    return (x->mtime < y->mtime) - (x->mtime > y->mtime);
}

static void serve_recordings_list(server_t *s, client_t *c)
{
    typedef struct { char name[256]; long long size; long long mtime; } ent_t;
    ent_t *e = NULL;
    int n = 0, cap = 0;
    DIR *d = opendir(s->recdir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' || !path_safe(de->d_name))
                continue;
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", s->recdir, de->d_name);
            struct stat st;
            if (stat(full, &st) < 0 || !S_ISREG(st.st_mode))
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                e = realloc(e, sizeof *e * (size_t)cap);
            }
            snprintf(e[n].name, sizeof e[n].name, "%s", de->d_name);
            e[n].size = st.st_size;
            e[n].mtime = st.st_mtime;
            n++;
        }
        closedir(d);
    }
    if (n > 1)
        qsort(e, (size_t)n, sizeof *e, cmp_mtime_desc);
    sb_t b = {0};
    sb_puts(&b, "{\"files\":[");
    for (int i = 0; i < n; i++) {
        if (i)
            sb_puts(&b, ",");
        sb_puts(&b, "{\"name\":");
        sb_json_str(&b, e[i].name);
        sb_printf(&b, ",\"size\":%lld,\"mtime\":%lld}", e[i].size, e[i].mtime);
    }
    sb_puts(&b, "]}");
    http_respond(s, c, 200, "OK", "application/json", b.data, b.len, NULL);
    sb_free(&b);
    free(e);
}

static void serve_recording_file(server_t *s, client_t *c, const char *name)
{
    if (!path_safe(name) || strchr(name, '/')) {
        http_error(s, c, 400, "Bad Request", "Nombre no permitido");
        return;
    }
    char full[1024];
    /* Si la ruta no cabe se trata como inexistente (antes se abría una ruta truncada). */
    if (!join_path(full, sizeof full, s->recdir, "/", name)) {
        http_error(s, c, 404, "Not Found", "No encontrado");
        return;
    }
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        http_error(s, c, 404, "Not Found", "No encontrado");
        return;
    }
    sb_t h = {0};
    sb_printf(&h,
              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
              "Content-Disposition: attachment; filename=\"%s\"\r\n",
              mime_for(name), (long long)st.st_size, name);
    sb_puts(&h, SEC_HEADERS);
    sb_puts(&h, "Connection: close\r\n\r\n");
    msg_t *m = msg_new(h.len);
    memcpy(m->data, h.data, h.len);
    sb_free(&h);
    pthread_mutex_lock(&s->lock);
    enqueue(c, m, false);
    if (m->refs == 0)
        free(m);
    c->state = C_FILE;
    c->file_fd = fd;
    c->file_left = st.st_size;
    pthread_mutex_unlock(&s->lock);
}

/* Busca una cabecera (sin distinguir mayúsculas) y copia su valor. */
static bool header_get(const char *hdrs, const char *name, char *out, size_t outlen)
{
    size_t nl = strlen(name);
    for (const char *p = hdrs; p && *p;) {
        const char *eol = strstr(p, "\r\n");
        if (!eol)
            break;
        if ((size_t)(eol - p) > nl && strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t len = (size_t)(eol - v);
            if (len >= outlen)
                len = outlen - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        p = eol + 2;
    }
    return false;
}

static bool ws_handshake(server_t *s, client_t *c, const char *hdrs)
{
    char key[128], upgrade[64], origin[256], host[256];
    if (!header_get(hdrs, "Upgrade", upgrade, sizeof upgrade) || strcasecmp(upgrade, "websocket") != 0 ||
        !header_get(hdrs, "Sec-WebSocket-Key", key, sizeof key)) {
        http_error(s, c, 400, "Bad Request", "Se esperaba una petición WebSocket");
        return false;
    }
    /* Protección contra "Cross-Site WebSocket Hijacking": el navegador
     * siempre envía Origin; debe coincidir con el Host al que se conecta. */
    if (header_get(hdrs, "Origin", origin, sizeof origin)) {
        const char *o = strstr(origin, "://");
        o = o ? o + 3 : origin;
        if (!header_get(hdrs, "Host", host, sizeof host) || strcasecmp(o, host) != 0) {
            LOG_W("WebSocket rechazado: Origin '%s' no coincide con Host", origin);
            http_error(s, c, 403, "Forbidden", "Origen no permitido");
            return false;
        }
    }
    char cat[256];
    snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    uint8_t dig[20];
    sha1(cat, strlen(cat), dig);
    char acc[64];
    base64_encode(dig, 20, acc);
    char resp[512];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     acc);
    msg_t *m = msg_new((size_t)n);
    memcpy(m->data, resp, (size_t)n);
    pthread_mutex_lock(&s->lock);
    enqueue(c, m, false);
    if (m->refs == 0)
        free(m);
    c->state = C_WS;
    pthread_mutex_unlock(&s->lock);
    return true;
}

/* Procesa la petición HTTP si ya llegó completa. Devuelve true si se
 * actualizó a WebSocket (hay que llamar a on_open). */
static bool handle_http(server_t *s, client_t *c, size_t *consumed)
{
    *consumed = 0;
    if (c->in_len < 4)
        return false;
    char *end = memmem(c->in, c->in_len, "\r\n\r\n", 4);
    if (!end) {
        if (c->in_len > HDR_MAX)
            http_error(s, c, 431, "Request Header Fields Too Large", "Cabeceras demasiado grandes");
        return false;
    }
    size_t hlen = (size_t)(end - (char *)c->in) + 4;
    *consumed = hlen;
    char *req = malloc(hlen + 1);
    memcpy(req, c->in, hlen);
    req[hlen] = '\0';

    char method[16] = "", path[1024] = "";
    sscanf(req, "%15s %1023s", method, path);
    char *q = strchr(path, '?');
    if (q)
        *q = '\0';
    const char *hdrs = strstr(req, "\r\n");
    hdrs = hdrs ? hdrs + 2 : "";
    bool upgraded = false;

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        http_error(s, c, 405, "Method Not Allowed", "Solo GET");
    } else if (strcmp(path, "/ws") == 0) {
        upgraded = ws_handshake(s, c, hdrs);
    } else if (strcmp(path, "/api/status") == 0) {
        sb_t b = {0};
        if (s->cb.on_status)
            s->cb.on_status(s->app, &b);
        http_respond(s, c, 200, "OK", "application/json", b.data ? b.data : "{}", b.data ? b.len : 2, NULL);
        sb_free(&b);
    } else if (strcmp(path, "/api/recordings") == 0) {
        serve_recordings_list(s, c);
    } else if (strncmp(path, "/recordings/", 12) == 0) {
        serve_recording_file(s, c, path + 12);
    } else {
        serve_static(s, c, path);
    }
    free(req);
    return upgraded;
}

/* ======================================================================= */
/*  WebSocket: lectura de tramas del cliente                               */
/* ======================================================================= */
/* Devuelve el nº de bytes consumidos (0 si falta la trama entera), o −1
 * para cerrar la conexión. */
static long ws_parse_one(server_t *s, client_t *c)
{
    const uint8_t *b = c->in;
    size_t n = c->in_len;
    if (n < 2)
        return 0;
    bool fin = b[0] & 0x80;
    int op = b[0] & 0x0F;
    bool masked = b[1] & 0x80;
    uint64_t len = b[1] & 0x7F;
    size_t pos = 2;
    if (len == 126) {
        if (n < 4)
            return 0;
        len = ((uint64_t)b[2] << 8) | b[3];
        pos = 4;
    } else if (len == 127) {
        if (n < 10)
            return 0;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | b[2 + i];
        pos = 10;
    }
    if (!masked || len > WS_MAX_PAYLOAD)
        return -1; /* RFC 6455: las tramas del cliente deben ir enmascaradas */
    if (n < pos + 4 + len)
        return 0;
    const uint8_t *mask = b + pos;
    uint8_t *p = c->in + pos + 4;
    for (uint64_t i = 0; i < len; i++)
        p[i] ^= mask[i & 3];
    long used = (long)(pos + 4 + len);

    switch (op) {
    case 0x0: /* continuación */
    case 0x1: /* texto */
    case 0x2: /* binario */
        if (op != 0) {
            c->frag_op = op;
            c->frag_len = 0;
        }
        if (!fin || op == 0) {
            /* Acumular fragmentos */
            if (c->frag_len + len > WS_MAX_PAYLOAD)
                return -1;
            c->frag = realloc(c->frag, c->frag_len + len + 1);
            memcpy(c->frag + c->frag_len, p, len);
            c->frag_len += len;
            if (!fin)
                return used;
            p = c->frag;
            len = c->frag_len;
            c->frag_len = 0;
        }
        if (c->frag_op == 1 && s->cb.on_text) {
            char save = (char)p[len]; /* terminar en '\0' temporalmente */
            p[len] = '\0';
            s->cb.on_text(s->app, c->id, (const char *)p, len);
            p[len] = (uint8_t)save;
        } else if (c->frag_op == 2 && s->cb.on_binary) {
            s->cb.on_binary(s->app, c->id, p, len);
        }
        return used;
    case 0x8: /* cierre: responder y cerrar */
        return -1;
    case 0x9: { /* ping → pong */
        msg_t *m = ws_frame(0xA, p, len);
        pthread_mutex_lock(&s->lock);
        enqueue(c, m, false);
        if (m->refs == 0)
            free(m);
        pthread_mutex_unlock(&s->lock);
        return used;
    }
    default: /* pong u otros: ignorar */
        return used;
    }
}

/* ======================================================================= */
/*  Bucle de eventos                                                       */
/* ======================================================================= */
static void accept_clients(server_t *s)
{
    for (;;) {
        int fd = accept4(s->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* baja latencia de audio */
        pthread_mutex_lock(&s->lock);
        client_t *c = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!s->cl[i].used) {
                c = &s->cl[i];
                break;
            }
        if (!c) {
            pthread_mutex_unlock(&s->lock);
            close(fd);
            LOG_W("Conexión rechazada: demasiados clientes");
            continue;
        }
        memset(c, 0, sizeof *c);
        c->used = true;
        c->fd = fd;
        c->id = s->next_id++;
        c->state = C_HTTP;
        pthread_mutex_unlock(&s->lock);
    }
}

static void client_read(server_t *s, client_t *c)
{
    for (;;) {
        if (c->in_cap - c->in_len < 65536) {
            size_t nc = c->in_cap ? c->in_cap * 2 : 131072;
            if (nc > IN_MAX + 131072) {
                c->kill = true;
                return;
            }
            c->in = realloc(c->in, nc);
            c->in_cap = nc;
        }
        ssize_t r = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len - 1, 0);
        if (r == 0) {
            c->kill = true;
            return;
        }
        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                c->kill = true;
            break;
        }
        c->in_len += (size_t)r;
    }

    size_t off = 0;
    for (;;) {
        if (c->kill)
            break;
        if (c->state == C_HTTP) {
            size_t used;
            /* handle_http trabaja sobre c->in desde el principio */
            if (off) {
                memmove(c->in, c->in + off, c->in_len - off);
                c->in_len -= off;
                off = 0;
            }
            bool up = handle_http(s, c, &used);
            if (!used)
                break;
            off = used;
            if (up && s->cb.on_open)
                s->cb.on_open(s->app, c->id);
            if (c->state != C_WS)
                break; /* respuesta HTTP normal: se ignora el resto */
        } else if (c->state == C_WS) {
            if (off) {
                memmove(c->in, c->in + off, c->in_len - off);
                c->in_len -= off;
                off = 0;
            }
            long used = ws_parse_one(s, c);
            if (used < 0) {
                c->kill = true;
                break;
            }
            if (used == 0)
                break;
            off = (size_t)used;
        } else {
            off = c->in_len; /* en otros estados se descarta la entrada */
            break;
        }
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }
}

static void client_write(server_t *s, client_t *c)
{
    pthread_mutex_lock(&s->lock);
    while (c->qn > 0) {
        msg_t *m = c->q[c->qh];
        ssize_t w = send(c->fd, m->data + c->qoff, m->len - c->qoff, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                c->kill = true;
            break;
        }
        s->sent += (unsigned long long)w;
        c->qoff += (size_t)w;
        if (c->qoff < m->len)
            break;
        c->qbytes -= m->len;
        c->qoff = 0;
        c->qh = (c->qh + 1) % QCAP;
        c->qn--;
        msg_unref(m);
    }
    bool empty = c->qn == 0;
    pthread_mutex_unlock(&s->lock);

    if (empty && c->state == C_FILE && !c->kill) {
        /* Siguiente trozo del fichero que se está descargando */
        size_t want = (size_t)MIN((off_t)FILE_CHUNK, c->file_left);
        msg_t *m = msg_new(want);
        ssize_t r = want ? read(c->file_fd, m->data, want) : 0;
        if (r <= 0) {
            free(m);
            close(c->file_fd);
            c->file_fd = 0;
            c->state = C_DRAIN;
        } else {
            m->len = (size_t)r;
            c->file_left -= r;
            pthread_mutex_lock(&s->lock);
            enqueue(c, m, false);
            pthread_mutex_unlock(&s->lock);
        }
    } else if (empty && c->state == C_DRAIN) {
        c->kill = true;
    }
}

void server_run(server_t *s)
{
    struct pollfd pfd[MAX_CLIENTS + 2];
    int map[MAX_CLIENTS + 2];
    while (!s->stop) {
        int n = 0;
        pfd[n].fd = s->lfd;
        pfd[n].events = POLLIN;
        map[n++] = -1;
        pfd[n].fd = s->efd;
        pfd[n].events = POLLIN;
        map[n++] = -2;
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &s->cl[i];
            if (!c->used)
                continue;
            pfd[n].fd = c->fd;
            pfd[n].events = POLLIN;
            if (c->qn > 0 || c->state == C_FILE || c->state == C_DRAIN || c->kill)
                pfd[n].events |= POLLOUT;
            map[n++] = i;
        }
        pthread_mutex_unlock(&s->lock);

        int r = poll(pfd, (nfds_t)n, 1000);
        if (r < 0 && errno != EINTR)
            break;
        for (int k = 0; k < n; k++) {
            if (!pfd[k].revents)
                continue;
            if (map[k] == -1) {
                accept_clients(s);
            } else if (map[k] == -2) {
                uint64_t v;
                ssize_t rr = read(s->efd, &v, sizeof v);
                (void)rr;
            } else {
                client_t *c = &s->cl[map[k]];
                if (pfd[k].revents & (POLLERR | POLLHUP | POLLNVAL))
                    c->kill = true;
                if (!c->kill && (pfd[k].revents & POLLIN))
                    client_read(s, c);
                if (!c->kill && (pfd[k].revents & POLLOUT))
                    client_write(s, c);
            }
        }
        /* Intento de escritura oportunista (mensajes recién encolados) y
         * cierre de los clientes marcados. */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &s->cl[i];
            if (!c->used)
                continue;
            if (!c->kill)
                client_write(s, c);
            pthread_mutex_lock(&s->lock);
            bool k = c->kill;
            pthread_mutex_unlock(&s->lock);
            if (k)
                client_close(s, c);
        }
    }
}
