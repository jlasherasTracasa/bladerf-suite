/*
 * dsc.c — Decodificador DSC (llamada selectiva digital) VHF, canal 70.
 *
 * Fuentes: ITU-R M.493 (formato de la llamada) y M.541; canal 70 = 156,525
 * MHz, FM de banda estrecha con subportadoras FSK 1300/2100 Hz a 1200 Bd
 * (FFSK de fase continua, "sub-tono" en el audio).
 *
 * Estructura de la señal (valores confirmados contra el decodificador libre
 * CuteSDR, fskdemod.cpp, salvo indicación):
 *   - Cada símbolo = 10 bits: 7 de información (LSB primero) + 3 bits de
 *     comprobación con el nº de CEROS de los 7 (ITU-R M.493 §3). El orden de
 *     los 3 bits no está verificado: se aceptan ambos (LSB o MSB primero). Para
 *     una codificación segura: los símbolos con 0 o 7 ceros son simétricos.
 *   - Patrón de puntos (dot pattern): bits alternos para sincronizar el reloj.
 *   - Secuencia de fase: posiciones DX con el símbolo 125 y posiciones RX con
 *     111,110,…,104.
 *   - Diversidad temporal: cada símbolo se envía dos veces, DX y RX, con 4
 *     símbolos entre ambas copias (el DX va en los huecos pares, el RX en los
 *     impares; la copia RX del símbolo del hueco 2k está en el 2k+5).
 *   - EOS: 117 (acuse pedido), 122 (acuse), 127 (fin), seguido del carácter de
 *     control de error (ECC) = XOR de los símbolos desde el especificador de
 *     formato hasta el EOS (también confirmado en CuteSDR).
 *   - Especificadores: 112 socorro, 114 grupo, 116 todos los barcos, 120
 *     individual, 102 área geográfica, 123 individual automático (los seis
 *     coinciden con GopherTrunk/CuteSDR). Categorías, naturaleza del socorro,
 *     telemandos, codificación de posición/hora y el formato de la llamada de
 *     socorro son de la norma pero SIN verificar contra el texto (el PDF no
 *     pudo leerse): se dicen en docs/decoders/dsc.md.
 *   - Dirección/ID propio: 5 símbolos de 2 dígitos decimales (10 dígitos: MMSI
 *     de 9 y un 0 final). Posición: 5 símbolos = cuadrante(0 NE,1 NO,2 SE,3 SO)
 *     + grados lat (2) + min lat (2) + grados lon (3) + min lon (2); 99… =
 *     desconocida. Hora UTC: 2 símbolos = hhmm; 8888 = no disponible.
 *   - La polaridad de tonos (qué tono es el "1") no está verificada: el
 *     decodificador prueba ambas y se queda con la que ve la secuencia de fase.
 *
 * Cadena DSP: discriminador FM → paso alto/bajo → correladores de 1300/2100 Hz
 * (afsk.h, dos ganancias en paralelo) → bits → 10 alineaciones de símbolo × 2
 * polaridades en paralelo (la alineación correcta es la que ve 125/111-104)
 * → des-intercalado DX/RX con ECC → mensaje.
 */
#include "dsc.h"

#include "afsk.h"
#include "decoder.h"
#include "../dsp/filter.h"
#include "../dsp/pulse.h"
#include "../dsp/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS_HZ 48000.0
#define NSL 2
#define RING 512
#define MAXSLOT 360
#define EVENT_TTL 3600.0
#define MAX_CALLS 64

/* ======================================================================= */
/*  Símbolos                                                               */
/* ======================================================================= */
int dsc_sym_decode(const uint8_t *b)
{
    int v = 0, ones = 0;
    for (int i = 0; i < 7; i++) {
        v |= (b[i] & 1) << i;
        ones += b[i] & 1;
    }
    int zeros = 7 - ones;
    int lsb = (b[7] & 1) | ((b[8] & 1) << 1) | ((b[9] & 1) << 2);
    int msb = ((b[7] & 1) << 2) | ((b[8] & 1) << 1) | (b[9] & 1);
    return (lsb == zeros || msb == zeros) ? v : -1;
}

void dsc_sym_encode(int v, uint8_t *b)
{
    int ones = 0;
    for (int i = 0; i < 7; i++) {
        b[i] = (uint8_t)((v >> i) & 1);
        ones += b[i];
    }
    int zeros = 7 - ones;
    b[7] = (uint8_t)(zeros & 1);
    b[8] = (uint8_t)((zeros >> 1) & 1);
    b[9] = (uint8_t)((zeros >> 2) & 1);
}

int dsc_ecc(const int *sym, int n)
{
    int e = 0;
    for (int i = 0; i < n; i++)
        e ^= sym[i];
    return e;
}

const char *dsc_format_name(int fs)
{
    switch (fs) {
    case 112: return "socorro";
    case 114: return "grupo";
    case 116: return "todos los barcos";
    case 120: return "individual";
    case 102: return "área geográfica";
    case 123: return "individual automático";
    }
    return "desconocido";
}

const char *dsc_category_name(int c)
{
    switch (c) {
    case 100: return "rutina";
    case 108: return "seguridad";
    case 110: return "urgencia";
    case 112: return "socorro";
    }
    return "";
}

const char *dsc_nature_name(int n)
{
    static const char *t[] = {"incendio/explosión", "inundación", "colisión", "varada", "escora/peligro de vuelco",
                              "hundimiento", "a la deriva sin gobierno", "no designado", "abandono del buque",
                              "piratería/robo a mano armada", "hombre al agua", "", "emisión de RLS"};
    return (n >= 100 && n <= 112) ? t[n - 100] : "";
}

const char *dsc_telecommand_name(int t)
{
    switch (t) {
    case 100: return "F3E/G3E telefonía todos los modos";
    case 101: return "F3E/G3E telefonía dúplex";
    case 103: return "sondeo";
    case 104: return "imposible cumplir";
    case 105: return "fin de llamada";
    case 106: return "datos";
    case 107: return "J3E telefonía";
    case 109: return "acuse de socorro";
    case 110: return "retransmisión de socorro";
    case 111: return "TTY-FEC";
    case 113: return "TTY-ARQ";
    case 118: return "prueba";
    case 121: return "petición de posición";
    case 126: return "sin información";
    }
    return "";
}

/* 5 símbolos → 10 dígitos. Devuelve false si algún valor > 99. */
static bool digits10(const int *s, char *out)
{
    for (int i = 0; i < 5; i++) {
        if (s[i] < 0 || s[i] > 99)
            return false;
        out[2 * i] = (char)('0' + s[i] / 10);
        out[2 * i + 1] = (char)('0' + s[i] % 10);
    }
    out[10] = 0;
    return true;
}

static void parse_coords(const int *s, dsc_msg_t *m)
{
    char d[12];
    if (!digits10(s, d) || !strcmp(d, "9999999999") || d[0] > '3')
        return;
    int q = d[0] - '0';
    int latd = (d[1] - '0') * 10 + d[2] - '0', latm = (d[3] - '0') * 10 + d[4] - '0';
    int lond = (d[5] - '0') * 100 + (d[6] - '0') * 10 + d[7] - '0', lonm = (d[8] - '0') * 10 + d[9] - '0';
    if (latd > 90 || latm > 59 || lond > 180 || lonm > 59)
        return;
    m->lat = latd + latm / 60.0;
    m->lon = lond + lonm / 60.0;
    if (q == 2 || q == 3)
        m->lat = -m->lat;
    if (q == 1 || q == 3)
        m->lon = -m->lon;
    m->pos_valid = true;
}

static void parse_time(const int *s, dsc_msg_t *m)
{
    if (s[0] < 0 || s[0] > 99 || s[1] < 0 || s[1] > 99)
        return;
    int hh = s[0], mm = s[1];
    if (hh <= 23 && mm <= 59) {
        m->hour = hh;
        m->minute = mm;
        m->time_valid = true;
    }
}

int dsc_parse_message(const int *s, int n, dsc_msg_t *m)
{
    memset(m, 0, sizeof *m);
    m->category = m->nature = m->tc1 = m->tc2 = m->subsequent = -1;
    if (n < 4)
        return -1;
    m->fs = s[0];
    m->eos = s[n - 2];
    int p = 1, end = n - 2; /* [p, end) = campos antes del EOS */
    char d[12];
    if (m->fs == 112) {
        if (end - p < 5 + 1 + 5 + 2)
            return -1;
        if (!digits10(s + p, d))
            return -1;
        snprintf(m->from, sizeof m->from, "%.9s", d);
        p += 5;
        m->nature = s[p++];
        parse_coords(s + p, m);
        p += 5;
        parse_time(s + p, m);
        p += 2;
        if (p < end)
            m->subsequent = s[p++];
        m->category = 112;
    } else {
        if (m->fs != 114 && m->fs != 116 && m->fs != 120 && m->fs != 102 && m->fs != 123)
            return -1;
        if (m->fs != 116) {
            if (end - p < 5 || !digits10(s + p, d))
                return -1;
            snprintf(m->to, sizeof m->to, "%.*s", m->fs == 102 ? 10 : 9, d); /* el área usa los 10 dígitos */
            p += 5;
        }
        if (end - p < 1 + 5 + 2)
            return -1;
        m->category = s[p++];
        if (!digits10(s + p, d))
            return -1;
        snprintf(m->from, sizeof m->from, "%.9s", d);
        p += 5;
        m->tc1 = s[p++];
        m->tc2 = s[p++];
        /* socorro retransmitido o acusado: MMSI, naturaleza, posición, hora, comunicación */
        if ((m->tc1 == 109 || m->tc1 == 110) && end - p >= 5 + 1 + 5 + 2 && digits10(s + p, d)) {
            snprintf(m->dist_mmsi, sizeof m->dist_mmsi, "%.9s", d);
            p += 5;
            m->nature = s[p++];
            parse_coords(s + p, m);
            p += 5;
            parse_time(s + p, m);
            p += 2;
            if (p < end)
                m->subsequent = s[p++];
        }
    }
    while (p < end && m->next < 16)
        m->ext[m->next++] = s[p++];
    return 0;
}

/* ======================================================================= */
/*  Receptor                                                               */
/* ======================================================================= */
typedef struct {
    int16_t ring[RING];   /* símbolos del hueco t % RING (−1 = erróneo) */
    long t;               /* nº de huecos recibidos */
    bool active;
    long s0;              /* hueco DX del primer símbolo (especificador de formato) */
    long cooldown;
} tracker_t;

typedef struct {
    afsk_t *demod;
    uint8_t hist[10];     /* últimos 10 bits */
    long nbit;
    tracker_t tr[10][2];  /* alineación × polaridad */
} dsl_t;

typedef struct {
    double t;
    dsc_msg_t m;
    char fmt[24];
} call_t;

typedef struct {
    fmdisc_t disc;
    fir_f_t lpf;
    float hp;
    float *tmp, *tmp2;
    int cap;
    dsl_t sl[NSL];
    int cur;
    call_t calls[MAX_CALLS];
    int ncalls;
    unsigned long total;
    uint32_t last_hash;
    long last_n, nsamp;
    double blk_now;
    bool loc_valid;
    double lat, lon;
    sb_t ev;
    dec_emit_fn emit;
    void *ctx;
} dsc_t;

static void jstr(sb_t *e, const char *k, const char *v)
{
    sb_printf(e, ",\"%s\":", k);
    sb_json_str(e, v);
}

static void emit_call(dsc_t *d, const dsc_msg_t *m, const int *sym, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ (uint32_t)sym[i]) * 16777619u;
    if (h == d->last_hash && d->nsamp - d->last_n < (long)(8 * FS_HZ))
        return; /* misma llamada vista por otro demodulador/alineación */
    d->last_hash = h;
    d->last_n = d->nsamp;

    sb_t *e = &d->ev;
    sb_clear(e);
    sb_printf(e, "{\"fs\":%d", m->fs);
    jstr(e, "format", dsc_format_name(m->fs));
    if (m->category >= 0) {
        sb_printf(e, ",\"category\":%d", m->category);
        jstr(e, "category_name", dsc_category_name(m->category));
    }
    if (m->from[0])
        jstr(e, "mmsi", m->from);
    if (m->to[0])
        jstr(e, "to", m->to);
    if (m->dist_mmsi[0])
        jstr(e, "distress_mmsi", m->dist_mmsi);
    if (m->nature >= 0) {
        sb_printf(e, ",\"nature\":%d", m->nature);
        jstr(e, "nature_name", dsc_nature_name(m->nature));
    }
    if (m->pos_valid) {
        sb_puts(e, ",\"lat\":");
        sb_json_num(e, m->lat, 4);
        sb_puts(e, ",\"lon\":");
        sb_json_num(e, m->lon, 4);
    }
    if (m->time_valid)
        sb_printf(e, ",\"utc\":\"%02d:%02d\"", m->hour, m->minute);
    if (m->tc1 >= 0) {
        sb_printf(e, ",\"tc1\":%d,\"tc2\":%d", m->tc1, m->tc2);
        jstr(e, "tc1_name", dsc_telecommand_name(m->tc1));
    }
    if (m->subsequent >= 0)
        sb_printf(e, ",\"subsequent\":%d", m->subsequent);
    sb_printf(e, ",\"eos\":%d,\"ext\":[", m->eos);
    for (int i = 0; i < m->next; i++)
        sb_printf(e, "%s%d", i ? "," : "", m->ext[i]);
    sb_puts(e, "],\"symbols\":[");
    for (int i = 0; i < n; i++)
        sb_printf(e, "%s%d", i ? "," : "", sym[i]);
    char text[240], pos[64] = "";
    if (m->pos_valid)
        snprintf(pos, sizeof pos, " pos %.3f,%.3f", m->lat, m->lon);
    snprintf(text, sizeof text, "DSC %s%s%s MMSI %s%s%s%s%s%s", dsc_format_name(m->fs),
             m->category >= 0 && m->fs != 112 ? " / " : "", m->fs != 112 ? dsc_category_name(m->category) : "", m->from,
             m->to[0] ? " → " : "", m->to, m->nature >= 0 ? " " : "", m->nature >= 0 ? dsc_nature_name(m->nature) : "", pos);
    sb_puts(e, "]");
    jstr(e, "text", text);
    sb_puts(e, "}");
    if (d->ncalls >= MAX_CALLS) {
        memmove(d->calls, d->calls + 1, sizeof(call_t) * (MAX_CALLS - 1));
        d->ncalls--;
    }
    call_t *c = &d->calls[d->ncalls++];
    c->t = d->blk_now + (double)d->nsamp / FS_HZ;
    c->m = *m;
    d->total++;
    if (d->emit)
        d->emit(d->ctx, e->data, e->len);
}

/* Intenta montar el mensaje con los símbolos ya recibidos. 1 = emitido,
 * 0 = faltan símbolos, −1 = abortar. DX en el hueco s0+2i, RX en s0+2i+5. */
static int assemble(dsc_t *d, tracker_t *t)
{
    long k = t->t - 1;
    int sym[DSC_MAX_SYM + 2], alt[DSC_MAX_SYM + 2];
    int e = -1;
    for (int i = 0; i < DSC_MAX_SYM; i++) {
        long dx = t->s0 + 2 * i, rx = dx + 5;
        if (dx > k)
            return 0;
        int vdx = t->ring[dx % RING], vrx = rx <= k ? t->ring[rx % RING] : -2;
        if (vdx < 0 && vrx == -2)
            return 0; /* esperar a la copia RX */
        sym[i] = vdx >= 0 ? vdx : vrx;
        alt[i] = (vdx >= 0 && vrx >= 0 && vrx != vdx) ? vrx : -1;
        if (sym[i] < 0)
            return -1;
        if (i >= 8 && (sym[i] == 117 || sym[i] == 122 || sym[i] == 127)) {
            e = i;
            break;
        }
    }
    if (e < 0)
        return -1;
    /* carácter de control ECC (hueco e+1) con su copia RX */
    long dx = t->s0 + 2 * (e + 1), rx = dx + 5;
    if (dx > k || (rx > k))
        return 0;
    int vdx = t->ring[dx % RING], vrx = t->ring[rx % RING];
    int eccs[2] = {vdx, vrx}, necc = (vdx >= 0 && vrx >= 0 && vdx != vrx) ? 2 : 1;
    if (vdx < 0)
        eccs[0] = vrx;
    if (eccs[0] < 0)
        return -1;
    /* ambigüedades DX≠RX: se prueban combinaciones hasta que el ECC cuadre */
    int amb[8], na = 0;
    for (int i = 0; i <= e && na < 8; i++)
        if (alt[i] >= 0)
            amb[na++] = i;
    for (int mask = 0; mask < (1 << na); mask++) {
        int tmp[DSC_MAX_SYM + 2];
        memcpy(tmp, sym, sizeof(int) * (size_t)(e + 1));
        for (int j = 0; j < na; j++)
            if (mask & (1 << j))
                tmp[amb[j]] = alt[amb[j]];
        int ecc = dsc_ecc(tmp, e + 1);
        for (int q = 0; q < necc; q++)
            if (eccs[q] == ecc) {
                tmp[e + 1] = ecc;
                dsc_msg_t m;
                if (dsc_parse_message(tmp, e + 2, &m) == 0) {
                    emit_call(d, &m, tmp, e + 2);
                    return 1;
                }
                return -1;
            }
    }
    return -1;
}

static void tracker_push(dsc_t *d, tracker_t *t, int v)
{
    t->ring[t->t % RING] = (int16_t)v;
    long k = t->t++;
    if (t->active) {
        int r = assemble(d, t);
        if (r != 0 || k - t->s0 > 2 * DSC_MAX_SYM + 12) {
            t->active = false;
            t->cooldown = 40;
        }
        return;
    }
    if (t->cooldown > 0) {
        t->cooldown--;
        return;
    }
    if (v != 104 || k < 16)
        return;
    int rxm = 0, dxm = 0;
    for (int j = 1; j <= 7; j++)
        rxm += t->ring[(k - 2 * j) % RING] == 104 + j;
    for (int m = 0; m < 6; m++)
        dxm += t->ring[(k - 5 - 2 * m + RING) % RING] == 125;
    if (rxm >= 3 && dxm >= 3) {
        t->active = true;
        t->s0 = k - 3;
    }
}

static void on_bit(void *ctx, int bit)
{
    dsc_t *d = ctx;
    dsl_t *s = &d->sl[d->cur];
    memmove(s->hist, s->hist + 1, 9);
    s->hist[9] = (uint8_t)bit;
    if (++s->nbit < 10)
        return;
    int al = (int)(s->nbit % 10);
    for (int pol = 0; pol < 2; pol++) {
        uint8_t b[10];
        for (int i = 0; i < 10; i++)
            b[i] = pol ? (uint8_t)(s->hist[i] ^ 1) : s->hist[i];
        tracker_push(d, &s->tr[al][pol], dsc_sym_decode(b));
    }
}

static void *dsc_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    dsc_t *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    fmdisc_init(&d->disc, FS_HZ, 2000.0);
    float h[41];
    fir_design_lowpass(h, 41, 2700.0 / FS_HZ, 5.0);
    bool ok = fir_f_init(&d->lpf, h, 41, 1) == 0;
    static const double gains[NSL] = {0.7, 1.4};
    for (int i = 0; ok && i < NSL; i++) {
        d->sl[i].demod = afsk_create(FS_HZ, 1200.0, 1300.0, 2100.0, gains[i], 0.75);
        ok = d->sl[i].demod != NULL;
    }
    if (!ok) {
        snprintf(err, errlen, "sin memoria");
        for (int i = 0; i < NSL; i++)
            afsk_destroy(d->sl[i].demod);
        free(d);
        return NULL;
    }
    if (cfg && cfg->loc_valid) {
        d->loc_valid = true;
        d->lat = cfg->lat;
        d->lon = cfg->lon;
    }
    return d;
}

static void dsc_destroy(void *st)
{
    dsc_t *d = st;
    if (!d)
        return;
    for (int i = 0; i < NSL; i++)
        afsk_destroy(d->sl[i].demod);
    fir_f_free(&d->lpf);
    free(d->tmp);
    free(d->tmp2);
    sb_free(&d->ev);
    free(d);
}

static void dsc_set_location(void *st, bool valid, double lat, double lon)
{
    dsc_t *d = st;
    d->loc_valid = valid;
    d->lat = lat;
    d->lon = lon;
}

static void dsc_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    dsc_t *d = st;
    if (n > d->cap) {
        d->cap = n + 4096;
        d->tmp = realloc(d->tmp, sizeof(float) * (size_t)d->cap);
        d->tmp2 = realloc(d->tmp2, sizeof(float) * (size_t)d->cap + 64);
    }
    d->emit = emit;
    d->ctx = ctx;
    d->blk_now = now - (double)d->nsamp / FS_HZ;
    fmdisc_process(&d->disc, iq, n, d->tmp);
    for (int i = 0; i < n; i++) { /* paso alto ≈ 70 Hz: quita el desvío de portadora */
        d->hp += 0.009f * (d->tmp[i] - d->hp);
        d->tmp[i] -= d->hp;
    }
    int m = fir_f_process(&d->lpf, d->tmp, n, d->tmp2);
    long n0 = d->nsamp;
    for (int s = 0; s < NSL; s++) {
        d->cur = s;
        d->nsamp = n0;
        afsk_process(d->sl[s].demod, d->tmp2, m, on_bit, d);
    }
    d->nsamp = n0 + m;
}

static void dsc_state_json(void *st, sb_t *sb, double now)
{
    dsc_t *d = st;
    int w = 0;
    for (int i = 0; i < d->ncalls; i++)
        if (now - d->calls[i].t < EVENT_TTL)
            d->calls[w++] = d->calls[i];
    d->ncalls = w;
    sb_printf(sb, "{\"calls_total\":%lu,\"count\":%d,\"calls\":[", d->total, d->ncalls);
    for (int i = d->ncalls - 1; i >= 0; i--) {
        const dsc_msg_t *m = &d->calls[i].m;
        if (i != d->ncalls - 1)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"fs\":%d,\"format\":", m->fs);
        sb_json_str(sb, dsc_format_name(m->fs));
        sb_puts(sb, ",\"mmsi\":");
        sb_json_str(sb, m->from);
        sb_puts(sb, ",\"to\":");
        sb_json_str(sb, m->to);
        sb_puts(sb, ",\"category\":");
        sb_json_str(sb, m->category >= 0 ? dsc_category_name(m->category) : "");
        sb_puts(sb, ",\"nature\":");
        sb_json_str(sb, m->nature >= 0 ? dsc_nature_name(m->nature) : "");
        if (m->pos_valid) {
            sb_puts(sb, ",\"lat\":");
            sb_json_num(sb, m->lat, 4);
            sb_puts(sb, ",\"lon\":");
            sb_json_num(sb, m->lon, 4);
        }
        sb_puts(sb, ",\"age\":");
        sb_json_num(sb, now - d->calls[i].t, 0);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_DSC = {
    .id = "dsc",
    .name = "DSC VHF",
    .group = "Marítimo",
    .desc = "Llamada selectiva digital en el canal 70 (FSK 1200).",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 16000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {156525000LL, 0LL, 0LL, 0LL},
    .create = dsc_create,
    .destroy = dsc_destroy,
    .process = dsc_process,
    .state_json = dsc_state_json,
    .set_location = dsc_set_location,
};
