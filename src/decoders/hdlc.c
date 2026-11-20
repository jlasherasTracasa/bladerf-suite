/*
 * hdlc.c — Ver hdlc.h. Desentramado HDLC con FCS CRC-16/X-25 sobre bits.
 */
#include "hdlc.h"

#include "../dsp/bits.h"
#include "../dsp/crc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static crc_t g_crc;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void x25_build(void) { crc_init(&g_crc, crc_preset("CRC-16/X-25")); }

/* Tabla del CRC compartida entre hilos: se construye una sola vez. */
static const crc_t *x25(void)
{
    pthread_once(&g_once, x25_build);
    return &g_crc;
}

int hdlc_rx_init(hdlc_rx_t *h, int cap_bits)
{
    memset(h, 0, sizeof *h);
    h->bits = malloc((size_t)cap_bits);
    if (!h->bits)
        return -1;
    h->cap = cap_bits;
    (void)x25();
    return 0;
}

void hdlc_rx_free(hdlc_rx_t *h)
{
    free(h->bits);
    memset(h, 0, sizeof *h);
}

void hdlc_rx_reset(hdlc_rx_t *h)
{
    h->n = 0;
    h->ones = 0;
    h->inframe = false;
}

/* Cierra la trama en curso al llegar una bandera. n ya excluye la bandera. */
static int close_frame(hdlc_rx_t *h)
{
    int n = h->n, ret = 0;
    if (h->inframe && n >= 24) {              /* ≥ 8 bits de carga + 16 de FCS */
        int nb = n - 16;
        uint32_t fcs = 0;
        for (int i = 0; i < 16; i++)
            fcs |= (uint32_t)(h->bits[nb + i] & 1) << i;
        if (crc_compute_bits(x25(), h->bits, (size_t)nb) == fcs) {
            h->frames_ok++;
            ret = nb;
        } else {
            h->frames_bad++;
        }
    }
    return ret;
}

int hdlc_rx_push_bit(hdlc_rx_t *h, int bit)
{
    if (bit) {
        h->ones++;
        if (h->inframe) {
            if (h->ones >= 7) {            /* aborto: 7 unos seguidos */
                h->inframe = false;
                h->n = 0;
            } else if (h->n < h->cap) {
                h->bits[h->n++] = 1;
            } else {
                h->inframe = false;        /* desbordamiento: trama demasiado larga */
                h->n = 0;
            }
        }
        return 0;
    }
    int ones = h->ones, ret = 0;
    h->ones = 0;
    if (ones == 6) {                       /* bandera 0 111111 0 */
        if (h->inframe && h->n >= 7)
            h->n -= 7;                     /* quita el 0 inicial de la bandera y los seis unos */
        else
            h->n = 0;
        ret = close_frame(h);
        h->last_start = h->frame_start;
        h->last_end = h->pos;
        h->frame_start = h->pos;
        h->inframe = true;                 /* la bandera de cierre abre la siguiente */
        h->n = 0;
        return ret;
    }
    if (ones == 5)                         /* cero insertado: se descarta */
        return 0;
    if (h->inframe) {
        if (h->n < h->cap) {
            h->bits[h->n++] = 0;
        } else {
            h->inframe = false;
            h->n = 0;
        }
    }
    return 0;
}

int hdlc_rx_push_level(hdlc_rx_t *h, int level)
{
    level &= 1;
    int bit = (level == h->prev_level) ? 1 : 0;
    h->prev_level = level;
    return hdlc_rx_push_bit(h, bit);
}

void hdlc_rx_bytes(const hdlc_rx_t *h, int nbits, uint8_t *out)
{
    bits_pack_lsb(h->bits, nbits, out);
}

int hdlc_bytes_to_bits(const uint8_t *bytes, int n, uint8_t *bits)
{
    bits_unpack_lsb(bytes, n * 8, bits);
    return n * 8;
}

int hdlc_encode(const uint8_t *payload_bits, int nbits, int pre_flags, int post_flags, uint8_t *out, int cap)
{
    static const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    uint32_t fcs = crc_compute_bits(x25(), payload_bits, (size_t)nbits);
    int o = 0, ones = 0;
#define PUT(b)                                                                           \
    do {                                                                                 \
        if (o >= cap)                                                                    \
            return -1;                                                                   \
        out[o++] = (uint8_t)(b);                                                         \
    } while (0)
    for (int f = 0; f < pre_flags; f++)
        for (int i = 0; i < 8; i++)
            PUT(flag[i]);
    for (int i = 0; i < nbits + 16; i++) {
        int b = i < nbits ? payload_bits[i] & 1 : (int)((fcs >> (i - nbits)) & 1);
        PUT(b);
        ones = b ? ones + 1 : 0;
        if (ones == 5) {
            PUT(0);
            ones = 0;
        }
    }
    for (int f = 0; f < post_flags; f++)
        for (int i = 0; i < 8; i++)
            PUT(flag[i]);
#undef PUT
    return o;
}
