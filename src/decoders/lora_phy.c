/*
 * lora_phy.c — Capa física LoRa: codificación, modulación y receptor.
 * La teoría y el esquema general están en lora_phy.h; aquí van los detalles
 * numéricos y su justificación.
 */
#include "lora_phy.h"

#include "../dsp/fft.h"
#include "../dsp/filter.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/*  Tablas y bloques de codificación                                      */
/* ===================================================================== */

/* Secuencia de blanqueo: LFSR de 8 bits, polinomio x^8+x^6+x^5+x^4+1, semilla
 * 0xFF; la salida de cada paso es el estado. Empieza FF FE FC F8 F0 E1 C2 85
 * 0B 17 2F 5E BC 78 F1 E3 … (coincide con la tabla de gr-lora_sdr). Periodo
 * 255. El byte i de la carga se opera con el byte i de la secuencia. */
static uint8_t whiten_tab[256];
/* Decodificación Hamming por búsqueda del codeword más cercano. */
static uint8_t ham_nib[5][256];
static uint8_t ham_stat[5][256]; /* 0 sin errores, 1 corregido, 2 no corregible */
static pthread_once_t tabs_once = PTHREAD_ONCE_INIT;

/* Palabra de 4+cr bits: bits 0..3 = datos d0..d3 (d0 = LSB del nibble) y
 * paridades en los bits 4..: p0=d0^d1^d2, p1=d1^d2^d3, p2=d0^d1^d3,
 * p3=d0^d2^d3. Para cr=1 hay una sola paridad (bit 4) = d0^d1^d2^d3. */
static unsigned ham_encode(unsigned nib, int cr)
{
    unsigned d0 = nib & 1, d1 = (nib >> 1) & 1, d2 = (nib >> 2) & 1, d3 = (nib >> 3) & 1;
    unsigned w = nib & 15;
    if (cr == 1)
        return w | ((d0 ^ d1 ^ d2 ^ d3) << 4);
    unsigned p[4] = {d0 ^ d1 ^ d2, d1 ^ d2 ^ d3, d0 ^ d1 ^ d3, d0 ^ d2 ^ d3};
    for (int i = 0; i < cr; i++)
        w |= p[i] << (4 + i);
    return w;
}

static void init_tabs(void)
{
    unsigned s = 0xFF;
    for (int i = 0; i < 256; i++) {
        whiten_tab[i] = (uint8_t)s;
        unsigned fb = ((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1;
        s = ((s << 1) | fb) & 0xFF;
    }
    for (int cr = 1; cr <= 4; cr++)
        for (unsigned w = 0; w < (1u << (4 + cr)); w++) {
            int best = 99, nb = 0, nbest = 0;
            for (unsigned n = 0; n < 16; n++) {
                int d = __builtin_popcount(w ^ ham_encode(n, cr));
                if (d < best) {
                    best = d;
                    nb = (int)n;
                    nbest = 1;
                } else if (d == best)
                    nbest++;
            }
            if (nbest > 1) { /* empate: no se puede corregir; se devuelven los bits de datos */
                ham_nib[cr][w] = (uint8_t)(w & 15);
                ham_stat[cr][w] = 2;
            } else {
                ham_nib[cr][w] = (uint8_t)nb;
                ham_stat[cr][w] = best == 0 ? 0 : 1;
            }
        }
}

uint8_t lora_whitening_byte(int i)
{
    pthread_once(&tabs_once, init_tabs);
    return whiten_tab[((unsigned)i) % 255];
}

/* CRC de carga: CCITT (poly 0x1021, init 0, MSB primero, sin xor final) de los
 * n−2 primeros bytes, XOR con los dos últimos bytes de la carga como palabra
 * (penúltimo en el byte alto). Es la rareza documentada por gr-lora_sdr
 * (add_crc / crc_verif) y rpp0/gr-lora. Se transmite primero el byte bajo. */
uint16_t lora_crc16(const uint8_t *p, int n)
{
    uint16_t crc = 0;
    int m = n >= 2 ? n - 2 : n;
    for (int i = 0; i < m; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    if (n >= 2)
        crc ^= (uint16_t)((p[n - 2] << 8) | p[n - 1]);
    else if (n == 1)
        crc ^= 0; /* una carga de 1 byte no tiene los 2 últimos bytes: sin definir */
    return crc;
}

/* Checksum de 5 bits de la cabecera explícita (gr-lora_sdr header_impl.cc):
 * h0 = longitud>>4, h1 = longitud&15, h2 = (cr<<1)|crc. */
static unsigned bit(unsigned v, int i) { return (v >> i) & 1; }
static unsigned header_checksum(unsigned h0, unsigned h1, unsigned h2)
{
    unsigned c4 = bit(h0, 3) ^ bit(h0, 2) ^ bit(h0, 1) ^ bit(h0, 0);
    unsigned c3 = bit(h0, 3) ^ bit(h1, 3) ^ bit(h1, 2) ^ bit(h1, 1) ^ bit(h2, 0);
    unsigned c2 = bit(h0, 2) ^ bit(h1, 3) ^ bit(h1, 0) ^ bit(h2, 3) ^ bit(h2, 1);
    unsigned c1 = bit(h0, 1) ^ bit(h1, 2) ^ bit(h1, 0) ^ bit(h2, 2) ^ bit(h2, 1) ^ bit(h2, 0);
    unsigned c0 = bit(h0, 0) ^ bit(h1, 1) ^ bit(h2, 3) ^ bit(h2, 2) ^ bit(h2, 1) ^ bit(h2, 0);
    return (c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0;
}

bool lora_ldro_auto(int sf, int bw_hz) { return (double)(1 << sf) / bw_hz > 0.016; }

/* Nº de símbolos de una trama con cabecera explícita. */
int lora_frame_nsyms(int len, int sf, int cr, bool crc, bool ldro)
{
    int ppm_p = ldro ? sf - 2 : sf;
    int nn = 2 * (len + (crc ? 2 : 0)); /* nibbles de carga (+CRC) */
    int in_hdr = (sf - 2) - 5;          /* nibbles de carga que caben en el bloque de cabecera */
    int rem = nn > in_hdr ? nn - in_hdr : 0;
    int blocks = (rem + ppm_p - 1) / ppm_p;
    return 8 + blocks * (4 + cr);
}

/* Gray: s = binario → código; inverso por prefijo XOR. */
static unsigned gray_enc(unsigned v) { return v ^ (v >> 1); }
static unsigned gray_dec(unsigned g)
{
    unsigned v = g;
    for (unsigned s = 1; s < 16; s <<= 1)
        v ^= v >> s;
    return v;
}

/* Entrelazado diagonal (LoRa-SDR / gr-lora_sdr interleaver_impl.cc): el bit k
 * del codeword (m+k) mod PPM va al bit m del símbolo k. */
static void interleave(const unsigned *cw, int ppm, int cwlen, unsigned *sym)
{
    for (int k = 0; k < cwlen; k++) {
        unsigned v = 0;
        for (int m = 0; m < ppm; m++)
            v |= ((cw[(m + k) % ppm] >> k) & 1u) << m;
        sym[k] = v;
    }
}

static void deinterleave(const unsigned *sym, int ppm, int cwlen, unsigned *cw)
{
    for (int m = 0; m < ppm; m++)
        cw[m] = 0;
    for (int k = 0; k < cwlen; k++)
        for (int m = 0; m < ppm; m++)
            cw[(m + k) % ppm] |= ((sym[k] >> m) & 1u) << k;
}

/* Símbolo transmitido a partir de los PPM bits entrelazados: Gray inverso,
 * relleno de ceros hasta SF bits y desplazamiento +1 (gr-lora_sdr gray_demap:
 * los datos van 1 bin por encima de preámbulo y sync). */
static uint16_t tx_symbol(unsigned bits, int sf, int ppm)
{
    unsigned v = gray_dec(bits) << (sf - ppm);
    return (uint16_t)((v + 1) & ((1u << sf) - 1));
}

int lora_encode_symbols(const uint8_t *pl, int len, int sf, int cr, bool crc, bool ldro, uint16_t *syms, int cap)
{
    pthread_once(&tabs_once, init_tabs);
    if (sf < 7 || sf > 12 || cr < 1 || cr > 4 || len < 1 || len > LORA_MAX_PAYLOAD)
        return -1;
    int nsym = lora_frame_nsyms(len, sf, cr, crc, ldro);
    if (nsym > cap)
        return -1;
    int nbytes = len + (crc ? 2 : 0);
    uint8_t bytes[LORA_MAX_PAYLOAD + 2];
    memcpy(bytes, pl, (size_t)len);
    if (crc) {
        uint16_t c = lora_crc16(pl, len);
        bytes[len] = (uint8_t)(c & 0xFF);
        bytes[len + 1] = (uint8_t)(c >> 8);
    }
    /* Carga y CRC blanqueados con la secuencia continua. */
    uint8_t nib[2 * (LORA_MAX_PAYLOAD + 2) + 16];
    int nn = 0;
    unsigned h0 = (unsigned)len >> 4, h1 = (unsigned)len & 15, h2 = ((unsigned)cr << 1) | (crc ? 1u : 0u);
    unsigned ck = header_checksum(h0, h1, h2);
    nib[nn++] = (uint8_t)h0;
    nib[nn++] = (uint8_t)h1;
    nib[nn++] = (uint8_t)h2;
    nib[nn++] = (uint8_t)(ck >> 4);
    nib[nn++] = (uint8_t)(ck & 15);
    for (int i = 0; i < nbytes; i++) {
        /* El blanqueo solo cubre la carga; los 2 bytes de CRC van SIN blanquear
         * (contrastado con loratx/lorarx de dxlaprs-lora y gr-lora_sdr). */
        uint8_t b = (uint8_t)(i < len ? bytes[i] ^ lora_whitening_byte(i) : bytes[i]);
        nib[nn++] = b & 15;
        nib[nn++] = b >> 4;
    }
    while (nn < 5 + (sf - 2 - 5) || (nn - (sf - 2)) % (ldro ? sf - 2 : sf) != 0)
        nib[nn++] = 0; /* relleno hasta cerrar bloques */
    /* Bloque de cabecera: PPM = SF−2 codewords a 4/8, 8 símbolos. */
    unsigned cw[16], sy[8];
    int pos = 0, ns = 0;
    int ppm = sf - 2;
    for (int i = 0; i < ppm; i++)
        cw[i] = ham_encode(nib[pos++], 4);
    interleave(cw, ppm, 8, sy);
    for (int k = 0; k < 8; k++)
        syms[ns++] = tx_symbol(sy[k], sf, ppm);
    ppm = ldro ? sf - 2 : sf;
    while (pos < nn) {
        for (int i = 0; i < ppm; i++)
            cw[i] = ham_encode(nib[pos++], cr);
        interleave(cw, ppm, 4 + cr, sy);
        for (int k = 0; k < 4 + cr; k++)
            syms[ns++] = tx_symbol(sy[k], sf, ppm);
    }
    return ns == nsym ? ns : -1;
}

/* ===================================================================== */
/*  Modulador                                                             */
/* ===================================================================== */

/* Fase del chirp de valor s en el instante u (en chips): continua en el salto
 * de +BW/2 a −BW/2 (ambos tramos valen 0 rad allí). Chirp base: π(u²/N − u). */
static double chirp_phase(double u, double s, double N)
{
    double x = s + u;
    if (x >= N)
        x -= N;
    return M_PI * (x * x / N - x);
}

static void push_chirp(iqbuf_t *o, double fs, int bw, int sf, double s, int down, double nsamp_frac, double ph0, float amp)
{
    double N = (double)(1 << sf), os = fs / bw;
    int ns = (int)lrint(N * os * nsamp_frac);
    iqbuf_reserve(o, ns);
    for (int n = 0; n < ns; n++) {
        double ph = chirp_phase((double)n / os, s, N);
        if (down)
            ph = -ph;
        ph += ph0;
        iqbuf_push(o, (cf32){(float)(amp * cos(ph)), (float)(amp * sin(ph))});
    }
}

void lora_push_silence(iqbuf_t *o, int n)
{
    iqbuf_reserve(o, n);
    for (int i = 0; i < n; i++)
        iqbuf_push(o, (cf32){0, 0});
}

void lora_modulate_frame(iqbuf_t *o, double fs, int bw, int sf, int sync, int npre, const uint16_t *syms, int nsym,
                         float amp)
{
    o->rate = fs;
    double ph0 = 0.37; /* fase de portadora arbitraria: el receptor es no coherente entre símbolos */
    for (int i = 0; i < npre; i++)
        push_chirp(o, fs, bw, sf, 0, 0, 1.0, ph0, amp);
    push_chirp(o, fs, bw, sf, (double)(((sync >> 4) & 15) << 3), 0, 1.0, ph0, amp);
    push_chirp(o, fs, bw, sf, (double)((sync & 15) << 3), 0, 1.0, ph0, amp);
    push_chirp(o, fs, bw, sf, 0, 1, 1.0, ph0, amp);
    push_chirp(o, fs, bw, sf, 0, 1, 1.0, ph0, amp);
    push_chirp(o, fs, bw, sf, 0, 1, 0.25, ph0, amp);
    for (int i = 0; i < nsym; i++)
        push_chirp(o, fs, bw, sf, (double)syms[i], 0, 1.0, ph0, amp);
}

/* ===================================================================== */
/*  Receptor                                                              */
/* ===================================================================== */

#define THR_DET 9.0f   /* pico/media de bins ≥ este valor: candidato */
#define THR_PRE 5.0f   /* preámbulo todavía "activo" */
#define RUN_MIN 4      /* ventanas consecutivas con el mismo bin */
#define FIR_NT 31
#define FIR_H 15
#define MAX_PRE_WIN 96 /* ventanas de preámbulo+sync antes de rendirse */
#define MAX_SYMS 1024

typedef enum { ST_SEARCH, ST_PRE, ST_DATA } rx_state_t;

typedef struct {
    int sf, N;
    fft_plan_t *p2, *p1;
    cf32 *dn2, *up2; /* chirp base conjugado / directo a 2·BW (2N) */
    cf32 *dn1, *up1; /* ídem a BW (N) */
    cf32 *z;         /* mezcla: 2N + 2H muestras */
    cf32 *w2;        /* 2N */
    cf32 *w1;        /* N */
    float *pw;       /* N */
    float fir[FIR_NT];
    /* búfer de entrada a 2·BW */
    cf32 *buf;
    int64_t base;    /* índice absoluto de buf[0] */
    int64_t len, cap;
    rx_state_t st;
    int64_t wpos;    /* próxima ventana de detección */
    int run;
    int64_t run_first;
    int last_bin;
    /* ventanas de preámbulo */
    int npw;
    int pbin_u[MAX_PRE_WIN], pbin_d[MAX_PRE_WIN];
    float prat_u[MAX_PRE_WIN], prat_d[MAX_PRE_WIN];
    int pre_quiet;
    int down_f;      /* primera ventana con chirp descendente, o −1 */
    /* trama en curso */
    int64_t D;       /* inicio del primer símbolo de datos */
    double eps;      /* CFO en bins */
    double m, v;     /* offset de bin predicho y su pendiente (bins/símbolo) */
    int sync_word;
    double slope0;
    int nsym_done;
    int nsym_total;  /* 0 hasta conocer la cabecera */
    uint16_t sraw[MAX_SYMS];
    int hdr_cr, hdr_len;
    bool hdr_crc, ldro;
    double acc_sig, acc_noise;
    int acc_n;
    int64_t frame_t0;
} sf_rx_t;

struct lora_rx {
    lora_rx_cfg_t cfg;
    int decim;
    firdec_t dec;
    bool have_dec;
    cf32 *tmp;
    int tmp_cap;
    sf_rx_t *sf[13];
    unsigned long n_sync_ok, n_hdr_bad, n_crc_bad, n_frames;
    int64_t in_count;  /* muestras de entrada procesadas (tasa fs) */
};

static void sf_free(sf_rx_t *s)
{
    if (!s)
        return;
    fft_plan_destroy(s->p2);
    fft_plan_destroy(s->p1);
    free(s->dn2);
    free(s->up2);
    free(s->dn1);
    free(s->up1);
    free(s->z);
    free(s->w2);
    free(s->w1);
    free(s->pw);
    free(s->buf);
    free(s);
}

static void make_fir(float *h, double delta);

static sf_rx_t *sf_new(int sf)
{
    sf_rx_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    int N = 1 << sf;
    s->sf = sf;
    s->N = N;
    s->p2 = fft_plan_create(2 * N);
    s->p1 = fft_plan_create(N);
    s->dn2 = malloc(sizeof(cf32) * 2 * (size_t)N);
    s->up2 = malloc(sizeof(cf32) * 2 * (size_t)N);
    s->dn1 = malloc(sizeof(cf32) * (size_t)N);
    s->up1 = malloc(sizeof(cf32) * (size_t)N);
    s->z = malloc(sizeof(cf32) * (2 * (size_t)N + 2 * FIR_H + 2));
    s->w2 = malloc(sizeof(cf32) * 2 * (size_t)N);
    s->w1 = malloc(sizeof(cf32) * (size_t)N);
    s->pw = malloc(sizeof(float) * (size_t)N);
    if (!s->p2 || !s->p1 || !s->dn2 || !s->up2 || !s->dn1 || !s->up1 || !s->z || !s->w2 || !s->w1 || !s->pw) {
        sf_free(s);
        return NULL;
    }
    for (int n = 0; n < 2 * N; n++) {
        double u = n / 2.0, ph = M_PI * (u * u / N - u);
        s->up2[n] = (cf32){(float)cos(ph), (float)sin(ph)};
        s->dn2[n] = (cf32){(float)cos(ph), (float)-sin(ph)};
    }
    for (int n = 0; n < N; n++) {
        double ph = M_PI * ((double)n * n / N - n);
        s->up1[n] = (cf32){(float)cos(ph), (float)sin(ph)};
        s->dn1[n] = (cf32){(float)cos(ph), (float)-sin(ph)};
    }
    /* Paso bajo de diezmado 2→1: corte en 0,55·BW (0,275 de fs=2·BW). */
    make_fir(s->fir, 0.0);
    s->down_f = -1;
    return s;
}

/* ---- búfer ---- */
static void buf_append(sf_rx_t *s, const cf32 *x, int n)
{
    if (s->len + n > s->cap) {
        int64_t nc = (s->len + n) * 2 + 4096;
        cf32 *nb = realloc(s->buf, sizeof(cf32) * (size_t)nc);
        if (!nb)
            return;
        s->buf = nb;
        s->cap = nc;
    }
    memcpy(s->buf + s->len, x, sizeof(cf32) * (size_t)n);
    s->len += n;
}

/* Descarta lo anterior a keep_from (índice absoluto). */
static void buf_trim(sf_rx_t *s, int64_t keep_from)
{
    int64_t drop = keep_from - s->base;
    if (drop <= 0)
        return;
    if (drop > s->len)
        drop = s->len;
    if (drop < (1 << 16) && drop < s->len / 2)
        return; /* evita mover memoria a cada paso */
    memmove(s->buf, s->buf + drop, sizeof(cf32) * (size_t)(s->len - drop));
    s->len -= drop;
    s->base += drop;
}

static inline int64_t buf_end(const sf_rx_t *s) { return s->base + s->len; }

/* ---- ventana de detección a 2·BW: dechirp + FFT(2N) + pliegue de potencia ----
 * Con muestreo 2·BW la parte del chirp tras el salto de frecuencia aparece en
 * el bin k+N en lugar de k; sumar potencias de ambos recoge toda la energía
 * (≥ 50 %). Devuelve bin de pico y razón pico/media del resto. */
static void det_window(sf_rx_t *s, int64_t start, bool down, int *bin, float *ratio)
{
    int N = s->N;
    const cf32 *ref = down ? s->up2 : s->dn2;
    const cf32 *x = s->buf + (start - s->base);
    for (int n = 0; n < 2 * N; n++) {
        s->w2[n].re = x[n].re * ref[n].re - x[n].im * ref[n].im;
        s->w2[n].im = x[n].re * ref[n].im + x[n].im * ref[n].re;
    }
    fft_exec(s->p2, s->w2);
    double tot = 0;
    float pk = 0;
    int kb = 0;
    for (int k = 0; k < N; k++) {
        float p = s->w2[k].re * s->w2[k].re + s->w2[k].im * s->w2[k].im + s->w2[k + N].re * s->w2[k + N].re +
                  s->w2[k + N].im * s->w2[k + N].im;
        tot += p;
        if (p > pk) {
            pk = p;
            kb = k;
        }
    }
    *bin = kb;
    *ratio = (float)(pk / ((tot - pk) / (N - 1) + 1e-30));
}

/* ---- ventana alineada: mezcla −eps bins, FIR + diezmado /2, dechirp, FFT(N) ----
 * start: índice absoluto (a 2·BW) de la primera muestra de la ventana.
 * dir: 0 = chirp ascendente (dechirp con el descendente), 1 = descendente. */
static int aligned_fft_f(sf_rx_t *s, int64_t start, double eps, int dir, cf32 *out, const float *fir)
{
    int N = s->N, nz = 2 * N + 2 * FIR_H;
    int64_t i0 = start - FIR_H;
    if (i0 < s->base || i0 + nz > buf_end(s))
        return -1;
    const cf32 *x = s->buf + (i0 - s->base);
    double w = -2.0 * M_PI * eps / (2.0 * N);
    double ph = fmod(w * (double)i0, 2.0 * M_PI);
    double cr = cos(ph), ci = sin(ph), sr = cos(w), si = sin(w);
    for (int j = 0; j < nz; j++) {
        s->z[j].re = (float)(x[j].re * cr - x[j].im * ci);
        s->z[j].im = (float)(x[j].re * ci + x[j].im * cr);
        double nr = cr * sr - ci * si, ni = cr * si + ci * sr;
        cr = nr;
        ci = ni;
        if ((j & 255) == 255) {
            double m = 1.0 / sqrt(cr * cr + ci * ci);
            cr *= m;
            ci *= m;
        }
    }
    const cf32 *ref = dir ? s->up1 : s->dn1;
    for (int m = 0; m < N; m++) {
        float ar = 0, ai = 0;
        const cf32 *zz = s->z + 2 * m;
        for (int t = 0; t < FIR_NT; t++) {
            ar += fir[t] * zz[t].re;
            ai += fir[t] * zz[t].im;
        }
        out[m].re = ar * ref[m].re - ai * ref[m].im;
        out[m].im = ar * ref[m].im + ai * ref[m].re;
    }
    fft_exec(s->p1, out);
    return 0;
}

static int aligned_fft(sf_rx_t *s, int64_t start, double eps, int dir, cf32 *out)
{
    return aligned_fft_f(s, start, eps, dir, out, s->fir);
}

/* Paso bajo de diezmado con retardo fraccionario delta (muestras a 2·BW):
 * h[t] = g(H − t + δ), g(u) = 2fc·sinc(2fc·u)·Kaiser(u/(H+1)), ganancia DC 1.
 * Así una misma etapa filtra, diezma y corrige el desfase de temporización
 * (necesario: un desfase fraccionario sin corregir sesga la posición del pico
 * según el valor del símbolo, porque el chirp envuelto tiene un salto de fase). */
static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 30; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

static void make_fir(float *h, double delta)
{
    const double fc = 0.30, beta = 6.0;
    double sum = 0, tmp[FIR_NT];
    for (int t = 0; t < FIR_NT; t++) {
        double u = FIR_H - t + delta, a = 2.0 * M_PI * fc * u;
        double sinc = fabs(a) < 1e-9 ? 1.0 : sin(a) / a;
        double r = u / (FIR_H + 1.0), w = fabs(r) < 1 ? bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta) : 0;
        tmp[t] = 2.0 * fc * sinc * w;
        sum += tmp[t];
    }
    for (int t = 0; t < FIR_NT; t++)
        h[t] = (float)(tmp[t] / sum);
}

typedef struct {
    double pos;      /* posición fraccionaria del pico en [0,N) */
    int k;           /* bin del máximo (decisión entera del símbolo) */
    double psig;     /* potencia de señal (3 bins) − ruido */
    double pnoise;   /* potencia de ruido por bin */
} peak_t;

static inline float cabs2(cf32 a) { return a.re * a.re + a.im * a.im; }

/* Pico + interpolación de Jacobsen (exacta para ventana rectangular):
 * δ = Re[(X[k−1] − X[k+1]) / (2X[k] − X[k−1] − X[k+1])]. */
static peak_t find_peak(const cf32 *X, int N)
{
    int k = 0;
    float pk = 0;
    double tot = 0;
    for (int i = 0; i < N; i++) {
        float p = cabs2(X[i]);
        tot += p;
        if (p > pk) {
            pk = p;
            k = i;
        }
    }
    cf32 a = X[(k + N - 1) % N], b = X[k], c = X[(k + 1) % N];
    double nr = a.re - c.re, ni = a.im - c.im;
    double dr = 2 * b.re - a.re - c.re, di = 2 * b.im - a.im - c.im;
    double den = dr * dr + di * di, d = 0;
    if (den > 1e-30)
        d = (nr * dr + ni * di) / den;
    d = CLAMP(d, -0.5, 0.5);
    double p3 = cabs2(a) + cabs2(b) + cabs2(c);
    peak_t r;
    r.pos = k + d;
    r.k = k;
    r.pnoise = (tot - p3) / (N - 3);
    r.psig = p3 - 3 * r.pnoise;
    return r;
}

static double wrap_pm(double x, double N)
{
    x = fmod(x, N);
    if (x > N / 2)
        x -= N;
    else if (x < -N / 2)
        x += N;
    return x;
}

/* Hamming: devuelve nibble; acumula estado. */
static unsigned ham_decode(unsigned w, int cr, int *fixed, int *bad)
{
    if (ham_stat[cr][w] == 1)
        (*fixed)++;
    else if (ham_stat[cr][w] == 2)
        (*bad)++;
    return ham_nib[cr][w];
}

/* Bloque de símbolos (valores crudos s_raw: bin tras restar el offset) →
 * PPM nibbles. reduced: PPM = SF−2 (símbolos múltiplos de 4). */
static void decode_block(const sf_rx_t *s, const uint16_t *sraw, int cwlen, int ppm, int cr, uint8_t *nib, int *fixed,
                         int *bad)
{
    unsigned sym[8], cw[16];
    int N = s->N;
    for (int k = 0; k < cwlen; k++) {
        int r = ((int)sraw[k] - 1 + N) & (N - 1);
        unsigned d;
        if (ppm == s->sf)
            d = (unsigned)r;
        else
            d = (unsigned)(((r + 2) >> 2) & ((N >> 2) - 1));
        sym[k] = gray_enc(d);
    }
    deinterleave(sym, ppm, cwlen, cw);
    for (int i = 0; i < ppm; i++)
        nib[i] = (uint8_t)ham_decode(cw[i], cr, fixed, bad);
}

/* ---- Fin de trama: ensambla bytes, valida CRC y entrega ---- */
static void finish_frame(lora_rx_t *rx, sf_rx_t *s, lora_frame_fn cb, void *ctx, int fixed)
{
    int sf = s->sf, cr = s->hdr_cr, len = s->hdr_len;
    uint8_t nib[2 * (LORA_MAX_PAYLOAD + 2) + 32];
    int nn = 0, bad = 0, fx = fixed;
    int ppm = sf - 2;
    uint8_t blk[16];
    decode_block(s, s->sraw, 8, ppm, 4, blk, &fx, &bad);
    for (int i = 5; i < ppm; i++)
        nib[nn++] = blk[i];
    int pos = 8;
    ppm = s->ldro ? sf - 2 : sf;
    int need = 2 * (len + (s->hdr_crc ? 2 : 0));
    while (nn < need) {
        decode_block(s, s->sraw + pos, 4 + cr, ppm, cr, blk, &fx, &bad);
        pos += 4 + cr;
        for (int i = 0; i < ppm; i++)
            nib[nn++] = blk[i];
    }
    uint8_t raw[LORA_MAX_PAYLOAD + 2];
    int nbytes = len + (s->hdr_crc ? 2 : 0);
    for (int i = 0; i < nbytes; i++)
        raw[i] = (uint8_t)(nib[2 * i] | (nib[2 * i + 1] << 4));
    lora_frame_t f;
    memset(&f, 0, sizeof f);
    f.sf = sf;
    f.bw_hz = rx->cfg.bw_hz;
    f.cr = cr;
    f.has_crc = s->hdr_crc;
    f.ldro = s->ldro;
    f.len = len;
    f.sync = s->sync_word;
    f.fec_fixed = fx;
    for (int i = 0; i < len; i++)
        f.data[i] = (uint8_t)(raw[i] ^ lora_whitening_byte(i));
    bool ok = true;
    if (s->hdr_crc) {
        /* El CRC va sin blanquear, byte bajo primero: contrastado con una trama de
         * loratx (dxlaprs-lora) en SF12, donde coincide con nuestro cálculo. */
        uint16_t c = lora_crc16(f.data, len);
        uint16_t ru = (uint16_t)(raw[len] | (raw[len + 1] << 8));
        ok = c == ru;
    }
    f.crc_ok = s->hdr_crc ? ok : false;
    double navg = s->acc_n ? s->acc_noise / s->acc_n : 1e-30, savg = s->acc_n ? s->acc_sig / s->acc_n : 0;
    f.snr_db = savg > 0 ? 10.0 * log10(savg / ((double)s->N * navg)) : -99;
    f.rssi_dbfs = savg > 0 ? 10.0 * log10(savg / ((double)s->N * s->N)) : -120;
    f.cfo_hz = s->eps * rx->cfg.bw_hz / s->N;
    f.sfo_ppm = s->slope0 / s->N * 1e6;
    f.t_end = (double)(s->frame_t0) / (2.0 * rx->cfg.bw_hz);
    if (s->hdr_crc && !ok) {
        rx->n_crc_bad++;
        return;
    }
    rx->n_frames++;
    if (cb)
        cb(ctx, &f);
}

/* ---- Sincronismo fino ----------------------------------------------------
 * Entrada: ventanas de detección (sin alinear) ya calculadas en s->pbin_*.
 *   q_up  = ε + τ   (bin del pico en los upchirps del preámbulo)
 *   q_dn  = ε − τ   (bin del pico en el SFD con dechirp ascendente)
 * ⇒ ε = (q_up+q_dn)/2, τ = (q_up−q_dn)/2 (módulo N/2: se elige |ε| < N/4,
 * que es el margen de CFO del receptor; la otra solución se prueba también y
 * la decide la palabra de sincronía). Con ε y τ enteros-ish se alinean
 * ventanas y se mide el offset residual m de cada upchirp del preámbulo: su
 * pendiente por símbolo es la deriva de reloj (SFO). */
static bool sync_allowed(const lora_rx_t *rx, int sw, bool strict)
{
    if (rx->cfg.sync_any)
        return !strict || sw == 0x34 || sw == 0x12 || sw == 0x2B;
    for (int i = 0; i < rx->cfg.nsync; i++)
        if (rx->cfg.sync[i] == sw)
            return true;
    return false;
}

static bool try_candidate(lora_rx_t *rx, sf_rx_t *s, double eps, double q_u, int64_t tJ, bool strict)
{
    int N = s->N;
    double tau = fmod(q_u - eps + 4.0 * N, N);
    int64_t b = tJ - (int64_t)llround(2.0 * tau);
    cf32 *X = s->w1;
    int nib[2], sw = 0;
    bool sync_pending = true;
    /* Localiza el inicio del SFD: primer símbolo alineado (rejilla b + k·N,
     * k = −1..1) dominado por el chirp descendente cuyo predecesor es
     * ascendente. J puede ser una ventana solo parcialmente dentro del SFD, de
     * ahí que S0 pueda estar a ambos lados de b. */
    bool dn[4];
    for (int k = -2; k <= 1; k++) {
        int64_t st0 = b + (int64_t)k * 2 * N;
        if (aligned_fft(s, st0, eps, 0, X) < 0)
            return false;
        peak_t pu = find_peak(X, N);
        if (aligned_fft(s, st0, eps, 1, X) < 0)
            return false;
        peak_t pd = find_peak(X, N);
        dn[k + 2] = pd.psig > pu.psig && pd.psig > 12.0 * pd.pnoise;
    }
    int64_t S0 = 0;
    bool found = false;
    for (int k = -1; k <= 1 && !found; k++)
        if (dn[k + 2] && !dn[k + 1]) {
            S0 = b + (int64_t)k * 2 * N;
            found = true;
        }
    if (!found)
        return false;
    /* Estimación iterativa de (ε, τ): cada pasada alinea las ventanas con la
     * estimación vigente (mezcla de ε y retardo fraccionario τ en el filtro de
     * diezmado) y mide el residuo. En ventanas alineadas:
     *   upchirps:   p_u(t) = ε_r + τ_w(t)      (t en símbolos relativos a S0)
     *   downchirps: p_d(t) = ε_r − τ_w(t)
     * con τ_w(t) = τ0 + b·t (b = SFO en muestras/símbolo). Con A = p_u(0) y
     * C = p_d(0): ε_r = (A+C)/2 y τ0 = (A−C)/2. Un desfase fraccionario sin
     * corregir sesga el pico (el chirp envuelto tiene un salto de fase 2πτ en
     * el punto de salto), por eso el residuo se vuelve a medir tras corregir. */
    double eps_cur = eps, tau_tot = 0, sl = 0, ic = 0;
    float fir[FIR_NT];
    for (int it = 0; it < 3; it++) {
        double shift = -2.0 * tau_tot;
        int64_t ish = (int64_t)llround(shift);
        make_fir(fir, shift - (double)ish);
        double tt[8], pp[8], ppow = 0;
        int cnt = 0;
        for (int k = 1; k <= 6; k++) {
            int64_t st = S0 - (int64_t)(2 + k) * 2 * N + ish;
            if (aligned_fft_f(s, st, eps_cur, 0, X, fir) < 0)
                break;
            peak_t pk = find_peak(X, N);
            if (pk.psig < 12.0 * pk.pnoise)
                break;
            tt[cnt] = -(2.0 + k);
            pp[cnt] = pk.pos;
            ppow += pk.psig;
            cnt++;
        }
        if (cnt < 3)
            return false;
        ppow /= cnt;
        double ref = wrap_pm(pp[0], N);
        for (int i = 0; i < cnt; i++)
            pp[i] = ref + wrap_pm(pp[i] - ref, N);
        double st_ = 0, sp = 0, stt = 0, stp = 0;
        for (int i = 0; i < cnt; i++) {
            st_ += tt[i];
            sp += pp[i];
            stt += tt[i] * tt[i];
            stp += tt[i] * pp[i];
        }
        double den = cnt * stt - st_ * st_;
        sl = den != 0 ? (cnt * stp - st_ * sp) / den : 0;
        ic = (sp - sl * st_) / cnt;
        for (int i = 0; i < cnt; i++)
            if (fabs(pp[i] - (ic + sl * tt[i])) > 1.6)
                return false;
        double pd[2];
        for (int i = 0; i < 2; i++) {
            if (aligned_fft_f(s, S0 + (int64_t)i * 2 * N + ish, eps_cur, 1, X, fir) < 0)
                return false;
            peak_t pk = find_peak(X, N);
            if (pk.psig < 12.0 * pk.pnoise || pk.psig < 0.5 * ppow)
                return false; /* un símbolo a caballo entre dos reparte la energía */
            pd[i] = pk.pos;
        }
        double ref_d = wrap_pm(ic, N);
        pd[0] = ref_d + wrap_pm(pd[0] - ref_d, N);
        pd[1] = ref_d + wrap_pm(pd[1] - ref_d, N);
        double C = (pd[0] + pd[1] + sl) / 2.0; /* p_d(t) = C − sl·t en t = 0 y 1 */
        double eps_r = (ic + C) / 2.0, tau0 = (ic - C) / 2.0;
        if (it == 0 && sync_pending) {
            /* palabra de sincronía con la primera estimación */
            sync_pending = false;
            for (int i = 0; i < 2; i++) {
                int64_t sst = S0 - (int64_t)(2 - i) * 2 * N + ish;
                if (aligned_fft_f(s, sst, eps_cur, 0, X, fir) < 0)
                    return false;
                peak_t pk = find_peak(X, N);
                if (pk.psig < 0.5 * ppow)
                    return false;
                double v = wrap_pm(pk.pos - (ic + sl * (-2.0 + i)), N);
                int nb = (int)lround(v / 8.0);
                if (nb < 0 || nb > 15 || fabs(v - 8.0 * nb) > 3.0)
                    return false;
                nib[i] = nb;
            }
            sw = (nib[0] << 4) | nib[1];
            if (!sync_allowed(rx, sw, strict))
                return false;
        }
        eps_cur += eps_r;
        tau_tot += tau0;
    }
    s->sync_word = sw;
    s->eps = eps_cur;
    s->D = S0 + (int64_t)9 * N / 2; /* 2,25 símbolos de SFD (2N muestras/símbolo a 2·BW) */
    s->m = tau_tot + sl * 2.25;
    s->v = sl;
    s->slope0 = sl;
    return true;
}

static bool try_sync(lora_rx_t *rx, sf_rx_t *s)
{
    int N = s->N, f = s->down_f;
    int J = f;
    for (int j = f; j <= f + 2; j++)
        if (s->prat_d[j] > s->prat_d[J])
            J = j;
    int64_t tJ = s->run_first + (int64_t)J * 2 * N;
    int hi = f - 4 < 0 ? 0 : f - 4;
    double q_u = s->pbin_u[hi], q_d = s->pbin_d[J];
    double e0 = fmod(q_u + q_d, N) / 2.0; /* [0, N/2) */
    if (e0 > N / 4.0)
        e0 -= N / 2.0;
    double e1 = e0 > 0 ? e0 - N / 2.0 : e0 + N / 2.0;
    /* Con "sync":"any" se prefieren primero las palabras conocidas (0x34, 0x12,
     * 0x2B): así la ambigüedad ε ↔ ε±N/2 se resuelve igual que con lista. */
    if (try_candidate(rx, s, e0, q_u, tJ, true) || try_candidate(rx, s, e1, q_u, tJ, true))
        return true;
    return rx->cfg.sync_any && (try_candidate(rx, s, e0, q_u, tJ, false) || try_candidate(rx, s, e1, q_u, tJ, false));
}

static void to_search(sf_rx_t *s, int64_t wpos)
{
    s->st = ST_SEARCH;
    s->wpos = wpos;
    s->run = 0;
    s->npw = 0;
    s->down_f = -1;
    s->nsym_done = 0;
    s->nsym_total = 0;
}

/* Un paso de la máquina de estados; true si hubo progreso. */
static bool sf_step(lora_rx_t *rx, sf_rx_t *s, lora_frame_fn cb, void *ctx)
{
    int N = s->N, W = 2 * N;
    if (s->st == ST_SEARCH) {
        if (s->wpos < s->base)
            s->wpos = s->base;
        if (s->wpos + W > buf_end(s))
            return false;
        int bin;
        float ratio;
        det_window(s, s->wpos, false, &bin, &ratio);
        int d = abs(bin - s->last_bin);
        d = MIN(d, N - d);
        if (ratio >= THR_DET) {
            if (s->run > 0 && d <= 2)
                s->run++;
            else {
                s->run = 1;
                s->run_first = s->wpos;
            }
            s->last_bin = bin;
        } else
            s->run = 0;
        s->wpos += W;
        if (s->run >= RUN_MIN) {
            s->st = ST_PRE;
            s->npw = 0;
            s->down_f = -1;
            s->pre_quiet = 0;
        } else
            buf_trim(s, s->wpos - 6 * (int64_t)W);
        return true;
    }
    if (s->st == ST_PRE) {
        int64_t st = s->run_first + (int64_t)s->npw * W;
        if (st + W > buf_end(s))
            return false;
        int j = s->npw;
        det_window(s, st, false, &s->pbin_u[j], &s->prat_u[j]);
        det_window(s, st, true, &s->pbin_d[j], &s->prat_d[j]);
        s->npw++;
        if (s->down_f < 0 && s->prat_d[j] >= THR_DET && s->prat_d[j] > s->prat_u[j])
            s->down_f = j;
        if (s->prat_u[j] < THR_PRE && s->prat_d[j] < THR_PRE)
            s->pre_quiet++;
        else
            s->pre_quiet = 0;
        if (s->down_f < 0 && (s->pre_quiet >= 3 || s->npw >= MAX_PRE_WIN - 4)) {
            to_search(s, st + W);
            return true;
        }
        if (s->down_f >= 0 && s->npw > s->down_f + 4) {
            /* hacen falta además las muestras de la ventana alineada que sigue */
            if (try_sync(rx, s)) {
                rx->n_sync_ok++;
                s->st = ST_DATA;
                s->nsym_done = 0;
                s->nsym_total = 0;
                s->acc_sig = s->acc_noise = 0;
                s->acc_n = 0;
                buf_trim(s, s->D - 2 * (int64_t)W);
            } else
                to_search(s, s->run_first + (int64_t)(s->down_f + 3) * W);
        }
        return true;
    }
    /* ST_DATA */
    int64_t st = s->D + (int64_t)s->nsym_done * W;
    cf32 *X = s->w1;
    /* La ventana se desplaza −m (m = offset de bin predicho = desfase de
     * temporización en muestras a BW): parte entera por índice, fracción por
     * el retardo del filtro de diezmado. */
    /* La ventana se desplaza −m (m = offset de bin predicho = desfase de
     * temporización en muestras a BW): parte entera por índice, fracción por
     * el retardo del filtro de diezmado. */
    double shift = -2.0 * s->m;
    int64_t ish = (int64_t)llround(shift);
    float fir[FIR_NT];
    make_fir(fir, shift - (double)ish);
    if (aligned_fft_f(s, st + ish, s->eps, 0, X, fir) < 0) {
        if (st - FIR_H < s->base) {
            to_search(s, s->base);
            return true;
        }
        return false; /* faltan muestras */
    }
    peak_t pk = find_peak(X, N);
    double raw = pk.pos;
    bool reduced = s->nsym_done < 8 || s->ldro;
    /* La decisión entera es el bin del máximo, no el redondeo de la posición
     * interpolada: con una transición de fase en el salto del chirp (los chips
     * reales son de fase continua; loratx lo imita) la interpolación de Jacobsen
     * se sesga hasta ±0,5 bin y redondeaba al símbolo vecino. */
    int ir = pk.k & (N - 1);
    s->sraw[s->nsym_done] = (uint16_t)ir;
    double e;
    /* El error fraccionario solo es fiable cuando el salto del chirp cae junto a un
     * extremo de la ventana (valor del símbolo cerca de 0 o N). Con el salto en
     * mitad de la ventana y una transición de fase en él (los chips reales son de
     * fase continua; loratx lo imita) la interpolación se sesga hasta ±0,5 bin y
     * arrastraba el lazo hasta equivocar símbolos (grabaciones de loratx, ver
     * docs/protocolos/lora.md). Fuera de los extremos solo se avanza con la pendiente. */
    bool edge = (unsigned)pk.k < (unsigned)(N / 6) || (unsigned)pk.k > (unsigned)(N - N / 6);
    if (!edge)
        e = 0.0;
    else if (reduced) {
        double r1 = fmod(raw - 1.0 + N, N);
        e = r1 - 4.0 * round(r1 / 4.0);
        if (e > 2)
            e = 2;
    } else {
        e = raw - (double)pk.k;
        e = CLAMP(e, -0.5, 0.5);
    }
    const double alpha = 0.2, beta = 0.01;
    s->m += s->v + alpha * e;
    s->v += beta * e;
    if (pk.psig > 0) {
        s->acc_sig += pk.psig;
        s->acc_noise += pk.pnoise;
        s->acc_n++;
    }
    s->nsym_done++;
    if (s->nsym_done == 8) {
        int fx = 0, bad = 0;
        uint8_t blk[16];
        decode_block(s, s->sraw, 8, s->sf - 2, 4, blk, &fx, &bad);
        unsigned h0 = blk[0], h1 = blk[1], h2 = blk[2], ck = ((unsigned)(blk[3] & 1) << 4) | (blk[4] & 15);
        int cr = (int)(h2 >> 1), len = (int)((h0 << 4) | h1);
        if (ck != header_checksum(h0, h1, h2) || cr < 1 || cr > 4 || len < 1) {
            rx->n_hdr_bad++;
            to_search(s, s->D + (int64_t)8 * W);
            return true;
        }
        s->hdr_cr = cr;
        s->hdr_len = len;
        s->hdr_crc = h2 & 1;
        s->ldro = rx->cfg.ldro < 0 ? lora_ldro_auto(s->sf, rx->cfg.bw_hz) : rx->cfg.ldro > 0;
        s->nsym_total = lora_frame_nsyms(len, s->sf, cr, s->hdr_crc, s->ldro);
        if (s->nsym_total > MAX_SYMS) {
            to_search(s, s->D + (int64_t)8 * W);
            return true;
        }
    }
    if (s->nsym_total && s->nsym_done == s->nsym_total) {
        int64_t end = s->D + (int64_t)s->nsym_total * W;
        s->frame_t0 = end;
        finish_frame(rx, s, cb, ctx, 0);
        to_search(s, end);
        buf_trim(s, end);
    } else if (s->nsym_done > 0)
        buf_trim(s, s->D + (int64_t)s->nsym_done * W - 2 * (int64_t)W);
    return true;
}

/* ---- API ---- */
lora_rx_t *lora_rx_create(const lora_rx_cfg_t *cfg, char *err, size_t errlen)
{
    pthread_once(&tabs_once, init_tabs);
    double ratio = cfg->fs / (2.0 * cfg->bw_hz);
    if (cfg->bw_hz < 7800 || ratio < 1 || fabs(ratio - round(ratio)) > 1e-6) {
        snprintf(err, errlen, "la tasa %.0f debe ser múltiplo entero de 2·BW (%d)", cfg->fs, 2 * cfg->bw_hz);
        return NULL;
    }
    lora_rx_t *rx = calloc(1, sizeof *rx);
    if (!rx) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    rx->cfg = *cfg;
    rx->decim = (int)lround(ratio);
    if (rx->decim > 1) {
        if (firdec_init(&rx->dec, rx->decim, cfg->fs, 0.8 * cfg->bw_hz, 255) < 0) {
            snprintf(err, errlen, "no se pudo diseñar el filtro de canal");
            free(rx);
            return NULL;
        }
        rx->have_dec = true;
    }
    for (int sf = 7; sf <= 12; sf++)
        if (cfg->sf_mask & (1u << sf)) {
            rx->sf[sf] = sf_new(sf);
            if (!rx->sf[sf]) {
                snprintf(err, errlen, "sin memoria");
                lora_rx_destroy(rx);
                return NULL;
            }
        }
    return rx;
}

void lora_rx_destroy(lora_rx_t *rx)
{
    if (!rx)
        return;
    for (int sf = 0; sf < 13; sf++)
        sf_free(rx->sf[sf]);
    if (rx->have_dec)
        firdec_free(&rx->dec);
    free(rx->tmp);
    free(rx);
}

void lora_rx_process(lora_rx_t *rx, const cf32 *in, int n, lora_frame_fn cb, void *ctx)
{
    const cf32 *x = in;
    int nx = n;
    if (rx->have_dec) {
        if (rx->tmp_cap < n / rx->decim + 8) {
            rx->tmp_cap = n / rx->decim + 8;
            free(rx->tmp);
            rx->tmp = malloc(sizeof(cf32) * (size_t)rx->tmp_cap);
            if (!rx->tmp) {
                rx->tmp_cap = 0;
                return;
            }
        }
        nx = firdec_process(&rx->dec, in, n, rx->tmp);
        x = rx->tmp;
    }
    for (int sf = 7; sf <= 12; sf++) {
        sf_rx_t *s = rx->sf[sf];
        if (!s)
            continue;
        buf_append(s, x, nx);
        while (sf_step(rx, s, cb, ctx))
            ;
    }
    rx->in_count += n;
}

void lora_rx_stats(const lora_rx_t *rx, unsigned long *sync_ok, unsigned long *hdr_bad, unsigned long *crc_bad,
                   unsigned long *frames)
{
    if (sync_ok)
        *sync_ok = rx->n_sync_ok;
    if (hdr_bad)
        *hdr_bad = rx->n_hdr_bad;
    if (crc_bad)
        *crc_bad = rx->n_crc_bad;
    if (frames)
        *frames = rx->n_frames;
}
