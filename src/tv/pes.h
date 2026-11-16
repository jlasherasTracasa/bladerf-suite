/*
 * pes.h — Reensamblado de paquetes PES (ISO/IEC 13818-1 §2.4.3.6–7) y unidades
 * de acceso de vídeo H.264 en Annex B.
 *
 * Los flujos elementales (vídeo, audio) viajan troceados en paquetes TS dentro de
 * «paquetes PES»:
 *
 *   00 00 01 stream_id  PES_packet_length(16)  '10' flags(2) PTS_DTS_flags(2)…
 *   PES_header_data_length  [PTS(5 B)] [DTS(5 B)] …  datos del flujo elemental
 *
 * El PTS/DTS son 33 bits a 90 kHz repartidos en 5 bytes con bits marcadores:
 *   '0010'|'0011'|'0001'  PTS[32..30] 1  PTS[29..15] 1  PTS[14..0] 1
 * PES_packet_length = 0 (permitido solo en vídeo) significa «hasta el siguiente
 * PUSI»; por eso un PES de vídeo se da por completo al llegar el siguiente.
 *
 * Unidad de acceso (AU) H.264: todas las NAL de una imagen. En Annex B las NAL van
 * precedidas de 00 00 01 (o 00 00 00 01). La TDT emite un AU por PES, que empieza
 * con una NAL tipo 9 (access unit delimiter, AUD). h264_au_t corta por AUD y, si no
 * hay AUD, por inicio de PES con PTS. Una AU es clave si contiene una NAL tipo 5
 * (IDR) o un SEI de punto de recuperación (tipo 6, payloadType 6).
 */
#ifndef BLADERF_LAB_PES_H
#define BLADERF_LAB_PES_H

#include "ts.h"

#define PTS_NONE INT64_MIN

/* Un PES completo: es/len = datos del flujo elemental. */
typedef void (*pes_fn)(void *ctx, const uint8_t *es, size_t len, int64_t pts, int64_t dts);

typedef struct pes_asm pes_asm_t;
pes_asm_t *pes_create(pes_fn fn, void *ctx);
void pes_destroy(pes_asm_t *p);
void pes_reset(pes_asm_t *p);
void pes_feed(pes_asm_t *p, const ts_pkt_t *k);
unsigned long pes_dropped(const pes_asm_t *p); /* PES descartados por errores de continuidad */

/* PTS/DTS de 5 bytes → ticks de 90 kHz (o −1 si los marcadores no son válidos). */
int64_t pes_read_ts(const uint8_t *p);

/* ---------------- H.264 ---------------- */
typedef struct {
    const uint8_t *data;
    size_t len;
    int64_t pts, dts; /* PTS_NONE si la AU no empezó en un PES con PTS */
    bool key;
    bool has_sps;     /* lleva SPS (NAL 7) y PPS (NAL 8) */
} h264_au_t;

typedef void (*h264_au_fn)(void *ctx, const h264_au_t *au);
typedef struct h264_split h264_split_t;
h264_split_t *h264_split_create(h264_au_fn fn, void *ctx);
void h264_split_destroy(h264_split_t *s);
void h264_split_reset(h264_split_t *s);
/* Un PES de vídeo H.264; emite las AU que quedan completas (la última queda
 * pendiente hasta el siguiente PES: así se sabe dónde acaba). */
void h264_split_feed(h264_split_t *s, const uint8_t *es, size_t len, int64_t pts, int64_t dts);
void h264_split_flush(h264_split_t *s);

/* Busca la NAL del tipo dado en un buffer Annex B: devuelve puntero al primer byte
 * de la NAL (el de cabecera) y su longitud, o NULL. */
const uint8_t *annexb_find_nal(const uint8_t *d, size_t len, int type, size_t *nal_len);
/* Anchura/altura (con recorte) desde un SPS (NAL con su byte de cabecera). 0 si falla. */
int h264_sps_size(const uint8_t *nal, size_t len, int *w, int *h);

#endif
