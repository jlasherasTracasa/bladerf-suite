/*
 * pocsag.c — Decodificador POCSAG (512 / 1200 / 2400 bit/s). Ver docs/protocolos/pocsag.md.
 *
 * Cadena: IQ 48 kHz → FIR de canal → discriminador FM → tres receptores en
 * paralelo (uno por velocidad: sps = 93,75 / 40 / 20), cada uno con su filtro
 * adaptado (media por bit) y su reloj de Gardner (pocsag_fsk.h). La velocidad
 * se detecta "por casualidad": solo el receptor que va a la tasa correcta
 * encuentra la palabra de sincronía 0x7CD215D8.
 *
 * Polaridad. Según el equipo, el bit 1 es la desviación positiva o la negativa.
 * Se busca la sincronía y su complemento bit a bit; si aparece el complemento
 * se invierte el lote entero.
 *
 * Lote. Tras la sincronía vienen 16 palabras de 32 bits. De cada una se
 * corrigen hasta 2 errores con BCH(31,21) (g(x) = x^10+x^9+x^8+x^6+x^5+x^3+1)
 * y se comprueba la paridad par del bit 32: si tras corregir la paridad no
 * cuadra se cuenta un error más; con 2 correcciones + paridad mala son ≥ 3
 * errores y la palabra se descarta (no se emite nada basado en ella).
 *
 *   bit 31 = 0  dirección: RIC = (bits 30..13)<<3 | índice de trama (pos/2);
 *               bits 12..11 = función (0..3).
 *   bit 31 = 1  mensaje: 20 bits de datos (bits 30..11).
 *   0x7A89C197  palabra de reposo (idle).
 *
 * Mensajes. Las palabras de mensaje siguen a su dirección hasta la siguiente
 * dirección o palabra de reposo, y pueden cruzar lotes (la sincronía del lote
 * siguiente no interrumpe el mensaje). El contenido se interpreta según la
 * función: 0 → numérico (BCD de 4 bits, LSB primero); 3 → alfanumérico
 * (ASCII de 7 bits, LSB primero); 1 y 2 → alfanumérico si hay palabras de
 * mensaje, tono si no. (La asignación función→formato es de uso común, no de la
 * norma: M.584 deja el significado de los 2 bits al operador.)
 *
 * Un mensaje con una palabra no corregible a mitad se descarta entero.
 *
 * Reloj de símbolo (P3c). El lazo de Gardner de pocsag_fsk.h recortaba a 0 la
 * fase t cuando una corrección de RETRASO la dejaba negativa (t1 −= 1 tras
 * ≥ 1): el retraso se perdía, el lazo solo sabía adelantar y, si el punto de
 * muestreo arrancaba por detrás del pico del filtro adaptado, se quedaba
 * donde empezó (muestreando a 0,2–0,5 bit del pico con la preámbulo, y
 * perdiendo un bit dentro de la palabra de sincronía). El 5–10 % de las fases
 * iniciales fallaba, de forma determinista: el «techo» del ≈ 94 %.
 *
 * Falsos positivos (P3c). BCH(31,21)+paridad tiene distancia 6: corregir 2
 * errores deja en el radio de corrección el ≈ 48 % de las palabras de 32 bits
 * AL AZAR (2^21 palabras de código × (1+31+465) vecinos / 2^31). Una palabra
 * aislada "válida" no prueba nada; solo el lote entero lo hace. Por eso:
 *   - 0x00000000 y 0xFFFFFFFF (portadora sin modular, o un receptor de otra
 *     velocidad viendo un tramo constante) son palabras de código válidas del
 *     BCH pero no son palabras POCSAG: se tratan como no válidas;
 *   - cada lote recibe un coste Σ (errores corregidos; 3 si no es válida) y se
 *     descarta entero si supera BATCH_MAX_COST. Basura da ≈ 2,4 por palabra
 *     (≈ 38 por lote); una señal con BER de 1 % da ≈ 5;
 *   - un mensaje solo se emite si sus correcciones suman ≤ 2 + palabras/2 y su
 *     dirección lleva ≤ 1 error; un tono (dirección sin palabras de mensaje)
 *     exige dirección sin errores.
 */
#include "decoder.h"
#include "pocsag.h"
#include "pocsag_fsk.h"

#include "../dsp/bits.h"
#include "../dsp/fec.h"
#include "../util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCH 3
#define MAX_MSG_BITS 4096
#define MAX_RECENT 32
#define RECENT_TTL 600.0
#define TEXT_MAX 700
#define BATCH_MAX_COST 20
#define MSG_MAX_FIX_BASE 2      /* correcciones permitidas a un mensaje, más… */
#define MSG_MAX_FIX_WORDS_DIV 2 /* …1 por cada 2 palabras de mensaje */
#define ADDR_MAX_FIX 1          /* errores corregidos tolerados en la dirección */

static const double BAUDS[NCH] = {512, 1200, 2400};

typedef enum { ST_SEARCH = 0, ST_BATCH, ST_NEXT_SYNC } pstate_t;

typedef struct {
    bool active;
    uint32_t ric;
    int func, nwords, corrected, addr_fix;
    int nbits;
    int64_t smp0, smp1; /* índice absoluto de muestra: inicio de la dirección / fin de la última palabra */
    uint8_t bits[MAX_MSG_BITS];
} pending_t;

typedef struct {
    fskch_t ch;
    uint32_t sr;
    int nsr;          /* bits acumulados desde el último reinicio (para llenar sr) */
    pstate_t st;
    bool inv;
    uint32_t word;
    int wbits, widx;
    uint32_t words[16];
    int64_t batch_end; /* muestra en que acabó el lote en curso */
    int64_t tx_start;  /* inicio estimado de la transmisión (preámbulo); −1 si ya se usó */
    pending_t pend;
} chain_t;

typedef struct {
    uint32_t ric;
    int func, baud, count;
    char kind[8];
    char msg[TEXT_MAX];
    double t;
} recent_t;

typedef struct {
    fskfe_t fe;
    chain_t c[NCH];
    bch_t bch;
    double rate, now, t_block;
    int64_t samples; /* muestras recibidas antes de la llamada en curso */
    int64_t cur;     /* índice absoluto de la muestra en proceso */
    unsigned long n_msgs, n_bad_words, n_dropped, n_batches, n_bad_batches, n_rejected;
    int last_baud;
    recent_t rec[MAX_RECENT];
    int nrec;
    sb_t sb;
    dec_emit_fn emit;
    void *emit_ctx;
    float *fm;
    int fm_cap;
} pocsag_t;

/* ------------------------------------------------------------------------- */
/*  Palabras                                                                 */
/* ------------------------------------------------------------------------- */
/* Corrige una palabra de 32 bits. Devuelve nº de errores o −1. */
static int word_fix(const bch_t *b, uint32_t w, uint32_t *out)
{
    if (w == 0u || w == 0xFFFFFFFFu) /* palabras de código del BCH, pero no de POCSAG */
        return -1;
    uint32_t c31 = w >> 1;
    int par = (int)(w & 1);
    int r = bch_decode_u32(b, &c31, 31);
    if (r < 0)
        return -1;
    int p = bits_parity32(c31);
    int e = r;
    if (p != par) {
        if (r >= 2)
            return -1;
        e++;
    }
    *out = (c31 << 1) | (uint32_t)p;
    return e;
}

static void sanitize(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7F)
            *s = ' ';
}

/* Convierte los bits acumulados del mensaje en texto. Devuelve el tipo. */
static const char *render(const pending_t *p, char *out, size_t cap)
{
    out[0] = 0;
    if (p->nwords == 0)
        return "tone";
    if (p->func == 0) {
        static const char dig[] = "0123456789*U -][";
        int n = p->nbits / 4, k = 0;
        for (int i = 0; i < n && (size_t)k < cap - 1; i++) {
            int v = p->bits[4 * i] | p->bits[4 * i + 1] << 1 | p->bits[4 * i + 2] << 2 | p->bits[4 * i + 3] << 3;
            out[k++] = dig[v];
        }
        out[k] = 0;
        while (k > 0 && out[k - 1] == ' ')
            out[--k] = 0; /* relleno */
        return "numeric";
    }
    int n = p->nbits / 7, k = 0;
    for (int i = 0; i < n && (size_t)k < cap - 1; i++) {
        int v = 0;
        for (int b = 0; b < 7; b++)
            v |= p->bits[7 * i + b] << b;
        if (v == 0x04 || v == 0x00) { /* EOT / relleno: fin del texto */
            if (v == 0x04)
                break;
            continue;
        }
        out[k++] = (char)v;
    }
    out[k] = 0;
    sanitize(out);
    while (k > 0 && out[k - 1] == ' ')
        out[--k] = 0;
    return "alpha";
}

static void recent_add(pocsag_t *d, uint32_t ric, int func, const char *kind, const char *msg, int baud, double now)
{
    for (int i = 0; i < d->nrec; i++)
        if (d->rec[i].ric == ric && d->rec[i].func == func && !strcmp(d->rec[i].msg, msg)) {
            d->rec[i].count++;
            d->rec[i].t = now;
            return;
        }
    int idx = d->nrec;
    if (idx == MAX_RECENT) { /* sustituye la más antigua */
        idx = 0;
        for (int i = 1; i < d->nrec; i++)
            if (d->rec[i].t < d->rec[idx].t)
                idx = i;
    } else
        d->nrec++;
    recent_t *r = &d->rec[idx];
    r->ric = ric;
    r->func = func;
    r->baud = baud;
    r->count = 1;
    r->t = now;
    snprintf(r->kind, sizeof r->kind, "%s", kind);
    snprintf(r->msg, sizeof r->msg, "%s", msg);
}

static void flush_pending(pocsag_t *d, chain_t *c, int baud)
{
    pending_t *p = &c->pend;
    if (!p->active)
        return;
    p->active = false;
    if (p->addr_fix > ADDR_MAX_FIX || (p->nwords == 0 && p->addr_fix > 0) /* un tono no tiene más evidencia que su dirección */ ||
        p->corrected > MSG_MAX_FIX_BASE + p->nwords / MSG_MAX_FIX_WORDS_DIV) {
        d->n_rejected++; /* demasiadas correcciones: probable falsa alarma */
        return;
    }
    char msg[TEXT_MAX];
    const char *kind = render(p, msg, sizeof msg);
    d->n_msgs++;
    d->last_baud = baud;
    recent_add(d, p->ric, p->func, kind, msg, baud, d->now);
    if (!d->emit)
        return;
    sb_t *s = &d->sb;
    sb_clear(s);
    sb_printf(s, "{\"ric\":%u,\"func\":%d,\"kind\":\"%s\",\"msg\":", p->ric, p->func, kind);
    sb_json_str(s, msg);
    sb_printf(s, ",\"baud\":%d,\"corrected\":%d,\"text\":", baud, p->corrected);
    char line[TEXT_MAX + 80];
    if (!strcmp(kind, "tone"))
        snprintf(line, sizeof line, "POCSAG %d RIC %u F%d tono", baud, p->ric, p->func);
    else
        snprintf(line, sizeof line, "POCSAG %d RIC %u F%d %s: %s", baud, p->ric, p->func, kind, msg);
    sb_json_str(s, line);
    sb_puts(s, "}");
    /* Marca de posición (decoder.h): inicio de la dirección y fin de la última palabra,
     * relativos a la primera muestra de esta llamada a process(). */
    dec_emit_span(d->emit, d->emit_ctx, p->smp0 - d->samples, p->smp1 - d->samples, s->data, s->len);
}

static void handle_word(pocsag_t *d, chain_t *c, int pos, uint32_t raw, int baud)
{
    /* la palabra pos acaba (15 − pos) palabras antes del fin del lote */
    double wlen = 32.0 * d->rate / (double)baud;
    int64_t w_end = c->batch_end - (int64_t)((15 - pos) * wlen);
    int64_t w_start = w_end - (int64_t)wlen;
    uint32_t w;
    int e = word_fix(&d->bch, raw, &w);
    pending_t *p = &c->pend;
    if (e < 0) {
        d->n_bad_words++;
        if (p->active) { /* hueco en mitad de un mensaje: se pierde */
            p->active = false;
            d->n_dropped++;
        }
        return;
    }
    if (w == POCSAG_IDLE) {
        flush_pending(d, c, baud);
        return;
    }
    if (!(w & 0x80000000u)) { /* dirección */
        flush_pending(d, c, baud);
        p->active = true;
        p->ric = (((w >> 13) & 0x3FFFFu) << 3) | (uint32_t)(pos >> 1);
        p->func = (int)((w >> 11) & 3);
        p->nwords = 0;
        p->nbits = 0;
        p->corrected = e;
        p->addr_fix = e;
        /* El primer mensaje de una transmisión arranca en el preámbulo (la señal está en el aire
         * desde entonces, y el suelo de ruido que mide el host debe salir de ANTES de ella); los
         * siguientes, en su propia dirección. */
        p->smp0 = c->tx_start >= 0 ? c->tx_start : w_start;
        c->tx_start = -1;
        p->smp1 = w_end;
    } else if (p->active) {
        uint32_t data = (w >> 11) & 0xFFFFFu;
        if (p->nbits + 20 > MAX_MSG_BITS) {
            p->active = false;
            d->n_dropped++;
            return;
        }
        for (int i = 19; i >= 0; i--)
            p->bits[p->nbits++] = (uint8_t)((data >> i) & 1);
        p->nwords++;
        p->corrected += e;
        p->smp1 = w_end;
    }
}

/* Coste de un lote: errores corregidos por palabra, 3 por palabra no válida. */
static int batch_cost(const pocsag_t *d, const uint32_t *w16)
{
    int cost = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t w;
        int e = word_fix(&d->bch, w16[i], &w);
        cost += e < 0 ? 3 : e;
    }
    return cost;
}

/* ------------------------------------------------------------------------- */
/*  Máquina de estados por receptor                                          */
/* ------------------------------------------------------------------------- */
static void chain_bit(pocsag_t *d, int ci, int bit)
{
    chain_t *c = &d->c[ci];
    int baud = (int)BAUDS[ci];
    c->sr = (c->sr << 1) | (uint32_t)bit;
    if (c->nsr < 32)
        c->nsr++;
    switch (c->st) {
    case ST_SEARCH:
        if (c->nsr >= 32) {
            int e0 = bits_popcount32(c->sr ^ POCSAG_SYNC), e1 = bits_popcount32(c->sr ^ ~POCSAG_SYNC);
            if (e0 <= 2 || e1 <= 2) {
                c->inv = e1 < e0;
                c->tx_start = MAX(0, d->cur - (int64_t)((32 + POCSAG_PREAMBLE_BITS) * d->rate / baud));
                c->st = ST_BATCH;
                c->wbits = c->widx = 0;
                c->word = 0;
            }
        }
        break;
    case ST_BATCH:
        c->word = (c->word << 1) | (uint32_t)(bit ^ c->inv);
        if (++c->wbits == 32) {
            c->words[c->widx++] = c->word;
            c->wbits = 0;
            c->word = 0;
            if (c->widx == 16) {
                d->n_batches++;
                c->batch_end = d->cur;
                if (batch_cost(d, c->words) > BATCH_MAX_COST) {
                    d->n_bad_batches++;
                    if (c->pend.active) {
                        c->pend.active = false;
                        d->n_dropped++;
                    }
                } else
                    for (int i = 0; i < 16; i++)
                        handle_word(d, c, i, c->words[i], baud);
                c->st = ST_NEXT_SYNC;
                c->nsr = 0;
            }
        }
        break;
    case ST_NEXT_SYNC:
        if (c->nsr >= 32) {
            uint32_t ref = c->inv ? ~POCSAG_SYNC : POCSAG_SYNC;
            if (bits_popcount32(c->sr ^ ref) <= 4) {
                c->st = ST_BATCH;
                c->wbits = c->widx = 0;
                c->word = 0;
            } else {
                flush_pending(d, c, baud); /* fin de la transmisión */
                c->st = ST_SEARCH;
                c->nsr = 0;
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------------- */
/*  Descriptor                                                               */
/* ------------------------------------------------------------------------- */
static void *pocsag_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    pocsag_t *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    d->rate = cfg->rate;
    if (bch_init_pocsag(&d->bch) < 0) {
        snprintf(err, errlen, "BCH no inicializado");
        free(d);
        return NULL;
    }
    fskfe_init(&d->fe, cfg->rate, 9500.0, POCSAG_DEV_HZ);
    for (int i = 0; i < NCH; i++)
        if (fskch_init(&d->c[i].ch, cfg->rate / BAUDS[i], NULL, 0, 0.03) < 0) {
            snprintf(err, errlen, "sin memoria");
            for (int j = 0; j < i; j++)
                fskch_free(&d->c[j].ch);
            free(d);
            return NULL;
        }
    return d;
}

static void pocsag_destroy(void *st)
{
    pocsag_t *d = st;
    if (!d)
        return;
    for (int i = 0; i < NCH; i++)
        fskch_free(&d->c[i].ch);
    sb_free(&d->sb);
    free(d->fm);
    free(d);
}

static void pocsag_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    pocsag_t *d = st;
    d->emit = emit;
    d->emit_ctx = ctx;
    if (n > d->fm_cap) {
        d->fm_cap = n;
        d->fm = realloc(d->fm, sizeof(float) * (size_t)n);
        if (!d->fm)
            return;
    }
    fskfe_process(&d->fe, iq, n, d->fm);
    for (int k = 0; k < n; k++) {
        d->now = now + (double)k / d->rate;
        d->cur = d->samples + k;
        for (int i = 0; i < NCH; i++) {
            float y;
            if (fskch_push(&d->c[i].ch, d->fm[k], &y))
                chain_bit(d, i, y > 0 ? 1 : 0);
        }
    }
    d->samples += n;
}

static void pocsag_state(void *st, sb_t *sb, double now)
{
    pocsag_t *d = st;
    sb_printf(sb, "{\"msgs\":%lu,\"batches\":%lu,\"bad_words\":%lu,\"bad_batches\":%lu,\"rejected\":%lu,\"dropped\":%lu,\"baud\":%d,\"pages\":[", d->n_msgs,
              d->n_batches, d->n_bad_words, d->n_bad_batches, d->n_rejected, d->n_dropped, d->last_baud);
    /* caducidad: se olvidan las entradas con más de RECENT_TTL s */
    int w = 0;
    for (int i = 0; i < d->nrec; i++)
        if (now - d->rec[i].t <= RECENT_TTL)
            d->rec[w++] = d->rec[i];
    d->nrec = w;
    for (int i = 0; i < d->nrec; i++) {
        const recent_t *r = &d->rec[d->nrec - 1 - i]; /* más recientes primero (orden de inserción) */
        if (i)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"ric\":%u,\"func\":%d,\"kind\":\"%s\",\"baud\":%d,\"count\":%d,\"seen\":%.1f,\"msg\":", r->ric,
                  r->func, r->kind, r->baud, r->count, fmax(0.0, now - r->t));
        sb_json_str(sb, r->msg);
        sb_puts(sb, "}");
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_POCSAG = {
    .id = "pocsag",
    .name = "POCSAG",
    .group = "Buscapersonas",
    .desc = "Buscapersonas POCSAG 512/1200/2400 bps (BCH 31,21).",
    .kind = DEC_NARROW,
    .rate = 48000,
    .bw_hz = 12500,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {466075000LL, 0LL, 0LL, 0LL},
    .create = pocsag_create,
    .destroy = pocsag_destroy,
    .process = pocsag_process,
    .state_json = pocsag_state,
};
