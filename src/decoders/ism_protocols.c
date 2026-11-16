/*
 * ism_protocols.c — Protocolos de sensores ISM sobre trenes de pulsos, analizador
 * genérico tipo URH y codificadores meteorológicos. Ver ism_protocols.h y
 * docs/decoders/ism.md (allí se detalla qué constantes están verificadas).
 *
 * Codificaciones de línea que aparecen:
 *   PWM  la anchura de la marca (o del espacio) lleva el bit: corta = 0, larga = 1.
 *   PPM  la marca es fija y el espacio entre marcas lleva el bit.
 *   Manchester  cada bit son dos medios bits de nivel opuesto; el sentido de la
 *        transición (alto→bajo o bajo→alto) es el valor. El flanco central
 *        asegura reloj, por eso la duración de un nivel es 1 o 2 medios bits.
 *
 * Todos los decodificadores trabajan con tolerancias amplias en las duraciones
 * (los relojes baratos de los sensores varían ±5 % y el muestreo a 250 kHz
 * cuantiza a 4 µs) y confían en la suma/CRC para descartar falsos positivos.
 */
#include "ism_protocols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================= */
/*  Utilidades                                                             */
/* ======================================================================= */
void ism_train_clear(ism_train_t *t) { t->n = 0; }

static void push_d(ism_train_t *t, uint32_t v)
{
    if (t->n < ISM_MAXD)
        t->d[t->n++] = v;
}

void ism_train_pair(ism_train_t *t, uint32_t mark_us, uint32_t space_us)
{
    if (t->n & 1)   /* debe acabar tras un espacio */
        push_d(t, 0);
    push_d(t, mark_us);
    push_d(t, space_us);
}

void ism_train_gap(ism_train_t *t, uint32_t space_us)
{
    if (t->n & 1)
        push_d(t, space_us);
    else if (t->n > 0)
        t->d[t->n - 1] += space_us;
}

void ism_train_manch(ism_train_t *t, const uint8_t *bits, int nbits, uint32_t half_us, int inv)
{
    if (t->n & 1)
        push_d(t, 0);
    int level = 1, run = 0; /* nivel actual (1 = marca) y medios bits acumulados */
    bool first = true;
    for (int i = 0; i < nbits * 2; i++) {
        int b = bits[i >> 1] ^ inv;
        int lv = (i & 1) ? !b : b;   /* bit 1: alto→bajo */
        if (first) {
            if (lv == 0)
                push_d(t, 0);        /* empieza en bajo: marca de longitud 0 */
            level = lv;
            run = 1;
            first = false;
        } else if (lv == level) {
            run++;
        } else {
            push_d(t, (uint32_t)run * half_us);
            level = lv;
            run = 1;
        }
    }
    if (run)
        push_d(t, (uint32_t)run * half_us);
}

int ism_put_bits(uint8_t *bits, int pos, uint32_t v, int nb)
{
    for (int i = 0; i < nb; i++)
        bits[pos + i] = (uint8_t)((v >> (nb - 1 - i)) & 1);
    return pos + nb;
}

static uint32_t get_bits(const uint8_t *b, int pos, int nb)
{
    uint32_t v = 0;
    for (int i = 0; i < nb; i++)
        v = (v << 1) | (b[pos + i] & 1);
    return v;
}

static void pack_bytes(const uint8_t *bits, int nbits, uint8_t *out)
{
    memset(out, 0, (size_t)(nbits + 7) / 8);
    for (int i = 0; i < nbits; i++)
        out[i >> 3] |= (uint8_t)((bits[i] & 1) << (7 - (i & 7)));
}

/* CRC-8 genérico MSB primero (poly sin el x^8). */
uint8_t ism_crc8(const uint8_t *p, int n, uint8_t poly, uint8_t init)
{
    uint8_t c = init;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (uint8_t)((c & 0x80) ? (c << 1) ^ poly : c << 1);
    }
    return c;
}

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), usado por la trama de prueba. */
static uint16_t crc16_ccitt(const uint8_t *p, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++)
            c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

/* Resumen de "LFSR digest" de 8 bits: por cada bit de dato a 1 se acumula (XOR) la
 * clave; después la clave se desplaza a la derecha y, si salió un 1, se aplica
 * el generador. Es el tipo de suma que usan algunos sensores de Ambient/LaCrosse. */
uint8_t ism_lfsr_digest8(const uint8_t *p, int n, uint8_t gen, uint8_t key)
{
    uint8_t sum = 0;
    for (int k = 0; k < n; k++)
        for (int i = 7; i >= 0; i--) {
            if ((p[k] >> i) & 1)
                sum ^= key;
            key = (uint8_t)((key & 1) ? (key >> 1) ^ gen : key >> 1);
        }
    return sum;
}

static void hex_of(char *dst, size_t cap, const uint8_t *b, int n)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 3 < cap; i++)
        o += (size_t)snprintf(dst + o, cap - o, "%02X", b[i]);
    if (o < cap)
        dst[o] = 0;
}

typedef struct {
    int a, b;   /* índices de duración [a, b) */
} row_t;

/* Parte el tren en filas en los espacios ≥ gap_us. */
static int split_rows(const ism_train_t *t, uint32_t gap_us, row_t *rows, int max)
{
    int nr = 0, a = 0;
    for (int i = 1; i < t->n && nr < max; i += 2)
        if (t->d[i] >= gap_us) {
            if (i - a >= 2)
                rows[nr++] = (row_t){a, i};
            a = i + 1;
        }
    if (a < t->n && t->n - a >= 2 && nr < max)
        rows[nr++] = (row_t){a, t->n};
    return nr;
}

/* Bits PWM/PPM de una fila: por marca (by_mark) o por espacio. Se saltan al
 * principio los pares cuya marca supera skip_over (pulsos de sincronía). */
static int row_bits(const ism_train_t *t, row_t r, bool by_mark, uint32_t th, uint32_t skip_over,
                    uint8_t *bits, int max)
{
    int i = r.a, n = 0;
    while (skip_over && i + 1 < r.b && t->d[i] > skip_over)
        i += 2;
    /* Por marca, la última marca de la fila puede no tener espacio propio (el
     * espacio final es el hueco que separa filas): también cuenta. */
    for (; i < r.b && n < max; i += 2) {
        if (!by_mark && i + 1 >= r.b)
            break;
        bits[n++] = (uint8_t)((by_mark ? t->d[i] : t->d[i + 1]) > th);
    }
    return n;
}

/* Nº de pulsos de sincronía (marcas > over) al principio de la fila. */
static int sync_count(const ism_train_t *t, row_t r, uint32_t over)
{
    int c = 0;
    for (int i = r.a; i + 1 < r.b && t->d[i] > over; i += 2)
        c++;
    return c;
}

static bool all_zero(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* Medios bits de un segmento Manchester a partir del índice *s. */
#define MAXHB 4096
static int halfbits(const ism_train_t *t, int *s, double half_us, uint8_t *hb)
{
    int n = 0, i = *s;
    bool closed = false;   /* el segmento terminó en un espacio largo (o fin de tren) */
    for (; i < t->n; i++) {
        double d = t->d[i];
        if (d > 2.6 * half_us) {   /* silencio / separación entre paquetes */
            closed = (i & 1) == 1;
            i++;
            break;
        }
        int k = (int)(d / half_us + 0.5);
        for (int j = 0; j < k && n < MAXHB; j++)
            hb[n++] = (uint8_t)((i & 1) == 0);
    }
    if (i >= t->n && t->n > 0 && ((t->n - 1) & 1) == 0)
        closed = true;
    /* El último espacio del paquete se recorta (ráfaga o hueco largo): si el último
     * bit era un 1 (alto→bajo) su medio bit bajo queda dentro de ese hueco. */
    if (closed && (n & 1) && n < MAXHB)
        hb[n++] = 0;
    *s = i;
    return n;
}

#define MAXMB 2048
typedef struct {
    uint8_t b[2][2][MAXMB];   /* [alineación][polaridad] */
    int n[2][2];
} mstream_t;

static void manch_streams(const uint8_t *hb, int nhb, mstream_t *m)
{
    for (int al = 0; al < 2; al++)
        for (int pol = 0; pol < 2; pol++) {
            int n = 0;
            for (int p = al; p + 1 < nhb && n < MAXMB; p += 2) {
                if (hb[p] == hb[p + 1])
                    break;
                m->b[al][pol][n++] = (uint8_t)(hb[p] ^ pol);
            }
            m->n[al][pol] = n;
        }
}

static int find_pat(const uint8_t *b, int n, uint32_t pat, int plen, int from)
{
    for (int i = from; i + plen <= n; i++)
        if (get_bits(b, i, plen) == pat)
            return i;
    return -1;
}

static int msg_add(ism_msg_t *out, int *cnt, int max)
{
    if (*cnt >= max)
        return -1;
    memset(&out[*cnt], 0, sizeof out[*cnt]);
    return (*cnt)++;
}

/* ======================================================================= */
/*  Oregon Scientific v2.1 / v3 (OOK, Manchester a 1024 Hz)                */
/* ======================================================================= */
#define OREGON_HALF_US 488.0
static const char *oregon_model(uint32_t id, int *nnib, bool *has_hum)
{
    switch (id) {
    case 0x1D20: *nnib = 17;   /* real: 17 nibbles (nib 14 = 0, suma en 15-16) */
         *has_hum = true; return "Oregon-THGR122N";
    case 0xEC40: *nnib = 14; *has_hum = false; return "Oregon-THN132N";
    case 0xF824: *nnib = 17; *has_hum = true; return "Oregon-THGR810";
    default: return NULL;
    }
}

static int oregon_try_plain(const uint8_t *b, int n, ism_msg_t *out, int *cnt, int max)
{
    int run = 0;
    for (int j = 0; j + 8 <= n; j++) {
        if (b[j]) {
            run++;
            continue;
        }
        bool sync = run >= 10 && j + 4 <= n && b[j + 1] == 1 && b[j + 2] == 0 && b[j + 3] == 1;
        run = 0;
        if (!sync)
            continue;
        int p = j + 4;
        uint8_t nib[20];
        int nn = 0;
        while (nn < 20 && p + 4 <= n) {
            nib[nn++] = (uint8_t)(b[p] | (b[p + 1] << 1) | (b[p + 2] << 2) | (b[p + 3] << 3));
            p += 4;
        }
        if (nn < 14)
            continue;
        uint32_t id = (uint32_t)(nib[0] << 12 | nib[1] << 8 | nib[2] << 4 | nib[3]);
        int nnib;
        bool hum;
        const char *model = oregon_model(id, &nnib, &hum);
        if (!model || nn < nnib)
            continue;
        int sum = 0, nsum = nnib - 2;
        for (int i = 0; i < nsum; i++)
            sum += nib[i];
        if ((sum & 0xFF) != (nib[nnib - 2] | (nib[nnib - 1] << 4)))
            continue;
        int ch = nib[4] == 4 ? 3 : nib[4];
        if (nib[4] != 1 && nib[4] != 2 && nib[4] != 4)
            continue;
        int k = msg_add(out, cnt, max);
        if (k < 0)
            return 0;
        ism_msg_t *m = &out[k];
        m->model = model;
        m->id = (uint32_t)(nib[5] | (nib[6] << 4));
        m->channel = ch;
        m->has = ISM_HAS_TEMP | ISM_HAS_BATT;
        m->batt_low = (nib[7] & 4) != 0;
        double tt = nib[10] * 10 + nib[9] + nib[8] * 0.1;
        m->temp_c = (nib[11] & 8) ? -tt : tt;
        if (hum) {
            m->has |= ISM_HAS_HUM;
            m->humidity = nib[13] * 10 + nib[12];
        }
        uint8_t by[10] = {0};
        for (int i = 0; i < (nnib + 1) / 2; i++)
            by[i] = (uint8_t)(nib[2 * i] << 4 | (2 * i + 1 < nnib ? nib[2 * i + 1] : 0));
        hex_of(m->hex, sizeof m->hex, by, (nnib + 1) / 2);
        return 1;
    }
    return 0;
}

/* Oregon v2.1 (THN132N, THGR122N…): contrastado con la grabación pública
 * oregon_scientific/THN132N de rtl_433_tests. Cada bit del mensaje se envía como
 * DOS bits Manchester complementarios (b, ¬b): el preámbulo de 16 unos lógicos
 * aparece en el aire como 1010…10 (rachas de 976 µs) y los datos como pares
 * 10/01. Este envoltorio reconstruye el flujo lógico (primer bit de cada par,
 * en las dos fases) y reutiliza la búsqueda de sincronía/nibbles. La versión 3
 * (THGR810, preámbulo de 24 unos) va en Manchester simple y se prueba primero. */
static int oregon_try(const uint8_t *b, int n, ism_msg_t *out, int *cnt, int max)
{
    if (oregon_try_plain(b, n, out, cnt, max))
        return 1;
    static uint8_t lg[MAXMB];
    for (int ph = 0; ph < 2; ph++) {
        int m = 0;
        for (int i = ph; i + 1 < n && m < MAXMB; i += 2) {
            if (b[i] == b[i + 1])
                break;
            lg[m++] = b[i];
        }
        if (m > 24 && oregon_try_plain(lg, m, out, cnt, max))
            return 1;
    }
    return 0;
}

/* ======================================================================= */
/*  Ambient Weather F007TH (OOK, Manchester a 1000 bit/s)                  */
/* ======================================================================= */
static int f007_try(const uint8_t *b, int n, ism_msg_t *out, int *cnt, int max)
{
    int p = find_pat(b, n, 0xFFD45, 20, 0);
    while (p >= 0) {
        if (p + 12 + 48 <= n) {
            uint8_t by[6];
            pack_bytes(b + p + 12, 48, by);
            if (by[0] == 0x45 && ((ism_lfsr_digest8(by, 5, 0x98, 0x3E) ^ 0x64) == by[5])) {
                int raw = ((by[2] & 0x0F) << 8) | by[3];
                int k = msg_add(out, cnt, max);
                if (k < 0)
                    return 0;
                ism_msg_t *m = &out[k];
                m->model = "Ambient-F007TH";
                m->id = by[1];
                m->channel = ((by[2] >> 4) & 7) + 1;
                m->has = ISM_HAS_TEMP | ISM_HAS_HUM | ISM_HAS_BATT;
                m->temp_c = ((raw - 400) / 10.0 - 32.0) / 1.8;
                m->humidity = by[4];
                m->batt_low = (by[2] & 0x80) != 0;
                hex_of(m->hex, sizeof m->hex, by, 6);
                return 1;
            }
        }
        p = find_pat(b, n, 0xFFD45, 20, p + 1);
    }
    return 0;
}

/* ======================================================================= */
/*  TPMS en solo lectura (FSK Manchester y OOK Manchester)                 */
/* ======================================================================= */
/* FSK: sincronía 0x2DD4, id(4) presión temp estado suma8. Presión = 2,5 kPa/LSB,
 * temperatura = LSB − 50 °C. Variante genérica: NO corresponde a un fabricante
 * concreto verificado. */
static int tpms_fsk_try(const uint8_t *b, int n, ism_msg_t *out, int *cnt, int max)
{
    int p = find_pat(b, n, 0x2DD4, 16, 0);
    while (p >= 0) {
        if (p + 16 + 64 <= n) {
            uint8_t by[8];
            pack_bytes(b + p + 16, 64, by);
            uint8_t s = 0;
            for (int i = 0; i < 7; i++)
                s = (uint8_t)(s + by[i]);
            if (s == by[7]) {
                int k = msg_add(out, cnt, max);
                if (k < 0)
                    return 0;
                ism_msg_t *m = &out[k];
                m->model = "TPMS-FSK";
                m->id = (uint32_t)by[0] << 24 | (uint32_t)by[1] << 16 | (uint32_t)by[2] << 8 | by[3];
                m->has = ISM_HAS_PRESS | ISM_HAS_TEMP | ISM_HAS_BATT;
                m->pressure_kpa = by[4] * 2.5;
                m->temp_c = by[5] - 50.0;
                m->batt_low = (by[6] & 1) != 0;
                hex_of(m->hex, sizeof m->hex, by, 8);
                return 1;
            }
        }
        p = find_pat(b, n, 0x2DD4, 16, p + 1);
    }
    return 0;
}

/* OOK: sincronía 0xA5, id(3) presión temp CRC-8(poly 0x07, init 0). */
static int tpms_ook_try(const uint8_t *b, int n, ism_msg_t *out, int *cnt, int max)
{
    int p = find_pat(b, n, 0xA5, 8, 0);
    while (p >= 0) {
        if (p + 8 + 48 <= n) {
            uint8_t by[6];
            pack_bytes(b + p + 8, 48, by);
            if (ism_crc8(by, 5, 0x07, 0) == by[5]) {
                int k = msg_add(out, cnt, max);
                if (k < 0)
                    return 0;
                ism_msg_t *m = &out[k];
                m->model = "TPMS-OOK";
                m->id = (uint32_t)by[0] << 16 | (uint32_t)by[1] << 8 | by[2];
                m->has = ISM_HAS_PRESS | ISM_HAS_TEMP;
                m->pressure_kpa = by[3] * 2.5;
                m->temp_c = by[4] - 50.0;
                hex_of(m->hex, sizeof m->hex, by, 6);
                return 1;
            }
        }
        p = find_pat(b, n, 0xA5, 8, p + 1);
    }
    return 0;
}

#define OOK_MANCH_HALF_F007 500.0
#define TPMS_FSK_HALF 60.0
#define TPMS_OOK_HALF 120.0

typedef int (*manch_try_fn)(const uint8_t *b, int n, ism_msg_t *out, int *cnt, int max);

/* Recorre todos los segmentos Manchester del tren con medio bit 'half' y
 * prueba 'fn' sobre las cuatro combinaciones alineación×polaridad. */
static int manch_scan(const ism_train_t *t, double half, manch_try_fn fn, ism_msg_t *out, int *cnt, int max)
{
    static uint8_t hb[MAXHB];   /* uso exclusivo desde el hilo del decodificador */
    static mstream_t ms;
    int s = 0, found = 0;
    while (s < t->n) {
        int nh = halfbits(t, &s, half, hb);
        if (nh < 40)
            continue;
        manch_streams(hb, nh, &ms);
        for (int al = 0; al < 2; al++)
            for (int pol = 0; pol < 2; pol++)
                if (ms.n[al][pol] > 24)
                    found += fn(ms.b[al][pol], ms.n[al][pol], out, cnt, max);
        if (found)
            break;
    }
    return found;
}

/* ======================================================================= */
/*  Sensores PWM/PPM                                                       */
/* ======================================================================= */
/* Nexus / Rubicson (OOK PPM): marca ~500 µs; espacio 1000 µs = 0, 2000 µs = 1;
 * filas de 36 bits separadas por ~4 ms, repetidas hasta 12 veces. Campos:
 * id(8) batería(1: 1 = bien) 0 canal(2) temp(12, con signo, 0,1 °C) 0xF hum(8).
 * No lleva suma: se exige que dos filas sean idénticas. */
static int dec_nexus(const ism_train_t *t, ism_msg_t *out, int *cnt, int max)
{
    row_t rows[40];
    int nr = split_rows(t, 3000, rows, 40);
    uint8_t bits[40][40];
    int nb[40];
    for (int r = 0; r < nr; r++)
        nb[r] = row_bits(t, rows[r], false, 1500, 0, bits[r], 40);
    for (int i = 0; i < nr; i++) {
        if (nb[i] != 36 || get_bits(bits[i], 24, 4) != 0xF)
            continue;
        for (int j = i + 1; j < nr; j++) {
            if (nb[j] != 36 || memcmp(bits[i], bits[j], 36))
                continue;
            int raw = (int)get_bits(bits[i], 12, 12);
            if (raw & 0x800)
                raw -= 4096;
            int k = msg_add(out, cnt, max);
            if (k < 0)
                return 0;
            ism_msg_t *m = &out[k];
            m->model = "Nexus-TH";
            m->id = get_bits(bits[i], 0, 8);
            m->channel = (int)get_bits(bits[i], 10, 2) + 1;
            m->has = ISM_HAS_TEMP | ISM_HAS_HUM | ISM_HAS_BATT;
            m->batt_low = get_bits(bits[i], 8, 1) == 0;
            m->temp_c = raw / 10.0;
            m->humidity = get_bits(bits[i], 28, 8);
            uint8_t by[5];
            pack_bytes(bits[i], 36, by);
            hex_of(m->hex, sizeof m->hex, by, 5);
            return 1;
        }
    }
    return 0;
}

/* LaCrosse TX141-B (v2 de 37 bits y v3 de 33 bits; OOK PWM). Temporización
 * contrastada con grabaciones públicas de rtl_433_tests (lacrosse/tx141-bv2 y
 * -bv3): 4 pulsos de sincronía de ~960 µs con espacio de ~990 µs y después los
 * bits, con periodo ~740 µs: marca ~236 µs = 0 y marca ~480 µs = 1.
 * id(8) | batería OK(1) test(1) canal(2) temp_hi(4) | temp_lo(8) | resto (12-13
 * bits, resumen de comprobación). Temperatura = (raw − 500)/10 °C.
 * El resumen final NO se ha podido reconstruir (no coincide con ningún CRC-8 ni
 * LFSR simple probado con las dos grabaciones disponibles): se exige que al menos
 * DOS filas de la ráfaga sean idénticas (el sensor repite 3-12 veces). */
#define TX141_MAXROWS 16
static int dec_tx141(const ism_train_t *t, ism_msg_t *out, int *cnt, int max)
{
    static uint8_t rb[TX141_MAXROWS][48];
    int rn[TX141_MAXROWS], nr = 0, sync = 0;
    bool inrow = false;
    for (int i = 0; i < t->n && nr <= TX141_MAXROWS; i += 2) {
        uint32_t m = t->d[i];
        if (m > 700) {
            if (++sync == 3 && nr < TX141_MAXROWS) {
                rn[nr++] = 0;
                inrow = true;
            }
            continue;
        }
        sync = 0;
        if (inrow && rn[nr - 1] < 48)
            rb[nr - 1][rn[nr - 1]++] = (uint8_t)(m > 360);
    }
    for (int a = 0; a < nr; a++) {
        if (rn[a] < 32 || rn[a] > 40)
            continue;
        for (int b = a + 1; b < nr; b++) {
            if (rn[b] != rn[a] || memcmp(rb[a], rb[b], (size_t)rn[a]))
                continue;
            uint8_t by[5];
            pack_bytes(rb[a], rn[a], by);
            if (all_zero(by, 3))
                continue;
            int raw = ((by[1] & 0x0F) << 8) | by[2];
            if (raw < 200 || raw > 1000)   /* −30…+50 °C */
                continue;
            int k = msg_add(out, cnt, max);
            if (k < 0)
                return 0;
            ism_msg_t *m = &out[k];
            m->model = "LaCrosse-TX141B";
            m->id = by[0];
            m->has = ISM_HAS_TEMP | ISM_HAS_BATT;
            m->batt_low = (by[1] & 0x80) == 0;
            m->temp_c = (raw - 500) / 10.0;
            hex_of(m->hex, sizeof m->hex, by, (rn[a] + 7) / 8);
            return 1;
        }
    }
    return 0;
}

static int parity7(uint8_t b) { return __builtin_parity(b & 0x7F); }

/* Acurite 592TXR (OOK PWM): 4 pulsos de sincronía de 600 µs y 56 bits; bit 1 =
 * marca 400/espacio 220 µs, bit 0 = marca 220/espacio 400 µs.
 * b0: canal(2) id_hi(6) | b1: id_lo | b2: bit7 paridad, bit6 batería OK, tipo(6) |
 * b3: paridad + humedad(7) | b4: paridad + temp_hi(4) | b5: paridad + temp_lo(7) |
 * b6: suma de b0..b5 mod 256. Paridad par: bit7 = XOR de los 7 bits bajos.
 * Temperatura = (raw − 1000)/10 °C. */
static int dec_acurite(const ism_train_t *t, ism_msg_t *out, int *cnt, int max)
{
    row_t rows[16];
    int nr = split_rows(t, 1500, rows, 16);
    for (int r = 0; r < nr; r++) {
        uint8_t bits[80], by[7];
        int n = row_bits(t, rows[r], true, 310, 500, bits, 80);
        if (n < 56 || sync_count(t, rows[r], 500) < 3)
            continue;
        pack_bytes(bits, 56, by);
        unsigned s = 0;
        for (int i = 0; i < 6; i++)
            s += by[i];
        if ((s & 0xFF) != by[6])
            continue;
        if (((by[3] >> 7) & 1) != parity7(by[3]) || ((by[4] >> 7) & 1) != parity7(by[4]) ||
            ((by[5] >> 7) & 1) != parity7(by[5]))
            continue;
        int raw = ((by[4] & 0x0F) << 7) | (by[5] & 0x7F);
        int k = msg_add(out, cnt, max);
        if (k < 0)
            return 0;
        ism_msg_t *m = &out[k];
        m->model = "Acurite-592TXR";
        m->id = (uint32_t)((by[0] & 0x3F) << 8) | by[1];
        m->channel = by[0] >> 6;
        m->has = ISM_HAS_TEMP | ISM_HAS_HUM | ISM_HAS_BATT;
        m->batt_low = (by[2] & 0x40) == 0;
        m->temp_c = (raw - 1000) / 10.0;
        m->humidity = by[3] & 0x7F;
        hex_of(m->hex, sizeof m->hex, by, 7);
        return 1;
    }
    return 0;
}

/* Fine Offset WH1080/WH3080 (OOK PWM, 433/868 MHz): preámbulo 0xFF y 10 octetos;
 * espacio fijo de ~1000 µs, marca CORTA de ~500 µs = 1 y marca LARGA de ~1500 µs
 * = 0. Temporización contrastada con las grabaciones públicas de rtl_433_tests
 * (fineoffset_wh1080/01 y /03; ver docs/decoders/ism.md): marcas de 504/1481 µs.
 * b0: 0xA | id_hi(4) | b1: id_lo(4) temp_hi(4) | b2: temp_lo | b3: hum | b4: viento
 * medio (0,34 m/s) | b5: racha | b6,b7: lluvia (0,3 mm) | b8: bit4 batería baja,
 * bits 3..0 dirección (22,5°) | b9: CRC-8 (poly 0x31, init 0) de b0..b8.
 * Temperatura = (raw − 400)/10 °C. */
static int dec_wh1080(const ism_train_t *t, ism_msg_t *out, int *cnt, int max)
{
    row_t rows[16];
    int nr = split_rows(t, 2500, rows, 16);
    for (int r = 0; r < nr; r++) {
        uint8_t bits[128], by[10];
        int n = row_bits(t, rows[r], true, 1000, 0, bits, 128);
        if (n < 88)
            continue;
        for (int i = 0; i < 88; i++)
            bits[i] ^= 1;   /* marca corta = 1 */
        if (get_bits(bits, 0, 8) != 0xFF)
            continue;
        pack_bytes(bits + 8, 80, by);
        if ((by[0] >> 4) != 0xA || all_zero(by, 10) || ism_crc8(by, 9, 0x31, 0) != by[9])
            continue;
        int raw = ((by[1] & 0x0F) << 8) | by[2];
        int k = msg_add(out, cnt, max);
        if (k < 0)
            return 0;
        ism_msg_t *m = &out[k];
        m->model = "FineOffset-WH1080";
        m->id = (uint32_t)((by[0] & 0x0F) << 4) | (by[1] >> 4);
        m->has = ISM_HAS_TEMP | ISM_HAS_HUM | ISM_HAS_WIND | ISM_HAS_RAIN | ISM_HAS_DIR | ISM_HAS_BATT;
        m->temp_c = (raw - 400) / 10.0;
        m->humidity = by[3];
        m->wind_avg_ms = by[4] * 0.34;
        m->wind_gust_ms = by[5] * 0.34;
        m->rain_mm = (((by[6] & 0x0F) << 8) | by[7]) * 0.3;   /* 12 bits; el nibble alto es de estado */
        m->wind_dir_deg = (int)((by[8] & 0x0F) * 22.5);
        m->batt_low = (by[8] & 0x10) != 0;
        hex_of(m->hex, sizeof m->hex, by, 10);
        return 1;
    }
    return 0;
}

/* Trama de prueba de BladeRF Lab (OOK PWM): 3 pulsos de sincronía de 1200 µs,
 * luego 80 bits (marca 300 µs = 0, 600 µs = 1, espacio 300 µs):
 * 0xB1AD | contador(16) | carga(32) | CRC-16/CCITT-FALSE de los 8 octetos. */
static int dec_labtest(const ism_train_t *t, ism_msg_t *out, int *cnt, int max)
{
    row_t rows[16];
    int nr = split_rows(t, 2500, rows, 16);
    for (int r = 0; r < nr; r++) {
        uint8_t bits[128], by[10];
        int n = row_bits(t, rows[r], true, 450, 900, bits, 128);
        if (n < 80 || sync_count(t, rows[r], 900) < 2)
            continue;
        pack_bytes(bits, 80, by);
        if (by[0] != 0xB1 || by[1] != 0xAD || crc16_ccitt(by, 8) != (uint16_t)(by[8] << 8 | by[9]))
            continue;
        int k = msg_add(out, cnt, max);
        if (k < 0)
            return 0;
        ism_msg_t *m = &out[k];
        m->model = "BladeRFLab-test";
        m->id = (uint32_t)(by[2] << 8 | by[3]);
        hex_of(m->hex, sizeof m->hex, by, 10);
        return 1;
    }
    return 0;
}

int ism_decode_train(const ism_train_t *t, bool fsk, ism_msg_t *out, int max)
{
    int cnt = 0;
    if (t->n < 8)
        return 0;
    if (fsk) {
        manch_scan(t, TPMS_FSK_HALF, tpms_fsk_try, out, &cnt, max);
        return cnt;
    }
    if (dec_labtest(t, out, &cnt, max) || dec_tx141(t, out, &cnt, max) || dec_acurite(t, out, &cnt, max) ||
        dec_wh1080(t, out, &cnt, max) || dec_nexus(t, out, &cnt, max))
        return cnt;
    if (manch_scan(t, OREGON_HALF_US, oregon_try, out, &cnt, max) ||
        manch_scan(t, OOK_MANCH_HALF_F007, f007_try, out, &cnt, max) ||
        manch_scan(t, TPMS_OOK_HALF, tpms_ook_try, out, &cnt, max))
        return cnt;
    return 0;
}

/* ======================================================================= */
/*  Analizador genérico (estilo Universal Radio Hacker)                    */
/* ======================================================================= */
/* Agrupa valores 1-D: ordenados, un valor nuevo abre grupo si supera en más de
 * 'tol' (relativo) al centro del grupo actual. Se descartan grupos con pocos
 * miembros (pulsos de sincronía aislados). Devuelve nº de grupos por orden de
 * centro creciente. */
typedef struct {
    double c;
    int cnt;
} cluster_t;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int cluster1d(const uint32_t *v, int n, double tol, cluster_t *cl, int max)
{
    uint32_t *s = malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
    if (!s)
        return 0;
    memcpy(s, v, sizeof(uint32_t) * (size_t)n);
    qsort(s, (size_t)n, sizeof *s, cmp_u32);
    int nc = 0;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        if (nc > 0 && s[i] <= cl[nc - 1].c * (1.0 + tol) + 12) {
            sum += s[i];
            cl[nc - 1].cnt++;
            cl[nc - 1].c = sum / cl[nc - 1].cnt;
        } else if (nc < max) {
            cl[nc].c = s[i];
            cl[nc].cnt = 1;
            sum = s[i];
            nc++;
        }
    }
    free(s);
    return nc;
}

/* Quita los grupos poco poblados (< 6 % del total y < 3). */
static int prune_clusters(cluster_t *cl, int nc, int total)
{
    int o = 0;
    for (int i = 0; i < nc; i++)
        if (cl[i].cnt >= 3 && cl[i].cnt * 100 >= total * 6)
            cl[o++] = cl[i];
    return o;
}

static int nearest(const cluster_t *cl, int nc, uint32_t v, double tol)
{
    for (int i = 0; i < nc; i++)
        if (fabs((double)v - cl[i].c) <= cl[i].c * tol + 12)
            return i;
    return -1;
}

static void bits_to_hex(const uint8_t *bits, int n, char *dst, size_t cap)
{
    uint8_t by[64] = {0};
    if (n > 512)
        n = 512;
    pack_bytes(bits, n, by);
    hex_of(dst, cap, by, (n + 7) / 8);
}

bool ism_analyze(const ism_train_t *t, bool fsk, ism_generic_t *g)
{
    memset(g, 0, sizeof *g);
    if (t->n < 16)
        return false;
    /* 1) Filas: se separa por los espacios del grupo mayor si es mucho más
     *    largo (≥ 3×) que el siguiente grupo poblado. */
    uint32_t sp[ISM_MAXD / 2], mk[ISM_MAXD / 2];
    int nsp = 0, nmk = 0;
    for (int i = 0; i + 1 < t->n; i += 2) {
        mk[nmk++] = t->d[i];
        sp[nsp++] = t->d[i + 1];
    }
    cluster_t cs[16], cm[16], raw[16];
    int nraw = cluster1d(sp, nsp, 0.25, raw, 16);
    int ncs = prune_clusters(memcpy(cs, raw, sizeof raw), nraw, nsp), ncm;
    /* Separador de filas: grupo de espacios (≥ 2 miembros) al menos 3× mayor que
     * el mayor de los grupos principales. */
    uint32_t row_gap = 0xFFFFFFFFu;
    if (ncs >= 1)
        for (int i = 0; i < nraw; i++)
            if (raw[i].cnt >= 2 && raw[i].c >= 3.0 * cs[ncs - 1].c) {
                row_gap = (uint32_t)(raw[i].c * 0.6);
                break;
            }
    row_t rows[64];
    int nr = split_rows(t, row_gap, rows, 64);
    if (nr < 1)
        return false;
    /* 2) Modulación a partir de los grupos de marcas y espacios de DENTRO de las filas. */
    nsp = nmk = 0;
    for (int r = 0; r < nr; r++)
        for (int i = rows[r].a; i + 1 < rows[r].b; i += 2) {
            mk[nmk++] = t->d[i];
            sp[nsp++] = t->d[i + 1];
        }
    ncs = prune_clusters(cs, cluster1d(sp, nsp, 0.25, cs, 16), nsp);
    ncm = prune_clusters(cm, cluster1d(mk, nmk, 0.25, cm, 16), nmk);
    if (ncs == 0 || ncm == 0)
        return false;
    uint8_t rb[64][512];
    int rn[64];
    const char *kind = NULL;
    if (ncm == 2 && ncs <= 2 && !(ncs == 2 && cm[1].c / cm[0].c < 1.4)) {
        kind = fsk ? "FSK-PWM" : "OOK-PWM";
        g->short_us = cm[0].c;
        g->long_us = cm[1].c;
        g->gap_us = cs[ncs - 1].c;
        g->bit_us = cm[0].c + cs[0].c;
        for (int r = 0; r < nr; r++) {
            rn[r] = 0;
            for (int i = rows[r].a; i + 1 < rows[r].b && rn[r] < 512; i += 2) {
                int c = nearest(cm, ncm, t->d[i], 0.3);
                if (c >= 0)
                    rb[r][rn[r]++] = (uint8_t)c;
            }
        }
    } else if (ncm == 1 && ncs == 2) {
        kind = fsk ? "FSK-PPM" : "OOK-PPM";
        g->short_us = cs[0].c;
        g->long_us = cs[1].c;
        g->gap_us = cs[1].c;
        g->bit_us = cm[0].c;
        for (int r = 0; r < nr; r++) {
            rn[r] = 0;
            for (int i = rows[r].a; i + 1 < rows[r].b && rn[r] < 512; i += 2) {
                int c = nearest(cs, ncs, t->d[i + 1], 0.3);
                if (c >= 0)
                    rb[r][rn[r]++] = (uint8_t)c;
            }
        }
    } else {
        /* Duraciones múltiplos de un tiempo de bit: Manchester (1×/2×) o NRZ/PCM. */
        double unit = 1e9;
        for (int i = 0; i < ncm; i++)
            unit = fmin(unit, cm[i].c);
        for (int i = 0; i < ncs; i++)
            unit = fmin(unit, cs[i].c);
        if (unit < 16)
            return false;
        bool multiples = true, max2 = true;
        for (int i = 0; i < ncm + ncs; i++) {
            double c = i < ncm ? cm[i].c : cs[i - ncm].c;
            double k = c / unit;
            if (fabs(k - floor(k + 0.5)) > 0.25)
                multiples = false;
            if (k > 2.4)
                max2 = false;
        }
        if (!multiples)
            return false;
        g->bit_us = unit;
        g->short_us = unit;
        g->long_us = unit * 2;
        if (max2 && ncm + ncs >= 2) {
            kind = fsk ? "FSK-Manchester" : "OOK-Manchester";
            g->bit_us = unit * 2;   /* un bit = dos medios bits */
        } else {
            kind = fsk ? "FSK-NRZ" : "OOK-NRZ";
        }
        for (int r = 0; r < nr; r++) {
            rn[r] = 0;
            uint8_t hb[2048];
            int nh = 0;
            for (int i = rows[r].a; i < rows[r].b; i++) {
                int k = (int)(t->d[i] / unit + 0.5);
                for (int j = 0; j < k && nh < 2048; j++)
                    hb[nh++] = (uint8_t)((i & 1) == 0);
            }
            if (!strstr(kind, "Manchester")) {
                for (int i = 0; i < nh && rn[r] < 512; i++)
                    rb[r][rn[r]++] = hb[i];
            } else {
                for (int p = 0; p + 1 < nh && rn[r] < 512; p += 2) {
                    if (hb[p] == hb[p + 1])
                        p--;   /* desalineado: se resincroniza un medio bit */
                    else
                        rb[r][rn[r]++] = hb[p];
                }
            }
        }
    }
    snprintf(g->mod, sizeof g->mod, "%s", kind);
    /* 3) Fila más repetida (con tolerancia de 2 bits en la longitud). */
    int best = -1, bestc = 0;
    for (int r = 0; r < nr; r++) {
        if (rn[r] < 12)
            continue;
        int c = 0;
        for (int q = 0; q < nr; q++)
            if (rn[q] == rn[r] && !memcmp(rb[q], rb[r], (size_t)rn[r]))
                c++;
        if (c > bestc) {
            bestc = c;
            best = r;
        }
    }
    if (best < 0)
        return false;
    g->nrows = nr;
    g->repeats = bestc;
    g->nbits = rn[best];
    if (bestc < 2 && rn[best] < 32)
        return false;   /* una fila corta sin repetición: demasiado dudosa */
    bits_to_hex(rb[best], rn[best], g->hex, sizeof g->hex);
    return true;
}

/* ======================================================================= */
/*  Codificadores (meteorología + trama de prueba)                         */
/* ======================================================================= */
const char *const ISM_TX_MODELS[] = {"Oregon-THGR122N", "Oregon-THN132N", "Oregon-THGR810", "LaCrosse-TX141B",
                                     "Acurite-592TXR",  "Nexus-TH",       "FineOffset-WH1080", "Ambient-F007TH",
                                     "BladeRFLab-test", NULL};

static void pwm_row_bits(ism_train_t *t, const uint8_t *bits, int n, uint32_t m0, uint32_t m1, uint32_t s0,
                         uint32_t s1)
{
    for (int i = 0; i < n; i++)
        ism_train_pair(t, bits[i] ? m1 : m0, bits[i] ? s1 : s0);
}

static void enc_oregon(const ism_msg_t *m, ism_train_t *t, int repeat)
{
    uint32_t idn = 0x1D20;
    int nnib = 17, pre = 16;
    bool hum = true;
    if (!strcmp(m->model, "Oregon-THN132N")) {
        idn = 0xEC40;
        nnib = 14;
        hum = false;
    } else if (!strcmp(m->model, "Oregon-THGR810")) {
        idn = 0xF824;
        nnib = 17;
        pre = 24;
    }
    uint8_t nib[20] = {0};
    nib[0] = (uint8_t)(idn >> 12);
    nib[1] = (uint8_t)((idn >> 8) & 15);
    nib[2] = (uint8_t)((idn >> 4) & 15);
    nib[3] = (uint8_t)(idn & 15);
    nib[4] = (uint8_t)(m->channel == 3 ? 4 : (m->channel == 2 ? 2 : 1));
    nib[5] = (uint8_t)(m->id & 15);
    nib[6] = (uint8_t)((m->id >> 4) & 15);
    nib[7] = m->batt_low ? 4 : 0;
    int t10 = (int)lrint(fabs(m->temp_c) * 10.0);
    nib[8] = (uint8_t)(t10 % 10);
    nib[9] = (uint8_t)((t10 / 10) % 10);
    nib[10] = (uint8_t)((t10 / 100) % 10);
    nib[11] = m->temp_c < 0 ? 8 : 0;
    if (hum) {
        int h = (int)lrint(m->humidity);
        nib[12] = (uint8_t)(h % 10);
        nib[13] = (uint8_t)((h / 10) % 10);
    }
    int sum = 0;
    for (int i = 0; i < nnib - 2; i++)
        sum += nib[i];
    nib[nnib - 2] = (uint8_t)(sum & 15);
    nib[nnib - 1] = (uint8_t)((sum >> 4) & 15);
    uint8_t bits[320];
    int n = 0;
    for (int i = 0; i < pre; i++)
        bits[n++] = 1;
    bits[n++] = 0;
    bits[n++] = 1;
    bits[n++] = 0;
    bits[n++] = 1;
    for (int i = 0; i < nnib + 2; i++)   /* + 2 nibbles de cola (la trama real los lleva) */
        for (int b = 0; b < 4; b++)
            bits[n++] = (nib[i] >> b) & 1;
    if (idn != 0xF824) {   /* v2.1: cada bit se envía seguido de su complemento */
        uint8_t dbl[320];
        for (int i = 0; i < n; i++) {
            dbl[2 * i] = (uint8_t)!bits[i];   /* polaridad contrastada con rtl_433 (ver oregon_try) */
            dbl[2 * i + 1] = bits[i];
        }
        memcpy(bits, dbl, 2 * (size_t)n);   /* bits[] tiene sitio: 160 bits máx. de entrada */
        n *= 2;
    }
    for (int r = 0; r < repeat; r++) {
        ism_train_manch(t, bits, n, 488, 0);
        ism_train_gap(t, 60000);
    }
}

static void enc_f007(const ism_msg_t *m, ism_train_t *t, int repeat)
{
    uint8_t by[6];
    int raw = (int)lrint((m->temp_c * 1.8 + 32.0) * 10.0 + 400.0) & 0xFFF;
    by[0] = 0x45;
    by[1] = (uint8_t)m->id;
    by[2] = (uint8_t)((m->batt_low ? 0x80 : 0) | (((m->channel > 0 ? m->channel : 1) - 1) & 7) << 4 | (raw >> 8));
    by[3] = (uint8_t)(raw & 0xFF);
    by[4] = (uint8_t)lrint(m->humidity);
    by[5] = (uint8_t)(ism_lfsr_digest8(by, 5, 0x98, 0x3E) ^ 0x64);
    uint8_t bits[64];
    int n = ism_put_bits(bits, 0, 0xFFD, 12);
    for (int i = 0; i < 6; i++)
        n = ism_put_bits(bits, n, by[i], 8);
    for (int r = 0; r < repeat; r++) {
        ism_train_manch(t, bits, n, 500, 0);
        ism_train_gap(t, 40000);
    }
}

static void enc_tx141(const ism_msg_t *m, ism_train_t *t, int repeat)
{
    /* Fila de 37 bits como el v2 real: id, 0x9 = batería OK, temperatura y un
     * resto fijo copiado de la grabación real (su resumen no es reconstruible). */
    int raw = (int)lrint(m->temp_c * 10.0 + 500.0) & 0xFFF;
    uint8_t by[5] = {(uint8_t)m->id, (uint8_t)((m->batt_low ? 0x10 : 0x90) | (raw >> 8)), (uint8_t)(raw & 0xFF), 0x1C, 0xC0};
    uint8_t bits[40];
    for (int i = 0; i < 37; i++)
        bits[i] = (by[i >> 3] >> (7 - (i & 7))) & 1;
    for (int r = 0; r < repeat; r++) {
        for (int s = 0; s < 4; s++)
            ism_train_pair(t, 960, 990);
        for (int i = 0; i < 37; i++)
            ism_train_pair(t, bits[i] ? 480 : 236, bits[i] ? 256 : 500);
        ism_train_gap(t, 1200);
    }
    ism_train_gap(t, 4000);
}

static void enc_acurite(const ism_msg_t *m, ism_train_t *t, int repeat)
{
    uint8_t by[7];
    int raw = (int)lrint(m->temp_c * 10.0 + 1000.0) & 0x7FF;
    by[0] = (uint8_t)(((m->channel & 3) << 6) | ((m->id >> 8) & 0x3F));
    by[1] = (uint8_t)(m->id & 0xFF);
    by[2] = (uint8_t)((m->batt_low ? 0 : 0x40) | 0x04);
    by[3] = (uint8_t)(lrint(m->humidity) & 0x7F);
    by[4] = (uint8_t)(raw >> 7);
    by[5] = (uint8_t)(raw & 0x7F);
    for (int i = 3; i <= 5; i++)
        by[i] |= (uint8_t)(parity7(by[i]) << 7);
    unsigned s = 0;
    for (int i = 0; i < 6; i++)
        s += by[i];
    by[6] = (uint8_t)s;
    uint8_t bits[56];
    for (int i = 0; i < 56; i++)
        bits[i] = (by[i >> 3] >> (7 - (i & 7))) & 1;
    for (int r = 0; r < repeat; r++) {
        for (int k = 0; k < 4; k++)
            ism_train_pair(t, 600, 600);
        pwm_row_bits(t, bits, 56, 220, 400, 400, 220);
        ism_train_pair(t, 400, 2500);
    }
}

static void enc_nexus(const ism_msg_t *m, ism_train_t *t, int repeat)
{
    uint8_t bits[36];
    int raw = (int)lrint(m->temp_c * 10.0) & 0xFFF;
    int n = ism_put_bits(bits, 0, m->id & 0xFF, 8);
    n = ism_put_bits(bits, n, m->batt_low ? 0 : 1, 1);
    n = ism_put_bits(bits, n, 0, 1);
    n = ism_put_bits(bits, n, (uint32_t)((m->channel > 0 ? m->channel : 1) - 1), 2);
    n = ism_put_bits(bits, n, (uint32_t)raw, 12);
    n = ism_put_bits(bits, n, 0xF, 4);
    n = ism_put_bits(bits, n, (uint32_t)lrint(m->humidity), 8);
    for (int r = 0; r < repeat; r++) {
        pwm_row_bits(t, bits, 36, 500, 500, 1000, 2000);
        ism_train_pair(t, 500, 4000);
    }
}

static void enc_wh1080(const ism_msg_t *m, ism_train_t *t, int repeat)
{
    uint8_t by[10];
    int raw = (int)lrint(m->temp_c * 10.0 + 400.0) & 0xFFF;
    int rain = (int)lrint(m->rain_mm / 0.3) & 0xFFF;
    by[0] = (uint8_t)(0xA0 | ((m->id >> 4) & 0x0F));
    by[1] = (uint8_t)(((m->id & 0x0F) << 4) | (raw >> 8));
    by[2] = (uint8_t)(raw & 0xFF);
    by[3] = (uint8_t)lrint(m->humidity);
    by[4] = (uint8_t)lrint(m->wind_avg_ms / 0.34);
    by[5] = (uint8_t)lrint(m->wind_gust_ms / 0.34);
    by[6] = (uint8_t)(rain >> 8);
    by[7] = (uint8_t)(rain & 0xFF);
    by[8] = (uint8_t)((m->batt_low ? 0x10 : 0) | (((int)lrint(m->wind_dir_deg / 22.5)) & 0x0F));
    by[9] = ism_crc8(by, 9, 0x31, 0);
    uint8_t bits[88];
    int n = ism_put_bits(bits, 0, 0xFF, 8);
    for (int i = 0; i < 10; i++)
        n = ism_put_bits(bits, n, by[i], 8);
    for (int r = 0; r < repeat; r++) {
        pwm_row_bits(t, bits, n, 1500, 500, 1000, 1000);   /* bit 1 = marca corta */
        ism_train_pair(t, 500, 5000);
    }
}

void ism_encode_test(uint16_t counter, const uint8_t payload[4], int repeat, ism_train_t *t)
{
    uint8_t by[10];
    by[0] = 0xB1;
    by[1] = 0xAD;
    by[2] = (uint8_t)(counter >> 8);
    by[3] = (uint8_t)counter;
    memcpy(by + 4, payload, 4);
    uint16_t c = crc16_ccitt(by, 8);
    by[8] = (uint8_t)(c >> 8);
    by[9] = (uint8_t)c;
    uint8_t bits[80];
    for (int i = 0; i < 80; i++)
        bits[i] = (by[i >> 3] >> (7 - (i & 7))) & 1;
    for (int r = 0; r < repeat; r++) {
        for (int s = 0; s < 3; s++)
            ism_train_pair(t, 1200, 600);
        pwm_row_bits(t, bits, 80, 300, 600, 300, 300);
        ism_train_pair(t, 300, 4000);
    }
}

int ism_encode(const ism_msg_t *m, int repeat, ism_train_t *t)
{
    const char *md = m->model;
    if (!md)
        return -1;
    if (!strncmp(md, "Oregon-", 7) && (!strcmp(md, "Oregon-THGR122N") || !strcmp(md, "Oregon-THN132N") ||
                                      !strcmp(md, "Oregon-THGR810")))
        enc_oregon(m, t, repeat);
    else if (!strcmp(md, "Ambient-F007TH"))
        enc_f007(m, t, repeat);
    else if (!strcmp(md, "LaCrosse-TX141B"))
        enc_tx141(m, t, repeat);
    else if (!strcmp(md, "Acurite-592TXR"))
        enc_acurite(m, t, repeat);
    else if (!strcmp(md, "Nexus-TH"))
        enc_nexus(m, t, repeat);
    else if (!strcmp(md, "FineOffset-WH1080"))
        enc_wh1080(m, t, repeat);
    else if (!strcmp(md, "BladeRFLab-test")) {
        const uint8_t pl[4] = {'B', 'L', 'A', 'B'};
        ism_encode_test((uint16_t)m->id, pl, repeat, t);
    } else
        return -1;   /* TPMS, mandos, alarmas…: prohibido en el modulador */
    return 0;
}

/* ======================================================================= */
/*  Renderizado                                                            */
/* ======================================================================= */
static int grow(cf32 **buf, int *n, int *cap, int extra)
{
    if (*n + extra <= *cap)
        return 0;
    int nc = (*cap ? *cap : 1024);
    while (nc < *n + extra)
        nc *= 2;
    cf32 *p = realloc(*buf, sizeof(cf32) * (size_t)nc);
    if (!p)
        return -1;
    *buf = p;
    *cap = nc;
    return 0;
}

void ism_render_silence(cf32 **buf, int *n, int *cap, int ns)
{
    if (grow(buf, n, cap, ns) < 0)
        return;
    memset(*buf + *n, 0, sizeof(cf32) * (size_t)ns);
    *n += ns;
}

void ism_render(cf32 **buf, int *n, int *cap, const ism_train_t *t, bool fsk, double fs, double amp,
                double dev_hz, double *phase)
{
    double ph = phase ? *phase : 0.0;
    double carry = 0; /* fracción de muestra pendiente: evita acumular error de redondeo */
    for (int i = 0; i < t->n; i++) {
        double exact = t->d[i] * 1e-6 * fs + carry;
        int ns = (int)floor(exact + 0.5);
        carry = exact - ns;
        if (grow(buf, n, cap, ns) < 0)
            return;
        int level = (i & 1) == 0;
        for (int k = 0; k < ns; k++) {
            cf32 v;
            if (fsk) {
                ph += 2.0 * M_PI * (level ? dev_hz : -dev_hz) / fs;
                v.re = (float)(amp * cos(ph));
                v.im = (float)(amp * sin(ph));
            } else {
                v.re = level ? (float)amp : 0.f;
                v.im = 0.f;
            }
            (*buf)[(*n)++] = v;
        }
    }
    if (phase)
        *phase = fmod(ph, 2.0 * M_PI);
}
