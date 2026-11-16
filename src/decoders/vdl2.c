/*
 * vdl2.c — Decodificador VDL Modo 2 (D8PSK 10 500 símb/s, ICAO Doc 9776 /
 * ETSI EN 301 841-1). Ver docs/decoders/vdl2.md.
 *
 * Señal. Cada ráfaga lleva 3 bits por símbolo codificados en el INCREMENTO de fase
 * (D8PSK: Δφ = k·π/4), filtrada con raíz de coseno alzado α = 0,6. Una ráfaga =
 * rampa | palabra única (UW, 16 símbolos conocidos) | cabecera de 25 bits | datos.
 *
 * Cadena (a 105 kHz = 10 muestras/símbolo). El decodificador trabaja POR RÁFAGAS:
 *   1. Detector de energía por bloques de 256 muestras frente a un suelo de ruido
 *      que se sigue solo en reposo; la ráfaga se guarda entera (con 3 bloques
 *      previos) y se procesa al cerrarse el squelch. Así no hay coste en reposo.
 *   2. CFO grueso: centroide del espectro de la ráfaga (Welch, FFT 256, suelo de
 *      ruido restado). Hace falta porque el desvío admitido (±6 kHz) es del orden
 *      del ancho de la señal (16,8 kHz) y se perdería la mitad con el filtro
 *      adaptado. Se desrota y se filtra con el RRC adaptado.
 *   3. Búsqueda de la UW: para cada posición de muestra se correlaciona el
 *      producto diferencial z_k·conj(z_{k−1}) con los 15 incrementos conocidos. El
 *      módulo de la suma no depende del desvío de frecuencia ni de la fase
 *      inicial; sí fija el instante de muestreo (resolución de 1 muestra).
 *   4. CFO fino = arg(Σ d_k·e^{−jΔ_k}) / (2π·T): la suma conserva una fase
 *      constante 2π·f·T, inambigua hasta ±5,25 kHz. Se vuelve a desrotar.
 *   5. Reloj de símbolo: lazo de Gardner con interpolador de Farrow cúbico,
 *      semillado por la UW, que absorbe ±100 ppm (4 muestras en una ráfaga de 4 s).
 *   6. Demodulación diferencial: Δ = round((arg(y_k·conj(y_{k−1})) − θ)/(π/4)); θ
 *      sigue la deriva de fase residual (lazo de primer orden).
 *   7. Cabecera (25,20) → longitud; reparto en bloques RS; desaleatorizado;
 *      desentrelazado; RS(255,249) con borrados; flujo HDLC; FCS.
 *   8. AVLC: direcciones, tipo de trama, y ACARS sobre AVLC si la información
 *      empieza por SOH.
 *
 * Solo se emiten tramas con FCS correcto. Las constantes de la capa física que no
 * se han podido contrastar con una norma están marcadas en vdl2_frame.h/.c.
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
    fft_plan_t *fft;
    cf32 *fbuf;
    float *psd;
    /* tablas y estadísticas */
    node_t ac[MAX_TAB], gs[MAX_TAB];
    int nac, ngs;
    unsigned long bursts, frames, bad;
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
    /* FIR de 81 coeficientes, convolución directa con ceros previos */
    const int nt = 81;
    for (int i = 0; i < n; i++) {
        float r = 0, q = 0;
        int k0 = i - (nt - 1) > 0 ? 0 : (nt - 1) - i;
        for (int k = k0; k < nt; k++) {
            const cf32 *p = &in[i - (nt - 1) + k];
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
    s->frames++;
    /* ACARS sobre AVLC */
    acars_blk_t b;
    bool has_acars = false;
    if (a.info_len > 14 && a.info[0] == ACARS_SOH && (a.ctrl & 1) == 0) {
        uint8_t pl[300];
        int m = 0, e = -1;
        for (int i = 1; i < a.info_len && m < (int)sizeof pl; i++) {
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
    node_t *src = node_get(a.src.type == 4 || a.src.type == 6 ? s->gs : s->ac,
                           a.src.type == 4 || a.src.type == 6 ? &s->ngs : &s->nac, a.src.icao, a.src.type, s->now);
    src->msgs++;
    src->last_t = s->now;
    src->type = a.src.type;
    if (has_acars) {
        snprintf(src->reg, sizeof src->reg, "%s", b.reg);
        if (b.flight[0])
            snprintf(src->flight, sizeof src->flight, "%s", b.flight);
    }
    node_t *dst = NULL;
    if (a.dst.type == 4 || a.dst.type == 6)
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

static void burst(vdl2_t *s, const cf32 *x, int n, int start, double now, dec_emit_fn emit, void *ctx)
{
    s->bursts++;
    s->now = now;
    if (n > s->wcap) {
        s->wcap = n + 4096;
        s->x1 = realloc(s->x1, sizeof(cf32) * (size_t)s->wcap);
        s->z = realloc(s->z, sizeof(cf32) * (size_t)s->wcap);
    }
    if (n < 2000)
        return;
    /* nivel medio de la ráfaga (zona fuerte) */
    double pw = 0;
    int cnt = 0;
    for (int i = start; i < n; i++, cnt++)
        pw += (double)x[i].re * x[i].re + (double)x[i].im * x[i].im;
    s->last_level = power_to_db((float)(cnt ? pw / cnt : 0));
    /* amplitud típica de z en la zona útil: descarta posiciones de "silencio" en las
     * que el cociente módulo/suma amplificaría ruido numérico */
    float amin = 0.35f * sqrtf((float)(cnt ? pw / cnt : 0)) * 15.0f;

    int a = start, b = MIN(n, start + 8192);
    double f0 = coarse_cfo(s, x, a, b);
    derotate(x, n, f0, s->rate, s->x1);
    rrc_filter(s, s->x1, n, s->z);

    /* --- búsqueda de la UW --- */
    float inc_re[16], inc_im[16];
    for (int j = 1; j < 16; j++) {
        int v = VDL2_UW_BITS[3 * j] << 2 | VDL2_UW_BITS[3 * j + 1] << 1 | VDL2_UW_BITS[3 * j + 2];
        double ang = VDL2_GRAY_TO_INC[v] * M_PI / 4.0;
        inc_re[j] = (float)cos(ang);
        inc_im[j] = (float)sin(ang);
    }
    int i_lo = MAX(0, start - 4 * BLK), i_hi = MIN(n - 16 * VDL2_SPS - 8, start + 2000 * VDL2_SPS);
    float best = 0;
    int bi = -1;
    cf32 bsum = {0, 0};
    for (int i0 = i_lo; i0 < i_hi; i0++) {
        float sr = 0, si = 0, den = 0;
        for (int j = 1; j < 16; j++) {
            cf32 d = cmul_conj(s->z[i0 + j * VDL2_SPS], s->z[i0 + (j - 1) * VDL2_SPS]);
            /* d·conj(inc) */
            sr += d.re * inc_re[j] + d.im * inc_im[j];
            si += d.im * inc_re[j] - d.re * inc_im[j];
            den += hypotf(d.re, d.im);
        }
        float m = den > amin ? hypotf(sr, si) / den : 0;
        if (m > best) {
            best = m;
            bi = i0;
            bsum = (cf32){sr, si};
        }
    }
    if (bi < 0 || best < UW_THRESH)
        return;
    double f1 = atan2(bsum.im, bsum.re) / (2.0 * M_PI) * VDL2_BAUD;
    double ftot = f0 + f1;
    derotate(x, n, ftot, s->rate, s->x1);
    rrc_filter(s, s->x1, n, s->z);
    s->last_cfo = ftot;

    /* --- símbolos con lazo de reloj de Gardner --- */
    int maxsym = (n - bi) / VDL2_SPS - 2;
    if (maxsym < 40)
        return;
    uint8_t *bits = malloc((size_t)(maxsym + 4) * 3);
    int nb = 0; /* bits a partir del símbolo 16 (índice de flujo 48) */
    double t = bi, w = VDL2_SPS;
    cf32 yprev = interp(s->z, t);
    float th = 0, pwm = yprev.re * yprev.re + yprev.im * yprev.im + 1e-12f;
    float pw_ref = 0; /* potencia media de los primeros símbolos (para detectar el final) */
    int low = 0, nsym_end = maxsym;
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
        double tnext = tn - 0.02 * e; /* e > 0: muestreo tardío */
        w -= 0.0005 * e;
        w = CLAMP(w, VDL2_SPS * 0.995, VDL2_SPS * 1.005);
        t = tnext;
        /* final de ráfaga: la potencia cae por debajo de 1/4 de la inicial */
        float pk = y.re * y.re + y.im * y.im;
        if (k <= VDL2_UW_SYMS)
            pw_ref += pk / VDL2_UW_SYMS;
        else if (pk < 0.25f * pw_ref) {
            if (++low >= 2) {
                nsym_end = k - 1;
                break;
            }
        } else
            low = 0;
        /* decisión diferencial */
        cf32 d = cmul_conj(y, yprev);
        float ang = atan2f(d.im, d.re) - th;
        int inc = (int)lrintf(ang / ((float)M_PI / 4.0f)) & 7;
        float r = wrap_pi(ang - (float)inc * ((float)M_PI / 4.0f));
        th += 0.02f * r;
        yprev = y;
        if (k >= VDL2_UW_SYMS) {
            int v = VDL2_INC_TO_GRAY[inc];
            bits[nb++] = (uint8_t)((v >> 2) & 1);
            bits[nb++] = (uint8_t)((v >> 1) & 1);
            bits[nb++] = (uint8_t)(v & 1);
        }
    }
    (void)nsym_end;
    if (nb < VDL2_HDR_BITS + 8 * 12) {
        free(bits);
        s->bad++;
        return;
    }
    /* La longitud: la cabecera de 25 bits (3 reservados + 17 de longitud + 5 de paridad) NO se
     * pudo contrastar con la grabación real (ver docs/decoders/vdl2.md), así que no se usa
     * como única fuente: se prueba primero la longitud de la cabecera si es verosímil y después
     * todas las longitudes D tales que datos+paridad RS cabe en los bits recibidos (±3 bytes por
     * los símbolos de rampa de bajada). */
    uint32_t hlen = 0;
    int hrc = vdl2_hdr_decode(bits, &hlen);
    int avail = (nb - VDL2_HDR_BITS) / 8;
    int cand[64], nc = 0;
    if (hrc >= 0 && hlen >= 100 && hlen <= VDL2_MAX_LEN && (int)((hlen + 7) / 8) <= avail)
        cand[nc++] = (int)((hlen + 7) / 8);
    for (int tb = avail; tb >= avail - 3 && tb > 12; tb--)
        for (int D = tb; D >= 1 && D >= tb - 24; D--)
            if (vdl2_encoded_bytes(D) == tb && nc < 64)
                cand[nc++] = D;
    uint8_t *db = bits + VDL2_HDR_BITS;
    int nbits_db = nb - VDL2_HDR_BITS;
    vdl2_scramble(db, nbits_db);
    bool done = false;
    for (int c = 0; c < nc && !done; c++) {
        int D = cand[c], E = vdl2_encoded_bytes(D);
        if (8 * E > nbits_db)
            continue;
        uint8_t *enc = malloc((size_t)E), *dat = malloc((size_t)D + 255);
        for (int i = 0; i < E; i++) {
            uint8_t v = 0;
            for (int q = 0; q < 8; q++)
                v = (uint8_t)(v << 1 | db[8 * i + q]);
            enc[i] = v;
        }
        int fixed = vdl2_fec_decode(enc, D, dat);
        if (fixed >= 0) {
            int hl = 8 * D;
            uint8_t *hb = malloc((size_t)hl);
            for (int i = 0; i < hl; i++)
                hb[i] = (uint8_t)((dat[i >> 3] >> (7 - (i & 7))) & 1);
            cbctx_t cc = {s, emit, ctx, fixed};
            s->last_rs = fixed;
            if (vdl2_hdlc_decode(hb, hl, on_frame, &cc) > 0)
                done = true;
            free(hb);
        }
        free(enc);
        free(dat);
    }
    if (!done)
        s->bad++;
    free(bits);
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
