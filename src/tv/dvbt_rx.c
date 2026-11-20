/*
 * dvbt_rx.c — Receptor DVB-T (ETSI EN 300 744): de IQ a paquetes TS.
 *
 * ESTADOS
 *   0 BÚSQUEDA   correlación del prefijo cíclico (CP) sobre ~16 símbolos: modo (2k/8k),
 *                guarda y CFO fraccional (van de Beek, Sandell, Borjesson 1997).
 *                Para cada modo N y guarda N/g se calcula
 *                    ρ(i) = |Σ_{j<W} x[i+j]·x*[i+j+N]| / ½Σ(|x[i+j]|²+|x[i+j+N]|²),  W = N/g
 *                y se pliega módulo el periodo de símbolo Ts = N+W: solo el (N, g)
 *                verdadero alinea los picos de todos los símbolos. Criterio: el pico del
 *                pliegue debe superar en ≥ 7σ la media del pliegue (CFAR); el pico marca el
 *                inicio del CP y el ángulo de la suma, el CFO fraccional
 *                    f = −arg(Σ x[n]·x*[n+N]) / (2πN)   [ciclos/muestra].
 *   1 LOTE       24 símbolos con la ventana a mitad del CP. Con los pilotos continuos
 *                (constantes de un símbolo al siguiente, independientes del canal) se halla
 *                el CFO ENTERO (desplazamiento de bins con máxima correlación entre
 *                símbolos consecutivos), el perfil de retardos (IFFT de los pilotos
 *                continuos ya divididos por su valor PRBS) para colocar la ventana FFT con el
 *                trayecto principal a W/2 del CP, y la fase de los pilotos dispersos (qué
 *                símbolo es l mod 4) por la correlación entre pilotos vecinos (k, k+12).
 *                Los 24 símbolos inicializan la referencia de canal H̄ (SNR +13,8 dB).
 *   2 SEGUIMIENTO + TPS   por símbolo: derotación por NCO (CFO fraccional) → FFT → fase
 *                común a y pendiente b (error de temporización) por máxima correlación de
 *                los pilotos continuos con H̄ → ajuste de la ventana (±1 muestra) y del
 *                NCO → palabra TPS: bit DBPSK diferencial por símbolo, acumulado entre
 *                tramas del mismo hueco del superframe y comprobado con BCH(67,53).
 *   3 DATOS      estimación de canal: los pilotos dispersos de 4 símbolos consecutivos
 *                cubren una rejilla cada 3 portadoras; se promedian en el tiempo
 *                (EMA) y se interpola en frecuencia con un FIR de sinc enventanada
 *                (3× sobremuestreo). Ecualización, LLR max-log por eje (tabla), desentrelazado
 *                de símbolo y de bit en un solo escatter, Viterbi, Forney, RS, PRBS.
 *
 * CONVENCIONES. Portadora k ↔ bin (k − (K−1)/2 + ish) mod N, con ish el CFO entero. La
 * ventana FFT empieza W/2 muestras después del inicio del CP del trayecto principal
 * (retardos del canal en ±W/2 respecto a la ventana: el FIR de interpolación 3× admite
 * ±N/6 muestras, de sobra para la guarda 1/4 = ±N/8).
 */
#include "dvbt_rx.h"

#include "../decoders/dvbt_phy.h"
#include "../dsp/fft.h"
#include "dvbt_outer.h"
#include "dvbt_tab.h"
#include "dvbt_vit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NMAX 8192
/* Ventana máxima (muestras) de la búsqueda por CP en cada modo: 8k usa 24
 * símbolos, 2k usa 120. Compartido entre la reserva y search_cp para que no
 * vuelvan a divergir. El acumulado sc_* necesita nl+1 entradas. */
#define SEARCH_WIN_8K (24 * (8192 + 8192 / 4))
#define SEARCH_WIN_2K (120 * (2048 + 2048 / 4))
#define SEARCH_WIN_MAX (SEARCH_WIN_8K > SEARCH_WIN_2K ? SEARCH_WIN_8K : SEARCH_WIN_2K)
#define SC_CAP (SEARCH_WIN_MAX + 16)
#define BUF_MAX_SAMPLES ((size_t)1 << 24) /* tope duro del búfer de entrada */
#define KMAX 6817
#define NGRID 2273         /* (6816/3)+1 puntos de la rejilla de canal */
#define BATCH 24           /* símbolos del lote de adquisición */
#define ACQ_SYMS 16        /* símbolos de la búsqueda por CP */
#define HMAX (68 * 24)     /* historia de bits TPS blandos */
#define NSLOPE 61          /* pasos de la búsqueda de pendiente */
#define SLOPE_STEP 0.1     /* muestras */
#define FIR_T 12           /* taps del interpolador por fase */

/* ---------------------------------------------------------------------------
 * Tablas por modo
 * ------------------------------------------------------------------------- */
typedef struct {
    int n, K, ndata, ncp, ntps, kc0, ng;
    int cp[177], tps[68];
    float cpP[177];               /* valor del piloto continuo (±4/3) */
    float tpsref[68];             /* ±1 del símbolo 0 */
    uint8_t kind[KMAX];           /* 1 continuo, 2 TPS */
    float pil[KMAX];              /* 4/3·2(½−w_k) */
    int *data[4];                 /* portadoras de datos según l mod 4 */
    int *scat[4];                 /* pilotos dispersos no continuos */
    int nscat[4];
    cf32 *slope_tab;              /* [NSLOPE][ncp]: e^{−j b_s·kc_i} */
    float fir[3][FIR_T];          /* fase r de la interpolación (r = 0 no se usa) */
} modetab_t;

static modetab_t *mt_new(int n)
{
    modetab_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->n = n;
    m->K = dvbt_ncar(n);
    m->ndata = dvbt_ndata(n);
    m->kc0 = (m->K - 1) / 2;
    m->ng = (m->K - 1) / 3 + 1;
    m->ncp = dvbt_cp_list(n, m->cp);
    m->ntps = dvbt_tps_list(n, m->tps);
    for (int i = 0; i < m->ncp; i++) {
        m->kind[m->cp[i]] = 1;
        m->cpP[i] = (float)(4.0 / 3.0 * 2.0 * (0.5 - dvbt_prbs(m->cp[i])));
    }
    for (int i = 0; i < m->ntps; i++) {
        m->kind[m->tps[i]] = 2;
        m->tpsref[i] = (float)(2.0 * (0.5 - dvbt_prbs(m->tps[i])));
    }
    for (int k = 0; k < m->K; k++)
        m->pil[k] = (float)(4.0 / 3.0 * 2.0 * (0.5 - dvbt_prbs(k)));
    for (int p = 0; p < 4; p++) {
        m->data[p] = malloc(sizeof(int) * (size_t)m->ndata);
        m->scat[p] = malloc(sizeof(int) * (size_t)(m->K / 12 + 2));
        int nd = 0, ns = 0;
        for (int k = 0; k < m->K; k++) {
            if (m->kind[k] == 2)
                continue;
            if (k % 12 == 3 * p) {
                if (m->kind[k] != 1)
                    m->scat[p][ns++] = k;
                continue;
            }
            if (m->kind[k] == 1)
                continue;
            m->data[p][nd++] = k;
        }
        m->nscat[p] = ns;
        if (nd != m->ndata) {
            fprintf(stderr, "dvbt_rx: tabla de datos inconsistente (%d != %d)\n", nd, m->ndata);
        }
    }
    m->slope_tab = malloc(sizeof(cf32) * NSLOPE * (size_t)m->ncp);
    for (int s = 0; s < NSLOPE; s++) {
        double b = (s - NSLOPE / 2) * SLOPE_STEP * 2.0 * M_PI / n;
        for (int i = 0; i < m->ncp; i++) {
            double ph = -b * (m->cp[i] - m->kc0);
            m->slope_tab[s * m->ncp + i] = (cf32){(float)cos(ph), (float)sin(ph)};
        }
    }
    /* FIR 3×: valor en x = r/3 − j (unidades de la rejilla), corte 0,9, Kaiser β = 5 */
    for (int r = 1; r < 3; r++) {
        double sum = 0, h[FIR_T];
        for (int t = 0; t < FIR_T; t++) {
            int j = t - FIR_T / 2 + 1; /* −5…6 */
            double x = r / 3.0 - j;
            double fc = 0.9;
            double s = fabs(x) < 1e-9 ? fc : sin(M_PI * fc * x) / (M_PI * x);
            double u = x / (FIR_T / 2.0);
            double w = 0;
            if (fabs(u) < 1) {
                /* Kaiser(β=5): I0(β√(1−u²))/I0(β) */
                double a = 5.0 * sqrt(1 - u * u), i0a = 1, term = 1, i0b = 1, tb = 1;
                for (int q = 1; q < 25; q++) {
                    term *= (a / (2 * q)) * (a / (2 * q));
                    i0a += term;
                    tb *= (2.5 / q) * (2.5 / q);
                    i0b += tb;
                }
                w = i0a / i0b;
            }
            h[t] = s * w;
            sum += h[t];
        }
        for (int t = 0; t < FIR_T; t++)
            m->fir[r][t] = (float)(h[t] / sum);
    }
    return m;
}

static void mt_free(modetab_t *m)
{
    if (!m)
        return;
    for (int p = 0; p < 4; p++) {
        free(m->data[p]);
        free(m->scat[p]);
    }
    free(m->slope_tab);
    free(m);
}

/* ---------------------------------------------------------------------------
 * Receptor
 * ------------------------------------------------------------------------- */
struct dvbt_rx {
    double fs;
    cf32 *buf;
    size_t cap, len;
    int64_t base;              /* índice absoluto de buf[0] */
    int state;
    int forced_n, forced_g;

    /* búsqueda por CP */
    double *sc_re, *sc_im, *sc_e;
    float *fold;
    double cp_rho;

    /* geometría adquirida */
    modetab_t *mt[2];
    modetab_t *m;              /* modo en uso */
    int N, W, Ts, gden, gidx;
    fft_plan_t *plan[2];
    fft_plan_t *plan_cur;

    /* CFO / ventana */
    double f_nco, phase;       /* ciclos/muestra y fase en rad */
    int64_t ws;                /* inicio absoluto de la ventana del próximo símbolo */
    int ish;
    int64_t t0;                /* inicio de CP candidato (índice absoluto) */

    /* símbolo */
    cf32 *Y;                   /* N: salida de la FFT */
    cf32 *Yr;                  /* K: portadoras corregidas */
    cf32 *Yb;                  /* BATCH × K del lote */

    /* referencia de canal */
    cf32 Hbar[177];
    bool hbar_ok;
    cf32 G[NGRID];
    uint8_t ginit[NGRID];
    int pphase;                /* l mod 4 del próximo símbolo según los pilotos */
    int g_age;                 /* símbolos desde el último reinicio de G */
    double a_prev;
    bool a_prev_ok;
    double noise;              /* varianza de ruido por portadora en el dominio Y (EMA) */
    double hpow;               /* |H|² medio (EMA) */
    double pilot_q;            /* calidad media de pilotos */
    int bad_q;                 /* símbolos seguidos con mala calidad */
    double symbols;

    /* TPS */
    float tps_prev[68][2];
    bool tps_prev_ok;
    float *hist;               /* historia blanda, ring HMAX */
    int64_t hj;                /* nº de símbolo de seguimiento (absoluto) */
    bool fs_ok;
    int64_t fs0;               /* índice de símbolo que inicia una trama */
    float acc[4][68];
    int accn[4];
    int64_t fs_attempt;
    bool tps_ok;
    dvbt_tps_t tps;
    int tps_fix, tps_mismatch;
    int lpos;                  /* posición del próximo símbolo en la trama (−1 desconocida) */

    /* datos */
    int v, alpha, rate;
    bool hier;
    const dvbt_llrtab_t *llr;
    const uint32_t *bmap[2];   /* [paridad] stream 0 */
    int8_t *cb;                /* bits codificados de un símbolo */
    int8_t *soft;              /* 2 por paso */
    uint8_t *vout;
    dvbt_vit_t *vit;
    dvbt_outer_t *outer;
    ts_sink_fn sink;
    void *sink_ctx;
    cf32 *H;                   /* canal interpolado (N entradas útiles K) */
    int8_t *shist;             /* historial de soft para BER (ring) */
    int64_t vsteps, vblocks;   /* pasos metidos en el Viterbi / bloques ya sacados */
    uint32_t renc;             /* estado del recodificador del último bloque */
    unsigned long ber_err, ber_tot;
    double mer_acc, mer_sig;
    int mer_n;
    double mer_db, snr_db;
    double ber_pre_vit;
    unsigned long dsyms;
    int tps_cons;
    int cell_b[2];             /* bytes del cell id (alto en tramas pares, bajo en impares), −1 si no vistos */
    double t_sym_s;
    int fail_run;
    double info_cp_rho, info_snr_pilot;
};

static double wrap_pi(double a)
{
    while (a > M_PI)
        a -= 2 * M_PI;
    while (a <= -M_PI)
        a += 2 * M_PI;
    return a;
}

dvbt_rx_t *dvbt_rx_create(double rate_in, char *err, size_t errlen)
{
    if (fabs(rate_in - DVBT_FS) > 3.0) {
        snprintf(err, errlen, "requiere 9 142 857 Hz (64/7 MHz), recibido %.1f", rate_in);
        return NULL;
    }
    dvbt_rx_t *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->fs = DVBT_FS;
    r->cap = 1 << 20;
    r->buf = malloc(sizeof(cf32) * r->cap);
    size_t A = SC_CAP;
    r->sc_re = malloc(sizeof(double) * A);
    r->sc_im = malloc(sizeof(double) * A);
    r->sc_e = malloc(sizeof(double) * A);
    r->fold = malloc(sizeof(float) * (NMAX + NMAX / 4 + 8));
    r->Y = malloc(sizeof(cf32) * NMAX);
    r->Yr = malloc(sizeof(cf32) * KMAX);
    r->Yb = malloc(sizeof(cf32) * (size_t)BATCH * KMAX);
    r->H = malloc(sizeof(cf32) * KMAX);
    r->hist = malloc(sizeof(float) * HMAX);
    r->cb = malloc(6048 * 6 + 64);
    r->soft = malloc(2 * 6048 * 6 + 64);
    r->vout = malloc(6048 * 6 / 8 + 256);
    r->plan[0] = fft_plan_create(2048);
    r->plan[1] = fft_plan_create(8192);
    r->vit = dvbt_vit_create();
    r->outer = dvbt_outer_create();
    r->shist = calloc(2 * 8192, 1);
    if (!r->buf || !r->sc_re || !r->sc_im || !r->sc_e || !r->fold || !r->Y || !r->Yr || !r->Yb || !r->H || !r->hist ||
        !r->cb || !r->soft || !r->vout || !r->plan[0] || !r->plan[1] || !r->vit || !r->outer || !r->shist) {
        snprintf(err, errlen, "sin memoria");
        dvbt_rx_destroy(r);
        return NULL;
    }
    dvbt_rx_reset(r);
    return r;
}

void dvbt_rx_destroy(dvbt_rx_t *r)
{
    if (!r)
        return;
    free(r->buf); free(r->sc_re); free(r->sc_im); free(r->sc_e); free(r->fold); free(r->Y); free(r->Yr);
    free(r->Yb); free(r->H); free(r->hist); free(r->cb); free(r->soft); free(r->vout); free(r->shist);
    mt_free(r->mt[0]);
    mt_free(r->mt[1]);
    if (r->plan[0]) fft_plan_destroy(r->plan[0]);
    if (r->plan[1]) fft_plan_destroy(r->plan[1]);
    dvbt_vit_destroy(r->vit);
    dvbt_outer_destroy(r->outer);
    free(r);
}

void dvbt_rx_force_params(dvbt_rx_t *r, int mode_k, int guard_den)
{
    r->forced_n = mode_k == 2 ? 2048 : mode_k == 8 ? 8192 : 0;
    r->forced_g = guard_den;
}

void dvbt_rx_reset(dvbt_rx_t *r)
{
    r->len = 0;
    r->base = 0;
    r->state = 0;
    r->m = NULL;
    r->N = r->W = r->Ts = r->gden = 0;
    r->f_nco = r->phase = 0;
    r->ish = 0;
    r->hbar_ok = false;
    memset(r->ginit, 0, sizeof r->ginit);
    r->a_prev_ok = false;
    r->noise = 0;
    r->hpow = 0;
    r->pilot_q = 0;
    r->bad_q = 0;
    r->tps_prev_ok = false;
    r->hj = 0;
    r->fs_ok = false;
    memset(r->acc, 0, sizeof r->acc);
    memset(r->accn, 0, sizeof r->accn);
    r->tps_ok = false;
    r->tps_mismatch = 0;
    r->cell_b[0] = r->cell_b[1] = -1;
    r->lpos = -1;
    r->g_age = 0;
    r->symbols = 0;
    r->fs_attempt = 0;
    r->hier = false;
    dvbt_vit_reset(r->vit);
    dvbt_outer_reset(r->outer);
    r->vsteps = r->vblocks = 0;
    r->renc = 0;
    r->ber_err = r->ber_tot = 0;
    r->mer_n = 0;
    r->mer_acc = r->mer_sig = 0;
    r->mer_db = r->snr_db = -99;
    r->ber_pre_vit = 0;
    r->dsyms = 0;
    r->fail_run = 0;
    r->info_cp_rho = 0;
    r->info_snr_pilot = -99;
}

/* ---------------------------------------------------------------------------
 * Utilidades de buffer
 * ------------------------------------------------------------------------- */
static void buf_append(dvbt_rx_t *r, const cf32 *iq, int n)
{
    if (n <= 0)
        return;
    if (r->len + (size_t)n > BUF_MAX_SAMPLES) {
        /* Sin enganche el búfer no debe crecer sin límite: se descarta lo más viejo. */
        size_t drop = r->len + (size_t)n - BUF_MAX_SAMPLES;
        if (drop >= r->len) {
            r->base += (int64_t)r->len + (int64_t)(drop - r->len);
            iq += drop - r->len;
            n -= (int)(drop - r->len);
            r->len = 0;
        } else {
            memmove(r->buf, r->buf + drop, sizeof(cf32) * (r->len - drop));
            r->len -= drop;
            r->base += (int64_t)drop;
        }
    }
    if (r->len + (size_t)n > r->cap) {
        size_t nc = r->cap;
        while (nc < r->len + (size_t)n)
            nc *= 2;
        cf32 *nb = realloc(r->buf, sizeof(cf32) * nc);
        if (!nb)
            return;
        r->buf = nb;
        r->cap = nc;
    }
    memcpy(r->buf + r->len, iq, sizeof(cf32) * (size_t)n);
    r->len += (size_t)n;
}

/* Descarta todo lo anterior al índice absoluto 'keep_from'. */
static void buf_drop_before(dvbt_rx_t *r, int64_t keep_from)
{
    int64_t d = keep_from - r->base;
    if (d <= 0)
        return;
    if ((size_t)d >= r->len) {
        r->base += (int64_t)r->len;
        r->len = 0;
        return;
    }
    memmove(r->buf, r->buf + d, sizeof(cf32) * (r->len - (size_t)d));
    r->len -= (size_t)d;
    r->base = keep_from;
}

/* ---------------------------------------------------------------------------
 * Búsqueda por prefijo cíclico
 * ------------------------------------------------------------------------- */
static bool search_cp(dvbt_rx_t *r)
{
    static const int gd[4] = {4, 8, 16, 32};
    int navail = (int)r->len;
    double best_z = 0, best_peak = 0;
    int best_N = 0, best_g = 0;
    int64_t best_t = 0;
    double best_ang = 0;
    for (int ni = 0; ni < 2; ni++) {
        const int N = ni ? 2048 : 8192;
        if (r->forced_n && r->forced_n != N)
            continue;
        int nl = MIN(navail, N == 8192 ? SEARCH_WIN_8K : SEARCH_WIN_2K) - N;
        if (nl + 1 > (int)SC_CAP)
            nl = (int)SC_CAP - 1; /* defensa: nunca escribir fuera de sc_* */
        if (nl < 6 * (N + N / 32))
            continue;
        double cr = 0, ci = 0, e = 0;
        r->sc_re[0] = r->sc_im[0] = r->sc_e[0] = 0;
        for (int i = 0; i < nl; i++) {
            const cf32 a = r->buf[i], b = r->buf[i + N];
            cr += (double)a.re * b.re + (double)a.im * b.im;       /* a·conj(b) */
            ci += (double)a.im * b.re - (double)a.re * b.im;
            e += 0.5 * ((double)a.re * a.re + (double)a.im * a.im + (double)b.re * b.re + (double)b.im * b.im);
            r->sc_re[i + 1] = cr;
            r->sc_im[i + 1] = ci;
            r->sc_e[i + 1] = e;
        }
        for (int gi = 0; gi < 4; gi++) {
            if (r->forced_g && r->forced_g != gd[gi])
                continue;
            const int W = N / gd[gi], Ts = N + W;
            int M = (nl - W) / Ts;
            if (M < 4)
                continue;
            double mean = 0, m2 = 0, mx = -1;
            int tmx = 0;
            for (int t = 0; t < Ts; t++) {
                double s = 0;
                int cnt = 0;
                for (int m = 0; m < M + 1; m++) {
                    int i = t + m * Ts;
                    if (i + W > nl)
                        break;
                    double re = r->sc_re[i + W] - r->sc_re[i], im = r->sc_im[i + W] - r->sc_im[i];
                    double en = r->sc_e[i + W] - r->sc_e[i] + 1e-9;
                    s += sqrt(re * re + im * im) / en;
                    cnt++;
                }
                s /= cnt;
                r->fold[t] = (float)s;
                mean += s;
                m2 += s * s;
                if (s > mx) {
                    mx = s;
                    tmx = t;
                }
            }
            /* línea base sin el entorno del pico (el pico es un triángulo de semianchura W) */
            (void)mean; (void)m2;
            double bm = 0, b2 = 0;
            int bc = 0;
            for (int t = 0; t < Ts; t++) {
                int dd = abs(t - tmx);
                dd = MIN(dd, Ts - dd);
                if (dd < W + W / 8)
                    continue;
                bm += r->fold[t];
                b2 += (double)r->fold[t] * r->fold[t];
                bc++;
            }
            if (bc < 16)
                continue;
            mean = bm / bc;
            double sd = sqrt(MAX(b2 / bc - mean * mean, 1e-18));
            double z = (mx - mean) / sd;
            if (z > best_z) {
                best_z = z;
                best_peak = mx;
                best_N = N;
                best_g = gd[gi];
                /* CFO: suma de las ventanas de CP en el pico */
                double sr = 0, si = 0;
                for (int m = 0; m < M + 1; m++) {
                    int i = tmx + m * Ts;
                    if (i + W > nl)
                        break;
                    sr += r->sc_re[i + W] - r->sc_re[i];
                    si += r->sc_im[i + W] - r->sc_im[i];
                }
                best_ang = atan2(si, sr);
                best_t = tmx;
            }
        }
    }
    if (getenv("DVBT_DEBUG"))
        fprintf(stderr, "search: z=%.1f peak=%.3f N=%d g=%d t=%lld ang=%.3f\n", best_z, best_peak, best_N, best_g, (long long)best_t, best_ang);
    if (best_z < 7.0 || best_peak < 0.02)
        return false;
    r->N = best_N;
    r->gden = best_g;
    r->W = best_N / best_g;
    r->Ts = r->N + r->W;
    r->m = r->mt[best_N == 8192];
    if (!r->m) {
        r->m = r->mt[best_N == 8192] = mt_new(best_N);
        if (!r->m)
            return false;
    }
    r->plan_cur = r->plan[best_N == 8192];
    r->f_nco = -best_ang / (2.0 * M_PI * best_N);
    r->phase = 0;
    r->t0 = r->base + best_t;
    r->ws = r->t0 + r->W / 2;
    /* primera ventana: avanza hasta el siguiente símbolo si ya se ha pasado */
    while (r->ws < r->base)
        r->ws += r->Ts;
    r->cp_rho = best_peak;
    r->info_cp_rho = best_peak;
    r->t_sym_s = (double)r->Ts / r->fs;
    return true;
}

/* ---------------------------------------------------------------------------
 * FFT de un símbolo con derotación por NCO. Devuelve los K portadoras en Yr (con
 * desplazamiento entero) sin corregir fase.
 * ------------------------------------------------------------------------- */
static void fft_symbol(dvbt_rx_t *r, int64_t start, cf32 *out)
{
    const int N = r->N;
    const cf32 *x = r->buf + (start - r->base);
    const double w = -2.0 * M_PI * r->f_nco;
    double c = cos(-r->phase), s = sin(-r->phase);
    const float wr = (float)cos(w), wi = (float)sin(w);
    float pr = (float)c, pi_ = (float)s;
    cf32 *Y = r->Y;
    for (int i = 0; i < N; i++) {
        Y[i].re = x[i].re * pr - x[i].im * pi_;
        Y[i].im = x[i].re * pi_ + x[i].im * pr;
        float nr = pr * wr - pi_ * wi, ni = pr * wi + pi_ * wr;
        pr = nr;
        pi_ = ni;
        if ((i & 1023) == 1023) { /* renormaliza */
            float m = 1.0f / sqrtf(pr * pr + pi_ * pi_);
            pr *= m;
            pi_ *= m;
        }
    }
    fft_exec(r->plan_cur, Y);
    /* Quita la rampa de fase del retardo medio W/2 (el trayecto principal queda en n ≈ 0 y los
     * ecos en ±W/2): H(k)·e^{+j2π k_c (W/2)/N}. Así el FIR de interpolación ve retardos pequeños. */
    const int K = r->m->K, kc0 = r->m->kc0;
    const double dphi = 2.0 * M_PI * (double)(r->W / 2) / N;
    float rr = (float)cos(-dphi * kc0), ri = (float)sin(-dphi * kc0);
    const float wr2 = (float)cos(dphi), wi2 = (float)sin(dphi);
    for (int k = 0; k < K; k++) {
        int b = ((k - kc0 + r->ish) % N + N) % N;
        out[k].re = Y[b].re * rr - Y[b].im * ri;
        out[k].im = Y[b].re * ri + Y[b].im * rr;
        float nr = rr * wr2 - ri * wi2, ni = rr * wi2 + ri * wr2;
        rr = nr;
        ri = ni;
        if ((k & 511) == 511) {
            float sn = 1.0f / sqrtf(rr * rr + ri * ri);
            rr *= sn;
            ri *= sn;
        }
    }
}

/* Avanza el NCO por un periodo de símbolo + desplazamiento de ventana. */
static void nco_advance(dvbt_rx_t *r, int64_t nsamp)
{
    r->phase += 2.0 * M_PI * r->f_nco * (double)nsamp;
    r->phase = fmod(r->phase, 2.0 * M_PI);
}

/* ---------------------------------------------------------------------------
 * Pendiente de fase + fase común: máxima |Σ z_i e^{−j b kc_i}|, z_i = Hs_i·conj(H̄_i)
 * ------------------------------------------------------------------------- */
static void est_slope(const modetab_t *m, const cf32 *Hs, const cf32 *Hb, double *a, double *b, double *q)
{
    cf32 z[177];
    double zabs = 0;
    for (int i = 0; i < m->ncp; i++) {
        z[i].re = Hs[i].re * Hb[i].re + Hs[i].im * Hb[i].im;
        z[i].im = Hs[i].im * Hb[i].re - Hs[i].re * Hb[i].im;
        zabs += sqrt((double)z[i].re * z[i].re + (double)z[i].im * z[i].im);
    }
    double mag[NSLOPE];
    int bi = 0;
    double bm = -1;
    for (int s = 0; s < NSLOPE; s++) {
        const cf32 *tb = m->slope_tab + s * m->ncp;
        double ar = 0, ai = 0;
        for (int i = 0; i < m->ncp; i++) {
            ar += (double)z[i].re * tb[i].re - (double)z[i].im * tb[i].im;
            ai += (double)z[i].re * tb[i].im + (double)z[i].im * tb[i].re;
        }
        mag[s] = sqrt(ar * ar + ai * ai);
        if (mag[s] > bm) {
            bm = mag[s];
            bi = s;
        }
    }
    double off = 0;
    if (bi > 0 && bi < NSLOPE - 1) {
        double y0 = mag[bi - 1], y1 = mag[bi], y2 = mag[bi + 1];
        double den = y0 - 2 * y1 + y2;
        if (fabs(den) > 1e-12)
            off = 0.5 * (y0 - y2) / den;
        off = CLAMP(off, -0.5, 0.5);
    }
    double bb = ((bi - NSLOPE / 2) + off) * SLOPE_STEP * 2.0 * M_PI / m->n;
    double ar = 0, ai = 0;
    for (int i = 0; i < m->ncp; i++) {
        double ph = -bb * (m->cp[i] - m->kc0), cr = cos(ph), ci = sin(ph);
        ar += z[i].re * cr - z[i].im * ci;
        ai += z[i].re * ci + z[i].im * cr;
    }
    *a = atan2(ai, ar);
    *b = bb;
    *q = zabs > 0 ? sqrt(ar * ar + ai * ai) / zabs : 0;
}

/* Hs_i = Y(cp_i)/P_i  (P real) */
static void cp_estimates(const modetab_t *m, const cf32 *Y, cf32 *Hs)
{
    for (int i = 0; i < m->ncp; i++) {
        float inv = 1.0f / m->cpP[i];
        Hs[i].re = Y[m->cp[i]].re * inv;
        Hs[i].im = Y[m->cp[i]].im * inv;
    }
}

/* Corrige las K portadoras con e^{−j(a + b·kc)} (recurrencia + renormalización). */
static void correct_symbol(const modetab_t *m, const cf32 *Y, cf32 *out, double a, double b)
{
    const int K = m->K;
    double ph0 = -(a + b * (0 - m->kc0));
    float pr = (float)cos(ph0), pi_ = (float)sin(ph0);
    const float wr = (float)cos(-b), wi = (float)sin(-b);
    for (int k = 0; k < K; k++) {
        out[k].re = Y[k].re * pr - Y[k].im * pi_;
        out[k].im = Y[k].re * pi_ + Y[k].im * pr;
        float nr = pr * wr - pi_ * wi, ni = pr * wi + pi_ * wr;
        pr = nr;
        pi_ = ni;
        if ((k & 511) == 511) {
            float s = 1.0f / sqrtf(pr * pr + pi_ * pi_);
            pr *= s;
            pi_ *= s;
        }
    }
}

/* Actualiza la rejilla de canal con los pilotos de un símbolo corregido (fase p). */
static void update_grid(dvbt_rx_t *r, const cf32 *Yc, int p, float alpha)
{
    const modetab_t *m = r->m;
    for (int i = 0; i < m->nscat[p]; i++) {
        int k = m->scat[p][i], g = k / 3;
        float inv = 0.5625f * m->pil[k]; /* 1/P = P·9/16 (|P| = 4/3) */
        cf32 h = {Yc[k].re * inv, Yc[k].im * inv};
        if (!r->ginit[g]) {
            r->G[g] = h;
            r->ginit[g] = 1;
        } else {
            r->G[g].re += alpha * (h.re - r->G[g].re);
            r->G[g].im += alpha * (h.im - r->G[g].im);
        }
    }
    for (int i = 0; i < m->ncp; i++) {
        int g = m->cp[i] / 3;
        float inv = 0.5625f * m->cpP[i];
        cf32 h = {Yc[m->cp[i]].re * inv, Yc[m->cp[i]].im * inv};
        if (!r->ginit[g]) {
            r->G[g] = h;
            r->ginit[g] = 1;
        } else {
            r->G[g].re += alpha * (h.re - r->G[g].re);
            r->G[g].im += alpha * (h.im - r->G[g].im);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Lote de adquisición: CFO entero, perfil de retardos, fase de pilotos dispersos.
 * ------------------------------------------------------------------------- */
static bool acquire_batch(dvbt_rx_t *r)
{
    const modetab_t *m = r->m;
    const int N = r->N, K = m->K;
    const int S = N == 8192 ? 80 : 24;
    r->ish = 0;
    int64_t ws0 = r->ws;
    r->phase = 0;
    for (int b = 0; b < BATCH; b++) {
        fft_symbol(r, ws0 + (int64_t)b * r->Ts, r->Yb + (size_t)b * K);
        nco_advance(r, r->Ts);
    }
    /* --- CFO entero: correlación de pilotos continuos entre símbolos consecutivos --- */
    double best = -1, second = -1, sum = 0;
    int bsh = 0;
    for (int sh = -S; sh <= S; sh++) {
        double met = 0;
        for (int b = 1; b < BATCH; b++) {
            const cf32 *a = r->Yb + (size_t)(b - 1) * K, *c = r->Yb + (size_t)b * K;
            /* los bins desplazados caen fuera de [0,K): se usan solo los continuos dentro de rango */
            double zr = 0, zi = 0, e1 = 0, e2 = 0;
            for (int i = 0; i < m->ncp; i++) {
                int k = m->cp[i] + sh;
                if (k < 0 || k >= K)
                    continue;
                zr += (double)c[k].re * a[k].re + (double)c[k].im * a[k].im;
                zi += (double)c[k].im * a[k].re - (double)c[k].re * a[k].im;
                e1 += (double)a[k].re * a[k].re + (double)a[k].im * a[k].im;
                e2 += (double)c[k].re * c[k].re + (double)c[k].im * c[k].im;
            }
            met += sqrt(zr * zr + zi * zi) / sqrt(e1 * e2 + 1e-12);
        }
        met /= BATCH - 1;
        sum += met;
        if (met > best) {
            second = best;
            best = met;
            bsh = sh;
        } else if (met > second)
            second = met;
    }
    double meanm = sum / (2 * S + 1);
    if (best < 0.05 || best < 1.6 * meanm + 0.03 || best < 1.25 * second) {
        return false;
    }
    r->ish = bsh;
    /* Yb contiene bins sin el desplazamiento: reextrae con ish (índices de portadora k ↔ bin) */
    r->phase = 0;
    for (int b = 0; b < BATCH; b++) {
        fft_symbol(r, ws0 + (int64_t)b * r->Ts, r->Yb + (size_t)b * K);
        nco_advance(r, r->Ts);
    }
    /* --- perfil de retardos con los pilotos continuos --- */
    float *pdp = calloc((size_t)N, sizeof(float));
    cf32 *tmp = malloc(sizeof(cf32) * (size_t)N);
    if (!pdp || !tmp) {
        free(pdp);
        free(tmp);
        return false;
    }
    for (int b = 0; b < BATCH; b++) {
        memset(tmp, 0, sizeof(cf32) * (size_t)N);
        const cf32 *y = r->Yb + (size_t)b * K;
        for (int i = 0; i < m->ncp; i++) {
            int k = m->cp[i], bin = ((k - m->kc0) % N + N) % N;
            float inv = 1.0f / m->cpP[i];
            tmp[bin].re = y[k].re * inv;
            tmp[bin].im = -y[k].im * inv; /* conj(u) */
        }
        fft_exec(r->plan_cur, tmp);
        for (int n = 0; n < N; n++)
            pdp[n] += tmp[n].re * tmp[n].re + tmp[n].im * tmp[n].im;
    }
    /* Trayecto principal: máximo dentro de ±(W + margen). Los pilotos continuos están todos en
     * múltiplos de 3 portadoras, así que el perfil se repite cada N/3 muestras: hay que acotar
     * la búsqueda para no confundir el trayecto con su alias. */
    int nm = 0;
    float pmax = 0;
    double psum = 0;
    const int lim = MIN(r->W + 96, N / 6 - 32);
    for (int n = 0; n < N; n++)
        psum += pdp[n];
    for (int d = -lim; d <= lim; d++) {
        float pv = pdp[(d + N) % N];
        if (pv > pmax) {
            pmax = pv;
            nm = d;
        }
    }
    double floor_ = (psum - pmax) / (N - 1);
    free(tmp);
    free(pdp);
    if (pmax < 6.0 * floor_)
        return false;
    int delta = nm; /* ventana nueva = actual + delta (retardo del trayecto principal respecto al centro) */
    /* El lote se reexpresa en la nueva ventana: Y' = Y·e^{+j2π k_c Δ/N} */
    /* --- fase de pilotos dispersos: |Σ (Y_k/P_k)·conj(Y_{k+12}/P_{k+12})| por hipótesis --- */
    double D[BATCH][4];
    for (int b = 0; b < BATCH; b++) {
        const cf32 *y = r->Yb + (size_t)b * K;
        for (int p = 0; p < 4; p++) {
            double zr = 0, zi = 0, en = 0;
            for (int k = 3 * p; k + 12 < K; k += 12) {
                float p1 = m->pil[k] * 0.5625f, p2 = m->pil[k + 12] * 0.5625f;
                float ar = y[k].re * p1, ai = y[k].im * p1, br = y[k + 12].re * p2, bi = y[k + 12].im * p2;
                zr += (double)ar * br + (double)ai * bi;
                zi += (double)ai * br - (double)ar * bi;
                en += sqrt(((double)ar * ar + (double)ai * ai) * ((double)br * br + (double)bi * bi));
            }
            D[b][p] = sqrt(zr * zr + zi * zi) / (en + 1e-12);
        }
    }
    double hs[4] = {0}, hb = -1, hsec = -1;
    int hbest = 0;
    for (int h = 0; h < 4; h++) {
        for (int b = 0; b < BATCH; b++)
            hs[h] += D[b][(b + h) & 3];
        if (hs[h] > hb) {
            hsec = hb;
            hb = hs[h];
            hbest = h;
        } else if (hs[h] > hsec)
            hsec = hs[h];
    }
    if (hb < 1.2 * hsec) {
        return false;
    }
    /* --- referencia de canal: promedio alineado en fase/pendiente --- */
    cf32 Hs[177], acc[177];
    memset(acc, 0, sizeof acc);
    memset(r->ginit, 0, sizeof r->ginit);
    cf32 *Yc = malloc(sizeof(cf32) * KMAX), *Ys = malloc(sizeof(cf32) * KMAX);
    if (!Yc || !Ys) {
        free(Yc);
        free(Ys);
        return false;
    }
    double a = 0, b = 0, q = 1, qsum = 0, cum_a = 0, cum_b = 0;
    cf32 Hb[177], Hprev[177];
    for (int s = 0; s < BATCH; s++) {
        const cf32 *y = r->Yb + (size_t)s * K;
        /* desplaza a la ventana nueva */
        correct_symbol(m, y, Ys, 0.0, -2.0 * M_PI * delta / N);
        cp_estimates(m, Ys, Hs);
        if (s == 0) {
            a = b = 0;
            q = 1;
        } else {
            /* pendiente y fase INCREMENTALES respecto al símbolo anterior (la deriva de reloj
             * puede sumar varias muestras a lo largo del lote, más que el margen de est_slope) */
            double ia, ib, iq;
            est_slope(m, Hs, Hprev, &ia, &ib, &iq);
            cum_a += wrap_pi(ia - 2.0 * M_PI * (double)r->ish * (double)r->W / (double)N);
            cum_b += ib;
            a = cum_a + 2.0 * M_PI * (double)r->ish * (double)r->W / (double)N * s;
            b = cum_b;
            q = iq;
        }
        memcpy(Hprev, Hs, sizeof Hprev);
        qsum += q;
        correct_symbol(m, Ys, Yc, a, b);
        cp_estimates(m, Yc, Hs);
        for (int i = 0; i < m->ncp; i++) {
            acc[i].re += Hs[i].re;
            acc[i].im += Hs[i].im;
        }
        int p = (s + hbest) & 3;
        update_grid(r, Yc, p, 0.35f);
    }
    (void)Hb;
    for (int i = 0; i < m->ncp; i++) {
        r->Hbar[i].re = acc[i].re / BATCH;
        r->Hbar[i].im = acc[i].im / BATCH;
    }
    free(Yc);
    free(Ys);
    if (getenv("DVBT_DEBUG"))
        fprintf(stderr, "batch: ish=%d delta=%d nm=%d pdp/floor=%.1f hbest=%d hb=%.2f hsec=%.2f qavg=%.3f\n", r->ish, delta, nm, pmax / floor_, hbest, hb, hsec, qsum / BATCH);
    r->hbar_ok = true;
    r->pphase = (BATCH + hbest) & 3;
    /* deriva acumulada del lote: la ventana siguiente se desplaza lo mismo (b = −2π n/N) */
    int dshift = (int)llround(-cum_b * (double)N / (2.0 * M_PI));
    r->ws = ws0 + (int64_t)BATCH * r->Ts + delta + dshift;
    r->a_prev_ok = false;
    r->g_age = BATCH;
    r->hj = 0;
    r->tps_prev_ok = false;
    r->fs_ok = false;
    memset(r->acc, 0, sizeof r->acc);
    memset(r->accn, 0, sizeof r->accn);
    r->pilot_q = qsum / BATCH;
    r->noise = 0;
    return true;
}

/* ---------------------------------------------------------------------------
 * TPS
 * ------------------------------------------------------------------------- */
static void tps_try_sync(dvbt_rx_t *r)
{
    const int64_t jmax = r->hj;           /* símbolos 0…jmax−1 almacenados */
    const int64_t jmin = MAX(0, jmax - (HMAX - 1));
    int F = (int)((jmax - jmin - 84) / 68) + 1; /* tramas completas disponibles (con holgura) */
    if (F < 2)
        return;
    F = MIN(F, 8);
    double bestc = 0, sumc2 = 0;
    int bestphi = 0;
    for (int phi = 0; phi < 68; phi++) {
        double ctot = 0;
        for (int c = 0; c < F; c++) {
            int64_t base = jmax - 84 - (int64_t)(F - 1) * 68 + c * 68 + phi;
            double cc = 0;
            for (int i = 1; i <= 16; i++) {
                int64_t j = base + i;
                if (j < jmin)
                    goto next_phi;
                float s = r->hist[j % HMAX];
                cc += DVBT_SYNC_EVEN[i - 1] ? -s : s;
            }
            ctot += (c & 1) ? -cc : cc;
        }
        sumc2 += ctot * ctot;
        if (fabs(ctot) > fabs(bestc)) {
            bestc = ctot;
            bestphi = phi;
        }
    next_phi:;
    }
    double rms = sqrt(sumc2 / 68.0);
    if (getenv("DVBT_DEBUG"))
        fprintf(stderr, "tps sync: F=%d best=%.1f phi=%d rms=%.1f\n", F, bestc, bestphi, rms);
    if (fabs(bestc) < 4.0 * rms || fabs(bestc) < 0.5 * 16 * F * 0.2)
        return;
    /* frame start: símbolo (jmax − F·68 − 1 + phi) − 0 es el inicio de trama c = 0 */
    int64_t start = jmax - 84 - (int64_t)(F - 1) * 68 + bestphi;
    start -= 0;
    r->fs0 = start;
    /* c = 0 es par si el signo de bestc es positivo (sincronía par); la paridad se corrige abajo */
    r->fs_ok = true;
    memset(r->acc, 0, sizeof r->acc);
    memset(r->accn, 0, sizeof r->accn);
    /* reproduce la historia en los acumuladores */
    for (int64_t j = jmin; j < jmax; j++) {
        int64_t d = j - start;
        if (d < 0)
            continue;
        int c = (int)(d / 68), i = (int)(d % 68);
        r->acc[c & 3][i] += r->hist[j % HMAX];
        if (i == 67)
            r->accn[c & 3]++;
    }
    (void)bestc;
}

/* Intenta decodificar TPS de un hueco del superframe. Devuelve true si es válido. */
static bool tps_decode_slot(dvbt_rx_t *r, int slot, dvbt_tps_t *out, int *fixed)
{
    uint8_t s[68];
    s[0] = 0;
    for (int i = 1; i < 68; i++)
        s[i] = r->acc[slot][i] < 0 ? 1 : 0;
    int alt;
    int f = dvbt_tps_check(s, &alt);
    if (f < 0)
        return false;
    /* sincronía: debe coincidir con una de las dos palabras */
    int ev = 1, od = 1;
    for (int i = 0; i < 16; i++) {
        ev &= s[1 + i] == DVBT_SYNC_EVEN[i];
        od &= s[1 + i] == DVBT_SYNC_ODD[i];
    }
    if (!ev && !od)
        return false;
    dvbt_tps_parse(s, out);
    out->bch_alt = alt;
    if (out->length != 23 && out->length != 31)
        return false;
    if (out->constellation > 2 || out->hp > 4 || out->lp > 4)
        return false;
    if ((out->mode == 1 ? 8192 : 2048) != r->N)
        return false;
    if (dvbt_guard_div(out->guard) != r->gden)
        return false;
    /* la paridad de la sincronía debe ser coherente con el nº de trama */
    if (ev && (out->frame & 1))
        return false;
    if (od && !(out->frame & 1))
        return false;
    *fixed = f;
    return true;
}

/* ---------------------------------------------------------------------------
 * Demapeo de un símbolo de datos y alimentación del Viterbi.
 * ------------------------------------------------------------------------- */
static inline float axis_slice(float x, int v, int alpha, float c)
{
    /* nivel más próximo de la constelación (eje) */
    float ax = fabsf(x) / c;
    float lv;
    if (v == 2)
        return x >= 0 ? c : -c;
    int nl = v == 4 ? 2 : 4;
    int j = (int)((ax - alpha) * 0.5f + 0.5f);
    j = CLAMP(j, 0, nl - 1);
    lv = (float)alpha + 2.0f * (float)j;
    return x >= 0 ? lv * c : -lv * c;
}

static void viterbi_feed(dvbt_rx_t *r, const int8_t *cb, int ncb);

static void decode_data_symbol(dvbt_rx_t *r, const cf32 *Yc, int l)
{
    const modetab_t *m = r->m;
    const int p = l & 3, par = l & 1, v = r->v, nd = m->ndata;
    const int *dl = m->data[p];
    const dvbt_llrtab_t *t = r->llr;
    const uint32_t *mp = r->bmap[par];
    const float lo = t->lo, inv = t->inv;
    const float nrm = v == 2 ? 0.70710678f : 0.0f;
    (void)nrm;
    /* normalización de la constelación para MER (nivel base c) */
    float c;
    if (v == 2)
        c = 0.70710678f;
    else {
        int nl = v == 4 ? 2 : 4;
        double s = 0;
        for (int j = 0; j < nl; j++)
            s += (r->alpha + 2.0 * j) * (r->alpha + 2.0 * j);
        c = (float)(1.0 / sqrt(2.0 * s / nl));
    }
    const double sig2 = MAX(r->noise, 1e-9);
    const float sc = (float)(16.0 / sig2);
    const bool do_mer = (r->dsyms & 3) == 0;
    double mer_e = 0, mer_s = 0;
    const int nab = t->nab;
    for (int idx = 0; idx < nd; idx++) {
        const int k = dl[idx];
        /* canal interpolado */
        cf32 h;
        const int g = k / 3, rr = k - 3 * g;
        if (rr == 0)
            h = r->G[g];
        else {
            const float *f = m->fir[rr];
            float hr = 0, hi = 0;
            for (int tt = 0; tt < FIR_T; tt++) {
                int gi = g + tt - FIR_T / 2 + 1;
                gi = gi < 0 ? 0 : gi >= m->ng ? m->ng - 1 : gi;
                hr += f[tt] * r->G[gi].re;
                hi += f[tt] * r->G[gi].im;
            }
            h = (cf32){hr, hi};
        }
        float w = h.re * h.re + h.im * h.im;
        float iw = 1.0f / (w + 1e-12f);
        float xr = (Yc[k].re * h.re + Yc[k].im * h.im) * iw;
        float xi = (Yc[k].im * h.re - Yc[k].re * h.im) * iw;
        int ir = (int)((xr - lo) * inv), iq = (int)((xi - lo) * inv);
        ir = ir < 0 ? 0 : ir >= DVBT_LLR_NT ? DVBT_LLR_NT - 1 : ir;
        iq = iq < 0 ? 0 : iq >= DVBT_LLR_NT ? DVBT_LLR_NT - 1 : iq;
        const float ws = sc * w;
        for (int a = 0; a < nab; a++) {
            int i0 = 2 * a, i1 = 2 * a + 1;
            uint32_t d0 = mp[idx * v + i0], d1 = mp[idx * v + i1];
            if (d0 != 0xFFFFFFFFu) {
                float q = t->t[a][ir] * ws;
                r->cb[d0] = (int8_t)(q > 127 ? 127 : q < -127 ? -127 : (int)lrintf(q));
            }
            if (d1 != 0xFFFFFFFFu) {
                float q = t->t[a][iq] * ws;
                r->cb[d1] = (int8_t)(q > 127 ? 127 : q < -127 ? -127 : (int)lrintf(q));
            }
        }
        if (do_mer) {
            float sr = axis_slice(xr, v, r->alpha, c), si = axis_slice(xi, v, r->alpha, c);
            mer_e += (double)(xr - sr) * (xr - sr) + (double)(xi - si) * (xi - si);
            mer_s += (double)sr * sr + (double)si * si;
        }
    }
    if (do_mer && mer_e > 0) {
        r->mer_acc += mer_e;
        r->mer_sig += mer_s;
        if (++r->mer_n >= 8) {
            double mdb = 10 * log10(r->mer_sig / r->mer_acc);
            r->mer_db = (float)(r->mer_db < -90 ? mdb : 0.7 * r->mer_db + 0.3 * mdb);
            r->mer_acc = r->mer_sig = 0;
            r->mer_n = 0;
        }
    }
    r->dsyms++;
    int ncb = dvbt_stream_bits(r->m->n, v, r->hier, 0);
    viterbi_feed(r, r->cb, ncb);
}

/* Depuncture y Viterbi; luego sincronismo y código externo. */
static void viterbi_feed(dvbt_rx_t *r, const int8_t *cb, int ncb)
{
    const dvbt_punct_t *pu = dvbt_punct(r->rate);
    int nsteps = ncb / pu->nout * pu->period;
    int8_t *s = r->soft;
    int ci = 0;
    for (int i = 0; i < nsteps;) {
        for (int ph = 0; ph < pu->period; ph++, i++) {
            s[2 * i] = pu->x[ph] ? cb[ci++] : 0;
            s[2 * i + 1] = pu->y[ph] ? cb[ci++] : 0;
        }
    }
    /* historial para el BER antes del Viterbi: se guarda por pasos absolutos */
    int nbytes = dvbt_vit_push(r->vit, s, nsteps, r->vout);
    /* BER: muestrea 1 de cada 8 bloques de 128 bits; recodifica y compara con los blandos */
    for (int i = 0; i < nsteps; i++) {
        int64_t abs = r->vsteps + i;
        r->shist[(abs & 8191) * 2] = s[2 * i];
        r->shist[(abs & 8191) * 2 + 1] = s[2 * i + 1];
    }
    r->vsteps += nsteps;
    for (int blk = 0; blk < nbytes / 16; blk++) {
        const uint8_t *bt = r->vout + blk * 16;
        int64_t first = (r->vblocks + blk) * 128;
        if (((r->vblocks + blk) & 7) == 0 && first + 128 <= r->vsteps && r->vsteps - first < 8000) {
            uint32_t st = r->renc;
            /* el estado inicial es el de los 6 bits previos: solo válido si el bloque anterior existe */
            for (int i = 0; i < 128; i++) {
                int bit = (bt[i >> 3] >> (7 - (i & 7))) & 1;
                uint32_t reg = ((uint32_t)bit << 6) | st;
                int x = __builtin_parity(reg & 0x79u), y = __builtin_parity(reg & 0x5Bu);
                int8_t sx = r->shist[((first + i) & 8191) * 2], sy = r->shist[((first + i) & 8191) * 2 + 1];
                if (sx) {
                    r->ber_tot++;
                    r->ber_err += (unsigned long)((sx < 0) != (x != 0));
                }
                if (sy) {
                    r->ber_tot++;
                    r->ber_err += (unsigned long)((sy < 0) != (y != 0));
                }
                st = (st >> 1) | ((uint32_t)bit << 5);
            }
        }
        /* actualiza el estado del recodificador con los últimos 6 bits del bloque */
        uint32_t st2 = 0;
        for (int i = 122; i < 128; i++) {
            int bit = (bt[i >> 3] >> (7 - (i & 7))) & 1;
            st2 = (st2 >> 1) | ((uint32_t)bit << 5);
        }
        r->renc = st2;
    }
    r->vblocks += nbytes / 16;
    if (nbytes)
        dvbt_outer_push(r->outer, r->vout, nbytes, r->sink, r->sink_ctx);
}

/* ---------------------------------------------------------------------------
 * Bucle de seguimiento: un símbolo.
 * ------------------------------------------------------------------------- */
static bool track_symbol(dvbt_rx_t *r)
{
    const modetab_t *m = r->m;
    const int K = m->K;
    cf32 Hs[177] = {{0, 0}};
    fft_symbol(r, r->ws, r->Yr);                 /* Yr = portadoras sin corregir */
    cp_estimates(m, r->Yr, Hs);
    double a, b, q;
    est_slope(m, Hs, r->Hbar, &a, &b, &q);
    cf32 *Yc = r->Yb;                            /* reutiliza el lote (ya no hace falta) */
    correct_symbol(m, r->Yr, Yc, a, b);
    cp_estimates(m, Yc, Hs);
    /* ruido: error de los pilotos continuos respecto a H̄ */
    double e = 0, hp = 0;
    for (int i = 0; i < m->ncp; i++) {
        double dr = Hs[i].re - r->Hbar[i].re, di = Hs[i].im - r->Hbar[i].im;
        e += dr * dr + di * di;
        hp += (double)r->Hbar[i].re * r->Hbar[i].re + (double)r->Hbar[i].im * r->Hbar[i].im;
    }
    e /= m->ncp;
    hp /= m->ncp;
    /* Hs = Y/P → el ruido de Y es e·P² (|P|² = 16/9); H̄ ruidosa suma ~ (α/(2−α)) */
    double sig2 = e * (16.0 / 9.0) / (1.0 + 0.06);
    r->noise = r->noise == 0 ? sig2 : 0.9 * r->noise + 0.1 * sig2;
    r->hpow = r->hpow == 0 ? hp : 0.9 * r->hpow + 0.1 * hp;
    double snrp = (hp * 16.0 / 9.0) / (sig2 + 1e-18); /* SNR de piloto (|P|² incluido) */
    r->info_snr_pilot = 10 * log10(snrp * 9.0 / 16.0 + 1e-18);   /* respecto a potencia de dato = 1 */
    r->snr_db = (float)r->info_snr_pilot;
    r->pilot_q = r->pilot_q * 0.95 + q * 0.05;
    /* actualización de H̄ */
    for (int i = 0; i < m->ncp; i++) {
        r->Hbar[i].re += 0.0625f * (Hs[i].re - r->Hbar[i].re);
        r->Hbar[i].im += 0.0625f * (Hs[i].im - r->Hbar[i].im);
    }
    /* lazo de CFO fraccional */
    if (r->a_prev_ok) {
        double da = wrap_pi(a - r->a_prev - 2.0 * M_PI * (double)r->ish * (double)r->W / (double)r->N);
        r->f_nco += 0.3 * da / (2.0 * M_PI * (double)r->Ts);
    }
    r->a_prev = a;
    r->a_prev_ok = true;
    /* lazo de ventana: b = −2π n/N con n el retardo del trayecto principal respecto a la ventana */
    double n_delta = -b * (double)r->N / (2.0 * M_PI);
    int64_t shift = 0;
    if (fabs(n_delta) > 1.0)
        shift = (int64_t)llround(n_delta);
    /* mala calidad prolongada → perdido */
    if (q < 0.12)
        r->bad_q++;
    else
        r->bad_q = 0;
    if (r->bad_q > (r->N == 8192 ? 400 : 1600))
        return false;
    /* --- canal: pilotos dispersos --- */
    const int p = r->pphase;
    update_grid(r, Yc, p, 0.25f);
    r->g_age++;
    /* --- TPS blando --- */
    {
        double num = 0, den = 1e-12;
        float cur[68][2];
        for (int i = 0; i < m->ntps; i++) {
            cur[i][0] = Yc[m->tps[i]].re;
            cur[i][1] = Yc[m->tps[i]].im;
        }
        if (r->tps_prev_ok) {
            for (int i = 0; i < m->ntps; i++) {
                num += (double)cur[i][0] * r->tps_prev[i][0] + (double)cur[i][1] * r->tps_prev[i][1];
                den += (double)r->tps_prev[i][0] * r->tps_prev[i][0] + (double)r->tps_prev[i][1] * r->tps_prev[i][1];
            }
            r->hist[r->hj % HMAX] = (float)(num / den);
        } else {
            r->hist[r->hj % HMAX] = 0;
        }
        memcpy(r->tps_prev, cur, sizeof(float) * 2 * (size_t)m->ntps);
        r->tps_prev_ok = true;
        r->hj++;
    }
    /* sincronismo de trama y TPS */
    int l = -1;
    if (r->fs_ok) {
        int64_t d = r->hj - 1 - r->fs0;
        int c = (int)(d / 68), i = (int)(d % 68);
        l = i;
        r->acc[c & 3][i] += r->hist[(r->hj - 1) % HMAX];
        if (i == 67) {
            r->accn[c & 3]++;
            dvbt_tps_t t;
            int fixed;
            if (tps_decode_slot(r, c & 3, &t, &fixed)) {
                if (t.length == 31)
                    r->cell_b[t.frame & 1] = t.cell_byte;
                if (!r->tps_ok) {
                    r->tps = t;
                    r->tps_ok = true;
                    r->tps_fix = fixed;
                    r->tps_cons = 0;
                    /* parámetros de datos */
                    r->v = 2 * (t.constellation + 1);
                    r->hier = (t.hierarchy & 3) != 0 && r->v > 2;
                    r->alpha = r->hier ? ((t.hierarchy & 3) == 1 ? 1 : (t.hierarchy & 3) == 2 ? 2 : 4) : 1;
                    r->rate = t.hp;
                    r->llr = dvbt_llrtab(r->v, r->alpha);
                    r->bmap[0] = dvbt_bitmap(r->N, r->v, r->hier, 0, 0);
                    r->bmap[1] = dvbt_bitmap(r->N, r->v, r->hier, 1, 0);
                    dvbt_vit_reset(r->vit);
                    dvbt_outer_reset(r->outer);
                    r->vsteps = r->vblocks = 0;
                    r->renc = 0;
                    r->dsyms = 0;
                    r->state = 3;
                    r->tps_mismatch = 0;
                    /* i = 67 aquí: la fase real de pilotos de este símbolo es i mod 4 */
                    if ((i & 3) != p) {
                        /* la hipótesis de pilotos dispersos no coincide con la trama: reaprende G */
                        memset(r->ginit, 0, sizeof r->ginit);
                        r->g_age = 0;
                        r->pphase = i & 3;
                    }
                } else {
                    if (t.constellation != r->tps.constellation || t.hp != r->tps.hp || t.guard != r->tps.guard ||
                        t.mode != r->tps.mode || t.hierarchy != r->tps.hierarchy) {
                        if (++r->tps_mismatch >= 3)
                            return false;
                    } else {
                        r->tps_mismatch = 0;
                        r->tps = t;
                    }
                }
            }
        }
    } else if (r->hj >= 68 * 3 && (r->hj % 17) == 0) {
        tps_try_sync(r);
    }
    /* --- datos --- */
    if (r->state == 3 && r->fs_ok && r->g_age >= 8) {
        int64_t d = r->hj - 1 - r->fs0;
        int lp_ = (int)(d % 68);
        /* el pilotaje del símbolo actual debe coincidir con lp_ & 3 */
        if ((lp_ & 3) == p)
            decode_data_symbol(r, Yc, lp_);
    }
    (void)l;
    (void)K;
    /* siguiente símbolo */
    r->pphase = (r->pphase + 1) & 3;
    r->symbols++;
    nco_advance(r, r->Ts);
    r->ws += r->Ts + shift;
    if (shift)
        r->phase += 2.0 * M_PI * r->f_nco * (double)shift, r->phase = fmod(r->phase, 2.0 * M_PI);
    return true;
}

/* ---------------------------------------------------------------------------
 * Bucle principal
 * ------------------------------------------------------------------------- */
void dvbt_rx_process(dvbt_rx_t *r, const cf32 *iq, int n, ts_sink_fn sink, void *ctx)
{
    r->sink = sink;
    r->sink_ctx = ctx;
    buf_append(r, iq, n);
    for (;;) {
        if (r->state == 0) {
            size_t need = (size_t)ACQ_SYMS * (NMAX + NMAX / 4) + NMAX;
            if (r->forced_n == 2048)
                need = 60 * (2048 + 512);
            if (r->len < need)
                break;
            if (search_cp(r)) {
                r->state = 1;
            } else {
                buf_drop_before(r, r->base + (int64_t)(r->len / 2));
                if (r->len < need)
                    break;
            }
        } else if (r->state == 1) {
            int64_t need_end = r->ws + (int64_t)BATCH * r->Ts + 2 * r->N;
            if (r->base + (int64_t)r->len < need_end)
                break;
            if (r->ws < r->base) {
                r->state = 0;
                continue;
            }
            if (acquire_batch(r)) {
                r->state = 2;
                buf_drop_before(r, r->ws - 256);
            } else {
                /* fallo: descarta el lote y vuelve a buscar */
                r->state = 0;
                buf_drop_before(r, r->ws + (int64_t)BATCH * r->Ts - 2 * r->N);
            }
        } else {
            if (r->base + (int64_t)r->len < r->ws + r->N + 8)
                break;
            if (r->ws < r->base) {
                r->state = 0;
                continue;
            }
            if (!track_symbol(r)) {
                /* perdido: reinicia la cadena entera (se conservan las estadísticas de RS) */
                dvbt_outer_stats_t os;
                dvbt_outer_stats(r->outer, &os);
                int64_t keep = r->ws;
                r->state = 0;
                r->tps_ok = false;
                r->fs_ok = false;
                r->hbar_ok = false;
                r->hier = false;
                dvbt_vit_reset(r->vit);
                dvbt_outer_reset(r->outer);
                buf_drop_before(r, keep);
                continue;
            }
            buf_drop_before(r, r->ws - 256);
        }
    }
}

void dvbt_rx_get_stats(dvbt_rx_t *r, dvbt_rx_stats_t *st)
{
    memset(st, 0, sizeof *st);
    dvbt_outer_stats_t os;
    dvbt_outer_stats(r->outer, &os);
    st->mode_k = r->N == 8192 ? 8 : r->N == 2048 ? 2 : 0;
    st->guard_den = r->gden;
    st->cell_id = -1;
    st->qam = 0;
    if (r->tps_ok) {
        static const int num[5] = {1, 2, 3, 5, 7}, den[5] = {2, 3, 4, 6, 8};
        st->qam = r->v * 2 > 0 ? (r->v == 2 ? 4 : r->v == 4 ? 16 : 64) : 0;
        st->cr_num = num[r->tps.hp];
        st->cr_den = den[r->tps.hp];
        if (r->tps.length == 31 && r->cell_b[0] >= 0 && r->cell_b[1] >= 0)
            st->cell_id = r->cell_b[0] << 8 | r->cell_b[1];
        double tsym = (double)r->Ts / r->fs;
        st->ts_bitrate = (double)r->m->ndata * (r->hier ? 2 : r->v) * ((double)num[r->tps.hp] / den[r->tps.hp]) * (188.0 / 204.0) / tsym;
    }
    st->cfo_hz = (float)(r->f_nco * r->fs + (r->N ? r->ish * r->fs / r->N : 0));
    st->mer_db = (float)r->mer_db;
    st->snr_db = (float)r->snr_db;
    st->ber_pre_viterbi = r->ber_tot ? (double)r->ber_err / (double)r->ber_tot : 0;
    st->ber_pre_rs = os.bits ? (double)os.bit_errors / (double)os.bits : 0;
    st->rs_ok = os.rs_ok;
    st->rs_corrected = os.rs_corrected;
    st->rs_failed = os.rs_failed;
    st->lock = r->state == 3 && os.synced && dvbt_outer_fail_pct(r->outer) < 50;
}

void dvbt_rx_get_info(dvbt_rx_t *r, dvbt_rx_info_t *in)
{
    memset(in, 0, sizeof *in);
    in->state = r->state;
    in->tps_ok = r->tps_ok;
    in->cp_rho = r->info_cp_rho;
    in->pilot_q = r->pilot_q;
    in->symbols = r->symbols;
    in->snr_pilot_db = r->info_snr_pilot;
    in->t_sym_s = r->t_sym_s;
    if (r->tps_ok) {
        static const int num[5] = {1, 2, 3, 5, 7}, den[5] = {2, 3, 4, 6, 8};
        in->hierarchy = (r->tps.hierarchy & 3) == 0 ? 0 : (r->tps.hierarchy & 3) == 1 ? 1 : (r->tps.hierarchy & 3) == 2 ? 2 : 4;
        in->rate_lp_num = num[r->tps.lp];
        in->rate_lp_den = den[r->tps.lp];
        in->frame = r->tps.frame;
        in->tps_bch_fixed = r->tps_fix;
    }
}
