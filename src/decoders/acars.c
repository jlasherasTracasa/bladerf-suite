/*
 * acars.c — Decodificador ACARS en VHF (ARINC 618). Ver docs/decoders/acars.md.
 *
 * Capa física. ACARS viaja en canales de voz de 25 kHz (131,525; 131,725;
 * 131,825 MHz…) con AM de baja profundidad (~30-50 %) cuya señal moduladora es
 * un MSK a 2400 bit/s: un "1" son 1200 Hz y un "0" 2400 Hz, con fase continua
 * (índice de modulación h = Δf·T = 1200·(1/2400) = 0,5, de ahí "MSK").
 *
 * Cadena de demodulación (a 48 kHz = 20 muestras por bit):
 *   1. Envolvente AM  a[n] = |x[n]|/media − 1. Es inmune al desvío de la
 *      portadora (|x·e^{jωn}| = |x|), por eso tolera ±¼ del canal.
 *   2. Se baja el audio 1800 Hz (centro de 1200/2400): el MSK queda como un FSK
 *      complejo de ±600 Hz alrededor de 0 Hz; un paso bajo (1500 Hz) quita la
 *      imagen en −3600 Hz y el ruido fuera de banda.
 *   3. Discriminador de fase promediado en un bit: la fase gira ±π/2 por bit
 *      (2π·600/2400). s[n] = Σ_{k<20} y[n−k]·conj(y[n−k−1]); Im(s) > 0 ⇒ 2400 Hz
 *      ⇒ bit 0; Im(s) < 0 ⇒ 1200 Hz ⇒ bit 1. Es el filtro adaptado no coherente
 *      y su paso por cero marca la frontera de bit (retrasada 10 muestras).
 *   4. Reloj de bit: un DLL empuja un contador de fase hacia el centro del bit
 *      cada vez que s cambia de signo. Con 100 ppm de error de reloj la deriva es
 *      de 0,2 bit en una trama larga, que el DLL absorbe de sobra.
 *
 * Capa de enlace. Se busca "+*" SYN SYN SOH (40 bits, hasta 3 errores, en ambas
 * polaridades por si la asignación 1200/2400 → bit estuviera invertida). Luego se
 * leen caracteres (8 bits, LSB primero) hasta ETX/ETB + BCS(2) + DEL, se
 * comprueba el BCS (CRC-16/KERMIT) y la paridad impar, y se interpreta con
 * acars_blk_parse(). Solo se emite lo que pasa el BCS.
 *
 * Limitaciones: sin corrección de un error de paridad con ayuda del BCS, sin
 * reensamblado de bloques ETB y sin vectores reales (ver el .md).
 */
#include "decoder.h"
#include "acars_blk.h"
#include "../dsp/bits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 48000.0
#define SPB 20              /* muestras por bit */
#define NTAPS 41
#define MAX_CHARS 280
#define MAX_AC 256
#define AC_TTL 900.0        /* s que se recuerda una aeronave */
#define MSDD_N 4            /* bits de la ventana de decisión coherente */
#define SYNC_BITS 40
#define SYNC_ERR 3

typedef struct {
    char reg[8];
    char flight[7];
    char label[3];
    char last[120];
    int msgs;
    double last_t;
} ac_t;

typedef struct {
    /* envolvente */
    float env_mean;
    /* mezcla a 1800 Hz */
    double ph, dph;
    /* paso bajo complejo: línea de retardo duplicada */
    float taps[NTAPS];
    cf32 hist[2 * NTAPS];
    int hpos;
    cf32 yprev;
    /* suma deslizante del discriminador (ventana de 20) */
    cf32 win[SPB];
    int wpos;
    cf32 sum;
    int since_renorm;
    /* reloj de bit */
    float cnt;
    int last_sign;
    /* detección coherente por ventana de varios bits (MSDD) */
    cf32 ramp[SPB];         /* e^{-jπ/2·(m+½)/20}: referencia de un bit a +600 Hz */
    cf32 yring[SPB];
    int ypos;
    cf32 cbit[MSDD_N][2];   /* correladores [bit reciente][0: +600 Hz, 1: −600 Hz] */
    int ncbit;
    /* nivel (potencia media del canal, constante lenta) */
    float pwr;
    /* enlace */
    int prev_b;             /* bit anterior de datos (decodificación diferencial) */
    uint64_t sr;            /* últimos bits, el más reciente en el LSB */
    uint64_t pat, pat_inv;
    int mode;               /* 0 = buscando, 1 = recibiendo */
    int inv;
    int nbits, curbyte;
    uint8_t chars[MAX_CHARS];
    int nch;
    int tail;               /* caracteres que faltan tras ETX/ETB (-1 = aún sin ETX) */
    /* estadísticas y tabla */
    unsigned long ok, bad;
    ac_t ac[MAX_AC];
    int nac;
    sb_t evsb;
    double now;
} acars_t;

static void *acars_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - FS) > 1.0) {
        snprintf(err, errlen, "ACARS requiere 48 kHz");
        return NULL;
    }
    acars_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    /* Paso bajo de ventana de Hamming, corte 1500 Hz */
    double fc = 1500.0 / FS, sum = 0;
    for (int i = 0; i < NTAPS; i++) {
        double m = i - (NTAPS - 1) / 2.0;
        double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        double w = 0.54 - 0.46 * cos(2 * M_PI * i / (NTAPS - 1));
        s->taps[i] = (float)(sinc * w);
        sum += s->taps[i];
    }
    for (int i = 0; i < NTAPS; i++)
        s->taps[i] /= (float)sum;
    s->dph = 2.0 * M_PI * 1800.0 / FS;
    for (int m = 0; m < SPB; m++) {
        double a = -(M_PI / 2.0) * (m + 0.5) / SPB;
        s->ramp[m] = (cf32){(float)cos(a), (float)sin(a)};
    }
    s->env_mean = 0.0f;
    /* Patrón en orden de transmisión (primer bit en el MSB del campo de 40 bits) */
    static const uint8_t ch[5] = {0x2B, 0x2A, ACARS_SYN, ACARS_SYN, ACARS_SOH};
    uint64_t p = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t c = acars_add_parity(ch[i]);
        for (int b = 0; b < 8; b++)
            p = (p << 1) | ((c >> b) & 1);
    }
    s->pat = p;
    s->pat_inv = ~p & ((1ULL << SYNC_BITS) - 1);
    s->cnt = 0;
    return s;
}

static void acars_destroy(void *st)
{
    acars_t *s = st;
    if (!s)
        return;
    sb_free(&s->evsb);
    free(s);
}

static void note_ac(acars_t *s, const acars_blk_t *b, const char *summary)
{
    ac_t *a = NULL;
    for (int i = 0; i < s->nac; i++)
        if (!strcmp(s->ac[i].reg, b->reg))
            a = &s->ac[i];
    if (!a) {
        if (s->nac < MAX_AC)
            a = &s->ac[s->nac++];
        else { /* sustituye la más antigua */
            a = &s->ac[0];
            for (int i = 1; i < s->nac; i++)
                if (s->ac[i].last_t < a->last_t)
                    a = &s->ac[i];
        }
        memset(a, 0, sizeof *a);
        snprintf(a->reg, sizeof a->reg, "%s", b->reg);
    }
    if (b->flight[0])
        snprintf(a->flight, sizeof a->flight, "%s", b->flight);
    snprintf(a->label, sizeof a->label, "%s", b->label);
    snprintf(a->last, sizeof a->last, "%.119s", summary);
    a->msgs++;
    a->last_t = s->now;
}

static void flat(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    for (; *src && j + 1 < cap; src++)
        dst[j++] = (*src < 32 || *src == 127) ? ' ' : *src;
    dst[j] = 0;
}

/* Un bloque completo: chars[0..nch-1] = Modo … ETX/ETB, BCS1, BCS2 [, DEL] (con paridad). */
static void frame_done(acars_t *s, dec_emit_fn emit, void *ctx)
{
    int n = s->nch;
    int e = -1;
    for (int i = 0; i < n; i++) {
        uint8_t c = s->chars[i] & 0x7F;
        if (c == ACARS_ETX || c == ACARS_ETB) {
            e = i;
            break;
        }
    }
    if (e < 12 || n < e + 3) { /* faltan los 2 bytes del BCS */
        s->bad++;
        return;
    }
    /* BCS: CRC sobre Modo…ETX con los 8 bits recibidos; se admite también sin paridad */
    uint16_t rx = (uint16_t)(s->chars[e + 1] | (s->chars[e + 2] << 8));
    int parity_ok = 1;
    uint8_t plain[MAX_CHARS];
    for (int i = 0; i <= e; i++) {
        plain[i] = s->chars[i] & 0x7F;
        if (!bits_parity32(s->chars[i]))
            parity_ok = 0;
    }
    bool crc_raw = acars_bcs(s->chars, e + 1) == rx;
    bool crc_strip = !crc_raw && acars_bcs(plain, e + 1) == rx;
    if (!crc_raw && !crc_strip) {
        s->bad++;
        return;
    }
    acars_blk_t b;
    if (acars_blk_parse(plain, e + 1, &b) < 0) {
        s->bad++;
        return;
    }
    s->ok++;
    char body[300], line[420];
    flat(body, sizeof body, b.text);
    snprintf(line, sizeof line, "%s%s%s %s %s%s%s: %s", b.reg, b.flight[0] ? " " : "", b.flight, b.downlink ? "A>G" : "G>A",
             b.label, b.msgno[0] ? " #" : "", b.msgno, body);
    float level = power_to_db(s->pwr);
    sb_t *o = &s->evsb;
    sb_clear(o);
    sb_puts(o, "{\"proto\":\"acars\",\"mode\":");
    char m[2] = {b.mode, 0};
    sb_json_str(o, m);
    sb_puts(o, ",\"reg\":");
    sb_json_str(o, b.reg);
    sb_puts(o, ",\"dir\":");
    sb_json_str(o, b.downlink ? "down" : "up");
    sb_puts(o, ",\"tak\":");
    if ((uint8_t)b.tak == ACARS_NAK)
        sb_json_str(o, "NAK");
    else {
        char t[2] = {b.tak, 0};
        sb_json_str(o, t);
    }
    sb_puts(o, ",\"label\":");
    sb_json_str(o, b.label);
    sb_puts(o, ",\"block\":");
    char bi[2] = {b.bid, 0};
    sb_json_str(o, bi);
    sb_puts(o, ",\"msgno\":");
    sb_json_str(o, b.msgno);
    sb_puts(o, ",\"flight\":");
    sb_json_str(o, b.flight);
    sb_printf(o, ",\"end\":\"%s\",\"parity_ok\":%s,\"body\":", b.etb ? "ETB" : "ETX", parity_ok ? "true" : "false");
    sb_json_str(o, b.text);
    sb_puts(o, ",\"level_db\":");
    sb_json_num(o, level, 1);
    sb_puts(o, ",\"text\":");
    sb_json_str(o, line);
    sb_puts(o, "}");
    emit(ctx, o->data, o->len);
    note_ac(s, &b, line);
}

/* Entrada de un bit ya decidido. */
static void on_bit(acars_t *s, int tone, dec_emit_fn emit, void *ctx)
{
    /* Codificación diferencial (validada con una grabación real, docs/VALIDACION.md):
     * 2400 Hz = el bit de datos es igual al anterior; 1200 Hz = cambia. Así el
     * "pre-key" de unos es un tono fijo de 2400 Hz y el "+*" (1101 0101 0101 0100,
     * casi todo alternancias) un tren de 1200 Hz. 'tone' vale 1 para 1200 Hz. */
    int bit = s->prev_b ^ tone;
    s->prev_b = bit;
    s->sr = (s->sr << 1) | (uint64_t)bit;
    if (s->mode == 0) {
        uint64_t w = s->sr & ((1ULL << SYNC_BITS) - 1);
        if (bits_popcount64(w ^ s->pat) <= SYNC_ERR) {
            s->inv = 0;
        } else if (bits_popcount64(w ^ s->pat_inv) <= SYNC_ERR) {
            s->inv = 1;
        } else
            return;
        s->mode = 1;
        s->nbits = 0;
        s->curbyte = 0;
        s->nch = 0;
        s->tail = -1;
        return;
    }
    bit ^= s->inv;
    s->curbyte |= bit << s->nbits;
    if (++s->nbits < 8)
        return;
    uint8_t c = (uint8_t)s->curbyte;
    s->nbits = 0;
    s->curbyte = 0;
    s->chars[s->nch++] = c;
    if (s->tail < 0) {
        uint8_t k = c & 0x7F;
        if (s->nch > 12 && (k == ACARS_ETX || k == ACARS_ETB))
            s->tail = 3; /* BCS1, BCS2, DEL */
        else if (s->nch >= MAX_CHARS - 4) {
            s->mode = 0;
            s->bad++;
        }
        return;
    }
    /* tail: 3 → BCS1 → 2 → BCS2 → 1: ya se puede validar (el DEL final no hace falta) */
    if (--s->tail == 1) {
        frame_done(s, emit, ctx);
        s->mode = 0;
        s->sr = 0;
    }
}

/* Detección coherente de secuencia (MSDD). Por cada bit se calculan dos
 * correlaciones del tramo de 20 muestras con la rampa de fase de un "0" (+600 Hz,
 * la fase sube π/2) y de un "1" (−600 Hz, baja π/2):
 *     c_±(k) = Σ_m y[m]·e^{−j(±π/2)(m+½)/20}
 * Para una hipótesis (b_0…b_{N−1}) de N bits la fase al inicio del bit i es
 * Φ_i = (π/2)·Σ_{j<i} b_j, y la métrica es |Σ_i c_{b_i}(i)·e^{−jΦ_i}|: integra
 * coherentemente N bits sin necesitar la fase de portadora (se descarta con el
 * módulo). Con N = 4 recupera casi los 3 dB que el detector no coherente de un
 * solo bit (discriminador) pierde frente al MSK coherente. Se decide el bit nº 1
 * de la ventana (los centrales tienen contexto a ambos lados). */
static int msdd_decide(const acars_t *s)
{
    static const float er[4] = {1, 0, -1, 0}, ei[4] = {0, -1, 0, 1}; /* e^{−jπ/2·q} */
    float best = -1;
    int bestb = 0;
    for (int h = 0; h < (1 << MSDD_N); h++) {
        int q = 0;
        float sr = 0, si = 0;
        for (int i = 0; i < MSDD_N; i++) {
            int b = (h >> i) & 1;      /* 1 → bit "1" → −600 Hz */
            cf32 c = s->cbit[i][b];
            sr += c.re * er[q] - c.im * ei[q];
            si += c.re * ei[q] + c.im * er[q];
            q = (q + (b ? 3 : 1)) & 3;
        }
        float m = sr * sr + si * si;
        if (m > best) {
            best = m;
            bestb = (h >> 1) & 1;
        }
    }
    return bestb;
}

static void bit_correlators(acars_t *s)
{
    cf32 cp = {0, 0}, cm = {0, 0};
    for (int m = 0; m < SPB; m++) {
        cf32 y = s->yring[(s->ypos + m) % SPB]; /* la más antigua primero */
        cf32 r = s->ramp[m];
        cp.re += y.re * r.re - y.im * r.im;
        cp.im += y.re * r.im + y.im * r.re;
        cm.re += y.re * r.re + y.im * r.im;     /* conj(r) */
        cm.im += y.im * r.re - y.re * r.im;
    }
    memmove(&s->cbit[0], &s->cbit[1], sizeof(s->cbit[0]) * (MSDD_N - 1));
    s->cbit[MSDD_N - 1][0] = cp;
    s->cbit[MSDD_N - 1][1] = cm;
    if (s->ncbit < MSDD_N)
        s->ncbit++;
}

static void acars_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    acars_t *s = st;
    s->now = now;
    for (int i = 0; i < n; i++) {
        float p = iq[i].re * iq[i].re + iq[i].im * iq[i].im;
        float e = sqrtf(p);
        s->pwr += 2e-4f * (p - s->pwr);
        if (s->env_mean == 0.0f)
            s->env_mean = e;
        s->env_mean += 2e-3f * (e - s->env_mean);
        float a = s->env_mean > 1e-6f ? e / s->env_mean - 1.0f : 0.0f;
        /* mezcla a banda base (−1800 Hz) */
        float c = (float)cos(s->ph), sn = (float)sin(s->ph);
        s->ph += s->dph;
        if (s->ph > 2 * M_PI)
            s->ph -= 2 * M_PI;
        cf32 m = {a * c, -a * sn};
        /* FIR (línea de retardo duplicada) */
        s->hist[s->hpos] = s->hist[s->hpos + NTAPS] = m;
        const cf32 *h = &s->hist[s->hpos + 1];
        float yr = 0, yi = 0;
        for (int k = 0; k < NTAPS; k++) {
            yr += s->taps[k] * h[k].re;
            yi += s->taps[k] * h[k].im;
        }
        if (++s->hpos == NTAPS)
            s->hpos = 0;
        cf32 y = {yr, yi};
        s->yring[s->ypos] = y;
        if (++s->ypos == SPB)
            s->ypos = 0;
        /* discriminador: y[n]·conj(y[n−1]) y suma deslizante de 20 */
        cf32 d = {y.re * s->yprev.re + y.im * s->yprev.im, y.im * s->yprev.re - y.re * s->yprev.im};
        s->yprev = y;
        s->sum.re += d.re - s->win[s->wpos].re;
        s->sum.im += d.im - s->win[s->wpos].im;
        s->win[s->wpos] = d;
        if (++s->wpos == SPB)
            s->wpos = 0;
        if (++s->since_renorm >= 4096) { /* evita la deriva del acumulador en coma flotante */
            s->since_renorm = 0;
            cf32 t = {0, 0};
            for (int k = 0; k < SPB; k++) {
                t.re += s->win[k].re;
                t.im += s->win[k].im;
            }
            s->sum = t;
        }
        float v = s->sum.im;
        int sign = v >= 0 ? 1 : -1;
        /* DLL: el cruce por cero ocurre 10 muestras después de la frontera de bit; el
         * instante óptimo de decisión es 10 muestras tras el cruce. */
        if (sign != s->last_sign && s->last_sign != 0) {
            float err = s->cnt - (SPB / 2);   /* debería valer 10 en el cruce */
            if (err > SPB / 2)
                err -= SPB;
            if (err < -SPB / 2)
                err += SPB;
            s->cnt -= 0.15f * err;
        }
        s->last_sign = sign;
        s->cnt += 1.0f;
        if (s->cnt >= SPB) {
            s->cnt -= SPB;
            /* bit 0 ⇔ 2400 Hz ⇔ fase sube. Las decisiones salen con 2 bits de retardo. */
            bit_correlators(s);
            if (s->ncbit == MSDD_N)
                on_bit(s, msdd_decide(s), emit, ctx);
        }
    }
}

static void acars_state(void *st, sb_t *sb, double now)
{
    acars_t *s = st;
    int w = 0;
    for (int i = 0; i < s->nac; i++)
        if (now - s->ac[i].last_t <= AC_TTL)
            s->ac[w++] = s->ac[i];
    s->nac = w;
    sb_printf(sb, "{\"msgs\":%lu,\"bad\":%lu,\"ac\":[", s->ok, s->bad);
    for (int i = 0; i < s->nac; i++) {
        const ac_t *a = &s->ac[i];
        if (i)
            sb_puts(sb, ",");
        sb_puts(sb, "{\"reg\":");
        sb_json_str(sb, a->reg);
        sb_puts(sb, ",\"flight\":");
        sb_json_str(sb, a->flight);
        sb_puts(sb, ",\"label\":");
        sb_json_str(sb, a->label);
        sb_printf(sb, ",\"msgs\":%d,\"age\":%.0f,\"last\":", a->msgs, now - a->last_t);
        sb_json_str(sb, a->last);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_ACARS = {
    .id = "acars",
    .name = "ACARS",
    .group = "Aviación",
    .desc = "Mensajes ACARS (AM + MSK 2400) en VHF aeronáutica.",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 20000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {131525000LL, 131725000LL, 131825000LL, 0LL},
    .create = acars_create,
    .destroy = acars_destroy,
    .process = acars_process,
    .state_json = acars_state,
};
