/*
 * channel.c — Implementación del receptor de canal. Ver channel.h y docs/arquitectura/DSP.md.
 */
#include "channel.h"

#include <stdlib.h>
#include <string.h>

static const char *MODE_NAMES[MODE_COUNT] = {"wfm", "nfm", "am", "usb", "lsb", "cw"};
/* Anchura por defecto del filtro de canal para cada modo (Hz). */
static const int MODE_DEF_BW[MODE_COUNT] = {180000, 12500, 10000, 2700, 2700, 500};
static const int MODE_MAX_BW[MODE_COUNT] = {220000, 40000, 20000, 6000, 6000, 3000};
static const int MODE_MIN_FCH[MODE_COUNT] = {250000, 48000, 24000, 12000, 12000, 12000};

const char *mode_name(demod_mode_t m) { return (m >= 0 && m < MODE_COUNT) ? MODE_NAMES[m] : "?"; }
int mode_default_bw(demod_mode_t m) { return MODE_DEF_BW[m]; }
int mode_max_bw(demod_mode_t m) { return MODE_MAX_BW[m]; }

int mode_from_name(const char *s)
{
    for (int i = 0; i < MODE_COUNT; i++)
        if (strcmp(s, MODE_NAMES[i]) == 0)
            return i;
    return -1;
}

#define CW_TONE_HZ 700.0   /* tono audible de la telegrafía */
#define SSB_LOW_HZ 200.0   /* borde inferior del filtro de BLU */
#define PCM_FRAME 1024     /* muestras por trama de audio enviada (~21 ms) */

/* ---- Etapa de diezmado rápida -------------------------------------------
 * firdec_process() (filter.c) escribe cada muestra dos veces en una línea de
 * retardo y calcula el producto escalar con UN acumulador: bucle serie, ~3 ns por
 * muestra de entrada. Con un flujo de 61–123 Msps eso es 0,2–0,4 núcleos por
 * canal. Aquí el diezmador trabaja directamente sobre un búfer contiguo
 * [historia (nt−1) | bloque] y el producto escalar usa vectores de 8 floats con
 * acumulador por vector (la suma se reasocia a propósito: sin -ffast-math el
 * compilador no lo haría solo). Los coeficientes se duplican (h0,h0,h1,h1…)
 * para multiplicar directamente el par (re,im) entrelazado. */
typedef float v8f __attribute__((vector_size(32), aligned(4), may_alias));
typedef int16_t v8h __attribute__((vector_size(16), aligned(2), may_alias));

#define MIX_CHUNK 256   /* muestras por tabla de fase del mezclador */
#define PRE 1056        /* hueco antes de los datos en bufa/bufb: ≥ ntaps máx. (1023) − 1 */
#define CHUNK 8192      /* muestras de entrada por pasada mezcla+diezmado: los intermedios
                           (64 KB) se quedan en caché en vez de recorrer la RAM entera */
#define SLACK 16        /* floats de margen tras los datos (lectura del relleno de la última etapa) */

typedef struct {
    int decim, nt, nv;
    float *taps2;       /* 8·nv floats */
    cf32 *hist;         /* nt−1 muestras de entrada anteriores */
    int phase;          /* muestras de entrada desde la última salida */
} dstage_t;

static int dstage_init(dstage_t *d, int decim, double fs_in, double pass_hz)
{
    memset(d, 0, sizeof *d);
    firdec_t f;
    if (firdec_init(&f, decim, fs_in, pass_hz, 1023) < 0)
        return -1;
    d->decim = decim;
    d->nt = f.ntaps;
    d->nv = (2 * d->nt + 7) / 8;
    d->taps2 = calloc((size_t)d->nv * 8, sizeof(float));
    d->hist = calloc((size_t)d->nt, sizeof(cf32));
    if (!d->taps2 || !d->hist) {
        firdec_free(&f);
        return -1;
    }
    for (int k = 0; k < d->nt; k++)
        d->taps2[2 * k] = d->taps2[2 * k + 1] = f.taps[k];
    firdec_free(&f);
    return 0;
}

static void dstage_free(dstage_t *d)
{
    free(d->taps2);
    free(d->hist);
    memset(d, 0, sizeof *d);
}

/* in[0..n) con nt−1 muestras escribibles delante (se pisan con la historia).
 * Devuelve el nº de salidas. */
static int dstage_process(dstage_t *d, cf32 *in, int n, cf32 *out)
{
    const int nt = d->nt, nv = d->nv;
    cf32 *w = in - (nt - 1);
    memcpy(w, d->hist, sizeof(cf32) * (size_t)(nt - 1));
    const v8f *h = (const v8f *)d->taps2;
    int nout = 0;
    for (int i = d->decim - 1 - d->phase; i < n; i += d->decim) {
        const v8f *x = (const v8f *)(const void *)(w + i);
        v8f a0 = h[0] * x[0], a1 = {0};
        int v = 1;
        for (; v + 1 < nv; v += 2) {
            a0 += h[v] * x[v];
            a1 += h[v + 1] * x[v + 1];
        }
        if (v < nv)
            a0 += h[v] * x[v];
        a0 += a1;
        out[nout].re = a0[0] + a0[2] + a0[4] + a0[6];
        out[nout].im = a0[1] + a0[3] + a0[5] + a0[7];
        nout++;
    }
    d->phase = (d->phase + n) % d->decim;
    memcpy(d->hist, w + n, sizeof(cf32) * (size_t)(nt - 1));
    return nout;
}

struct channel {
    /* --- configuración aplicada --- */
    chan_params_t p;
    double fs;              /* tasa de entrada (la del hardware) */
    int64_t center_hz;
    bool built;
    bool configured;        /* hay parámetros y tasa válidos (aunque no se haya construido la cadena) */

    /* --- etapa de selección de canal --- */
    /* mezclador que lleva la emisora a 0 Hz: e^{jωk}, k = 0…MIX_CHUNK−1, y un fasor
     * base por trozo (el recurrente de nco_mix() es una cadena serie de 4 productos
     * por muestra; con la tabla el bucle es vectorizable) */
    cf32 mix_tab[MIX_CHUNK];
    /* la misma tabla en forma "multiplicable" por vectores: tabd = (re,re), tabs = (−im,+im) */
    float tabd[2 * MIX_CHUNK] __attribute__((aligned(32)));
    float tabs[2 * MIX_CHUNK] __attribute__((aligned(32)));
    cf32 mix_step;          /* e^{jω·MIX_CHUNK} */
    cf32 mix_base;          /* fasor del inicio del próximo trozo */
    double mix_w;           /* ω del mezclador (rad/muestra) */
    dstage_t stage[CHAN_MAX_STAGES];
    int nstages;
    double fch;             /* tasa tras el diezmado */
    cfir_t chfilt;          /* filtro de canal */
    bool out_of_band;

    /* --- demodulación --- */
    cf32 prev;              /* muestra anterior para el discriminador FM */
    float dc;               /* seguimiento de continua para AM */
    float agc_peak;         /* detector de pico del AGC */
    float agc_decay;        /* factor de caída del pico por muestra */
    nco_t bfo;              /* oscilador de batido para CW */
    float deemph_a;         /* coeficiente del filtro de deénfasis (a 48 kHz) */
    float deemph_l, deemph_r;

    /* PLL del piloto estéreo de 19 kHz (solo WFM) */
    double pll_phase, pll_freq, pll_nominal;
    double pll_i, pll_q, pll_i2, pll_q2; /* filtros paso bajo de I/Q (dos polos) */
    float pilot_level;      /* amplitud estimada del piloto */
    int pilot_lock_count;
    bool pilot_locked;

    resamp_t rs_main;       /* audio mono o L+R */
    resamp_t rs_diff;       /* L−R (estéreo) */

    /* --- medida de nivel / squelch --- */
    float level_db;
    bool sql_open;
    double meter_accum;     /* segundos desde la última trama de nivel */

    /* --- búferes de trabajo --- */
    int cap;                /* capacidad de los búferes de baja tasa (muestras tras el diezmado) */
    int decim_total;        /* producto de los diezmados de las etapas */
    cf32 *bufa, *bufb;      /* trozo de entrada: CHUNK + PRE + SLACK */
    cf32 *front, *front2;   /* salida del diezmado y del filtro de canal (cap) */
    float *demod, *demod2, *aud, *aud2;
    int16_t pcm[PCM_FRAME * 2];
    int pcm_n;              /* tramas (frames) acumuladas en pcm */
    bool pcm_stereo;        /* formato de lo acumulado en pcm */
    unsigned pcm_flags;
};

channel_t *channel_create(void)
{
    /* alineado a 32 B: las tablas del mezclador son vectores AVX (calloc solo da 16) */
    const size_t sz = (sizeof(channel_t) + 31) & ~(size_t)31;
    channel_t *c = aligned_alloc(32, sz);
    if (c)
        memset(c, 0, sz);
    if (c) {
        c->level_db = -150.f;
        c->p.squelch_db = NAN;
    }
    return c;
}

static void free_chain(channel_t *c)
{
    for (int i = 0; i < c->nstages; i++)
        dstage_free(&c->stage[i]);
    c->nstages = 0;
    cfir_free(&c->chfilt);
    resamp_free(&c->rs_main);
    resamp_free(&c->rs_diff);
    c->built = false;
}

void channel_destroy(channel_t *c)
{
    if (!c)
        return;
    free_chain(c);
    free(c->bufa);
    free(c->bufb);
    free(c->front);
    free(c->front2);
    free(c->demod);
    free(c->demod2);
    free(c->aud);
    free(c->aud2);
    free(c);
}

float channel_level(const channel_t *c) { return c->level_db; }
double channel_rate(const channel_t *c) { return c->fch; }

/* Los búferes de trabajo se dimensionan por la tasa DESPUÉS del diezmado: con un
 * flujo de 1 Mi muestras a 61 Msps el canal solo maneja ~1 000 muestras de baja
 * tasa (antes se reservaban 48 MB por canal "por si acaso"). */
static void ensure_buffers(channel_t *c, int n)
{
    const int P = MAX(c->decim_total, 1);
    const int need = (n + CHUNK) / P + 64;
    if (!c->bufa) {
        /* calloc: el producto vectorial lee hasta 3 muestras más allá de la ventana
         * (con coeficientes 0); si fueran NaN de memoria sin inicializar, 0·NaN = NaN */
        c->bufa = calloc(CHUNK + PRE + SLACK, sizeof(cf32));
        c->bufb = calloc(CHUNK + PRE + SLACK, sizeof(cf32));
        if (!c->bufa || !c->bufb)
            abort();
    }
    if (need <= c->cap)
        return;
    c->cap = need;
    c->front = realloc(c->front, sizeof(cf32) * (size_t)need);
    c->front2 = realloc(c->front2, sizeof(cf32) * (size_t)need);
    c->demod = realloc(c->demod, sizeof(float) * (size_t)need);
    c->demod2 = realloc(c->demod2, sizeof(float) * (size_t)need);
    /* El audio de salida puede superar a la entrada si fch < 48 kHz. */
    size_t na = (size_t)need * 5 + 64;
    c->aud = realloc(c->aud, sizeof(float) * na);
    c->aud2 = realloc(c->aud2, sizeof(float) * na);
    if (!c->front || !c->front2 || !c->demod || !c->demod2 || !c->aud || !c->aud2)
        abort();
}

/* Frecuencia de corte del audio según el modo y la anchura del canal. */
static double audio_cutoff(const chan_params_t *p)
{
    switch (p->mode) {
    case MODE_WFM: return 15000.0;                       /* límite de la radio FM */
    case MODE_NFM: return CLAMP(p->bw_hz * 0.4, 3000.0, 15000.0);
    case MODE_AM: return CLAMP(p->bw_hz / 2.0, 1500.0, 10000.0);
    case MODE_USB:
    case MODE_LSB: return CLAMP((double)p->bw_hz, 1500.0, 6000.0);
    case MODE_CW: return CW_TONE_HZ + p->bw_hz / 2.0 + 300.0;
    default: return 5000.0;
    }
}

/* Construye toda la cadena para la tasa fs y el modo actual. */
static void build_chain(channel_t *c)
{
    free_chain(c);
    const demod_mode_t m = c->p.mode;

    /* 1) Plan de diezmado: factor total D = floor(fs / fch_min), repartido
     *    en etapas de como mucho ÷8. Etapas pequeñas tienen bandas de
     *    transición anchas y por tanto filtros cortos y baratos. */
    int D = (int)floor(c->fs / MODE_MIN_FCH[m]);
    if (D < 1)
        D = 1;
    double rate = c->fs;
    const double pass = fmin(MODE_MAX_BW[m] / 2.0, 0.45 * c->fs / D);
    int rem = D;
    while (rem > 1 && c->nstages < CHAN_MAX_STAGES) {
        int d = rem >= 8 ? 8 : rem;
        if (dstage_init(&c->stage[c->nstages], d, rate, pass) < 0)
            abort();
        c->nstages++;
        rate /= d;
        rem /= d;
    }
    c->fch = rate;
    c->decim_total = 1;
    for (int i = 0; i < c->nstages; i++)
        c->decim_total *= c->stage[i].decim;
    c->cap = 0; /* ensure_buffers recalcula con el nuevo diezmado total */

    /* 2) Filtro de canal a la tasa fch. */
    double bw = CLAMP((double)c->p.bw_hz, 100.0, fmin((double)MODE_MAX_BW[m], 0.9 * c->fch));
    double lo, hi, trans;
    switch (m) {
    case MODE_USB: lo = SSB_LOW_HZ; hi = bw; trans = 250.0; break;
    case MODE_LSB: lo = -bw; hi = -SSB_LOW_HZ; trans = 250.0; break;
    case MODE_CW: lo = -bw / 2; hi = bw / 2; trans = fmax(100.0, bw * 0.3); break;
    default: lo = -bw / 2; hi = bw / 2; trans = fmax(800.0, bw * 0.12); break;
    }
    cfir_init_bandpass(&c->chfilt, c->fch, lo, hi, trans, 1023);

    /* 3) Remuestreo a 48 kHz con el filtro de audio incorporado. */
    double fc = audio_cutoff(&c->p);
    double atrans = (m == MODE_WFM) ? 3500.0 : fmax(400.0, fc * 0.2);
    resamp_init(&c->rs_main, c->fch, AUDIO_RATE, fc, atrans);
    if (m == MODE_WFM)
        resamp_init(&c->rs_diff, c->fch, AUDIO_RATE, fc, atrans);

    /* 4) Estado del demodulador. */
    c->prev = (cf32){0, 0};
    c->dc = 0;
    c->agc_peak = 1e-3f;
    c->agc_decay = (float)exp(-1.0 / (0.6 * c->fch)); /* el pico cae 1/e en 0,6 s */
    nco_init(&c->bfo, CW_TONE_HZ, c->fch);
    double tau = c->p.deemph_us * 1e-6;
    c->deemph_a = tau > 0 ? (float)(1.0 - exp(-1.0 / (AUDIO_RATE * tau))) : 1.f;
    c->deemph_l = c->deemph_r = 0;
    c->pll_nominal = 2.0 * M_PI * 19000.0 / c->fch;
    c->pll_freq = c->pll_nominal;
    c->pll_phase = 0;
    c->pll_i = c->pll_q = c->pll_i2 = c->pll_q2 = 0;
    c->pilot_locked = false;
    c->pilot_lock_count = 0;
    c->pcm_n = 0;
    c->built = true;
}

void channel_configure(channel_t *c, double fs, int64_t center_hz, const chan_params_t *p)
{
    /* Un canal sin audio no necesita cadena de filtros (el nivel sale del espectro):
     * construirla cuesta diseñar ~6 FIR de 1023 coeficientes. Se difiere hasta que
     * se active el audio; al activarlo se reconstruye para empezar con el estado
     * de los filtros a cero (no con muestras de hace minutos). */
    bool rebuild = (p->audio_on && !c->built) || (p->audio_on && !c->p.audio_on) || fs != c->fs ||
                   p->mode != c->p.mode || p->bw_hz != c->p.bw_hz || p->deemph_us != c->p.deemph_us;
    bool retune = rebuild || !c->configured || p->freq_hz != c->p.freq_hz || center_hz != c->center_hz;
    c->p = *p;
    c->fs = fs;
    c->center_hz = center_hz;
    c->configured = true;
    if (rebuild) {
        if (p->audio_on)
            build_chain(c);
        else
            free_chain(c); /* c->built = false; se reconstruye al activar el audio */
    }
    if (retune) {
        /* Resta saturada: con frecuencias extremas (p. ej. INT64_MIN) la resta
         * desbordaría (comportamiento indefinido); ±1e12 Hz ya queda fuera de banda. */
        int64_t off;
        if (__builtin_sub_overflow(p->freq_hz, center_hz, &off))
            off = p->freq_hz < 0 ? -(int64_t)1000000000000LL : (int64_t)1000000000000LL;
        off = CLAMP(off, -(int64_t)1000000000000LL, (int64_t)1000000000000LL);
        /* Conservamos el fasor actual para que retocar la sintonía no
         * produzca un salto de fase (clic) en el audio. */
        cf32 keep = c->mix_base;
        const double w = -2.0 * M_PI * (double)off / fs;
        c->mix_w = w;
        for (int k = 0; k < MIX_CHUNK; k++)
            c->mix_tab[k] = (cf32){(float)cos(w * k), (float)sin(w * k)};
        for (int k = 0; k < MIX_CHUNK; k++) {
            c->tabd[2 * k] = c->tabd[2 * k + 1] = c->mix_tab[k].re;
            c->tabs[2 * k] = -c->mix_tab[k].im;
            c->tabs[2 * k + 1] = c->mix_tab[k].im;
        }
        c->mix_step = (cf32){(float)cos(w * MIX_CHUNK), (float)sin(w * MIX_CHUNK)};
        c->mix_base = (cf32){1.f, 0.f};
        if (!rebuild && c->built && (keep.re != 0 || keep.im != 0))
            c->mix_base = keep;
        double edge = fabs((double)off) + p->bw_hz / 2.0;
        c->out_of_band = edge > fs / 2.0 * 0.98;
    }
}

/* ---- Demoduladores -------------------------------------------------------
 * Todos escriben n muestras reales en out a partir de n muestras complejas. */

/* Discriminador de FM: la frecuencia instantánea es la derivada de la fase.
 * arg(x[n]·conj(x[n−1])) da el giro de fase entre muestras (−π…π) sin tener
 * que "desenrollar" la fase. Se escala para que ±desviación → ±1. */
static void demod_fm(channel_t *c, const cf32 *x, int n, float *out, double deviation)
{
    const float k = (float)(c->fch / (2.0 * M_PI * deviation));
    cf32 prev = c->prev;
    for (int i = 0; i < n; i++) {
        float re = x[i].re * prev.re + x[i].im * prev.im;
        float im = x[i].im * prev.re - x[i].re * prev.im;
        out[i] = atan2f(im, re) * k;
        prev = x[i];
    }
    c->prev = prev;
}

/* AM: detector de envolvente |x| menos su componente continua (la portadora). */
static void demod_am(channel_t *c, const cf32 *x, int n, float *out)
{
    float dc = c->dc;
    for (int i = 0; i < n; i++) {
        float env = sqrtf(x[i].re * x[i].re + x[i].im * x[i].im);
        dc += 0.0005f * (env - dc);
        out[i] = env - dc;
    }
    c->dc = dc;
}

/* AGC de pico: ataque instantáneo, caída exponencial lenta. Lleva el audio de
 * AM/BLU/CW a un nivel constante independientemente de la intensidad. */
static void agc(channel_t *c, float *y, int n)
{
    const float target = 0.3f, max_gain = 2.0e4f;
    float peak = c->agc_peak;
    for (int i = 0; i < n; i++) {
        float a = fabsf(y[i]);
        peak = a > peak ? a : peak * c->agc_decay;
        float g = target / (peak + 1e-9f);
        y[i] *= g < max_gain ? g : max_gain;
    }
    c->agc_peak = peak;
}

/* PLL del piloto de 19 kHz. Mezcla el multiplex (MPX) con cos/sin de la fase
 * local, filtra I y Q (dos polos) y usa atan2(Q, I) como error de fase: así
 * la ganancia del lazo no depende de la amplitud del piloto. Lazo de 2º orden
 * con frecuencia natural ~30 Hz y amortiguamiento 0,707.
 * Devuelve en ref38[] la subportadora de 38 kHz regenerada (×2), lista para
 * demodular L−R por producto. */
static void stereo_pll(channel_t *c, const float *mpx, int n, float *ref38)
{
    const double T = 1.0 / c->fch;
    const double wn = 2.0 * M_PI * 30.0, zeta = 0.707;
    const double alpha = 2.0 * zeta * wn * T, beta = (wn * T) * (wn * T);
    const double lp = 1.0 - exp(-2.0 * M_PI * 150.0 * T); /* polos a 150 Hz */
    double ph = c->pll_phase, fr = c->pll_freq;
    double i1 = c->pll_i, q1 = c->pll_q, i2 = c->pll_i2, q2 = c->pll_q2;
    const double fmin = c->pll_nominal * 0.995, fmax = c->pll_nominal * 1.005;
    for (int k = 0; k < n; k++) {
        double s = sin(ph), co = cos(ph);
        i1 += lp * (mpx[k] * co - i1);
        q1 += lp * (-mpx[k] * s - q1);
        i2 += lp * (i1 - i2);
        q2 += lp * (q1 - q2);
        double err = atan2(q2, i2);
        fr += beta * err;
        fr = CLAMP(fr, fmin, fmax);
        ph += fr + alpha * err;
        if (ph > 2.0 * M_PI)
            ph -= 2.0 * M_PI;
        /* Con cos(ph) siguiendo al piloto sen(ωt), la subportadora sen(2ωt)
         * equivale a −sen(2·ph). El ×2 compensa el ½ del producto. */
        ref38[k] = (float)(-2.0 * sin(2.0 * ph));
    }
    c->pll_phase = ph;
    c->pll_freq = fr;
    c->pll_i = i1;
    c->pll_q = q1;
    c->pll_i2 = i2;
    c->pll_q2 = q2;
    /* Amplitud del piloto ≈ 2·|I|. Enganche: piloto presente (> 2 % de la
     * desviación máxima) y fase estable (Q pequeño frente a I). */
    c->pilot_level = (float)(2.0 * sqrt(i2 * i2 + q2 * q2));
    bool good = c->pilot_level > 0.02f && i2 > 3.0 * fabs(q2);
    c->pilot_lock_count = good ? MIN(c->pilot_lock_count + 1, 20) : MAX(c->pilot_lock_count - 2, 0);
    if (c->pilot_lock_count >= 10)
        c->pilot_locked = true;
    else if (c->pilot_lock_count == 0)
        c->pilot_locked = false;
}

/* Añade audio a la trama en curso y la emite cuando está llena. */
static void push_pcm(channel_t *c, const float *l, const float *r, int n, unsigned flags, chan_out_fn out, void *ctx)
{
    const int ch = r ? 2 : 1;
    /* Si cambia el formato (mono <-> estéreo) con una trama a medias, se
     * envía lo acumulado con el formato antiguo para no mezclar disposiciones. */
    if (c->pcm_n > 0 && (bool)r != c->pcm_stereo) {
        out(ctx, c->pcm, c->pcm_n, c->level_db, c->pcm_flags);
        c->pcm_n = 0;
    }
    c->pcm_stereo = r != NULL;
    c->pcm_flags = flags;
    for (int i = 0; i < n; i++) {
        float vl = l[i], vr = r ? r[i] : 0.f;
        /* De-énfasis (paso bajo de un polo) para WFM; en otros modos a=1. */
        if (c->p.mode == MODE_WFM) {
            c->deemph_l += c->deemph_a * (vl - c->deemph_l);
            vl = c->deemph_l;
            if (r) {
                c->deemph_r += c->deemph_a * (vr - c->deemph_r);
                vr = c->deemph_r;
            }
        }
        if (!(flags & CHAN_FLAG_SQL_OPEN))
            vl = vr = 0.f;
        c->pcm[c->pcm_n * ch] = (int16_t)CLAMP(vl * 32767.f, -32767.f, 32767.f);
        if (r)
            c->pcm[c->pcm_n * ch + 1] = (int16_t)CLAMP(vr * 32767.f, -32767.f, 32767.f);
        if (++c->pcm_n == PCM_FRAME) {
            out(ctx, c->pcm, PCM_FRAME, c->level_db, flags);
            c->pcm_n = 0;
        }
    }
}

/* Banda del filtro de canal (Hz respecto al centro de la emisora), la misma que
 * diseña build_chain() para cada modo. */
static void channel_band(const chan_params_t *p, double fch_max, double *lo, double *hi)
{
    const demod_mode_t m = p->mode;
    double bw = CLAMP((double)p->bw_hz, 100.0, fmin((double)MODE_MAX_BW[m], 0.9 * fch_max));
    switch (m) {
    case MODE_USB: *lo = SSB_LOW_HZ; *hi = bw; break;
    case MODE_LSB: *lo = -bw; *hi = -SSB_LOW_HZ; break;
    case MODE_CW: *lo = -bw / 2; *hi = bw / 2; break;
    default: *lo = -bw / 2; *hi = bw / 2; break;
    }
}

bool channel_wants_samples(const channel_t *c) { return c->configured && c->p.audio_on && !c->out_of_band; }

/* Nivel suavizado (~50 ms) y squelch con 2 dB de histéresis para que no
 * "tartamudee" en el umbral. Devuelve las banderas de la trama. */
static unsigned update_level(channel_t *c, float blk_db, double dt)
{
    float a = (float)(1.0 - exp(-dt / 0.05));
    c->level_db += a * (blk_db - c->level_db);
    if (isnan(c->p.squelch_db))
        c->sql_open = true;
    else if (c->level_db > c->p.squelch_db)
        c->sql_open = true;
    else if (c->level_db < c->p.squelch_db - 2.f)
        c->sql_open = false;
    return c->sql_open ? CHAN_FLAG_SQL_OPEN : 0;
}

float spectrum_band_dbfs(const float *db, int n, double fs, double lo_hz, double hi_hz, double enbw_bins)
{
    const double bin = fs / n;
    /* Bin k (fftshift) cubre [(k − n/2 − ½)·bin, (k − n/2 + ½)·bin]. */
    double lo = fmax(lo_hz, -fs / 2), hi = fmin(hi_hz, fs / 2);
    if (hi <= lo)
        return -150.f;
    /* La ventana reparte un tono en ~ENBW bins: no se integra menos que eso. */
    double minw = enbw_bins * bin;
    if (hi - lo < minw) {
        double mid = 0.5 * (hi + lo);
        lo = mid - 0.5 * minw;
        hi = mid + 0.5 * minw;
    }
    double kl = lo / bin + n / 2.0 + 0.5, kh = hi / bin + n / 2.0 + 0.5; /* en unidades de bin, 0 = borde izdo. del bin 0 */
    int k0 = MAX(0, (int)floor(kl)), k1 = MIN(n - 1, (int)floor(kh));
    double pw = 0;
    for (int k = k0; k <= k1; k++) {
        double w = fmin(kh, k + 1.0) - fmax(kl, (double)k); /* fracción del bin dentro de la banda */
        if (w > 0)
            pw += w * pow(10.0, 0.1 * (double)db[k]);
    }
    return power_to_db((float)(pw / enbw_bins));
}

void channel_feed_spectrum(channel_t *c, const float *db, int n, double enbw_bins, double dt, chan_out_fn out,
                           void *ctx)
{
    if (!c->configured || c->p.audio_on || n <= 0)
        return;
    if (c->out_of_band) {
        c->level_db = -150.f;
        c->meter_accum += dt;
        if (c->meter_accum >= 0.1) {
            c->meter_accum = 0;
            out(ctx, NULL, 0, c->level_db, CHAN_FLAG_OUT_OF_BAND);
        }
        return;
    }
    double lo, hi;
    channel_band(&c->p, 1e18, &lo, &hi); /* el recorte 0,9·fch nunca actúa con las anchuras máximas por modo */
    int64_t off = c->p.freq_hz - c->center_hz;
    float blk = spectrum_band_dbfs(db, n, c->fs, (double)off + lo, (double)off + hi, enbw_bins);
    unsigned flags = update_level(c, blk, dt);
    c->meter_accum += dt;
    if (c->meter_accum >= 0.1) {
        c->meter_accum = 0;
        out(ctx, NULL, 0, c->level_db, flags);
    }
}

/* Producto complejo vectorial sobre pares (re,im) entrelazados: para a = (ar,ai,…)
 * y un factor b dado como bd = (br,br,…), bs = (−bi,+bi,…):
 *     a·bd + swap(a)·bs = (ar·br − ai·bi, ai·br + ar·bi, …) = a·b.
 * Con GCC -O2 el bucle escalar equivalente no se autovectoriza (accesos AoS), de
 * ahí los vectores explícitos (extensión de GCC, 4 muestras complejas por v8f). */
static inline v8f swap_ri(v8f a) { return __builtin_shufflevector(a, a, 1, 0, 3, 2, 5, 4, 7, 6); }

/* out[i] = x[i]·e^{jω·i}·base, por trozos de MIX_CHUNK con su tabla de fase. x es
 * cf32 (in_f) o, si in_q != NULL, Q11 entrelazado al que se le resta la continua
 * (dc_re, dc_im, en cuentas) y se normaliza a ±1,0: así un canal lee directamente
 * el bloque crudo y no hace falta la copia float de todo el flujo. Dos productos
 * complejos por muestra (tabla fija × fasor del trozo), sin recurrencia serie. */
static inline __attribute__((always_inline)) void mix_loop(channel_t *c, const cf32 *in_f, const int16_t *in_q,
                                                          const bool q, float dc_re, float dc_im, int n,
                                                          cf32 *out)
{
    cf32 base = c->mix_base;
    const float kq = 1.0f / 2048.0f;
    const v8f kv = {kq, kq, kq, kq, kq, kq, kq, kq};
    const v8f dcv = {dc_re * kq, dc_im * kq, dc_re * kq, dc_im * kq, dc_re * kq, dc_im * kq, dc_re * kq, dc_im * kq};
    for (int i0 = 0; i0 < n; i0 += MIX_CHUNK) {
        const int m = MIN(MIX_CHUNK, n - i0);
        const v8f bd = {base.re, base.re, base.re, base.re, base.re, base.re, base.re, base.re};
        const v8f bs = {-base.im, base.im, -base.im, base.im, -base.im, base.im, -base.im, base.im};
        float *o = (float *)(out + i0);
        int k = 0;
        for (; k + 4 <= m; k += 4) {
            v8f a;
            if (q)
                a = __builtin_convertvector(*(const v8h *)(const void *)(in_q + 2 * (size_t)(i0 + k)), v8f) * kv - dcv;
            else
                a = *(const v8f *)(const void *)(in_f + i0 + k);
            const v8f t = a * *(const v8f *)(const void *)(c->tabd + 2 * k) +
                          swap_ri(a) * *(const v8f *)(const void *)(c->tabs + 2 * k);
            *(v8f *)(void *)(o + 2 * k) = t * bd + swap_ri(t) * bs;
        }
        for (; k < m; k++) { /* cola (n no múltiplo de 4) */
            float xr, xi;
            if (q) {
                xr = (float)in_q[2 * (size_t)(i0 + k)] * kq - dc_re * kq;
                xi = (float)in_q[2 * (size_t)(i0 + k) + 1] * kq - dc_im * kq;
            } else {
                xr = in_f[i0 + k].re;
                xi = in_f[i0 + k].im;
            }
            const float tr = xr * c->mix_tab[k].re - xi * c->mix_tab[k].im;
            const float ti = xr * c->mix_tab[k].im + xi * c->mix_tab[k].re;
            out[i0 + k].re = tr * base.re - ti * base.im;
            out[i0 + k].im = tr * base.im + ti * base.re;
        }
        const float br = base.re * c->mix_step.re - base.im * c->mix_step.im;
        base.im = base.re * c->mix_step.im + base.im * c->mix_step.re;
        base.re = br;
        /* Renormaliza |base| = 1 (el redondeo lo haría derivar) */
        const float mag = sqrtf(base.re * base.re + base.im * base.im);
        base.re /= mag;
        base.im /= mag;
    }
    /* el fasor avanza solo lo que se consumió: con n no múltiplo de MIX_CHUNK la
     * última tabla se pasó de largo, así que se corrige con e^{jω(rest − MIX_CHUNK)} */
    const int rest = n % MIX_CHUNK;
    if (rest) {
        const double a = c->mix_w * (double)(rest - MIX_CHUNK);
        const float cr = (float)cos(a), ci = (float)sin(a);
        const float br = base.re * cr - base.im * ci;
        base.im = base.re * ci + base.im * cr;
        base.re = br;
    }
    c->mix_base = base;
}

static void mix_chunk(channel_t *c, const cf32 *in_f, const int16_t *in_q, float dc_re, float dc_im, int n,
                      cf32 *out)
{
    if (in_q)
        mix_loop(c, in_f, in_q, true, dc_re, dc_im, n, out);
    else
        mix_loop(c, in_f, in_q, false, dc_re, dc_im, n, out);
}

static void channel_run(channel_t *c, const cf32 *in_f, const int16_t *in_q, float dc_re, float dc_im, int n,
                        chan_out_fn out, void *ctx);

void channel_process(channel_t *c, const cf32 *in, int n, chan_out_fn out, void *ctx)
{
    channel_run(c, in, NULL, 0.f, 0.f, n, out, ctx);
}

void channel_process_q11(channel_t *c, const int16_t *iq, int n, float dc_re, float dc_im, chan_out_fn out,
                         void *ctx)
{
    channel_run(c, NULL, iq, dc_re, dc_im, n, out, ctx);
}

static void channel_run(channel_t *c, const cf32 *in_f, const int16_t *in_q, float dc_re, float dc_im, int n,
                        chan_out_fn out, void *ctx)
{
    if (n <= 0 || !c->configured)
        return;
    if (!c->built)
        build_chain(c); /* canal sin audio que alguien procesa por muestras (pruebas) */
    const double dt = n / c->fs;

    if (c->out_of_band) {
        c->level_db = -150.f;
        c->meter_accum += dt;
        if (c->meter_accum >= 0.1) {
            c->meter_accum = 0;
            out(ctx, NULL, 0, c->level_db, CHAN_FLAG_OUT_OF_BAND);
        }
        return;
    }
    ensure_buffers(c, n);

    /* 1) Mezcla y diezmado por trozos de CHUNK muestras (los datos van a partir de
     *    PRE para dejar sitio a la historia de cada etapa). La salida de baja tasa
     *    se acumula en front. */
    cf32 *const pa = c->bufa + PRE, *const pb = c->bufb + PRE;
    int m = 0;
    for (int i0 = 0; i0 < n; i0 += CHUNK) {
        const int k = MIN(CHUNK, n - i0);
        mix_chunk(c, in_f ? in_f + i0 : NULL, in_q ? in_q + 2 * (size_t)i0 : NULL, dc_re, dc_im, k, pa);
        cf32 *cur = pa, *alt = pb;
        int mm = k;
        for (int s = 0; s < c->nstages; s++) {
            mm = dstage_process(&c->stage[s], cur, mm, alt);
            cf32 *t = cur;
            cur = alt;
            alt = t;
        }
        memcpy(c->front + m, cur, sizeof(cf32) * (size_t)mm);
        m += mm;
    }
    if (m <= 0)
        return;
    cf32 *cur = c->front, *alt = c->front2;

    /* 2) Filtro de canal */
    cfir_process(&c->chfilt, cur, m, alt);
    cur = alt;

    /* 3) Nivel: potencia media del canal en dBFS, suavizada (~50 ms) */
    double pw = 0;
    for (int i = 0; i < m; i++)
        pw += cur[i].re * cur[i].re + cur[i].im * cur[i].im;
    float blk_db = power_to_db((float)(pw / m));
    unsigned flags = update_level(c, blk_db, dt);

    if (!c->p.audio_on) {
        c->meter_accum += dt;
        if (c->meter_accum >= 0.1) {
            c->meter_accum = 0;
            out(ctx, NULL, 0, c->level_db, flags);
        }
        return;
    }

    /* 4) Demodulación */
    float *d = c->demod;
    switch (c->p.mode) {
    case MODE_WFM: demod_fm(c, cur, m, d, 75000.0); break;
    case MODE_NFM: demod_fm(c, cur, m, d, 5000.0); break;
    case MODE_AM: demod_am(c, cur, m, d); agc(c, d, m); break;
    case MODE_USB:
    case MODE_LSB:
        for (int i = 0; i < m; i++)
            d[i] = cur[i].re;
        agc(c, d, m);
        break;
    case MODE_CW: {
        /* Batido con un oscilador de 700 Hz: la portadora (en 0 Hz) pasa a
         * ser un tono audible. Se usa el búfer que no contiene 'cur'. */
        cf32 *tmp = (cur == c->front) ? c->front2 : c->front;
        nco_mix(&c->bfo, cur, m, tmp);
        for (int i = 0; i < m; i++)
            d[i] = tmp[i].re;
        agc(c, d, m);
        break;
    }
    default: return;
    }

    /* 5) Estéreo (solo WFM con piloto enganchado) y remuestreo a 48 kHz */
    if (c->p.mode == MODE_WFM) {
        float *ref = c->demod2;
        stereo_pll(c, d, m, ref);
        int na = resamp_process(&c->rs_main, d, m, c->aud);
        if (c->p.stereo && c->pilot_locked) {
            for (int i = 0; i < m; i++)
                ref[i] *= d[i]; /* (L−R)·sub × 2·sub → L−R en banda base */
            int nb = resamp_process(&c->rs_diff, ref, m, c->aud2);
            int nn = MIN(na, nb);
            for (int i = 0; i < nn; i++) {
                float sum = c->aud[i], diff = c->aud2[i];
                c->aud[i] = 0.5f * (sum + diff);  /* L */
                c->aud2[i] = 0.5f * (sum - diff); /* R */
            }
            push_pcm(c, c->aud, c->aud2, nn, flags | CHAN_FLAG_STEREO | CHAN_FLAG_PILOT, out, ctx);
        } else {
            /* Mantiene el estado del remuestreador L−R en fase aunque no se use. */
            if (c->p.stereo)
                resamp_process(&c->rs_diff, ref, m, c->aud2);
            for (int i = 0; i < na; i++)
                c->aud[i] *= 0.5f; /* misma escala que (L+R)/2 en estéreo */
            push_pcm(c, c->aud, NULL, na, flags | (c->pilot_locked ? CHAN_FLAG_PILOT : 0), out, ctx);
        }
    } else {
        int na = resamp_process(&c->rs_main, d, m, c->aud);
        push_pcm(c, c->aud, NULL, na, flags, out, ctx);
    }
}
