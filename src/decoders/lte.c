/*
 * lte.c — Búsqueda de celdas LTE (PSS/SSS/CRS/PBCH) a 1,92 Msps. Ver docs/decoders/lte.md.
 *
 * Teoría resumida (3GPP TS 36.211 §6.11 y §6.10, TS 36.212 §5.3.1):
 *
 *  1. PSS. En el último símbolo OFDM de las ranuras 0 y 10 (FDD; en TDD, tercer
 *     símbolo de las subtramas 1 y 6) se emite una secuencia Zadoff-Chu de 62 puntos
 *     en frecuencia con raíz u = 25, 29 o 34 según N_ID2. Con FFT de 128 a
 *     1,92 Msps es un símbolo de 128 muestras. Se correlaciona con las 3 réplicas
 *     (Nassralla et al., arXiv:1511.07316; Schmidl-Cox 1997) en bloques de FFT por
 *     solape-guardado: c[n] = IFFT( FFT(x)·conj(FFT(p)) ), con la correlación
 *     normalizada NCC[n] = |c[n]|² / Σ|x|² (la PSS tiene Σ|p|² = 1). Como se repite
 *     cada 5 ms, la NCC se PLIEGA módulo 9600 muestras (detección no coherente);
 *     el pico respecto a la media es el estadístico. El pliegue se prueba con 7
 *     hipótesis de deriva del reloj (±100 ppm) para que el error de reloj no
 *     emborrone los picos a lo largo de 48 ms.
 *  2. CFO. Con las posiciones de las PSS se barre la frecuencia (±10 kHz) buscando
 *     el máximo de la correlación con la plantilla rotada (CFO grueso). Con la rejilla
 *     de símbolos ya conocida, el CP (copia de los últimos Ncp símbolos) da el CFO
 *     fraccionario fino: x[n]·conj(x[n+128]) = |x|²·e^{−j2π f·128/fs}.
 *  3. SSS. Es BPSK (±1) en el símbolo anterior a la PSS (FDD: 137 o 160 muestras
 *     antes según el CP) o al final de la subtrama anterior (TDD: 412/480). Se prueban
 *     las 4 hipótesis (FDD/TDD × CP normal/extendido) × 168 N_ID1 × 2 (subtrama 0 o
 *     5), equalizando con el canal medido en la PSS: corr = Re Σ Y_sss·conj(H)·d_sss.
 *     PCI = 3·N_ID1 + N_ID2. La paridad de la ocurrencia (subtrama 0/5) se combina a lo
 *     largo de todas las PSS de la ventana. La separación PSS–SSS dice FDD/TDD y CP.
 *  4. CRS. Con la trama fijada se correlaciona cada RE de CRS (puertos 0–3, rejilla
 *     desplazada N_ID mod 6, secuencia Gold de c_init de 36.211 §6.10.1.1) con lo
 *     recibido. Una pendiente de fase lineal en frecuencia (error de temporización)
 *     se estima y retira con la fase de H_m·conj(H_{m−1}). La métrica ρ = |ΣH|²/(12·Σ|H|²)
 *     valida la celda (ruido: ≈ 0,08), da RSRP (potencia media de las CRS menos ruido) y
 *     SINR aproximada y detecta el nº de puertos (1, 2 o 4).
 *  5. PBCH (MIB). Subtrama 0, ranura 1, símbolos 0–3, 72 subportadoras centrales: se
 *     estima el canal de cada puerto por interpolación de las CRS, se combina
 *     (1 puerto: MRC; 2 puertos: decodificación de Alamouti/SFBC), se desaleatoriza con
 *     la Gold de c_init = N_ID, se desfragmenta la adaptación de velocidad y se
 *     decodifica el convolucional tail-biting 1/3 con Viterbi. El CRC-16 va
 *     enmascarado con el nº de puertos (0000…, 1111…, 0101…); el trozo de 40 ms (4
 *     tramas) se prueba con las 4 hipótesis de SFN mod 4. MIB → ancho de banda
 *     (6…100 RB), PHICH y 8 bits altos del SFN.
 *
 * Funcionamiento: el decodificador no analiza continuamente (la FFT sería cara),
 * sino una ventana de 48 ms cada 'period' segundos (por defecto 1 s, opción
 * {"period":s}); coste típico ≈ 3 % de un núcleo. Las celdas se guardan en una tabla con
 * caducidad de 120 s; se emite un evento cuando una celda es nueva o cambia (MIB
 * decodificada, duplexado, puertos).
 *
 * Limitaciones: PBCH de 4 puertos (SFBC+FSTD) no se decodifica; la banda de 1,92 Msps
 * solo cubre los 6 RB centrales; sin soporte de movimiento relativo (Doppler).
 */
#include "decoder.h"

#include "lte_phy.h"
#include "../dsp/fft.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN 92160           /* 48 ms */
#define OSN 16384           /* FFT del solape-guardado */
#define NHYP 7              /* hipótesis de deriva de reloj */
#define BIAS 4              /* la FFT de símbolo empieza BIAS muestras antes (dentro del CP) */
#define MAXCELLS 16
#define MAXDET 3
#define MAXOCC 12
#define MAXROWS 640
#define CELL_TTL 120.0
#define R_THR 6.0           /* pico / media de la NCC plegada */
#define SSS_THR 0.30f
#define RHO_THR 0.35

typedef struct {
    int pci, grid, ports;
    bool mib_ok;
    lte_mib_t mib;
    int sfn;
    double rsrp_dbfs, sinr_db, cfo_hz, rho;
    double first_seen, last_seen;
    int hits;
} lte_cell_t;

typedef struct {
    double F, ratio;
    int grid;
    bool ext;
} lgrid_t;

typedef struct {
    cf32 H[4][12];
    bool has[4];
    int ns, l;
} crs_row_t;

typedef struct lte {
    double fs, period;
    int64_t freq_hz;
    cf32 *win, *xd;
    int fill;
    int64_t skip;
    double win_t0;
    fft_plan_t *p128, *pos;
    cf32 pss_t[3][LTE_N];
    cf32 *pss_f[3];                 /* FFT (OSN) de la plantilla */
    int8_t sss_tab[3][2][168][62];
    float sss_m[4][MAXOCC][2][168];     /* métricas SSS por hipótesis (scratch) */
    float *fold;                    /* [NHYP][3][LTE_HALF] */
    uint16_t *cnt;                  /* [NHYP][LTE_HALF] */
    double *ene;                    /* sumas acumuladas de |x|² */
    cf32 *blk;                      /* búfer de OSN */
    cf32 *acc;                      /* correlación de OSN */
    crs_row_t *rows;
    lte_cell_t cells[MAXCELLS];
    int ncells;
    double last_now;
    unsigned long n_windows, n_events;
    sb_t evsb;
} lte_t;

/* ---------------------------------------------------------------------- */
static void ifft_ip(const fft_plan_t *p, cf32 *x)
{
    int n = p->n;
    for (int i = 0; i < n; i++)
        x[i].im = -x[i].im;
    fft_exec(p, x);
    float s = 1.0f / (float)n;
    for (int i = 0; i < n; i++) {
        x[i].re *= s;
        x[i].im = -x[i].im * s;
    }
}

static inline cf32 cmul(cf32 a, cf32 b) { return (cf32){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
static inline cf32 cmulc(cf32 a, cf32 b) { return (cf32){a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im}; } /* a·conj(b) */
static inline float cabs2(cf32 a) { return a.re * a.re + a.im * a.im; }

/* Desvío de frecuencia: xd[n] = x[n]·e^{−j2π f n/fs} (fasor por recurrencia). */
static void derot(const cf32 *x, cf32 *y, int n, double f, double fs)
{
    double w = -2.0 * M_PI * f / fs;
    double cr = 1, ci = 0, dr = cos(w), di = sin(w);
    for (int i = 0; i < n; i++) {
        y[i].re = (float)(x[i].re * cr - x[i].im * ci);
        y[i].im = (float)(x[i].re * ci + x[i].im * cr);
        double nr = cr * dr - ci * di, ni = cr * di + ci * dr;
        cr = nr;
        ci = ni;
        if ((i & 1023) == 1023) {
            double m = 1.0 / sqrt(cr * cr + ci * ci);
            cr *= m;
            ci *= m;
        }
    }
}

static double gpos(const lgrid_t *g, int q, int ns, int off)
{
    return g->F + ((double)q * LTE_FRAME + (double)ns * LTE_SLOT + off) * g->ratio;
}

/* FFT de 128 de un símbolo cuya parte útil empieza (estimada) en start_useful.
 * Devuelve en *sh el desplazamiento (BIAS + parte fraccionaria) que hay que
 * deshacer con la rampa de fase e^{+j2π f·sh/128} al leer cada bin (rd()). */
static bool sym_fft(const lte_t *st, const cf32 *xd, double start_useful, cf32 *Y, double *sh)
{
    double s0 = start_useful - BIAS;
    int is = (int)floor(s0);
    if (is < 0 || is + LTE_N > WIN)
        return false;
    memcpy(Y, xd + is, sizeof(cf32) * LTE_N);
    fft_exec(st->p128, Y);
    *sh = BIAS + (s0 - is);
    return true;
}

static inline cf32 rd(const cf32 *Y, int f, double sh)
{
    cf32 v = Y[lte_f_to_bin(f)];
    double a = 2.0 * M_PI * f * sh / LTE_N;
    return cmul(v, (cf32){(float)cos(a), (float)sin(a)});
}

/* ---------------------------------------------------------------------- */
/*  Etapa 1: PSS por correlación FFT + pliegue                             */
/* ---------------------------------------------------------------------- */
typedef struct {
    int u;                  /* N_ID2 */
    double R;
    int nocc;
    double t[MAXOCC];       /* inicio útil de cada PSS (muestras) */
    double a, ratio;        /* recta t_i = a + P·i */
    double f_c;             /* CFO grueso */
} det_t;

static double pss_corr2(const lte_t *st, int u, int n, double f)
{
    if (n < 0 || n + LTE_N > WIN)
        return 0;
    const cf32 *x = st->win + n;
    const cf32 *p = st->pss_t[u];
    double re = 0, im = 0, w = -2.0 * M_PI * f / st->fs, cr = 1, ci = 0, dr = cos(w), di = sin(w);
    for (int k = 0; k < LTE_N; k++) {
        /* x·conj(p)·e^{−j2π f k/fs} */
        double ar = x[k].re * p[k].re + x[k].im * p[k].im, ai = x[k].im * p[k].re - x[k].re * p[k].im;
        re += ar * cr - ai * ci;
        im += ar * ci + ai * cr;
        double nr = cr * dr - ci * di, ni = cr * di + ci * dr;
        cr = nr;
        ci = ni;
    }
    return re * re + im * im;
}

static void fold_all(lte_t *st)
{
    memset(st->fold, 0, sizeof(float) * NHYP * 3 * LTE_HALF);
    memset(st->cnt, 0, sizeof(uint16_t) * NHYP * LTE_HALF);
    st->ene[0] = 0;
    for (int i = 0; i < WIN; i++)
        st->ene[i + 1] = st->ene[i] + cabs2(st->win[i]);
    static const double eps[NHYP] = {-100e-6, -67e-6, -33e-6, 0, 33e-6, 67e-6, 100e-6};
    const int step = OSN - (LTE_N - 1);
    for (int b = 0; b + LTE_N <= WIN; b += step) {
        int len = MIN(OSN, WIN - b);
        memcpy(st->blk, st->win + b, sizeof(cf32) * (size_t)len);
        memset(st->blk + len, 0, sizeof(cf32) * (size_t)(OSN - len));
        fft_exec(st->pos, st->blk);
        int mmax = MIN(OSN - LTE_N, WIN - LTE_N - b);
        for (int u = 0; u < 3; u++) {
            for (int k = 0; k < OSN; k++)
                st->acc[k] = cmulc(st->blk[k], st->pss_f[u][k]);
            ifft_ip(st->pos, st->acc);
            for (int m = 0; m <= mmax; m++) {
                int n = b + m;
                double e = st->ene[n + LTE_N] - st->ene[n] + 1e-12;
                float nc = (float)(cabs2(st->acc[m]) / e);
                for (int h = 0; h < NHYP; h++) {
                    int j = (int)floor((double)n * (1.0 - eps[h]) + 0.5) % LTE_HALF;
                    st->fold[((size_t)h * 3 + u) * LTE_HALF + j] += nc;
                    if (u == 0)
                        st->cnt[(size_t)h * LTE_HALF + j]++;
                }
            }
        }
    }
}

/* Regresión lineal t_i = a + P·i. */
static void fit_line(const double *t, int n, double *a, double *p)
{
    double si = 0, st_ = 0, sii = 0, sit = 0;
    for (int i = 0; i < n; i++) {
        si += i;
        st_ += t[i];
        sii += (double)i * i;
        sit += i * t[i];
    }
    double den = n * sii - si * si;
    if (n < 2 || fabs(den) < 1e-9) {
        *p = LTE_HALF;
        *a = n ? t[0] : 0;
        return;
    }
    *p = (n * sit - si * st_) / den;
    *a = (st_ - *p * si) / n;
}

static int find_peaks(lte_t *st, det_t *det, int maxdet)
{
    static const double eps[NHYP] = {-100e-6, -67e-6, -33e-6, 0, 33e-6, 67e-6, 100e-6};
    int nd = 0;
    for (int it = 0; it < maxdet; it++) {
        double best = 0;
        int bh = 0, bu = 0, bj = 0;
        for (int h = 0; h < NHYP; h++)
            for (int u = 0; u < 3; u++) {
                const float *f = st->fold + ((size_t)h * 3 + u) * LTE_HALF;
                const uint16_t *c = st->cnt + (size_t)h * LTE_HALF;
                for (int j = 0; j < LTE_HALF; j++)
                    if (c[j] >= 3) {
                        double v = f[j] / c[j];
                        if (v > best) {
                            best = v;
                            bh = h;
                            bu = u;
                            bj = j;
                        }
                    }
            }
        if (best <= 0)
            break;
        double mean = 0;
        long cn = 0;
        for (int u = 0; u < 3; u++) {
            const float *f = st->fold + ((size_t)bh * 3 + u) * LTE_HALF;
            const uint16_t *c = st->cnt + (size_t)bh * LTE_HALF;
            for (int j = 0; j < LTE_HALF; j++)
                if (c[j] >= 3) {
                    mean += f[j] / c[j];
                    cn++;
                }
        }
        mean /= (double)(cn ? cn : 1);
        double R = best / (mean + 1e-30);
        /* suprime el entorno para la siguiente búsqueda */
        for (int h = 0; h < NHYP; h++)
            for (int u = 0; u < 3; u++) {
                float *f = st->fold + ((size_t)h * 3 + u) * LTE_HALF;
                for (int d = -150; d <= 150; d++)
                    f[(bj + d + LTE_HALF) % LTE_HALF] = 0;
            }
        if (R < R_THR)
            break;
        det_t *d = &det[nd];
        memset(d, 0, sizeof *d);
        d->u = bu;
        d->R = R;
        double raw[MAXOCC];
        int no = 0;
        for (int i = 0; i < MAXOCC; i++) {
            double n = ((double)bj + (double)LTE_HALF * i) / (1.0 - eps[bh]);
            if (n + LTE_N + 1 > WIN)
                break;
            /* refinado local sin CFO */
            double bv = -1;
            int bn = (int)floor(n + 0.5);
            for (int dn = -8; dn <= 8; dn++) {
                double v = pss_corr2(st, bu, (int)floor(n + 0.5) + dn, 0.0);
                if (v > bv) {
                    bv = v;
                    bn = (int)floor(n + 0.5) + dn;
                }
            }
            raw[no++] = bn;
        }
        if (no < 1)
            continue;
        /* CFO grueso: barrido conjunto de f y dt (suma no coherente de las ocurrencias) */
        double bscore = -1, bf = 0, sc[21];
        int bdt = 0, bi = 0;
        for (int fi = 0; fi < 21; fi++) {
            double f = (fi - 10) * 1000.0;
            double fbest = -1;
            int fdt = 0;
            for (int dt = -2; dt <= 2; dt++) {
                double s = 0;
                for (int i = 0; i < no; i++)
                    s += pss_corr2(st, bu, (int)raw[i] + dt, f);
                if (s > fbest) {
                    fbest = s;
                    fdt = dt;
                }
            }
            sc[fi] = fbest;
            if (fbest > bscore) {
                bscore = fbest;
                bf = f;
                bdt = fdt;
                bi = fi;
            }
        }
        if (bi > 0 && bi < 20) {
            double y0 = sc[bi - 1], y1 = sc[bi], y2 = sc[bi + 1];
            double dd = y0 - 2 * y1 + y2;
            if (dd < 0)
                bf += 1000.0 * 0.5 * (y0 - y2) / dd;
        }
        d->f_c = bf;
        for (int i = 0; i < no; i++) {
            double bv = -1;
            int bn = (int)raw[i] + bdt;
            for (int dn = -2; dn <= 2; dn++) {
                double v = pss_corr2(st, bu, (int)raw[i] + bdt + dn, bf);
                if (v > bv) {
                    bv = v;
                    bn = (int)raw[i] + bdt + dn;
                }
            }
            d->t[i] = bn;
        }
        d->nocc = no;
        double a, P;
        fit_line(d->t, no, &a, &P);
        if (fabs(P / LTE_HALF - 1.0) > 3e-4) {
            P = LTE_HALF;
            a = d->t[0];
        }
        d->a = a;
        d->ratio = P / LTE_HALF;
        nd++;
    }
    return nd;
}

/* ---------------------------------------------------------------------- */
/*  Etapa 2: SSS                                                           */
/* ---------------------------------------------------------------------- */
typedef struct {
    int grid, parity, n_id1;
    float score;
} sss_res_t;

static bool find_sss(lte_t *st, const det_t *d, const cf32 *xd, sss_res_t *res)
{
    float (*M)[MAXOCC][2][168] = st->sss_m;   /* M[h][i][s][N1] */
    bool used[MAXOCC];
    cf32 dp[62];
    lte_pss_seq(d->u, dp);
    memset(used, 0, sizeof used);
    for (int i = 0; i < d->nocc; i++) {
        double t = d->a + d->ratio * LTE_HALF * i;
        int ti = (int)floor(t + 0.5);
        if (ti - 480 - BIAS < 0 || ti + LTE_N > WIN)
            continue;
        cf32 Yp[LTE_N], H[62];
        memcpy(Yp, xd + ti - BIAS, sizeof(cf32) * LTE_N);
        fft_exec(st->p128, Yp);
        for (int n = 0; n < 62; n++) {
            int f = n < 31 ? n - 31 : n - 30;
            H[n] = cmulc(Yp[lte_f_to_bin(f)], dp[n]);
        }
        used[i] = true;
        for (int h = 0; h < 4; h++) {
            int ts = ti - lte_sss_delta(h) - BIAS;
            cf32 Ys[LTE_N], Z[62];
            memcpy(Ys, xd + ts, sizeof(cf32) * LTE_N);
            fft_exec(st->p128, Ys);
            double nrm = 0;
            for (int n = 0; n < 62; n++) {
                int f = n < 31 ? n - 31 : n - 30;
                Z[n] = cmulc(Ys[lte_f_to_bin(f)], H[n]);
                nrm += cabs2(Z[n]);
            }
            nrm = sqrt(nrm * 62.0) + 1e-20;
            for (int s = 0; s < 2; s++)
                for (int n1 = 0; n1 < 168; n1++) {
                    const int8_t *tab = st->sss_tab[d->u][s][n1];
                    float c = 0;
                    for (int n = 0; n < 62; n++)
                        c += Z[n].re * (float)tab[n];
                    M[h][i][s][n1] = (float)(c / nrm);
                }
        }
    }
    float best = -2;
    for (int h = 0; h < 4; h++)
        for (int p = 0; p < 2; p++)
            for (int n1 = 0; n1 < 168; n1++) {
                float s = 0;
                int cn = 0;
                for (int i = 0; i < d->nocc; i++)
                    if (used[i]) {
                        s += M[h][i][(i + p) & 1][n1];
                        cn++;
                    }
                if (cn == 0)
                    continue;
                s /= (float)cn;
                if (s > best) {
                    best = s;
                    res->grid = h;
                    res->parity = p;
                    res->n_id1 = n1;
                    res->score = s;
                }
            }
    return best >= SSS_THR;
}

/* ---------------------------------------------------------------------- */
/*  Etapa 3: CRS (pendiente, ρ, RSRP, SINR, puertos) y CFO por CP           */
/* ---------------------------------------------------------------------- */
typedef struct {
    int nrows;
    double e_est;           /* error de temporización respecto a la PSS (muestras) */
    double rho[4];
    double rsrp_dbfs, sinr_db;
} crs_res_t;

static int crs_collect(lte_t *st, const cf32 *xd, const lgrid_t *g, int pci)
{
    int nr = 0;
    bool ext = g->ext;
    int l2 = lte_nsym(ext) - 3;
    int qmax = (int)(WIN / (LTE_FRAME * g->ratio)) + 1;
    for (int q = -1; q <= qmax; q++)
        for (int ns = 0; ns < 20; ns++) {
            int ls[3] = {0, l2, 1};
            for (int li = 0; li < 3; li++) {
                int l = ls[li];
                if (nr >= MAXROWS)
                    return nr;
                double su = gpos(g, q, ns, lte_sym_start(ext, l) + lte_cp_len(ext, l));
                cf32 Y[LTE_N];
                double sh;
                if (!sym_fft(st, xd, su, Y, &sh))
                    continue;
                crs_row_t *row = &st->rows[nr];
                memset(row, 0, sizeof *row);
                row->ns = ns;
                row->l = l;
                cf32 r[12];
                lte_crs_seq(pci, ns, l, ext, r);
                for (int p = 0; p < 4; p++) {
                    int j0 = lte_crs_j0(pci, p, ns, l, ext);
                    if (j0 < 0)
                        continue;
                    row->has[p] = true;
                    for (int m = 0; m < 12; m++)
                        row->H[p][m] = cmulc(rd(Y, lte_win_to_f(j0 + 6 * m), sh), r[m]);
                }
                nr++;
            }
        }
    return nr;
}

static void crs_analyze(lte_t *st, int nr, int pci, bool ext, crs_res_t *out)
{
    /* pendiente global a partir del puerto 0 */
    double zr = 0, zi = 0;
    for (int k = 0; k < nr; k++) {
        crs_row_t *row = &st->rows[k];
        if (!row->has[0])
            continue;
        int j0 = lte_crs_j0(pci, 0, row->ns, row->l, ext);
        for (int m = 1; m < 12; m++) {
            if (j0 + 6 * (m - 1) < 36 && j0 + 6 * m >= 36)
                continue;                       /* par que cruza la DC: separación 7, no 6 */
            cf32 z = cmulc(row->H[0][m], row->H[0][m - 1]);
            zr += z.re;
            zi += z.im;
        }
    }
    double phi = atan2(zi, zr);
    double u_rem = phi * LTE_N / (2.0 * M_PI * 6.0);
    out->e_est = -u_rem;
    double rho_sum[4] = {0}, sig = 0, nse = 0, pw = 0;
    int rho_n[4] = {0}, nrow0 = 0;
    for (int k = 0; k < nr; k++) {
        crs_row_t *row = &st->rows[k];
        for (int p = 0; p < 4; p++) {
            if (!row->has[p])
                continue;
            int j0 = lte_crs_j0(pci, p, row->ns, row->l, ext);
            cf32 H[12], S = {0, 0};
            double sum2 = 0;
            for (int m = 0; m < 12; m++) {
                double a = 2.0 * M_PI * lte_win_to_f(j0 + 6 * m) * out->e_est / LTE_N;
                H[m] = cmul(row->H[p][m], (cf32){(float)cos(a), (float)sin(a)});
                S.re += H[m].re;
                S.im += H[m].im;
                sum2 += cabs2(H[m]);
            }
            rho_sum[p] += cabs2(S) / (12.0 * sum2 + 1e-30);
            rho_n[p]++;
            if (p == 0) {
                cf32 mu = {S.re / 12, S.im / 12};
                double nn = 0;
                for (int m = 0; m < 12; m++)
                    nn += cabs2((cf32){H[m].re - mu.re, H[m].im - mu.im});
                nn /= 11.0;
                sig += cabs2(mu);
                nse += nn;
                pw += sum2 / 12;
                nrow0++;
            }
        }
    }
    for (int p = 0; p < 4; p++)
        out->rho[p] = rho_n[p] ? rho_sum[p] / rho_n[p] : 0;
    out->nrows = nrow0;
    if (nrow0 > 0) {
        sig /= nrow0;
        nse /= nrow0;
        pw /= nrow0;
        double s_clean = fmax(pw - nse, 1e-12 * pw);
        out->rsrp_dbfs = 10.0 * log10(s_clean / ((double)LTE_N * LTE_N) + 1e-30);
        out->sinr_db = 10.0 * log10(fmax(sig - nse / 12.0, 1e-9 * nse) / (nse + 1e-30));
    }
}

/* CFO fino por CP sobre la señal ya derotada con el CFO grueso. */
static double cp_cfo(const lte_t *st, const cf32 *xd, const lgrid_t *g, double e_est)
{
    bool ext = g->ext;
    int nsym = lte_nsym(ext);
    double re = 0, im = 0;
    int qmax = (int)(WIN / (LTE_FRAME * g->ratio)) + 1;
    for (int q = -1; q <= qmax; q++)
        for (int ns = 0; ns < 20; ns++)
            for (int l = 0; l < nsym; l++) {
                int ncp = lte_cp_len(ext, l);
                int s = (int)lround(gpos(g, q, ns, lte_sym_start(ext, l)) + e_est);
                for (int i = 2; i < ncp - 2; i++) {
                    int a = s + i, b = s + i + LTE_N;
                    if (a < 0 || b >= WIN)
                        continue;
                    cf32 z = cmulc(xd[a], xd[b]);
                    re += z.re;
                    im += z.im;
                }
            }
    return -atan2(im, re) * st->fs / (2.0 * M_PI * LTE_N);
}

/* ---------------------------------------------------------------------- */
/*  Etapa 4: PBCH                                                           */
/* ---------------------------------------------------------------------- */
static void interp_freq(const cf32 *Hc, int j0, cf32 *out72)
{
    for (int j = 0; j < 72; j++) {
        if (j <= j0) {
            out72[j] = Hc[0];
        } else if (j >= j0 + 66) {
            out72[j] = Hc[11];
        } else {
            int m = (j - j0) / 6;
            float w = (float)(j - j0 - 6 * m) / 6.0f;
            out72[j].re = Hc[m].re * (1 - w) + Hc[m + 1].re * w;
            out72[j].im = Hc[m].im * (1 - w) + Hc[m + 1].im * w;
        }
    }
}

/* Intenta decodificar la MIB de la trama q. Devuelve true si el CRC cuadra. */
static bool pbch_frame(lte_t *st, const cf32 *xd, const lgrid_t *g, int pci, int ports, double e_est, int q,
                       lte_mib_t *mib, int *seg_out)
{
    bool ext = g->ext;
    int l2 = lte_nsym(ext) - 3;
    cf32 Y[5][LTE_N];
    double sh[5];
    int lsel[5] = {0, 1, 2, 3, l2};
    for (int li = 0; li < 5; li++) {
        double su = gpos(g, q, 1, lte_sym_start(ext, lsel[li]) + lte_cp_len(ext, lsel[li]));
        if (!sym_fft(st, xd, su, Y[li], &sh[li]))
            return false;
        sh[li] += e_est;
    }
    cf32 Hf[2][5][72];       /* [puerto][l=0,1,2,3 y l2][j] se rellena para l=0 y l2 */
    for (int p = 0; p < ports; p++)
        for (int li = 0; li < 2; li++) {
            int l = li == 0 ? 0 : l2, yi = li == 0 ? 0 : 4;
            cf32 r[12], Hc[12];
            lte_crs_seq(pci, 1, l, ext, r);
            int j0 = lte_crs_j0(pci, p, 1, l, ext);
            for (int m = 0; m < 12; m++)
                Hc[m] = cmulc(rd(Y[yi], lte_win_to_f(j0 + 6 * m), sh[yi]), r[m]);
            interp_freq(Hc, j0, Hf[p][li]);
        }
    int lsym[288], jj[288];
    int n = lte_pbch_res(pci, ext, lsym, jj);
    cf32 y[288], h[2][288];
    for (int i = 0; i < n; i++) {
        int l = lsym[i];
        y[i] = rd(Y[l], lte_win_to_f(jj[i]), sh[l]);
        float w = (float)l / (float)l2;
        for (int p = 0; p < ports; p++) {
            cf32 a = Hf[p][0][jj[i]], b = Hf[p][1][jj[i]];
            h[p][i] = (cf32){a.re * (1 - w) + b.re * w, a.im * (1 - w) + b.im * w};
        }
    }
    float soft[576];
    if (ports == 1) {
        for (int i = 0; i < n; i++) {
            cf32 x = cmulc(y[i], h[0][i]);
            soft[2 * i] = x.re;
            soft[2 * i + 1] = x.im;
        }
    } else {
        for (int i = 0; i + 1 < n; i += 2) {
            cf32 h0 = {(h[0][i].re + h[0][i + 1].re) * 0.5f, (h[0][i].im + h[0][i + 1].im) * 0.5f};
            cf32 h1 = {(h[1][i].re + h[1][i + 1].re) * 0.5f, (h[1][i].im + h[1][i + 1].im) * 0.5f};
            cf32 ra = y[i], rb = y[i + 1];
            cf32 x0 = cmulc(ra, h0), t = cmul(h1, (cf32){rb.re, -rb.im});
            x0.re += t.re;
            x0.im += t.im;
            cf32 x1 = cmulc(rb, h0);
            t = cmul(h1, (cf32){ra.re, -ra.im});
            x1.re -= t.re;
            x1.im -= t.im;
            soft[2 * i] = x0.re;
            soft[2 * i + 1] = x0.im;
            soft[2 * i + 2] = x1.re;
            soft[2 * i + 3] = x1.im;
        }
    }
    for (int seg = 0; seg < 4; seg++) {
        int pr = lte_pbch_decode(soft, seg, pci, ext, mib);
        if (pr == ports) {
            *seg_out = seg;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------- */
/*  Tabla de celdas y eventos                                              */
/* ---------------------------------------------------------------------- */
static const char *grid_name(int g) { return lte_grid_tdd(g) ? "TDD" : "FDD"; }

static void cell_text(const lte_t *st, const lte_cell_t *c, sb_t *sb)
{
    sb_printf(sb, "LTE PCI %d (N_ID1 %d, N_ID2 %d) %s CP %s, %d puerto%s, CFO %+.2f kHz, RSRP %.1f dBFS, SINR %.1f dB",
              c->pci, c->pci / 3, c->pci % 3, grid_name(c->grid), lte_grid_ext(c->grid) ? "extendido" : "normal",
              c->ports, c->ports == 1 ? "" : "s", c->cfo_hz / 1e3, c->rsrp_dbfs, c->sinr_db);
    if (c->mib_ok)
        sb_printf(sb, ", MIB: %d RB, PHICH %s/%s, SFN %d", c->mib.bw_rb, c->mib.phich_ext ? "ext" : "norm",
                  c->mib.phich_res == 0 ? "1/6" : (c->mib.phich_res == 1 ? "1/2" : (c->mib.phich_res == 2 ? "1" : "2")),
                  c->sfn);
    (void)st;
}

static void cell_json(const lte_t *st, const lte_cell_t *c, sb_t *sb, bool with_text, double now)
{
    sb_puts(sb, "{");
    if (with_text) {
        sb_puts(sb, "\"text\":");
        sb_t t = {0};
        cell_text(st, c, &t);
        sb_json_str(sb, t.data);
        sb_free(&t);
        sb_puts(sb, ",");
    }
    sb_printf(sb, "\"pci\":%d,\"n_id1\":%d,\"n_id2\":%d,\"duplex\":\"%s\",\"cp\":\"%s\",\"ports\":%d,", c->pci,
              c->pci / 3, c->pci % 3, grid_name(c->grid), lte_grid_ext(c->grid) ? "extended" : "normal", c->ports);
    sb_puts(sb, "\"cfo_hz\":");
    sb_json_num(sb, c->cfo_hz, 0);
    sb_puts(sb, ",\"rsrp_dbfs\":");
    sb_json_num(sb, c->rsrp_dbfs, 1);
    sb_puts(sb, ",\"sinr_db\":");
    sb_json_num(sb, c->sinr_db, 1);
    sb_puts(sb, ",\"rho\":");
    sb_json_num(sb, c->rho, 2);
    sb_printf(sb, ",\"mib\":%s", c->mib_ok ? "true" : "false");
    if (c->mib_ok) {
        sb_printf(sb, ",\"bw_rb\":%d,\"phich_duration\":\"%s\",\"phich_resource\":%d,\"sfn\":%d", c->mib.bw_rb,
                  c->mib.phich_ext ? "extended" : "normal", c->mib.phich_res, c->sfn);
        sb_puts(sb, ",\"bw_mhz\":");
        sb_json_num(sb, c->mib.bw_rb == 6 ? 1.4 : (c->mib.bw_rb == 15 ? 3 : c->mib.bw_rb / 5.0), 1);
    }
    sb_printf(sb, ",\"hits\":%d", c->hits);
    if (!with_text) {
        sb_puts(sb, ",\"age\":");
        sb_json_num(sb, now - c->last_seen, 1);
    }
    sb_puts(sb, "}");
}

static void cell_update(lte_t *st, const lte_cell_t *cand, double now, dec_emit_fn emit, void *ctx)
{
    lte_cell_t *c = NULL;
    for (int i = 0; i < st->ncells; i++)
        if (st->cells[i].pci == cand->pci)
            c = &st->cells[i];
    bool changed = false;
    if (!c) {
        if (st->ncells == MAXCELLS) {          /* sustituye la más antigua */
            int o = 0;
            for (int i = 1; i < st->ncells; i++)
                if (st->cells[i].last_seen < st->cells[o].last_seen)
                    o = i;
            c = &st->cells[o];
        } else {
            c = &st->cells[st->ncells++];
        }
        memset(c, 0, sizeof *c);
        c->first_seen = now;
        changed = true;
    } else {
        if (c->grid != cand->grid || c->ports != cand->ports || (cand->mib_ok && !c->mib_ok) ||
            (cand->mib_ok && c->mib_ok && c->mib.bw_rb != cand->mib.bw_rb))
            changed = true;
    }
    bool had_mib = c->mib_ok;
    lte_mib_t old = c->mib;
    int old_sfn = c->sfn;
    int hits = c->hits;
    double fs = c->first_seen;
    *c = *cand;
    c->first_seen = fs;
    c->hits = hits + 1;
    c->last_seen = now;
    if (!cand->mib_ok && had_mib) {              /* conserva la última MIB */
        c->mib_ok = true;
        c->mib = old;
        c->sfn = old_sfn;
    }
    if (changed) {
        st->evsb.len = 0;
        cell_json(st, c, &st->evsb, true, now);
        st->n_events++;
        emit(ctx, st->evsb.data, st->evsb.len);
    }
}

/* ---------------------------------------------------------------------- */
/*  Análisis de una ventana                                                */
/* ---------------------------------------------------------------------- */
static void analyze(lte_t *st, double now, dec_emit_fn emit, void *ctx)
{
    st->n_windows++;
    fold_all(st);
    det_t det[MAXDET];
    int nd = find_peaks(st, det, MAXDET);
    for (int di = 0; di < nd; di++) {
        det_t *d = &det[di];
        derot(st->win, st->xd, WIN, d->f_c, st->fs);
        sss_res_t sr;
        if (!find_sss(st, d, st->xd, &sr))
            continue;
        lte_cell_t c;
        memset(&c, 0, sizeof c);
        c.pci = 3 * sr.n_id1 + d->u;
        c.grid = sr.grid;
        lgrid_t g = {.ratio = d->ratio, .grid = sr.grid, .ext = lte_grid_ext(sr.grid)};
        int s0 = sr.parity & 1;     /* la ocurrencia 0 es de la subtrama 0 (s=0) o 5 (s=1) */
        g.F = d->a - ((double)lte_pss_pos(sr.grid) + (double)LTE_HALF * s0) * g.ratio;
        int nr = crs_collect(st, st->xd, &g, c.pci);
        crs_res_t cr;
        crs_analyze(st, nr, c.pci, g.ext, &cr);
        if (cr.rho[0] < RHO_THR)
            continue;
        /* CFO fino y segunda pasada con la señal corregida */
        double fres = cp_cfo(st, st->xd, &g, cr.e_est);
        double f_tot = d->f_c + fres;
        derot(st->win, st->xd, WIN, f_tot, st->fs);
        nr = crs_collect(st, st->xd, &g, c.pci);
        crs_analyze(st, nr, c.pci, g.ext, &cr);
        c.cfo_hz = f_tot;
        c.rho = cr.rho[0];
        c.rsrp_dbfs = cr.rsrp_dbfs;
        c.sinr_db = cr.sinr_db;
        c.ports = cr.rho[2] > 0.3 && cr.rho[3] > 0.3 ? 4 : (cr.rho[1] > 0.3 ? 2 : 1);
        if (c.ports < 4) {
            int qmax = (int)(WIN / (LTE_FRAME * g.ratio)) + 1;
            for (int q = -1; q <= qmax && !c.mib_ok; q++) {
                lte_mib_t mib;
                int seg;
                if (pbch_frame(st, st->xd, &g, c.pci, c.ports, cr.e_est, q, &mib, &seg)) {
                    c.mib_ok = true;
                    c.mib = mib;
                    c.sfn = (mib.sfn8 << 2) | seg;
                }
            }
        }
        cell_update(st, &c, now, emit, ctx);
    }
}

/* ---------------------------------------------------------------------- */
/*  Interfaz del decodificador                                              */
/* ---------------------------------------------------------------------- */
static void lte_destroy(void *p)
{
    lte_t *st = p;
    if (!st)
        return;
    for (int u = 0; u < 3; u++)
        free(st->pss_f[u]);
    free(st->win);
    free(st->xd);
    free(st->fold);
    free(st->cnt);
    free(st->ene);
    free(st->blk);
    free(st->acc);
    free(st->rows);
    fft_plan_destroy(st->p128);
    fft_plan_destroy(st->pos);
    sb_free(&st->evsb);
    free(st);
}

static void *lte_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - LTE_FS) > 1.0) {
        snprintf(err, errlen, "LTE requiere exactamente 1,92 Msps");
        return NULL;
    }
    lte_t *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    st->fs = cfg->rate;
    st->freq_hz = cfg->freq_hz;
    st->period = 1.0;
    if (cfg->opts && *cfg->opts) {
        jtok_t t[16];
        int nt = json_parse(cfg->opts, strlen(cfg->opts), t, 16);
        double v;
        int i;
        if (nt > 0 && t[0].type == J_OBJ && (i = json_obj_get(cfg->opts, t, 0, "period")) >= 0 &&
            json_num(cfg->opts, t, i, &v))
            st->period = CLAMP(v, 0.0, 60.0);
    }
    st->win = malloc(sizeof(cf32) * WIN);
    st->xd = malloc(sizeof(cf32) * WIN);
    st->fold = malloc(sizeof(float) * NHYP * 3 * LTE_HALF);
    st->cnt = malloc(sizeof(uint16_t) * NHYP * LTE_HALF);
    st->ene = malloc(sizeof(double) * (WIN + 1));
    st->blk = malloc(sizeof(cf32) * OSN);
    st->acc = malloc(sizeof(cf32) * OSN);
    st->rows = malloc(sizeof(crs_row_t) * MAXROWS);
    st->p128 = fft_plan_create(LTE_N);
    st->pos = fft_plan_create(OSN);
    for (int u = 0; u < 3; u++)
        st->pss_f[u] = malloc(sizeof(cf32) * OSN);
    if (!st->win || !st->xd || !st->fold || !st->cnt || !st->ene || !st->blk || !st->acc || !st->rows ||
        !st->p128 || !st->pos || !st->pss_f[0] || !st->pss_f[1] || !st->pss_f[2]) {
        lte_destroy(st);
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    /* plantillas temporales de la PSS (Σ|p|² = 1) y sus FFT */
    for (int u = 0; u < 3; u++) {
        cf32 d[62], b[LTE_N];
        lte_pss_seq(u, d);
        memset(b, 0, sizeof b);
        for (int n = 0; n < 62; n++) {
            int f = n < 31 ? n - 31 : n - 30;
            b[lte_f_to_bin(f)] = d[n];
        }
        ifft_ip(st->p128, b);
        double e = 0;
        for (int n = 0; n < LTE_N; n++)
            e += cabs2(b[n]);
        float s = (float)(1.0 / sqrt(e));
        for (int n = 0; n < LTE_N; n++) {
            st->pss_t[u][n] = (cf32){b[n].re * s, b[n].im * s};
            st->pss_f[u][n] = st->pss_t[u][n];
        }
        memset(st->pss_f[u] + LTE_N, 0, sizeof(cf32) * (OSN - LTE_N));
        fft_exec(st->pos, st->pss_f[u]);
    }
    for (int u = 0; u < 3; u++)
        for (int s = 0; s < 2; s++)
            for (int n1 = 0; n1 < 168; n1++)
                lte_sss_seq(n1, u, s, st->sss_tab[u][s][n1]);
    return st;
}

static void lte_process(void *p, const cf32 *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    lte_t *st = p;
    st->last_now = now;
    int i = 0;
    while (i < n) {
        if (st->skip > 0) {
            int64_t k = MIN((int64_t)(n - i), st->skip);
            st->skip -= k;
            i += (int)k;
            continue;
        }
        if (st->fill == 0)
            st->win_t0 = now + (double)i / st->fs;
        int k = MIN(n - i, WIN - st->fill);
        memcpy(st->win + st->fill, iq + i, sizeof(cf32) * (size_t)k);
        st->fill += k;
        i += k;
        if (st->fill == WIN) {
            analyze(st, st->win_t0 + (double)WIN / st->fs, emit, ctx);
            st->fill = 0;
            st->skip = (int64_t)(st->period * st->fs);
        }
    }
}

static void lte_state(void *p, sb_t *sb, double now)
{
    lte_t *st = p;
    sb_puts(sb, "{\"windows\":");
    sb_printf(sb, "%lu,\"cells\":[", st->n_windows);
    bool first = true;
    for (int i = 0; i < st->ncells; i++) {
        lte_cell_t *c = &st->cells[i];
        if (now - c->last_seen > CELL_TTL)
            continue;
        if (!first)
            sb_puts(sb, ",");
        first = false;
        cell_json(st, c, sb, false, now);
    }
    sb_puts(sb, "]}");
}

const decoder_desc_t DEC_LTE = {
    .id = "lte",
    .name = "LTE",
    .group = "Celular",
    .desc = "Búsqueda de celda LTE (PSS/SSS → PCI, CFO, RSRP; MIB del PBCH).",
    .kind = DEC_NARROW,
    .rate = 1920000,
    .bw_hz = 1500000,
    .wide_rate = 0,
    .wide_min_rate = 0,
    .freqs = {0LL, 0LL, 0LL, 0LL},
    .create = lte_create,
    .destroy = lte_destroy,
    .process = lte_process,
    .state_json = lte_state,
};
