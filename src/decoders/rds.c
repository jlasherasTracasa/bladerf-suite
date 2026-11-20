/*
 * rds.c — Decodificador RDS (Radio Data System) de la FM comercial.
 *
 * Teoría (IEC 62106 / EN 50067)
 * -----------------------------
 * La señal multiplex (MPX) de una emisora FM estéreo es
 *     L+R (0–15 kHz) + piloto 19 kHz + (L−R) en DSB-SC a 38 kHz (23–53 kHz)
 *     + RDS: subportadora de 57 kHz (= 3 × piloto) modulada en BPSK.
 * RDS son 1187,5 bit/s (= 19000/16). Cada bit va codificado en BIFASE
 * (Manchester): el bit transmitido multiplica un pulso "+ − " de un periodo de
 * bit, de modo que el espectro tiene un nulo en 57 kHz y ocupa ±2,4 kHz. Antes
 * se aplica codificación DIFERENCIAL (b'[k] = b[k] XOR b'[k−1]), así la
 * ambigüedad de 180° de la portadora no importa.
 *
 * Cadena DSP (250 kS/s complejos entregados por el DDC):
 *   1. Discriminador FM (fmdisc) → MPX real, ±75 kHz de desvío = ±1. Convierte
 *      cualquier desvío de frecuencia del canal en una continua (se elimina
 *      sola), por eso el decodificador no necesita estimar el CFO del canal.
 *   2. Mezcla a −57 kHz y filtro paso bajo FIR de Kaiser (60 dB; pasa ±2,4 kHz,
 *      rechaza desde ±4 kHz, donde acaba el L−R) con diezmado ÷10 → 25 kS/s.
 *   3. Costas BPSK (sync.h, orden 2): fija la fase de la portadora de 57 kHz.
 *   4. Filtro adaptado al pulso bifásico (un periodo de seno de 1/1187,5 s) y
 *      recuperación de reloj de Gardner (symsync) a 21,05 muestras/símbolo.
 *      Tras el filtro adaptado el pulso es triangular y cumple Nyquist, así que
 *      Gardner enclava en el pico.
 *   5. Decisión dura + decodificación diferencial  b[k] = d[k] XOR d[k−1].
 *   6. Sincronía de bloque: en cada bit se calcula el síndrome de los últimos
 *      26 bits; la coincidencia con uno de los 5 síndromes de offset (A, B, C,
 *      C', D) marca un "pulso". Dos pulsos separados un múltiplo de 26 bits y
 *      con la secuencia de bloques correcta dan sincronía. Se pierde si ≥ 20 de
 *      los últimos 32 bloques fallan o hay 12 seguidos malos (→ resincroniza
 *      sola tras un cambio de emisora; el PI nuevo reinicia todo el estado).
 *   7. Corrección de ráfagas opcional (opts {"fec":N}, N = longitud máxima de
 *      ráfaga 1..5, por defecto 0 = apagada): tabla síndrome → patrón de error
 *      para el offset esperado, descartando síndromes ambiguos. Con N grande
 *      aumenta mucho la tasa de falsas correcciones; redsea usa N = 2.
 *   8. Grupos: 0A/0B (PS, TA, MS, DI, AF), 2A/2B (RadioText con flag A/B),
 *      4A (hora y fecha CT), 10A (PTY name). Cada segmento de texto debe
 *      recibirse dos veces con el mismo contenido antes de aceptarse (los 10
 *      bits de CRC dejan pasar 1 de cada 1024 bloques erróneos).
 *
 * Eventos (uno por cambio): {"kind":"pi|ps|rt|ptyn|ct", "pi":"C201", ..., "text":"…"}
 * state_json: {"pi","ps","rt","pty","pty_name","tp","ta","ms","af":[Hz],"ct"}
 *   - pi, ps, rt, pty_name, ct: cadenas ("" si no se conocen); ps/rt sin espacios finales.
 *   - pty entero 0..31; tp, ta booleanos; ms "music" | "speech" | "".
 *   - af: frecuencias VHF alternativas en Hz (LF/MF no se interpretan).
 *   - ct: hora local "AAAA-MM-DD HH:MM +HH:MM" (el RDS la manda en UTC + desfase).
 *   Caducidad: 30 s sin ningún grupo válido → todo vuelve a vacío.
 *
 * Limitaciones: no se decodifican ODA (RT+, TMC, …), EON ni grupos 1A/3A/14A
 * (se descartan sin error). Con CNR < ~20 dB en 250 kHz el FM está por debajo de
 * su umbral y el RDS (que va a 57 kHz, donde el ruido del discriminador crece
 * con f²) pierde la mayoría de los bloques: ver docs/protocolos/rds.md.
 */
#include "rds.h"

#include "decoder.h"

#include "../dsp/bits.h"
#include "../dsp/filter.h"
#include "../dsp/pulse.h"
#include "../dsp/sync.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/*  Código de bloque y tablas                                               */
/* ======================================================================== */
#define RDS_G 0x5B9u /* x^10+x^8+x^7+x^5+x^4+x^3+1 */

static const uint32_t OFFSET_WORD[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};

uint32_t rds_offset_word(int offset) { return OFFSET_WORD[offset]; }

uint32_t rds_rem(uint32_t v, int nbits)
{
    for (int i = nbits - 1; i >= 10; i--)
        if ((v >> i) & 1u)
            v ^= RDS_G << (i - 10);
    return v & 0x3FF;
}

uint32_t rds_block_encode(uint16_t data, int offset)
{
    uint32_t chk = rds_rem((uint32_t)data << 10, 26) ^ OFFSET_WORD[offset];
    return ((uint32_t)data << 10) | chk;
}

int rds_block_offset(uint32_t w26)
{
    uint32_t s = rds_rem(w26 & 0x3FFFFFF, 26);
    for (int i = 0; i < 5; i++)
        if (s == OFFSET_WORD[i])
            return i;
    return -1;
}

static const char *const PTY_NAMES[32] = {
    "None", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture",
    "Science", "Varied", "Pop music", "Rock music", "Easy listening", "Light classical",
    "Serious classical", "Other music", "Weather", "Finance", "Children's programmes",
    "Social affairs", "Religion", "Phone-in", "Travel", "Leisure", "Jazz music", "Country music",
    "National music", "Oldies music", "Folk music", "Documentary", "Alarm test", "Alarm"};

const char *rds_pty_name(int pty) { return pty >= 0 && pty < 32 ? PTY_NAMES[pty] : ""; }

/* "Complete EBU Latin based repertoire" (ETSI TS 101 756 v1.8.1 anexo C) →
 * Unicode. Tabla tomada de la implementación abierta welle.io (charsets.cpp),
 * que la genera a partir de ODR-PadEnc. */
static const uint16_t EBU_LATIN[256] = {
    0x00, 0x118, 0x12e, 0x172, 0x102, 0x116, 0x10e, 0x218,
    0x21a, 0x10a, 0xa, 0xb, 0x120, 0x139, 0x17b, 0x143,
    0x105, 0x119, 0x12f, 0x173, 0x103, 0x117, 0x10f, 0x219,
    0x21b, 0x10b, 0x147, 0x11a, 0x121, 0x13a, 0x17c, 0x82,
    0x20, 0x21, 0x22, 0x23, 0x142, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x16e, 0x5d, 0x141, 0x5f,
    0x104, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0xab, 0x16f, 0xbb, 0x13d, 0x126,
    0xe1, 0xe0, 0xe9, 0xe8, 0xed, 0xec, 0xf3, 0xf2,
    0xfa, 0xf9, 0xd1, 0xc7, 0x15e, 0xdf, 0xa1, 0x178,
    0xe2, 0xe4, 0xea, 0xeb, 0xee, 0xef, 0xf4, 0xf6,
    0xfb, 0xfc, 0xf1, 0xe7, 0x15f, 0x11f, 0x131, 0xff,
    0x136, 0x145, 0xa9, 0x122, 0x11e, 0x11b, 0x148, 0x151,
    0x150, 0x20ac, 0xa3, 0x24, 0x100, 0x112, 0x12a, 0x16a,
    0x137, 0x146, 0x13b, 0x123, 0x13c, 0x130, 0x144, 0x171,
    0x170, 0xbf, 0x13e, 0xb0, 0x101, 0x113, 0x12b, 0x16b,
    0xc1, 0xc0, 0xc9, 0xc8, 0xcd, 0xcc, 0xd3, 0xd2,
    0xda, 0xd9, 0x158, 0x10c, 0x160, 0x17d, 0xd0, 0x13f,
    0xc2, 0xc4, 0xca, 0xcb, 0xce, 0xcf, 0xd4, 0xd6,
    0xdb, 0xdc, 0x159, 0x10d, 0x161, 0x17e, 0x111, 0x140,
    0xc3, 0xc5, 0xc6, 0x152, 0x177, 0xdd, 0xd5, 0xd8,
    0xde, 0x14a, 0x154, 0x106, 0x15a, 0x179, 0x164, 0xf0,
    0xe3, 0xe5, 0xe6, 0x153, 0x175, 0xfd, 0xf5, 0xf8,
    0xfe, 0x14b, 0x155, 0x107, 0x15b, 0x17a, 0x165, 0x127,
};

int ebu_latin_to_utf8(uint8_t c, bool ascii, char out[5])
{
    uint32_t u = (ascii && c >= 0x20 && c <= 0x7E) ? c : EBU_LATIN[c];
    if (ascii && c < 0x20)
        u = ' '; /* RDS: 0x0D es fin de RadioText y 0x0A/0x0B saltos: espacio */
    if (u < 0x20 || u == 0x7F)
        u = ' '; /* controles (incluido el salto de línea): espacio */
    int n = 0;
    if (u < 0x80)
        out[n++] = (char)u;
    else if (u < 0x800) {
        out[n++] = (char)(0xC0 | (u >> 6));
        out[n++] = (char)(0x80 | (u & 0x3F));
    } else {
        out[n++] = (char)(0xE0 | (u >> 12));
        out[n++] = (char)(0x80 | ((u >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (u & 0x3F));
    }
    out[n] = 0;
    return n;
}

void ebu_latin_string(const uint8_t *src, int n, bool ascii, char *dst)
{
    int w = 0;
    for (int i = 0; i < n; i++)
        w += ebu_latin_to_utf8(src[i], ascii, dst + w);
    dst[w] = 0;
    while (w > 0 && dst[w - 1] == ' ')
        dst[--w] = 0;
}

/* MJD → fecha (EN 50067 anexo G):
 *   Y' = int((MJD − 15078,2)/365,25); M' = int((MJD − 14956,1 − int(Y'·365,25))/30,6001)
 *   D = MJD − 14956 − int(Y'·365,25) − int(M'·30,6001);  K = 1 si M' = 14 o 15
 *   Y = 1900 + Y' + K;  M = M' − 1 − 12K */
void rds_mjd_to_date(int mjd, int *y, int *m, int *d)
{
    int yp = (int)((mjd - 15078.2) / 365.25);
    int mp = (int)((mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001);
    int dd = mjd - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001);
    int k = (mp == 14 || mp == 15) ? 1 : 0;
    *y = 1900 + yp + k;
    *m = mp - 1 - k * 12;
    *d = dd;
}

int rds_date_to_mjd(int y, int m, int d)
{
    int yp = y - 1900, l = (m == 1 || m == 2) ? 1 : 0;
    return 14956 + d + (int)((yp - l) * 365.25) + (int)((m + 1 + l * 12) * 30.6001);
}

/* ======================================================================== */
/*  Estado del decodificador                                                */
/* ======================================================================== */
#define DEC_FACTOR 10
#define FS_MIX (RDS_RATE / DEC_FACTOR) /* 25 kHz */
#define SPS (FS_MIX / RDS_BITRATE)     /* 21,0526 muestras por bit */
#define MF_TAPS 22
#define AF_MAX 25
#define EXPIRE_S 30.0

typedef struct {
    uint8_t cur[64];     /* texto confirmado */
    uint8_t pend[32][4]; /* última recepción de cada segmento (pendiente de confirmar) */
    uint32_t pend_mask, ok_mask;
    int seglen, nseg;    /* nº de caracteres por segmento y de segmentos */
    int ab;              /* flag A/B vigente (−1 = ninguno) */
} text_t;

typedef struct {
    /* --- DSP --- */
    fmdisc_t fm;
    nco_t nco;
    fir_cf_t lpf;
    costas_t cos;
    fir_f_t mf;
    symsync_t *ss;
    float amp; /* media móvil de |salida del filtro adaptado| */
    float *mpx, *mf_in, *mf_out, *sym;
    cf32 *mix, *dec, *cs;
    int cap;
    /* --- bits y bloques --- */
    int prev_d;
    uint32_t reg;
    int64_t nbits, last_pulse_pos;
    int last_pulse_blk;
    bool synced;
    int blk, to_block; /* bloque esperado (0..3) y bits que faltan para completarlo */
    uint32_t bad_hist;
    int bad_run;
    int64_t bits_since_good;
    uint16_t gdat[4];
    bool gok[4];
    int gcprime;
    int fec_burst;
    int32_t errtab[1024]; /* patrón de error (≥0), −1 sin entrada, −2 ambiguo */
    /* --- estado RDS --- */
    bool pi_ok;
    uint16_t pi, pi_cand;
    int pi_cand_cnt;
    char ps[40], rt[300], ptyn[40], ct[40];
    int ptyn_ab;
    uint8_t ptyn_buf[8];
    int ptyn_mask;
    int pty, ptp_pend;
    bool tp, ta, ms_known, ms;
    int64_t af_hz[AF_MAX];
    int naf;
    int64_t af_pend[AF_MAX];
    int naf_pend, af_expect;
    text_t ps_t, rt_t;
    int ct_min_last;
    double now, last_group_t;
    void *emit_ctx;
    dec_emit_fn emit;
    char last_ps[40], last_rt[300], last_ptyn[40];
} rds_t;

/* ---- eventos ----------------------------------------------------------- */
static void emit_event(rds_t *r, const char *kind, const char *key, const char *val, const char *text)
{
    sb_t sb = {0};
    char pi[8];
    snprintf(pi, sizeof pi, "%04X", r->pi);
    sb_printf(&sb, "{\"kind\":\"%s\",\"pi\":\"%s\",\"pty\":%d,", kind, pi, r->pty);
    sb_puts(&sb, "\"pty_name\":");
    sb_json_str(&sb, rds_pty_name(r->pty));
    if (key) {
        sb_printf(&sb, ",\"%s\":", key);
        sb_json_str(&sb, val);
    }
    sb_puts(&sb, ",\"text\":");
    sb_json_str(&sb, text);
    sb_puts(&sb, "}");
    r->emit(r->emit_ctx, sb.data, sb.len);
    sb_free(&sb);
}

/* ---- texto por segmentos ---------------------------------------------- */
static void text_reset(text_t *t, int seglen, int nseg, int ab)
{
    memset(t, 0, sizeof *t);
    t->seglen = seglen;
    t->nseg = nseg;
    t->ab = ab;
}

/* Registra un segmento; solo se confirma cuando llega dos veces seguidas igual. */
static void text_put(text_t *t, int addr, const uint8_t *ch)
{
    if (addr >= t->nseg)
        return;
    uint32_t bit = 1u << addr;
    if ((t->pend_mask & bit) && memcmp(t->pend[addr], ch, (size_t)t->seglen) == 0) {
        memcpy(t->cur + addr * t->seglen, ch, (size_t)t->seglen);
        t->ok_mask |= bit;
    }
    memcpy(t->pend[addr], ch, (size_t)t->seglen);
    t->pend_mask |= bit;
}

/* Longitud del mensaje completo (en caracteres) o −1 si faltan segmentos.
 * Termina en el primer 0x0D o, si no lo hay, al llegar a nseg segmentos. */
static int text_complete(const text_t *t)
{
    for (int s = 0; s < t->nseg; s++) {
        if (!((t->ok_mask >> s) & 1))
            return -1;
        for (int i = 0; i < t->seglen; i++)
            if (t->cur[s * t->seglen + i] == 0x0D)
                return s * t->seglen + i;
    }
    return t->nseg * t->seglen;
}

/* ---- AF ---------------------------------------------------------------- */
static void af_code(rds_t *r, int code)
{
    if (code < 1 || code > 204 || r->naf_pend >= AF_MAX)
        return;
    int64_t hz = 87500000LL + (int64_t)code * 100000LL;
    for (int i = 0; i < r->naf_pend; i++)
        if (r->af_pend[i] == hz)
            return;
    r->af_pend[r->naf_pend++] = hz;
}

/* Un par de bytes del bloque C del grupo 0A: código de recuento (225..249 =
 * 1..25 frecuencias siguientes) y luego códigos de frecuencia (método A o B). */
static void af_pair(rds_t *r, int hi, int lo)
{
    if (hi >= 225 && hi <= 249) {
        r->af_expect = hi - 224;
        r->naf_pend = 0;
        af_code(r, lo);
        r->af_expect -= 1; /* el segundo byte ya es la primera frecuencia */
    } else if (r->af_expect > 0) {
        af_code(r, hi);
        af_code(r, lo);
        r->af_expect -= 2;
    } else {
        return;
    }
    if (r->af_expect <= 0 && r->naf_pend > 0) {
        memcpy(r->af_hz, r->af_pend, sizeof r->af_hz);
        r->naf = r->naf_pend;
        r->af_expect = 0;
        r->naf_pend = 0;
    }
}

/* ---- reinicio del estado al cambiar de emisora ------------------------- */
static void state_reset(rds_t *r)
{
    r->ps[0] = r->rt[0] = r->ptyn[0] = r->ct[0] = 0;
    r->last_ps[0] = r->last_rt[0] = r->last_ptyn[0] = 0;
    r->pty = 0;
    r->ptp_pend = -1;
    r->tp = r->ta = r->ms_known = r->ms = false;
    r->naf = r->naf_pend = r->af_expect = 0;
    text_reset(&r->ps_t, 2, 4, -1);
    text_reset(&r->rt_t, 4, 16, -1);
    r->ptyn_mask = 0;
    r->ptyn_ab = -1;
    r->ct_min_last = -1;
}

/* ---- manejo de un grupo completo --------------------------------------- */
static void group_0(rds_t *r, bool vb)
{
    uint16_t b = r->gdat[1];
    r->ta = (b >> 4) & 1;
    r->ms = (b >> 3) & 1;
    r->ms_known = true;
    int seg = b & 3;
    if (!vb && r->gok[2])
        af_pair(r, r->gdat[2] >> 8, r->gdat[2] & 0xFF);
    if (r->gok[3]) {
        uint8_t ch[2] = {(uint8_t)(r->gdat[3] >> 8), (uint8_t)r->gdat[3]};
        text_put(&r->ps_t, seg, ch);
        if (r->ps_t.ok_mask == 0xF) {
            ebu_latin_string(r->ps_t.cur, 8, true, r->ps);
            if (strcmp(r->ps, r->last_ps) != 0) {
                strcpy(r->last_ps, r->ps);
                char t[80];
                snprintf(t, sizeof t, "PS \"%s\" (PI %04X)", r->ps, r->pi);
                emit_event(r, "ps", "ps", r->ps, t);
            }
        }
    }
}

static void group_2(rds_t *r, bool vb)
{
    uint16_t b = r->gdat[1];
    int ab = (b >> 4) & 1, seg = b & 15;
    if (r->rt_t.ab != ab || r->rt_t.seglen != (vb ? 2 : 4)) {
        text_reset(&r->rt_t, vb ? 2 : 4, vb ? 16 : 16, ab); /* A/B cambia: mensaje nuevo */
        r->last_rt[0] = 0;
    }
    uint8_t ch[4];
    if (!vb) {
        if (!r->gok[2] || !r->gok[3])
            return;
        ch[0] = (uint8_t)(r->gdat[2] >> 8);
        ch[1] = (uint8_t)r->gdat[2];
        ch[2] = (uint8_t)(r->gdat[3] >> 8);
        ch[3] = (uint8_t)r->gdat[3];
    } else {
        if (!r->gok[3])
            return;
        ch[0] = (uint8_t)(r->gdat[3] >> 8);
        ch[1] = (uint8_t)r->gdat[3];
    }
    text_put(&r->rt_t, seg, ch);
    int len = text_complete(&r->rt_t);
    if (len < 0)
        return;
    char tmp[300];
    ebu_latin_string(r->rt_t.cur, len, true, tmp);
    if (strcmp(tmp, r->last_rt) != 0) {
        strcpy(r->last_rt, tmp);
        strcpy(r->rt, tmp);
        char t[340];
        snprintf(t, sizeof t, "RT \"%s\" (PI %04X)", tmp, r->pi);
        emit_event(r, "rt", "rt", tmp, t);
    }
}

static void group_4a(rds_t *r)
{
    if (!r->gok[2] || !r->gok[3])
        return;
    uint32_t mjd = ((uint32_t)(r->gdat[1] & 3) << 15) | ((uint32_t)r->gdat[2] >> 1);
    int hour = ((r->gdat[2] & 1) << 4) | (r->gdat[3] >> 12);
    int minute = (r->gdat[3] >> 6) & 63;
    int off = r->gdat[3] & 31; /* medias horas */
    if (r->gdat[3] & 32)
        off = -off;
    if (hour > 23 || minute > 59 || mjd < 40000)
        return;
    int total = hour * 60 + minute + off * 30;
    int day_shift = 0;
    if (total < 0) {
        total += 1440;
        day_shift = -1;
    } else if (total >= 1440) {
        total -= 1440;
        day_shift = 1;
    }
    int y, m, d;
    rds_mjd_to_date((int)mjd + day_shift, &y, &m, &d);
    int ao = abs(off) * 30;
    char s[40];
    snprintf(s, sizeof s, "%04d-%02d-%02d %02d:%02d %c%02d:%02d", y, m, d, total / 60, total % 60,
             off < 0 ? '-' : '+', ao / 60, ao % 60);
    int key = (int)mjd * 1440 + hour * 60 + minute;
    if (key != r->ct_min_last) {
        r->ct_min_last = key;
        strcpy(r->ct, s);
        char t[80];
        snprintf(t, sizeof t, "CT %s", s);
        emit_event(r, "ct", "ct", s, t);
    }
}

static void group_10a(rds_t *r)
{
    if (!r->gok[2] || !r->gok[3])
        return;
    int ab = (r->gdat[1] >> 4) & 1, seg = r->gdat[1] & 1;
    if (ab != r->ptyn_ab) {
        r->ptyn_ab = ab;
        r->ptyn_mask = 0;
    }
    r->ptyn_buf[seg * 4 + 0] = (uint8_t)(r->gdat[2] >> 8);
    r->ptyn_buf[seg * 4 + 1] = (uint8_t)r->gdat[2];
    r->ptyn_buf[seg * 4 + 2] = (uint8_t)(r->gdat[3] >> 8);
    r->ptyn_buf[seg * 4 + 3] = (uint8_t)r->gdat[3];
    r->ptyn_mask |= 1 << seg;
    if (r->ptyn_mask == 3) {
        char tmp[40];
        ebu_latin_string(r->ptyn_buf, 8, true, tmp);
        if (strcmp(tmp, r->last_ptyn) != 0) {
            strcpy(r->last_ptyn, tmp);
            strcpy(r->ptyn, tmp);
            char t[80];
            snprintf(t, sizeof t, "PTYN \"%s\" (PI %04X)", tmp, r->pi);
            emit_event(r, "ptyn", "ptyn", tmp, t);
        }
    }
}

static void handle_group(rds_t *r)
{
    /* PI: del bloque A cuando B también es válido; hay que verlo 2 veces seguidas
     * para aceptarlo (o cambiarlo), así una emisora nueva se reconoce en ~0,2 s. */
    if (r->gok[0] && r->gok[1]) {
        uint16_t pi = r->gdat[0];
        if (r->pi_ok && pi == r->pi) {
            r->pi_cand_cnt = 0;
        } else if (r->pi_cand_cnt > 0 && pi == r->pi_cand) {
            if (++r->pi_cand_cnt >= 2) {
                bool changed = !r->pi_ok || r->pi != pi;
                r->pi = pi;
                r->pi_ok = true;
                r->pi_cand_cnt = 0;
                if (changed) {
                    state_reset(r);
                    char t[40];
                    snprintf(t, sizeof t, "PI %04X", pi);
                    emit_event(r, "pi", NULL, NULL, t);
                }
            }
        } else {
            r->pi_cand = pi;
            r->pi_cand_cnt = 1;
        }
    }
    if (!r->pi_ok || !r->gok[1])
        return;
    /* El PI de los grupos B (bloque 3 con offset C') debe coincidir */
    r->last_group_t = r->now;
    uint16_t b = r->gdat[1];
    int type = b >> 12;
    bool vb = (b >> 11) & 1;
    if (r->gok[2] && (r->gcprime != 0) != vb)
        return; /* versión y offset de bloque 3 incoherentes */
    /* TP+PTY: se aceptan al verlos 2 veces seguidas (un bloque B corrupto que
     * pase el CRC no debe cambiar el tipo de programa). */
    int ptp = (b >> 5) & 63;
    if (ptp == ((r->tp << 5) | r->pty)) {
        r->ptp_pend = -1;
    } else if (ptp == r->ptp_pend) {
        r->tp = ptp >> 5;
        r->pty = ptp & 31;
        r->ptp_pend = -1;
    } else {
        r->ptp_pend = ptp;
    }
    switch (type) {
    case 0: group_0(r, vb); break;
    case 2: group_2(r, vb); break;
    case 4: if (!vb) group_4a(r); break;
    case 10: if (!vb) group_10a(r); break;
    default: break;
    }
}

/* ---- sincronía de bloque ----------------------------------------------- */
static int blk_of_offset(int off) { return off == RDS_OFF_A ? 0 : off == RDS_OFF_B ? 1 : off == RDS_OFF_D ? 3 : 2; }

static void build_errtab(rds_t *r)
{
    /* Para cada patrón de ráfaga e (longitud ≤ fec_burst, bits extremos a 1,
     * en cualquier posición del bloque) rem(e) es el síndrome adicional. */
    for (int i = 0; i < 1024; i++)
        r->errtab[i] = -1;
    for (int len = 1; len <= r->fec_burst; len++) {
        for (uint32_t mid = 0; mid < (len > 2 ? (1u << (len - 2)) : 1u); mid++) {
            uint32_t pat = len == 1 ? 1u : (1u | (mid << 1) | (1u << (len - 1)));
            for (int pos = 0; pos + len <= 26; pos++) {
                uint32_t e = pat << pos;
                uint32_t s = rds_rem(e, 26);
                if (r->errtab[s] == -1)
                    r->errtab[s] = (int32_t)e;
                else if (r->errtab[s] != (int32_t)e)
                    r->errtab[s] = -2;
            }
        }
    }
    r->errtab[0] = -1;
}

static void finish_block(rds_t *r)
{
    int expect = r->blk;
    uint32_t w = r->reg;
    int off = rds_block_offset(w);
    bool ok = off >= 0 && blk_of_offset(off) == expect && (expect != 2 || true);
    if (!ok && r->fec_burst > 0) {
        /* ¿Es una ráfaga corta sobre el offset esperado? (C o C' para el bloque 3) */
        int cands[2] = {expect == 0 ? RDS_OFF_A : expect == 1 ? RDS_OFF_B : expect == 3 ? RDS_OFF_D : RDS_OFF_C,
                        RDS_OFF_CP};
        for (int c = 0; c < (expect == 2 ? 2 : 1) && !ok; c++) {
            uint32_t s = rds_rem(w, 26) ^ OFFSET_WORD[cands[c]];
            int32_t e = r->errtab[s & 0x3FF];
            if (e >= 0) {
                w ^= (uint32_t)e;
                off = cands[c];
                ok = true;
            }
        }
    }
    r->gok[expect] = ok;
    if (ok) {
        r->gdat[expect] = (uint16_t)(w >> 10);
        if (expect == 2)
            r->gcprime = off == RDS_OFF_CP;
        r->bad_run = 0;
        r->bits_since_good = 0;
    } else {
        r->bad_run++;
    }
    r->bad_hist = (r->bad_hist << 1) | (ok ? 0u : 1u);
    if (expect == 3) {
        handle_group(r);
        memset(r->gok, 0, sizeof r->gok);
    }
    r->blk = (expect + 1) & 3;
    r->to_block = 26;
    if (r->bad_run >= 12 || bits_popcount32(r->bad_hist) >= 20) {
        r->synced = false;
        r->last_pulse_pos = -1000000;
        r->bad_hist = 0;
        r->bad_run = 0;
        memset(r->gok, 0, sizeof r->gok);
    }
}

static void on_bit(rds_t *r, int bit)
{
    r->reg = ((r->reg << 1) | (uint32_t)bit) & 0x3FFFFFF;
    r->nbits++;
    r->bits_since_good++;
    if (r->synced) {
        if (--r->to_block == 0)
            finish_block(r);
        return;
    }
    if (r->nbits < 26)
        return;
    int off = rds_block_offset(r->reg);
    if (off < 0)
        return;
    int bn = blk_of_offset(off);
    int64_t dist = r->nbits - r->last_pulse_pos;
    if (dist % 26 == 0 && dist / 26 <= 12 && ((r->last_pulse_blk + dist / 26) & 3) == bn) {
        /* dos pulsos coherentes → sincronía; el actual cuenta como bloque válido */
        r->synced = true;
        r->blk = bn;
        r->to_block = 1;
        r->bad_hist = 0;
        r->bad_run = 0;
        memset(r->gok, 0, sizeof r->gok);
        finish_block(r);
        return;
    }
    r->last_pulse_pos = r->nbits;
    r->last_pulse_blk = bn;
}

/* ======================================================================== */
/*  Cadena de señal                                                          */
/* ======================================================================== */
static int ensure_cap(rds_t *r, int n)
{
    if (n <= r->cap)
        return 0;
    int nc = n + 4096;
    int m = nc / DEC_FACTOR + 8;
    float *a = realloc(r->mpx, sizeof(float) * (size_t)nc);
    if (a)
        r->mpx = a;
    cf32 *b = realloc(r->mix, sizeof(cf32) * (size_t)nc);
    if (b)
        r->mix = b;
    cf32 *c = realloc(r->dec, sizeof(cf32) * (size_t)m);
    if (c)
        r->dec = c;
    cf32 *d = realloc(r->cs, sizeof(cf32) * (size_t)m);
    if (d)
        r->cs = d;
    float *e = realloc(r->mf_in, sizeof(float) * (size_t)m);
    if (e)
        r->mf_in = e;
    float *f = realloc(r->mf_out, sizeof(float) * (size_t)m);
    if (f)
        r->mf_out = f;
    float *g = realloc(r->sym, sizeof(float) * (size_t)symsync_max_out(r->ss, m) + 64);
    if (g)
        r->sym = g;
    if (!a || !b || !c || !d || !e || !f || !g)
        return -1;
    r->cap = nc;
    return 0;
}

static void rds_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    rds_t *r = st;
    if (n <= 0 || ensure_cap(r, n) < 0)
        return;
    r->now = now;
    r->emit = emit;
    r->emit_ctx = ctx;
    /* 1. FM → MPX */
    fmdisc_process(&r->fm, iq, n, r->mpx);
    /* 2. mezcla a −57 kHz, paso bajo y diezmado ÷10 */
    for (int i = 0; i < n; i++) {
        r->mix[i].re = r->mpx[i];
        r->mix[i].im = 0;
    }
    nco_mix(&r->nco, r->mix, n, r->mix);
    int nd = fir_cf_process(&r->lpf, r->mix, n, r->dec);
    /* 3. Costas BPSK */
    costas_process(&r->cos, r->dec, nd, r->cs);
    /* 4. filtro adaptado + normalización lenta + reloj de Gardner */
    for (int i = 0; i < nd; i++)
        r->mf_in[i] = r->cs[i].re;
    int nm = fir_f_process(&r->mf, r->mf_in, nd, r->mf_out);
    for (int i = 0; i < nm; i++) {
        float a = fabsf(r->mf_out[i]);
        r->amp += 0.0005f * (a - r->amp);
        r->mf_out[i] /= (r->amp + 1e-9f);
    }
    int ns = symsync_process_f(r->ss, r->mf_out, nm, r->sym, symsync_max_out(r->ss, nm));
    /* 5-6. bits, diferencial y sincronía */
    for (int i = 0; i < ns; i++) {
        int d = r->sym[i] > 0;
        on_bit(r, d ^ r->prev_d);
        r->prev_d = d;
    }
    /* Sin ningún bloque válido durante 3 s: reinicia los lazos (cambio de emisora) */
    if (r->bits_since_good > 3562) {
        symsync_reset(r->ss);
        r->cos.pll.freq = 0;
        r->bits_since_good = 0;
    }
}

/* ---- state_json -------------------------------------------------------- */
static void rds_state(void *st, sb_t *sb, double now)
{
    rds_t *r = st;
    bool valid = r->pi_ok && now - r->last_group_t < EXPIRE_S;
    char pi[8] = "";
    if (valid)
        snprintf(pi, sizeof pi, "%04X", r->pi);
    sb_puts(sb, "{\"pi\":");
    sb_json_str(sb, pi);
    sb_puts(sb, ",\"ps\":");
    sb_json_str(sb, valid ? r->ps : "");
    sb_puts(sb, ",\"rt\":");
    sb_json_str(sb, valid ? r->rt : "");
    sb_printf(sb, ",\"pty\":%d,\"pty_name\":", valid ? r->pty : 0);
    sb_json_str(sb, valid ? rds_pty_name(r->pty) : "");
    sb_printf(sb, ",\"tp\":%s,\"ta\":%s,\"ms\":", valid && r->tp ? "true" : "false", valid && r->ta ? "true" : "false");
    sb_json_str(sb, valid && r->ms_known ? (r->ms ? "music" : "speech") : "");
    sb_puts(sb, ",\"af\":[");
    for (int i = 0; valid && i < r->naf; i++)
        sb_printf(sb, "%s%lld", i ? "," : "", (long long)r->af_hz[i]);
    sb_puts(sb, "],\"ct\":");
    sb_json_str(sb, valid ? r->ct : "");
    sb_puts(sb, "}");
}

/* ---- ciclo de vida ------------------------------------------------------ */
static void rds_destroy(void *st)
{
    rds_t *r = st;
    if (!r)
        return;
    fir_cf_free(&r->lpf);
    fir_f_free(&r->mf);
    symsync_destroy(r->ss);
    free(r->mpx);
    free(r->mix);
    free(r->dec);
    free(r->cs);
    free(r->mf_in);
    free(r->mf_out);
    free(r->sym);
    free(r);
}

static void *rds_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - RDS_RATE) > 1.0) {
        snprintf(err, errlen, "RDS requiere 250000 S/s (recibido %.0f)", cfg->rate);
        return NULL;
    }
    rds_t *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    if (cfg->opts && cfg->opts[0]) {
        jtok_t t[16];
        int nt = json_parse(cfg->opts, strlen(cfg->opts), t, 16);
        double v;
        int i;
        if (nt > 0 && t[0].type == J_OBJ && (i = json_obj_get(cfg->opts, t, 0, "fec")) >= 0 &&
            json_num(cfg->opts, t, i, &v))
            r->fec_burst = (int)CLAMP(v, 0, 5);
    }
    build_errtab(r);
    fmdisc_init(&r->fm, RDS_RATE, 75000.0);
    nco_init(&r->nco, -57000.0, RDS_RATE);
    /* Paso bajo: pasa ±2,4 kHz, rechaza desde ±4 kHz (el L−R acaba en 53 kHz). */
    int nt = fir_kaiser_ntaps(1600.0 / RDS_RATE, 60.0, 2001);
    float *h = malloc(sizeof(float) * (size_t)nt);
    fir_design_lowpass(h, nt, 3200.0 / RDS_RATE, kaiser_beta(60.0));
    int ok = fir_cf_init(&r->lpf, h, nt, DEC_FACTOR);
    free(h);
    /* Filtro adaptado: un periodo de seno por bit (pulso bifásico suave). */
    float mf[MF_TAPS];
    for (int i = 0; i < MF_TAPS; i++)
        mf[i] = (i + 0.5 < SPS) ? (float)sin(2.0 * M_PI * (i + 0.5) / SPS) : 0.0f;
    ok |= fir_f_init(&r->mf, mf, MF_TAPS, 1);
    costas_init(&r->cos, 2, 40.0 / FS_MIX, 0.707);
    r->ss = symsync_create(TED_GARDNER, SLICE_BPSK, SPS, 0.004, 0.02, 1.0);
    if (ok < 0 || !r->ss) {
        snprintf(err, errlen, "no se pudo crear la cadena DSP");
        rds_destroy(r);
        return NULL;
    }
    r->amp = 1.0f;
    r->last_pulse_pos = -1000000;
    state_reset(r);
    r->last_group_t = -1e18;
    return r;
}

const decoder_desc_t DEC_RDS = {
    .id = "rds",
    .name = "RDS",
    .group = "Radiodifusión",
    .desc = "Datos RDS de la FM comercial (PI, PS, PTY, RT, CT, AF).",
    .kind = DEC_NARROW,
    .rate = 250000,
    .bw_hz = 200000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {100300000LL, 0LL, 0LL, 0LL},
    .create = rds_create,
    .destroy = rds_destroy,
    .process = rds_process,
    .state_json = rds_state,
};
