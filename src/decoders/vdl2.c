/*
 * vdl2.c — Decodificador VDL Modo 2 (D8PSK 10 500 símb/s, ICAO Doc 9776 /
 * ETSI EN 301 841-1). Ver docs/protocolos/vdl2.md.
 *
 * Señal. Cada ráfaga lleva 3 bits por símbolo codificados en el INCREMENTO de fase
 * (D8PSK: Δφ = k·π/4). Una ráfaga = rampa | palabra única (UW, 16 símbolos conocidos) |
 * cabecera de 25 bits | datos.
 *
 * Cadena (a 105 kHz = 10 muestras/símbolo). El decodificador trabaja POR RÁFAGAS:
 *   1. Detector de energía por bloques de 256 muestras frente a un suelo de ruido
 *      que se sigue solo en reposo; la ráfaga se guarda entera (con 3 bloques
 *      previos) y se procesa al cerrarse el squelch. Así no hay coste en reposo.
 *   2. CFO grueso: centroide del espectro de la ráfaga (Welch, FFT 256, suelo de
 *      ruido restado). Se desrota.
 *   3. Búsqueda de la UW sobre la señal filtrada con un paso bajo ancho (16 kHz): para cada
 *      posición se compara la FASE del producto diferencial z_k·conj(z_{k−1}) con los 15
 *      incrementos conocidos, SIN ponderar por la amplitud (el módulo de la suma
 *      de fasores unitarios no depende del desvío de frecuencia ni de la fase inicial). La
 *      ponderación por amplitud, que parecía natural, era un fallo grave: 1-2 productos
 *      fuertes de una rampa de subida/bajada dan módulo ≈ 1 y desplazaban la UW buena.
 *   4. CFO fino = arg(Σ fasores)/(2π·T): inambigua hasta ±5,25 kHz. Se vuelve a desrotar.
 *   5. HIPÓTESIS DE FILTRO DE ENTRADA, en orden, hasta que una dé una trama con FCS correcto:
 *      paso bajo de 10, 9, 12 y 8 kHz (señales ya conformadas con un Nyquist, como la
 *      grabación real de referencia, donde un segundo RRC mete interferencia entre símbolos
 *      y multiplica por 5 los errores) y RRC adaptado α = 0,6 (señal directa de antena).
 *      Se afina la posición de la UW (±12 muestras) con el filtro de cada hipótesis.
 *   6. Reloj de símbolo: lazo de Gardner de ganancia baja con interpolador de Farrow
 *      cúbico (±100 ppm); se sigue hasta el final del búfer de la ráfaga (la cabecera
 *      da la longitud; la caída de potencia por AGC o desvanecimiento no corta la
 *      demodulación).
 *   7. Demodulación diferencial: Δ = round((arg(y_k·conj(y_{k−1})) − θ)/(π/4)); θ
 *      sigue la deriva de fase residual (lazo de primer orden). Se guarda el residuo de
 *      cada decisión como medida de fiabilidad.
 *   8. Aleatorizador (cabecera y datos), cabecera (25,20) → longitud, desentrelazado,
 *      RS(255,249), HDLC, FCS. Si falla: decodificación tipo Chase (cambiar los símbolos
 *      menos fiables por su segunda mejor opción, en orden de peso) y RS con borrados en
 *      los octetos menos fiables, con un tope de pruebas por ráfaga.
 *   9. Hasta tres UW por hipótesis y ráfaga (puede haber varias tramas seguidas).
 *   10. AVLC: direcciones, tipo de trama, y ACARS sobre AVLC (FF FF 01).
 *
 * Solo se emiten tramas con FCS correcto y direcciones verosímiles. Validado con la
 * grabación real de sigidwiki «VDL-M2 IQ» (ver docs/protocolos/vdl2.md).
 */
#include "decoder.h"
#include "acars_blk.h"
#include "vdl2_avlc.h"
#include "vdl2_frame.h"
#include "../dsp/fft.h"
#include "../dsp/pulse.h"
#include "../dsp/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLK 256              /* muestras por bloque del detector de energía */
#define PRE_BLOCKS 3
#define QUIET_BLOCKS 8       /* bloques silenciosos que cierran una ráfaga */
#define TH_ON 2.5f           /* 4 dB sobre el suelo */
#define TH_OFF 1.8f
#define MAX_BURST (1 << 21)  /* ~20 s a 105 kHz: cota de memoria */
#define MAX_TAB 256
#define TAB_TTL 900.0
#define UW_THRESH 0.6f

typedef struct {
    uint32_t icao;
    int type;
    int msgs;
    double last_t;
    char reg[8], flight[7], last[100];
} node_t;

typedef struct {
    double rate;
    /* detector de energía */
    cf32 *buf;
    int nbuf, cap;
    float blk_pwr;
    int blk_n;
    float floor;
    bool floor_init;
    bool active;
    int quiet;
    int trig;               /* índice del primer bloque fuerte dentro de buf */
    /* trabajo por ráfaga */
    cf32 *x1, *z;
    int wcap;
    float rrc[81];
    cf32 *zs;                /* z a la frecuencia gruesa (búsqueda de UW) */
    float *met;              /* métrica de UW por posición */
    cf32 *msum;
    int mcap;
    int claim_lo[8], claim_hi[8], nclaim; /* zonas de tramas ya decodificadas en la ráfaga */
    bool unresolved;         /* alguna UW de la ráfaga sigue sin decodificarse */
    int last_span;           /* símbolos de la última trama decodificada, desde la UW */
    fft_plan_t *fft;
    cf32 *fbuf;
    float *psd;
    /* tablas y estadísticas */
    node_t ac[MAX_TAB], gs[MAX_TAB];
    int nac, ngs;
    unsigned long bursts, frames, bad, chase_ok;
    int trials;
    bool chase;              /* 2.ª pasada de la ráfaga: con Chase y borrados */
    double now;
    float last_level;
    double last_cfo;
    int last_rs;
    sb_t evsb;
} vdl2_t;

/* ---- utilidades ------------------------------------------------------------ */
static void derotate(const cf32 *in, int n, double f_hz, double fs, cf32 *out)
{
    double w = -2.0 * M_PI * f_hz / fs;
    double c = cos(w), s = sin(w), pr = 1, pi = 0;
    for (int i = 0; i < n; i++) {
        out[i] = (cf32){(float)(in[i].re * pr - in[i].im * pi), (float)(in[i].re * pi + in[i].im * pr)};
        double nr = pr * c - pi * s;
        pi = pr * s + pi * c;
        pr = nr;
        if ((i & 1023) == 1023) { /* renormaliza el fasor */
            double m = sqrt(pr * pr + pi * pi);
            pr /= m;
            pi /= m;
        }
    }
}

static void rrc_filter(const vdl2_t *s, const cf32 *in, int n, cf32 *out)
{
    /* FIR de 81 coeficientes centrado (fase cero: la salida i corresponde a la entrada i, así la posición
     * de la UW es la misma con cualquier filtro de entrada), convolución directa con ceros fuera */
    const int nt = 81, h = (nt - 1) / 2;
    for (int i = 0; i < n; i++) {
        float r = 0, q = 0;
        int k0 = MAX(0, h - i), k1 = MIN(nt, n - i + h);
        for (int k = k0; k < k1; k++) {
            const cf32 *p = &in[i - h + k];
            r += s->rrc[k] * p->re;
            q += s->rrc[k] * p->im;
        }
        out[i] = (cf32){r, q};
    }
}

/* CFO grueso: centroide del PSD de [a, b). */
static double coarse_cfo(vdl2_t *s, const cf32 *x, int a, int b)
{
    const int N = 256;
    for (int k = 0; k < N; k++)
        s->psd[k] = 0;
    int nseg = 0;
    for (int p = a; p + N <= b; p += N / 2) {
        for (int k = 0; k < N; k++) {
            float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)k / (float)N);
            s->fbuf[k] = (cf32){x[p + k].re * w, x[p + k].im * w};
        }
        fft_exec(s->fft, s->fbuf);
        for (int k = 0; k < N; k++)
            s->psd[k] += s->fbuf[k].re * s->fbuf[k].re + s->fbuf[k].im * s->fbuf[k].im;
        nseg++;
    }
    if (nseg < 2)
        return 0;
    /* mediana ≈ suelo de ruido */
    float tmp[256];
    memcpy(tmp, s->psd, sizeof tmp);
    for (int i = 1; i < N; i++) { /* ordenación por inserción: solo 256 elementos */
        float v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = v;
    }
    float med = tmp[N / 2];
    double sw = 0, sf = 0;
    for (int k = 0; k < N; k++) {
        int kk = k < N / 2 ? k : k - N;
        double f = kk * s->rate / N;
        if (fabs(f) > 24000.0)
            continue;
        double w = s->psd[k] - 2.0f * med;
        if (w > 0) {
            sw += w;
            sf += w * f;
        }
    }
    return sw > 0 ? sf / sw : 0.0;
}

/* ---- tablas ----------------------------------------------------------------- */
static node_t *node_get(node_t *tab, int *n, uint32_t icao, int type, double now)
{
    for (int i = 0; i < *n; i++)
        if (tab[i].icao == icao)
            return &tab[i];
    node_t *p;
    if (*n < MAX_TAB)
        p = &tab[(*n)++];
    else {
        p = &tab[0];
        for (int i = 1; i < *n; i++)
            if (tab[i].last_t < p->last_t)
                p = &tab[i];
    }
    memset(p, 0, sizeof *p);
    p->icao = icao;
    p->type = type;
    p->last_t = now;
    return p;
}

/* ---- trama AVLC válida ------------------------------------------------------- */
typedef struct {
    vdl2_t *s;
    dec_emit_fn emit;
    void *ctx;
    int rs;
} cbctx_t;

static void on_frame(void *vc, const uint8_t *f, int n)
{
    cbctx_t *c = vc;
    vdl2_t *s = c->s;
    avlc_t a;
    if (avlc_parse(f, n, &a) < 0) {
        s->bad++;
        return;
    }
    /* tipos de dirección reservados (0, 2, 3, 6), "todas las estaciones" como origen: no
     * ocurren en tráfico real; con el FCS de 16 bits sirve de filtro de falsos positivos de la decodificación
     * Chase y por borrados */
    int st = a.src.type, dt = a.dst.type;
    if (!(st == 1 || st == 4 || st == 5) || !(dt == 1 || dt == 4 || dt == 5 || dt == 7)) {
        s->bad++;
        return;
    }
    s->frames++;
    /* ACARS sobre AVLC */
    acars_blk_t b;
    bool has_acars = false;
    /* ACARS sobre AVLC: la información lleva FF FF 01 (SOH) y a continuación el bloque ACARS
     * (modo, matrícula…) con paridad impar en el bit 7; acaba en ETX/ETB + BCS + DEL. Se acepta
     * también el SOH solo (versión anterior del modulador). */
    int off = (a.info_len > 17 && a.info[0] == 0xFF && a.info[1] == 0xFF && a.info[2] == ACARS_SOH) ? 3
              : (a.info_len > 14 && a.info[0] == ACARS_SOH) ? 1 : 0;
    if (off && (a.ctrl & 1) == 0) {
        uint8_t pl[300];
        int m = 0, e = -1;
        for (int i = off; i < a.info_len && m < (int)sizeof pl; i++) {
            pl[m] = a.info[i] & 0x7F;
            if (m >= 12 && (pl[m] == ACARS_ETX || pl[m] == ACARS_ETB)) {
                e = m;
                break;
            }
            m++;
        }
        if (e >= 0 && acars_blk_parse(pl, e + 1, &b) == 0)
            has_acars = true;
    }
    node_t *src = node_get(a.src.type == 4 || a.src.type == 5 ? s->gs : s->ac,
                           a.src.type == 4 || a.src.type == 5 ? &s->ngs : &s->nac, a.src.icao, a.src.type, s->now);
    src->msgs++;
    src->last_t = s->now;
    src->type = a.src.type;
    if (has_acars) {
        snprintf(src->reg, sizeof src->reg, "%s", b.reg);
        if (b.flight[0])
            snprintf(src->flight, sizeof src->flight, "%s", b.flight);
    }
    node_t *dst = NULL;
    if (a.dst.type == 4 || a.dst.type == 5)
        dst = node_get(s->gs, &s->ngs, a.dst.icao, a.dst.type, s->now);
    else if (a.dst.type == 1)
        dst = node_get(s->ac, &s->nac, a.dst.icao, a.dst.type, s->now);
    if (dst)
        dst->last_t = s->now;

    char line[300];
    int ln = snprintf(line, sizeof line, "%06X(%s) > %06X(%s) %s", a.src.icao, avlc_type_name(a.src.type), a.dst.icao,
                      avlc_type_name(a.dst.type), a.ftype);
    if (a.ftype[0] == 'I')
        ln += snprintf(line + ln, sizeof line - (size_t)ln, " ns=%d nr=%d", a.ns, a.nr);
    if (has_acars) {
        char body[160];
        size_t j = 0;
        for (const char *p = b.text; *p && j + 1 < sizeof body; p++)
            body[j++] = (*p < 32 || *p == 127) ? ' ' : *p;
        body[j] = 0;
        snprintf(line + ln, sizeof line - (size_t)ln, " | ACARS %s %s %s: %s", b.reg, b.flight, b.label, body);
        snprintf(src->last, sizeof src->last, "%s %s: %.70s", b.label, b.flight, body);
    } else {
        snprintf(src->last, sizeof src->last, "%s", a.ftype);
    }

    sb_t *o = &s->evsb;
    sb_clear(o);
    sb_printf(o, "{\"proto\":\"vdl2\",\"src\":\"%06X\",\"src_type\":\"%s\",\"dst\":\"%06X\",\"dst_type\":\"%s\",", a.src.icao,
              avlc_type_name(a.src.type), a.dst.icao, avlc_type_name(a.dst.type));
    sb_printf(o, "\"cr\":%d,\"ftype\":\"%s\",\"ctrl\":%d,\"poll\":%d,", a.src.flag, a.ftype, a.ctrl, a.poll);
    if (a.ftype[0] == 'I')
        sb_printf(o, "\"ns\":%d,\"nr\":%d,", a.ns, a.nr);
    sb_printf(o, "\"len\":%d,\"info\":\"", a.info_len);
    for (int i = 0; i < a.info_len && i < 64; i++)
        sb_printf(o, "%02X", a.info[i]);
    sb_puts(o, "\"");
    if (has_acars) {
        sb_puts(o, ",\"acars\":{\"mode\":");
        char m[2] = {b.mode, 0};
        sb_json_str(o, m);
        sb_puts(o, ",\"reg\":");
        sb_json_str(o, b.reg);
        sb_puts(o, ",\"label\":");
        sb_json_str(o, b.label);
        sb_puts(o, ",\"block\":");
        char bi[2] = {b.bid, 0};
        sb_json_str(o, bi);
        sb_puts(o, ",\"msgno\":");
        sb_json_str(o, b.msgno);
        sb_puts(o, ",\"flight\":");
        sb_json_str(o, b.flight);
        sb_puts(o, ",\"body\":");
        sb_json_str(o, b.text);
        sb_puts(o, "}");
    }
    sb_puts(o, ",\"cfo_hz\":");
    sb_json_num(o, s->last_cfo, 0);
    sb_printf(o, ",\"rs_fixed\":%d,\"level_db\":", s->last_rs);
    sb_json_num(o, s->last_level, 1);
    sb_puts(o, ",\"text\":");
    sb_json_str(o, line);
    sb_puts(o, "}");
    c->emit(c->ctx, o->data, o->len);
}

/* ---- demodulación de una ráfaga --------------------------------------------- */
static float wrap_pi(float a)
{
    while (a > (float)M_PI)
        a -= 2.0f * (float)M_PI;
    while (a < -(float)M_PI)
        a += 2.0f * (float)M_PI;
    return a;
}

static cf32 cmul_conj(cf32 a, cf32 b) /* a·conj(b) */
{
    return (cf32){a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im};
}

/* Interpola z en la posición t (con z[-1..+2] disponibles). */
static cf32 interp(const cf32 *z, double t)
{
    int i = (int)floor(t);
    return farrow3_c(&z[i - 1], (float)(t - i));
}

/* Filtro de entrada de cada hipótesis (ver la cabecera): 0 = RRC adaptado (α = 0,6, lo que
 * pide la norma si la señal llega tal cual de la antena); 1 = paso bajo suave de 2 polos
 * (Butterworth, 7 kHz) para señales que ya vienen conformadas con un Nyquist (coseno alzado o un
 * RRC aplicado por el receptor/grabador), en las que un segundo RRC mete interferencia entre símbolos. */
typedef struct {
    int kind;      /* 0 = RRC, 1 = paso bajo */
    double fc;     /* kind 1: frecuencia de corte (Hz) */
    float th_gain; /* ganancia del seguimiento de fase diferencial */
} hyp_t;
/* Orden de prueba: primero el RRC de una señal directa de antena (lo que dice la norma); el paso bajo
 * de 9-10 kHz es el que decodifica la grabación real de sigidwiki (señal ya conformada por un Nyquist). */
static const hyp_t HYPS[] = {{0, 0.0, 0.02f}, {1, 10000.0, 0.03f}, {1, 9000.0, 0.01f}, {1, 12000.0, 0.02f},
                             {1, 8000.0, 0.02f}};
#define NHYP ((int)(sizeof HYPS / sizeof HYPS[0]))
/* Filtro de la búsqueda de la UW: ancho (16 kHz) para tolerar un CFO grueso de ±4 kHz sin recortar la señal */
static const hyp_t SEARCH_HYP = {1, 16000.0, 0.02f};

static void front_filter(const vdl2_t *s, const hyp_t *h, const cf32 *in, int n, cf32 *out)
{
    if (h->kind == 0) {
        rrc_filter(s, in, n, out);
        return;
    }
    /* Butterworth de 2 polos, fc = 7 kHz a 105 kHz (transformada bilineal), forma directa II */
    const double fc = h->fc, k = tan(M_PI * fc / 105000.0), q = 0.70710678;
    const double nrm = 1.0 / (1.0 + k / q + k * k);
    const float b0 = (float)(k * k * nrm), b1 = 2.0f * b0, b2 = b0;
    const float a1 = (float)(2.0 * (k * k - 1.0) * nrm), a2 = (float)((1.0 - k / q + k * k) * nrm);
    float wr1 = 0, wr2 = 0, wi1 = 0, wi2 = 0;
    for (int i = 0; i < n; i++) {
        float wr = in[i].re - a1 * wr1 - a2 * wr2, wi = in[i].im - a1 * wi1 - a2 * wi2;
        out[i] = (cf32){b0 * wr + b1 * wr1 + b2 * wr2, b0 * wi + b1 * wi1 + b2 * wi2};
        wr2 = wr1;
        wr1 = wr;
        wi2 = wi1;
        wi1 = wi;
    }
}

/* Del flujo de bits demodulado (desde el símbolo 16, es decir, justo tras la UW) al evento: desaleatoriza,
 * lee la longitud de la cabecera, RS, HDLC. Devuelve true si salió alguna trama con FCS correcto. */
static bool decode_bits(vdl2_t *s, uint8_t *bits, int nb, int nb_drop, int *hdr_d, bool only_hdr, const int *rank, int nrank, int n_era,
                        dec_emit_fn emit, void *ctx)
{
    if (hdr_d)
        *hdr_d = -1;
    if (nb < VDL2_HDR_BITS + 8 * 12)
        return false;
    /* El aleatorizador cubre cabecera y datos. La longitud sale de la cabecera (25,20) si es verosímil;
     * si no, se prueban todas las longitudes D cuyo bloque RS cabe en los bits recibidos (±3 octetos). */
    vdl2_scramble(bits, nb);
    uint32_t hlen = 0;
    int hrc = vdl2_hdr_decode(bits, &hlen);
    int avail = ((nb_drop >= 0 ? nb_drop : nb) - VDL2_HDR_BITS) / 8;
    int avail_hdr = (nb - VDL2_HDR_BITS) / 8;
    int cand[64], nc = 0;
    if (hrc >= 0 && hlen >= 100 && hlen <= VDL2_MAX_LEN && vdl2_encoded_bytes((int)((hlen + 7) / 8)) <= avail_hdr)
        cand[nc++] = (int)((hlen + 7) / 8);
    if (hdr_d && nc)
        *hdr_d = cand[0];
    for (int tb = avail; !only_hdr && tb >= avail - 3 && tb > 12; tb--)
        for (int D = tb; D >= 1 && D >= tb - 24; D--)
            if (vdl2_encoded_bytes(D) == tb && nc < 64)
                cand[nc++] = D;
    const uint8_t *db = bits + VDL2_HDR_BITS;
    int nbits_db = nb - VDL2_HDR_BITS;
    bool done = false;
    for (int c = 0; c < nc && !done; c++) {
        int D = cand[c], E = vdl2_encoded_bytes(D);
        if (8 * E > nbits_db)
            continue;
        uint8_t *enc = malloc((size_t)E), *dat = malloc((size_t)D + 255);
        for (int i = 0; i < E; i++) {
            uint8_t v = 0;
            for (int q = 0; q < 8; q++)
                v = (uint8_t)(v | db[8 * i + q] << q); /* octetos LSB primero */
            enc[i] = v;
        }
        uint8_t *ef = NULL;
        if (n_era > 0 && rank) { /* los n_era octetos que contienen los símbolos más dudosos se tratan como borrados */
            ef = calloc((size_t)E, 1);
            int got = 0;
            for (int r = 0; r < nrank && got < n_era; r++)
                for (int q = 0; q < 3; q++) {
                    int b = 3 * rank[r] + q - VDL2_HDR_BITS; /* bit de datos del flujo */
                    if (b >= 0 && b / 8 < E && !ef[b / 8]) {
                        ef[b / 8] = 1;
                        got++;
                    }
                }
        }
        int fixed = vdl2_fec_decode_era(enc, D, dat, ef);
        free(ef);
        if (fixed >= 0) {
            int hl = 8 * D;
            uint8_t *hb = malloc((size_t)hl);
            for (int i = 0; i < hl; i++)
                hb[i] = (uint8_t)((dat[i >> 3] >> (i & 7)) & 1);
            cbctx_t cc = {s, emit, ctx, fixed};
            s->last_rs = fixed;
            if (vdl2_hdlc_decode(hb, hl, on_frame, &cc) > 0) {
                done = true;
                s->last_span = (VDL2_HDR_BITS + 8 * E + 2) / 3;
            }
            free(hb);
        }
        free(enc);
        free(dat);
    }
    return done;
}

/* Símbolos (incrementos de fase 0..7 y su residuo) → bits → trama. Si la primera lectura no pasa RS y FCS
 * se hace decodificación tipo Chase: los símbolos de decisión menos fiable (mayor |residuo| respecto al
 * punto de la constelación) se cambian por su segunda mejor opción, en orden de peso creciente. Gray
 * garantiza que el vecino difiere en un solo bit. Cada prueba la valida RS + FCS, así que no genera
 * falsos positivos más allá de la probabilidad del FCS de 16 bits por prueba (se acota con el nº de
 * pruebas y con la comprobación de tipos de dirección AVLC). */
#define CHASE_MAX_W 10
#define TRIALS_PER_BURST 2000 /* tope de pruebas RS+HDLC por ráfaga: acota el coste de las ráfagas imposibles */

/* Mantiene en (val, idx) los 'cap' mayores valores vistos, en orden decreciente. */
static void top_insert(float *val, int *idx, int *n, int cap, float a, int i)
{
    int p;
    if (*n < cap)
        p = (*n)++;
    else if (a > val[cap - 1])
        p = cap - 1;
    else
        return;
    while (p > 0 && val[p - 1] < a) {
        val[p] = val[p - 1];
        idx[p] = idx[p - 1];
        p--;
    }
    val[p] = a;
    idx[p] = i;
}

static void syms_to_bits(const uint8_t *incs, int nsym, uint8_t *bits)
{
    for (int i = 0; i < nsym; i++) {
        int v = VDL2_INC_TO_GRAY[incs[i] & 7];
        bits[3 * i] = (uint8_t)((v >> 2) & 1);
        bits[3 * i + 1] = (uint8_t)((v >> 1) & 1);
        bits[3 * i + 2] = (uint8_t)(v & 1);
    }
}

static bool decode_syms(vdl2_t *s, const uint8_t *incs, const float *ress, int nsym, int nb_drop, dec_emit_fn emit, void *ctx)
{
    int nb = nsym * 3;
    uint8_t *bits = malloc((size_t)nb + 3);
    uint8_t *work = malloc((size_t)nsym + 1);
    if (!bits || !work) {
        free(bits);
        free(work);
        return false;
    }
    syms_to_bits(incs, nsym, bits);
    int hd = -1;
    bool ok = decode_bits(s, bits, nb, nb_drop, &hd, false, NULL, 0, 0, emit, ctx);
    if (!ok && hd > 0 && s->chase) {
        /* símbolos que cubren cabecera + datos + paridad de la longitud anunciada */
        int E = vdl2_encoded_bytes(hd), ns = MIN(nsym, (VDL2_HDR_BITS + 8 * E + 2) / 3);
        int W = ns > 400 ? 6 : ns > 150 ? 8 : 10;
        int idx[CHASE_MAX_W];
        float rel[CHASE_MAX_W];
        int nw = 0;
        for (int i = 0; i < ns; i++) /* los W de mayor |residuo| */
            top_insert(rel, idx, &nw, W, fabsf(ress[i]), i);
        for (int wgt = 1; wgt <= nw && !ok; wgt++)
            for (unsigned m = 1; m < (1u << nw) && !ok && s->trials > 0; m++) {
                if (__builtin_popcount(m) != wgt)
                    continue;
                s->trials--;
                memcpy(work, incs, (size_t)nsym);
                for (int q = 0; q < nw; q++)
                    if (m >> q & 1)
                        work[idx[q]] = (uint8_t)((work[idx[q]] + (ress[idx[q]] >= 0 ? 1 : 7)) & 7);
                syms_to_bits(work, nsym, bits);
                ok = decode_bits(s, bits, nb, nb_drop, NULL, true, NULL, 0, 0, emit, ctx);
                if (ok)
                    s->chase_ok++;
            }
    }
    if (!ok && hd > 0 && s->chase) {
        /* borrados por fiabilidad: se marcan como borrados los octetos con los símbolos menos fiables (el RS
         * corrige 6 borrados, pero solo 3 errores) */
        int ns2 = MIN(nsym, (VDL2_HDR_BITS + 8 * vdl2_encoded_bytes(hd) + 2) / 3), nr = 0;
        int rk[24];
        float rv[24];
        for (int i = 0; i < ns2; i++)
            top_insert(rv, rk, &nr, 24, fabsf(ress[i]), i);
        syms_to_bits(incs, nsym, bits);
        for (int ne = 2; ne <= 6 && !ok && s->trials > 0; ne++) {
            s->trials--;
            syms_to_bits(incs, nsym, bits);
            ok = decode_bits(s, bits, nb, nb_drop, NULL, true, rk, nr, ne, emit, ctx);
            if (ok)
                s->chase_ok++;
        }
    }
    free(bits);
    free(work);
    return ok;
}

/* ¿La posición i0 de la UW cae en una zona ya reclamada por una trama decodificada o ya probada? */
static bool is_claimed(const vdl2_t *s, const int *lo, const int *hi, int nc, int i0)
{
    for (int c = 0; c < nc; c++)
        if (i0 >= lo[c] && i0 < hi[c])
            return true;
    for (int c = 0; c < s->nclaim; c++)
        if (i0 >= s->claim_lo[c] && i0 < s->claim_hi[c])
            return true;
    return false;
}

/* Demodula desde una UW concreta (posición bi en s->z a la frecuencia f0, suma de correlación bsum) y
 * decodifica. Devuelve true si salió alguna trama. */
/* Afina la posición de la UW (±12 muestras) sobre la señal ya filtrada por la hipótesis: cada filtro
 * tiene su propio retardo de grupo. Usa la misma métrica diferencial que la búsqueda gruesa. */
static int uw_refine(const vdl2_t *s, int bi)
{
    float inc_re[16], inc_im[16];
    for (int j = 1; j < 16; j++) {
        int v = VDL2_UW_BITS[3 * j] << 2 | VDL2_UW_BITS[3 * j + 1] << 1 | VDL2_UW_BITS[3 * j + 2];
        double ang = VDL2_GRAY_TO_INC[v] * M_PI / 4.0;
        inc_re[j] = (float)cos(ang);
        inc_im[j] = (float)sin(ang);
    }
    float best = -1;
    int bb = bi;
    for (int i0 = MAX(0, bi - 12); i0 <= bi + 12; i0++) {
        float sr = 0, si = 0;
        for (int j = 1; j < 16; j++) {
            cf32 d = cmul_conj(s->z[i0 + j * VDL2_SPS], s->z[i0 + (j - 1) * VDL2_SPS]);
            float a = hypotf(d.re, d.im);
            if (a < 1e-30f)
                continue;
            float ur = d.re / a, ui = d.im / a;
            sr += ur * inc_re[j] + ui * inc_im[j];
            si += ui * inc_re[j] - ur * inc_im[j];
        }
        float m = hypotf(sr, si);
        if (m > best) {
            best = m;
            bb = i0;
        }
    }
    return bb;
}

static bool demod_at(vdl2_t *s, const hyp_t *h, const cf32 *x, int n, int bi, cf32 bsum, double f0, dec_emit_fn emit, void *ctx)
{
    double f1 = atan2(bsum.im, bsum.re) / (2.0 * M_PI) * VDL2_BAUD;
    double ftot = f0 + f1;
    derotate(x, n, ftot, s->rate, s->x1);
    front_filter(s, h, s->x1, n, s->z);
    s->last_cfo = ftot;
    bi = uw_refine(s, bi);

    /* --- símbolos con lazo de reloj de Gardner --- */
    int maxsym = (n - bi) / VDL2_SPS - 2;
    if (maxsym < 40)
        return false;
    uint8_t *incs = malloc((size_t)maxsym + 4); /* incrementos decididos, desde el símbolo 16 */
    float *ress = malloc(sizeof(float) * ((size_t)maxsym + 4)); /* residuo con signo de cada decisión */
    int nb = 0; /* nº de símbolos guardados (tras la UW) */
    double t = bi, w = VDL2_SPS;
    cf32 yprev = interp(s->z, t);
    float th = 0, pwm = yprev.re * yprev.re + yprev.im * yprev.im + 1e-12f;
    float pw_ref = 0; /* potencia media de los primeros símbolos (para detectar el final) */
    int low = 0, nb_drop = -1, stop_k = maxsym;
    const double GG1 = 0.01, GG2 = 0.0002, GTH = h->th_gain;
    for (int k = 1; k < maxsym; k++) {
        double tn = t + w;
        if (tn + 3 >= n)
            break;
        cf32 y = interp(s->z, tn);
        cf32 ym = interp(s->z, tn - w / 2);
        /* lazo de reloj (a partir de la UW, que es determinista pero no hace falta
         * excluirla del lazo) */
        float e = ym.re * (y.re - yprev.re) + ym.im * (y.im - yprev.im);
        pwm += 0.01f * ((y.re * y.re + y.im * y.im) - pwm);
        e /= pwm;
        e = CLAMP(e, -2.0f, 2.0f);
        double tnext = tn - GG1 * e; /* e > 0: muestreo tardío */
        w -= GG2 * e;
        w = CLAMP(w, VDL2_SPS * 0.995, VDL2_SPS * 1.005);
        t = tnext;
        /* final de ráfaga: la potencia cae por debajo de 1/4 de la inicial; se siguen 8 símbolos más
         * porque el filtro retrasa la caída */
        float pk = y.re * y.re + y.im * y.im;
        if (k <= VDL2_UW_SYMS)
            pw_ref += pk / VDL2_UW_SYMS;
        else if (pk < 0.25f * pw_ref) {
            if (++low >= 2 && nb_drop < 0) {
                nb_drop = nb * 3; /* solo vale para el reparto de longitudes alternativas: no se corta aquí */
            }
        } else
            low = 0;
        /* decisión diferencial */
        cf32 d = cmul_conj(y, yprev);
        float ang = atan2f(d.im, d.re) - th;
        int inc = (int)lrintf(ang / ((float)M_PI / 4.0f)) & 7;
        float r = wrap_pi(ang - (float)inc * ((float)M_PI / 4.0f));
        th += (float)GTH * r;
        yprev = y;
        if (k >= stop_k)
            break;
        if (k >= VDL2_UW_SYMS) {
            incs[nb] = (uint8_t)inc;
            ress[nb++] = r;
        }
    }
    s->last_span = 0;
    bool ok = decode_syms(s, incs, ress, nb, nb_drop, emit, ctx);
    if (ok && s->nclaim < 8) { /* reclama la zona de la trama para que otra UW no la repita */
        s->claim_lo[s->nclaim] = bi - 300;
        s->claim_hi[s->nclaim++] = bi + (VDL2_UW_SYMS + s->last_span) * VDL2_SPS + 300;
    }
    free(incs);
    free(ress);
    return ok;
}

/* Una hipótesis de demodulación sobre la ráfaga completa: hasta 3 UW (la ráfaga puede contener varias
 * tramas seguidas o una UW falsa más fuerte que la buena). Devuelve true si decodificó alguna trama. */
static bool demod_try(vdl2_t *s, const hyp_t *h, const cf32 *x, int n, int start, double f0, dec_emit_fn emit, void *ctx)
{
    /* La UW se busca sobre la señal SIN filtrar (solo desrotada por el CFO grueso): el producto diferencial
     * no depende del desvío de frecuencia, mientras que un filtro de ~10 kHz atenuaría una señal que cae
     * a +4 kHz del CFO grueso (que es un promedio de toda la ráfaga y puede mezclar dos emisores). */
    derotate(x, n, f0, s->rate, s->x1);
    front_filter(s, &SEARCH_HYP, s->x1, n, s->zs);

    float inc_re[16], inc_im[16];
    for (int j = 1; j < 16; j++) {
        int v = VDL2_UW_BITS[3 * j] << 2 | VDL2_UW_BITS[3 * j + 1] << 1 | VDL2_UW_BITS[3 * j + 2];
        double ang = VDL2_GRAY_TO_INC[v] * M_PI / 4.0;
        inc_re[j] = (float)cos(ang);
        inc_im[j] = (float)sin(ang);
    }
    /* umbral de potencia: 3× el suelo de ruido de la banda por los 15 productos; descarta ventanas de
     * puro ruido (la métrica de fase sin ponderar da ≈ 0,26 de media allí). No se usa la potencia media
     * de la ráfaga: si una trama débil precede a otra 10 dB más fuerte, esa media la dejaría fuera. */
    float amin = 3.0f * 15.0f * s->floor;
    int i_lo = MAX(0, start - 4 * BLK), i_hi = MIN(n - 16 * VDL2_SPS - 8, start + 2000 * VDL2_SPS);
    /* métrica de la UW en todas las posiciones (una sola vez) */
    int np = MAX(0, i_hi - i_lo);
    if (np == 0)
        return false;
    if (np > s->mcap) {
        s->mcap = np + 1024;
        s->met = realloc(s->met, sizeof(float) * (size_t)s->mcap);
        s->msum = realloc(s->msum, sizeof(cf32) * (size_t)s->mcap);
    }
    for (int q = 0; q < np; q++) {
        int i0 = i_lo + q;
        float sr = 0, si = 0, den = 0;
        for (int j = 1; j < 16; j++) {
            cf32 d = cmul_conj(s->zs[i0 + j * VDL2_SPS], s->zs[i0 + (j - 1) * VDL2_SPS]);
            float a = hypotf(d.re, d.im);
            den += a;
            if (a < 1e-30f)
                continue;
            /* fasor unitario del producto diferencial · conj(incremento esperado). Sin ponderar por la
             * amplitud: con la ponderación, 1-2 productos fuertes de la rampa de subida o de bajada dan
             * módulo ≈ 1 y desplazan la UW verdadera (que el dumpvdl2 de referencia sí encuentra, porque
             * mide solo la fase de los 16 símbolos) */
            float ur = d.re / a, ui = d.im / a;
            sr += ur * inc_re[j] + ui * inc_im[j];
            si += ui * inc_re[j] - ur * inc_im[j];
        }
        s->met[q] = den > amin ? hypotf(sr, si) / 15.0f : 0;
        s->msum[q] = (cf32){sr, si};
    }
    int tlo[3], thi[3], nt = 0; /* UW ya probadas por esta hipótesis */
    bool any = false;
    for (int att = 0; att < 3; att++) {
        float best = 0;
        int bq = -1;
        for (int q = 0; q < np; q++)
            if (s->met[q] > best && !is_claimed(s, tlo, thi, nt, i_lo + q)) {
                best = s->met[q];
                bq = q;
            }
        if (bq < 0 || best < UW_THRESH)
            break;
        int bi = i_lo + bq;
        tlo[nt] = bi - 300;
        thi[nt++] = bi + 300;
        if (demod_at(s, h, x, n, bi, s->msum[bq], f0, emit, ctx))
            any = true;
        else
            s->unresolved = true; /* hay una UW sin trama: que pruebe otra hipótesis */
    }
    return any;
}

static void burst(vdl2_t *s, const cf32 *x, int n, int start, double now, dec_emit_fn emit, void *ctx)
{
    s->bursts++;
    s->now = now;
    if (n > s->wcap) {
        s->wcap = n + 4096;
        s->x1 = realloc(s->x1, sizeof(cf32) * (size_t)s->wcap);
        s->z = realloc(s->z, sizeof(cf32) * (size_t)s->wcap);
        s->zs = realloc(s->zs, sizeof(cf32) * (size_t)s->wcap);
    }
    if (n < 2000)
        return;
    /* nivel medio de la ráfaga (zona fuerte) */
    double pw = 0;
    int cnt = 0;
    for (int i = start; i < n; i++, cnt++)
        pw += (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
    s->last_level = power_to_db((float)(cnt ? pw / cnt : 0));

    s->nclaim = 0;
    s->trials = TRIALS_PER_BURST;
    int a = start, b = MIN(n, start + 8192);
    double f0 = coarse_cfo(s, x, a, b);
    bool ok = false;
    /* Hipótesis en orden; se sigue solo mientras quede alguna UW sin trama (así el coste de una ráfaga
     * normal es el de una hipótesis). 1.ª pasada: lectura directa (barata); 2.ª, solo si algo quedó sin
     * decodificar: Chase y borrados por fiabilidad, que cuestan cientos de pruebas RS por intento. */
    for (int pass = 0; pass < 2; pass++) {
        s->chase = pass == 1;
        for (int h = 0; h < NHYP; h++) {
            s->unresolved = false;
            ok |= demod_try(s, &HYPS[h], x, n, start, f0, emit, ctx);
            if (ok && !s->unresolved)
                break;
        }
        if (ok && !s->unresolved)
            break;
    }
    if (!ok)
        s->bad++;
}

/* ---- ciclo de vida ----------------------------------------------------------- */
static void *vdl2_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - VDL2_RATE) > 1.0) {
        snprintf(err, errlen, "VDL2 requiere 105 kHz");
        return NULL;
    }
    vdl2_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->rate = cfg->rate;
    pulse_rrc(s->rrc, 0.6, VDL2_SPS, 8, PULSE_NORM_ENERGY);
    s->fft = fft_plan_create(256);
    s->fbuf = malloc(sizeof(cf32) * 256);
    s->psd = malloc(sizeof(float) * 256);
    s->cap = 4 * BLK;
    s->buf = malloc(sizeof(cf32) * (size_t)s->cap);
    if (!s->fft || !s->fbuf || !s->psd || !s->buf) {
        snprintf(err, errlen, "sin memoria");
        free(s);
        return NULL;
    }
    return s;
}

static void vdl2_destroy(void *st)
{
    vdl2_t *s = st;
    if (!s)
        return;
    fft_plan_destroy(s->fft);
    free(s->fbuf);
    free(s->psd);
    free(s->buf);
    free(s->x1);
    free(s->z);
    free(s->zs);
    free(s->met);
    free(s->msum);
    sb_free(&s->evsb);
    free(s);
}

static void finish_burst(vdl2_t *s, double now, dec_emit_fn emit, void *ctx)
{
    burst(s, s->buf, s->nbuf, s->trig, now, emit, ctx);
    s->nbuf = 0;
    s->active = false;
    s->quiet = 0;
}

static void vdl2_process(void *st, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    vdl2_t *s = st;
    for (int i = 0; i < n; i++) {
        if (s->nbuf == s->cap) {
            if (s->cap >= MAX_BURST) { /* ráfaga desmesurada: se procesa lo acumulado */
                finish_burst(s, now, emit, ctx);
            } else {
                s->cap *= 2;
                s->buf = realloc(s->buf, sizeof(cf32) * (size_t)s->cap);
            }
        }
        s->buf[s->nbuf++] = iq[i];
        s->blk_pwr += iq[i].re * iq[i].re + iq[i].im * iq[i].im;
        if (++s->blk_n < BLK)
            continue;
        float p = s->blk_pwr / BLK;
        s->blk_pwr = 0;
        s->blk_n = 0;
        if (!s->floor_init) {
            s->floor = p;
            s->floor_init = true;
        }
        if (s->floor < 1e-10f) /* señal sintética sin ruido: evita un umbral nulo */
            s->floor = 1e-10f;
        if (!s->active) {
            if (p > s->floor * TH_ON && s->nbuf >= BLK) {
                s->active = true;
                s->quiet = 0;
                s->trig = MAX(0, s->nbuf - BLK);
                /* conserva PRE_BLOCKS de contexto previo */
                int keep = MIN(s->nbuf, (PRE_BLOCKS + 1) * BLK);
                int drop = s->nbuf - keep;
                if (drop > 0) {
                    memmove(s->buf, s->buf + drop, sizeof(cf32) * (size_t)keep);
                    s->nbuf = keep;
                    s->trig -= drop;
                }
            } else {
                if (p < s->floor * 1.5f)
                    s->floor += 0.1f * (p - s->floor);
                else
                    s->floor *= 1.0003f;
                int keep = PRE_BLOCKS * BLK;
                if (s->nbuf > keep + BLK) { /* recorta el historial de reposo */
                    memmove(s->buf, s->buf + (s->nbuf - keep), sizeof(cf32) * (size_t)keep);
                    s->nbuf = keep;
                }
            }
        } else {
            if (p < s->floor * TH_OFF) {
                if (++s->quiet >= QUIET_BLOCKS)
                    finish_burst(s, now, emit, ctx);
            } else
                s->quiet = 0;
        }
    }
}

static void tab_json(sb_t *sb, node_t *tab, int *n, double now, bool aircraft)
{
    int w = 0;
    for (int i = 0; i < *n; i++)
        if (now - tab[i].last_t <= TAB_TTL)
            tab[w++] = tab[i];
    *n = w;
    for (int i = 0; i < w; i++) {
        const node_t *a = &tab[i];
        if (i)
            sb_puts(sb, ",");
        sb_printf(sb, "{\"icao\":\"%06X\",\"msgs\":%d,\"age\":%.0f", a->icao, a->msgs, now - a->last_t);
        if (aircraft) {
            sb_puts(sb, ",\"reg\":");
            sb_json_str(sb, a->reg);
            sb_puts(sb, ",\"flight\":");
            sb_json_str(sb, a->flight);
        }
        sb_puts(sb, ",\"last\":");
        sb_json_str(sb, a->last);
        sb_puts(sb, "}");
    }
}

static void vdl2_state(void *st, sb_t *sb, double now)
{
    vdl2_t *s = st;
    sb_printf(sb, "{\"bursts\":%lu,\"frames\":%lu,\"bad\":%lu,\"ac\":[", s->bursts, s->frames, s->bad);
    tab_json(sb, s->ac, &s->nac, now, true);
    sb_puts(sb, "],\"gs\":[");
    tab_json(sb, s->gs, &s->ngs, now, false);
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_VDL2 = {
    .id = "vdl2",
    .name = "VDL Modo 2",
    .group = "Aviación",
    .desc = "Enlace de datos VDL2 (D8PSK 10 500 Bd) con AVLC y ACARS.",
    .kind = DEC_NARROW,
    .rate = 105000,
    .bw_hz = 25000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {136975000LL, 136875000LL, 136775000LL, 136725000LL},
    .create = vdl2_create,
    .destroy = vdl2_destroy,
    .process = vdl2_process,
    .state_json = vdl2_state,
};
