/*
 * psi.h — Tablas PSI/SI de DVB: PAT, PMT (ISO/IEC 13818-1 §2.4.4) y SDT, NIT
 * (ETSI EN 300 468), más el texto de DVB (Anexo A de EN 300 468) a UTF-8.
 *
 * Un TS no dice por sí solo qué contiene: lo describen unas «secciones» que viajan
 * en PIDs fijos o anunciados por otras tablas:
 *
 *   PAT  PID 0       lista programa (service_id) → PID de su PMT
 *   PMT  PID anunciado en la PAT, uno por servicio: PID de PCR y de cada flujo
 *        elemental con su stream_type (0x1B H.264, 0x02 MPEG-2, 0x24 HEVC,
 *        0x03/0x04 audio MPEG, 0x0F/0x11 AAC, 0x06 privado: AC-3/E-AC-3/subtítulos
 *        según descriptor 0x6A/0x7A/0x59/0x56) y el idioma (descriptor 0x0A)
 *   SDT  PID 0x11    nombre y proveedor del servicio (descriptor 0x48)
 *   NIT  PID 0x10    nombre de red, frecuencias y numeración lógica de canal
 *                    (LCN, descriptor privado 0x83 de EACEM usado por la TDT)
 *
 * Las secciones llevan CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, sin
 * reflexión ni xor final): calculado sobre la sección completa, incluido su CRC,
 * da 0. Una sección con CRC malo se descarta (el RS deja pasar algún paquete
 * dañado y no se debe pintar «La 1» con basura).
 *
 * psi_t no es seguro entre hilos: lo usa un solo hilo (tv.c, bajo su cerrojo).
 */
#ifndef BLADERF_LAB_PSI_H
#define BLADERF_LAB_PSI_H

#include "ts.h"

typedef enum {
    CODEC_NONE = 0,
    CODEC_H264 = 1, /* los tres primeros coinciden con el byte [2] del binario tipo 5 */
    CODEC_MPEG2V = 2,
    CODEC_HEVC = 3,
    CODEC_MP2 = 4, /* audio MPEG-1/2 (capa I/II/III en stream_type 0x03/0x04) */
    CODEC_AAC = 5, /* ADTS (0x0F) o LATM (0x11) */
    CODEC_AC3 = 6,
    CODEC_EAC3 = 7,
    CODEC_SUB = 8,  /* subtítulos DVB */
    CODEC_TXT = 9,  /* teletexto */
    CODEC_OTHER = 10
} es_codec_t;

typedef enum { ES_OTHER = 0, ES_VIDEO = 1, ES_AUDIO = 2 } es_kind_t;

typedef struct {
    int pid;
    int stream_type;
    es_kind_t kind;
    es_codec_t codec;
    char lang[4];
    int w, h; /* los rellena tv.c al ver el SPS (0 = desconocido) */
} psi_es_t;

#define PSI_MAX_ES 16
#define PSI_MAX_SVC 40

typedef struct {
    int sid;
    int pmt_pid;     /* 0 = aún sin PAT */
    int pcr_pid;
    int lcn;         /* número lógico de canal (NIT), 0 = desconocido */
    int svc_type;    /* service_type del SDT: 1 TV, 2 radio, 0x16/0x19 TV H.264 SD/HD… (0 = desconocido) */
    bool ca;         /* alguna señal de acceso condicional en la PMT (cifrado) */
    char name[96], provider[96]; /* UTF-8 */
    int nes;
    psi_es_t es[PSI_MAX_ES];
    bool have_pmt, have_sdt;
} psi_service_t;

typedef struct psi psi_t;

psi_t *psi_create(void);
void psi_destroy(psi_t *p);
void psi_reset(psi_t *p);
/* ¿Hay que reensamblar secciones de este PID? */
bool psi_wants_pid(const psi_t *p, int pid);
/* Entrega un paquete (de un PID para el que psi_wants_pid es true). */
void psi_feed(psi_t *p, const ts_pkt_t *k);

int psi_nservices(const psi_t *p);
const psi_service_t *psi_service(const psi_t *p, int i);
const psi_service_t *psi_find(const psi_t *p, int sid);
psi_service_t *psi_find_mut(psi_t *p, int sid); /* para que tv.c anote w/h */
/* Contador que sube cada vez que cambia algo visible (nombres, flujos, LCN…). */
unsigned psi_generation(const psi_t *p);
int psi_ts_id(const psi_t *p);
const char *psi_network_name(const psi_t *p);
/* Frecuencias anunciadas por la NIT (Hz, 0 = ninguna) en nit_freq(i) */
int psi_nit_freqs(const psi_t *p, int64_t *out, int cap);

uint32_t crc32_mpeg2(const uint8_t *d, size_t n);

/* Texto DVB (EN 300 468 Anexo A) → UTF-8. Devuelve la longitud escrita (sin el NUL).
 * Juegos: ISO 6937 por omisión, ISO 8859-1…15 (bytes 0x01–0x0B y 0x10 xx xx), UCS-2 (0x11)
 * y UTF-8 (0x15). Descarta los códigos de control 0x80–0x9F (0x8A = salto → espacio). */
size_t dvb_text_to_utf8(const uint8_t *s, size_t len, char *out, size_t cap);

#endif
