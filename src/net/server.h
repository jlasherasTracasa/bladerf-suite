/*
 * server.h — Servidor HTTP/1.1 + WebSocket (RFC 6455) mínimo y propio.
 *
 * Funciona en un único hilo con un bucle poll(): todas las conexiones son no
 * bloqueantes. Otros hilos (el motor DSP) pueden encolar mensajes para los
 * clientes con server_send_*(): son funciones seguras entre hilos que añaden
 * el mensaje a la cola del cliente y despiertan al bucle con un eventfd.
 *
 * Rutas HTTP:
 *   GET /                 → web/index.html
 *   GET /<fichero>        → ficheros estáticos de web/
 *   GET /ws               → actualización a WebSocket (control + datos)
 *   GET /api/status       → estado en JSON (para scripts y comprobaciones)
 *   GET /api/recordings   → lista de grabaciones
 *   GET /recordings/<f>   → descarga de una grabación
 *
 * Seguridad:
 *   - Por defecto escucha solo en 127.0.0.1 (usar --host 0.0.0.0 para la LAN).
 *   - Las rutas se sanean: nada de "..", ni rutas absolutas, ni ocultos.
 *   - El WebSocket exige que la cabecera Origin coincida con Host: así una web
 *     cualquiera abierta en el navegador no puede manejar la radio.
 *   - Cabeceras CSP y nosniff en todas las respuestas.
 *
 * Control de flujo: si un cliente lento acumula más de ~2 MB pendientes, se
 * descartan sus tramas "descartables" (espectro, audio) en lugar de crecer sin
 * límite. Los mensajes de control nunca se descartan.
 */
#ifndef BLADERF_LAB_SERVER_H
#define BLADERF_LAB_SERVER_H

#include "../util/strbuf.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct server server_t;

typedef struct {
    void (*on_open)(void *app, int client);
    void (*on_text)(void *app, int client, const char *msg, size_t len);
    void (*on_binary)(void *app, int client, const void *data, size_t len);
    void (*on_close)(void *app, int client);
    /* Rellena body con JSON de estado (GET /api/status). */
    void (*on_status)(void *app, sb_t *body);
} server_callbacks_t;

server_t *server_create(const char *host, int port, const char *webroot, const char *recdir,
                        const server_callbacks_t *cb, void *app, char *err, size_t errlen);
/* Ejecuta el bucle de eventos hasta server_stop(). */
void server_run(server_t *s);
void server_stop(server_t *s);
void server_destroy(server_t *s);

/* Envío seguro entre hilos. client = −1 envía a todos.
 * droppable: puede descartarse si el cliente va retrasado. */
void server_send_text(server_t *s, int client, const char *text, size_t len);
void server_send_binary(server_t *s, int client, const void *data, size_t len, bool droppable);
/* Cierra la conexión de un cliente (seguro entre hilos). */
void server_kick(server_t *s, int client);
int server_client_count(server_t *s);
/* Bytes enviados desde el arranque (para estadísticas). */
unsigned long long server_bytes_sent(server_t *s);

#endif
