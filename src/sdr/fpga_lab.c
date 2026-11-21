/*
 * fpga_lab.c — Cliente de la imagen de FPGA "lab". Ver fpga_lab.h y docs/hardware/FIRMWARE.md.
 */
#include "fpga_lab.h"

#include <stdatomic.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FPGA_LAB_WITH_LIBBLADERF
#include <libbladeRF.h>
#endif

static void set_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- Detección y registros ----------------------------------------------- */

int fpga_lab_probe(const fpga_lab_io_t *io, fpga_lab_t *lab, char *err, size_t errlen)
{
    memset(lab, 0, sizeof *lab);
    if (!io || !io->fpga_version || !io->gpio_read || !io->wb_read) {
        set_err(err, errlen, "E/S incompleta");
        return -1;
    }
    lab->io = *io;
    uint32_t maj = 0, min = 0, patch = 0;
    if (io->fpga_version(io->ctx, &maj, &min, &patch) < 0) {
        set_err(err, errlen, "no se pudo leer la versión de la FPGA");
        return -1;
    }
    /* Paso 1 (seguro en cualquier imagen): la versión debe ser la de la lab. */
    if (patch < FPGA_LAB_VERSION_PATCH_MIN) {
        set_err(err, errlen, "FPGA %u.%u.%u: no es la imagen lab", maj, min, patch);
        return 0;
    }
    /* Paso 2 (seguro): marca de solo lectura en config_gpio. */
    uint32_t gpio = 0;
    if (io->gpio_read(io->ctx, &gpio) < 0) {
        set_err(err, errlen, "no se pudo leer config_gpio");
        return -1;
    }
    if ((gpio & FPGA_LAB_GPIO_MARK_MASK) != FPGA_LAB_GPIO_MARK_VALUE) {
        set_err(err, errlen, "versión %u.%u.%u pero sin marca lab en config_gpio (0x%08x)", maj, min, patch, gpio);
        return 0;
    }
    /* Paso 3: ahora sí, el bus Wishbone está conectado. Se verifica el ID. */
    uint32_t id = 0;
    if (io->wb_read(io->ctx, 4u * FPGA_LAB_REG_ID, &id) < 0) {
        set_err(err, errlen, "fallo leyendo el registro ID por Wishbone");
        return -1;
    }
    if (id != FPGA_LAB_ID_VALUE) {
        set_err(err, errlen, "ID Wishbone inesperado 0x%08x", id);
        return -1;
    }
    if (io->wb_read(io->ctx, 4u * FPGA_LAB_REG_VERSION, &lab->version) < 0 ||
        io->wb_read(io->ctx, 4u * FPGA_LAB_REG_FEATURES, &lab->features) < 0) {
        set_err(err, errlen, "fallo leyendo VERSION/FEATURES");
        return -1;
    }
    lab->present = true;
    /* La 1.1 devuelve 0xBAD0BAD0 en los registros 16..22: solo se leen si VERSION lo promete. */
    lab->v12 = lab->version >= FPGA_LAB_VERSION_1_2 && lab->version != 0xBAD0BAD0u;
    if (lab->v12 && (io->wb_read(io->ctx, 4u * FPGA_LAB_REG_CAPS, &lab->caps) < 0 ||
                     io->wb_read(io->ctx, 4u * FPGA_LAB_REG_SYNTH, &lab->synth) < 0)) {
        set_err(err, errlen, "fallo leyendo CAPS/SYNTH");
        lab->present = false;
        return -1;
    }
    return 1;
}

int fpga_lab_reg_read(const fpga_lab_t *lab, int index, uint32_t *val)
{
    if (!lab || !lab->present || index < 0 || index > FPGA_LAB_REG_MAX) return -1;
    return lab->io.wb_read(lab->io.ctx, 4u * (uint32_t)index, val);
}

int fpga_lab_reg_write(const fpga_lab_t *lab, int index, uint32_t val)
{
    if (!lab || !lab->present || index < 0 || index > FPGA_LAB_REG_MAX || !lab->io.wb_write) return -1;
    return lab->io.wb_write(lab->io.ctx, 4u * (uint32_t)index, val);
}

int fpga_lab_selftest(const fpga_lab_t *lab, char *err, size_t errlen)
{
    static const uint32_t pat[] = {0xA5C3F00Du, 0x5A3C0FF2u, 0x00000000u, 0xFFFFFFFFu, 0x12345678u};
    for (size_t i = 0; i < ARRAY_LEN(pat); i++) {
        uint32_t r = 0, rn = 0;
        if (fpga_lab_reg_write(lab, FPGA_LAB_REG_SCRATCH, pat[i]) < 0 ||
            fpga_lab_reg_read(lab, FPGA_LAB_REG_SCRATCH, &r) < 0 ||
            fpga_lab_reg_read(lab, FPGA_LAB_REG_SCRATCH_N, &rn) < 0) {
            set_err(err, errlen, "error de bus en la prueba del registro SCRATCH");
            return -1;
        }
        if (r != pat[i] || rn != ~pat[i]) {
            set_err(err, errlen, "SCRATCH: escrito 0x%08x, leído 0x%08x / 0x%08x", pat[i], r, rn);
            return -1;
        }
    }
    return 0;
}

int fpga_lab_config_spectrum(const fpga_lab_t *lab, int log2n, int k, bool pad)
{
    if (log2n < 10 || log2n > 13 || k < 1 || k > 65535) return -1;
    if (fpga_lab_reg_write(lab, FPGA_LAB_REG_LOG2N, (uint32_t)log2n) < 0) return -1;
    if (fpga_lab_reg_write(lab, FPGA_LAB_REG_K, (uint32_t)k) < 0) return -1;
    return fpga_lab_reg_write(lab, FPGA_LAB_REG_CTRL, pad ? 1u : 0u);
}

int fpga_lab_set_spectrum_mode(const fpga_lab_t *lab, bool on)
{
    if (!lab || !lab->present || !lab->io.gpio_write) return -1;
    uint32_t g = 0;
    if (lab->io.gpio_read(lab->io.ctx, &g) < 0) return -1;
    g &= ~FPGA_LAB_GPIO_RXMUX_MASK;
    if (on) g |= FPGA_LAB_RXMUX_SPECTRUM << 8;
    if (lab->io.gpio_write(lab->io.ctx, g) < 0) return -1;
    /* Al pasar a rx_mux = 3 se ponen a cero los contadores: así fifo_ovf, fft, etc. solo cuentan
     * lo ocurrido en modo espectro (con rx_mux = 0 la FIFO desborda mientras nadie lee). */
    return on ? fpga_lab_clear_counters(lab) : 0;
}

int fpga_lab_read_telemetry(const fpga_lab_t *lab, fpga_lab_telem_t *t)
{
    memset(t, 0, sizeof *t);
    if (!lab || !lab->present || !lab->v12) return -1;
    if (fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_ADC_SAT, &t->adc_sat) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_SAT_WIN, &t->sat_win) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_SPEC_DROP, &t->spec_drop) < 0)
        return -1;
    t->sat_pct = 100.0 * t->sat_win / FPGA_LAB_SAT_WIN_SAMPLES;
    return 0;
}

bool fpga_lab_has_gain8(const fpga_lab_t *lab)
{
    return lab && lab->present && lab->version >= FPGA_LAB_VERSION_1_3 && (lab->caps & FPGA_LAB_CAP_GAIN8);
}

int fpga_lab_gain8_set(const fpga_lab_t *lab, unsigned shift)
{
    if (!fpga_lab_has_gain8(lab) || shift > 3) return -1;
    return fpga_lab_reg_write(lab, FPGA_LAB_REG_GAIN8, shift);
}

int fpga_lab_gain8_get(const fpga_lab_t *lab, unsigned *shift)
{
    uint32_t v;
    if (!fpga_lab_has_gain8(lab) || fpga_lab_reg_read(lab, FPGA_LAB_REG_GAIN8, &v) < 0) return -1;
    *shift = v & 3u;
    return 0;
}

int fpga_lab_read_sat8(const fpga_lab_t *lab, uint32_t *cnt)
{
    if (!fpga_lab_has_gain8(lab)) return -1;
    return fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_SAT8, cnt);
}

int fpga_lab_read_time(const fpga_lab_t *lab, uint64_t *t)
{
    uint32_t lo = 0, hi = 0;
    if (!lab || !lab->present || !lab->v12) return -1;
    if (fpga_lab_reg_read(lab, FPGA_LAB_REG_TIME_LO, &lo) < 0 || fpga_lab_reg_read(lab, FPGA_LAB_REG_TIME_HI, &hi) < 0)
        return -1;
    *t = ((uint64_t)hi << 32) | lo;
    return 0;
}

int fpga_lab_set_k_hot(const fpga_lab_t *lab, int k)
{
    if (!lab || !lab->present || !lab->v12 || k < 1 || k > 65535) return -1;
    return fpga_lab_reg_write(lab, FPGA_LAB_REG_K, (uint32_t)k);
}

int fpga_lab_frame_words(int log2n) { return (((1 << log2n) + FPGA_SPEC_HDR_WORDS + 2047) / 2048) * 2048; }

int fpga_lab_read_counters(const fpga_lab_t *lab, fpga_lab_counters_t *c)
{
    uint32_t st = 0;
    if (fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_SAMPLES, &c->samples) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_FIFO_OVF, &c->fifo_ovf) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_FFT, &c->fft_frames) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_SPEC, &c->spec_frames) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_LATE, &c->late) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_CNT_WORDS, &c->words) < 0 ||
        fpga_lab_reg_read(lab, FPGA_LAB_REG_STATUS, &st) < 0)
        return -1;
    c->active = (st & 1u) != 0;
    return 0;
}

int fpga_lab_clear_counters(const fpga_lab_t *lab)
{
    return fpga_lab_reg_write(lab, FPGA_LAB_REG_CLEAR, 1u);
}

/* ---- Escala -------------------------------------------------------------- */

double fpga_lab_power_value(uint32_t word) { return ldexp((double)(word & 0x3FFFFFFu), (int)(word >> 26)); }

static double window_sum_calc(int n)
{
    /* Misma ventana que src/dsp/fft.c, cuantizada a Q15 como en la ROM de la FPGA. */
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    double s = 0;
    for (int i = 0; i < n / 2; i++) {
        double x = 2.0 * M_PI * i / (n - 1);
        double v = a0 - a1 * cos(x) + a2 * cos(2 * x) - a3 * cos(3 * x);
        long q = lround(v * 32768.0);
        s += 2.0 * (double)q / 32768.0;
    }
    return s;
}

/* La suma depende solo de N (potencias de 2 de 1024 a 8192) y calcularla cuesta 3·N/2 cosenos:
 * se guarda en una caché (_Atomic: la usan el hilo DSP y las pruebas; calcular dos veces es inocuo). */
double fpga_lab_window_sum(int n)
{
    static _Atomic double cache[14];
    int l = 0;
    while (l < 14 && (1 << l) != n) l++;
    if (l == 14) return window_sum_calc(n);
    double v = atomic_load_explicit(&cache[l], memory_order_relaxed);
    if (v == 0.0) {
        v = window_sum_calc(n);
        atomic_store_explicit(&cache[l], v, memory_order_relaxed);
    }
    return v;
}

double fpga_lab_power_to_dbfs(double acc, int n, int k)
{
    const double wsum = fpga_lab_window_sum(n);
    const double p = 4.0 * acc / (k > 0 ? k : 1);
    return 10.0 * log10(p + 1e-30) - 20.0 * log10(2048.0 * wsum);
}

void fpga_lab_frame_to_dbfs(const fpga_lab_frame_t *f, float *db)
{
    const double wsum = fpga_lab_window_sum(f->n);
    const double off = -20.0 * log10(2048.0 * wsum) + 10.0 * log10(4.0 / (f->k > 0 ? f->k : 1));
    /* dB = 10·log10(M·2^E) = 10·log10(2)·(log2(M) + E). Con M de 26 bits (exacto en float salvo los
     * 2 bits bajos, error relativo < 6e-8) log2(M) = e + log2(m), m en [1,2), con un polinomio de
     * grado 5 (error < 2e-5 en log2 → < 6e-5 dB, mil veces por debajo del escalón de 0,6 dB del
     * mensaje). Evita 4096 log10 en doble precisión + ldexp por imagen. */
    const float k10 = 3.0102999566f; /* 10·log10(2) */
    const float offf = (float)off;
    const float floor_db = -300.0f + offf; /* igual que el +1e-30 de la versión exacta */
    for (int i = 0; i < f->n; i++) {
        const uint32_t w = f->power[i], m26 = w & 0x3FFFFFFu;
        if (m26 == 0) { db[i] = floor_db; continue; }
        const float x = (float)m26;
        uint32_t b;
        memcpy(&b, &x, 4);
        const int ex = (int)(b >> 23) - 127;
        b = (b & 0x7FFFFFu) | 0x3F800000u;
        float mm;
        memcpy(&mm, &b, 4);
        const float t = (mm - 1.0f) / (mm + 1.0f); /* log2(m) = 2/ln2 · atanh(t), |t| ≤ 1/3 */
        const float t2 = t * t;
        const float l2 = t * (2.8853900818f + t2 * (0.9617966939f + t2 * (0.5770780164f + t2 * 0.4121985831f)));
        db[i] = k10 * ((float)ex + l2 + (float)(w >> 26)) + offf;
    }
}

/* ---- Intérprete ---------------------------------------------------------- */

typedef enum { PS_MAGIC0, PS_MAGIC1, PS_HEADER, PS_BINS, PS_SKIP } ps_state_t;

struct fpga_lab_parser {
    fpga_lab_frame_fn cb;
    void *ctx;
    ps_state_t st;
    uint32_t hdr[FPGA_SPEC_HDR_WORDS];
    int hpos;
    int bpos;
    uint32_t skip;
    fpga_lab_frame_t frame;
    bool have_prev;
    uint32_t prev_group;
    bool pending;        /* trama completa a la espera de validar el relleno */
    uint64_t frames, skipped, gaps;
};

fpga_lab_parser_t *fpga_lab_parser_create(fpga_lab_frame_fn cb, void *ctx)
{
    fpga_lab_parser_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cb = cb;
    p->ctx = ctx;
    p->st = PS_MAGIC0;
    return p;
}

void fpga_lab_parser_destroy(fpga_lab_parser_t *p) { free(p); }

void fpga_lab_parser_stats(const fpga_lab_parser_t *p, uint64_t *frames, uint64_t *skipped_words, uint64_t *gaps)
{
    if (frames) *frames = p->frames;
    if (skipped_words) *skipped_words = p->skipped;
    if (gaps) *gaps = p->gaps;
}

static bool header_ok(const uint32_t *h)
{
    const uint32_t n = h[2] & 0xFFFFu, k = h[2] >> 16;
    if (n < 1024 || n > 8192 || (n & (n - 1))) return false;
    if (k < 1) return false;
    if ((h[6] & 0xFFu) != FPGA_SPEC_HDR_WORDS) return false;
    if (((h[6] >> 8) & 0xFFu) != 1) return false; /* versión del formato */
    if (h[7] < FPGA_SPEC_HDR_WORDS + n || h[7] > FPGA_SPEC_HDR_WORDS + n + 2048u) return false;
    return true;
}

static void deliver(fpga_lab_parser_t *p)
{
    if (p->have_prev && p->frame.group != p->prev_group + 1) {
        p->gaps++;
        p->frame.dropped = true;
    }
    p->prev_group = p->frame.group;
    p->have_prev = true;
    p->frames++;
    if (p->cb) p->cb(p->ctx, &p->frame);
}

void fpga_lab_parser_feed_words(fpga_lab_parser_t *p, const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint32_t x = w[i];
        switch (p->st) {
        case PS_MAGIC0:
            if (x == FPGA_SPEC_MAGIC0) p->st = PS_MAGIC1; else p->skipped++;
            break;
        case PS_MAGIC1:
            if (x == FPGA_SPEC_MAGIC1) {
                p->hdr[0] = FPGA_SPEC_MAGIC0;
                p->hdr[1] = FPGA_SPEC_MAGIC1;
                p->hpos = 2;
                p->st = PS_HEADER;
            } else {
                p->st = (x == FPGA_SPEC_MAGIC0) ? PS_MAGIC1 : PS_MAGIC0;
                p->skipped += 2;
            }
            break;
        case PS_HEADER:
            p->hdr[p->hpos++] = x;
            if (p->hpos == FPGA_SPEC_HDR_WORDS) {
                if (!header_ok(p->hdr)) { p->st = PS_MAGIC0; p->skipped += FPGA_SPEC_HDR_WORDS; break; }
                fpga_lab_frame_t *f = &p->frame;
                f->n = (int)(p->hdr[2] & 0xFFFFu);
                f->k = (int)(p->hdr[2] >> 16);
                f->group = p->hdr[3];
                f->ts = ((uint64_t)p->hdr[5] << 32) | p->hdr[4];
                f->dropped = (p->hdr[6] >> 16) & 1u;
                f->total_words = p->hdr[7];
                p->bpos = 0;
                p->st = PS_BINS;
            }
            break;
        case PS_BINS:
            /* dos números mágicos seguidos: empieza otra trama, la actual está truncada */
            if (p->bpos > 0 && x == FPGA_SPEC_MAGIC1 && p->frame.power[p->bpos - 1] == FPGA_SPEC_MAGIC0) {
                p->skipped += (uint64_t)p->bpos + FPGA_SPEC_HDR_WORDS;
                p->gaps++;
                p->hdr[0] = FPGA_SPEC_MAGIC0;
                p->hdr[1] = FPGA_SPEC_MAGIC1;
                p->hpos = 2;
                p->st = PS_HEADER;
                break;
            }
            p->frame.power[p->bpos++] = x;
            if (p->bpos == p->frame.n) {
                p->skip = p->frame.total_words - FPGA_SPEC_HDR_WORDS - (uint32_t)p->frame.n;
                if (p->skip) {
                    p->pending = true; /* se entrega al verificar que el relleno llega completo */
                    p->st = PS_SKIP;
                } else {
                    deliver(p);
                    p->st = PS_MAGIC0;
                }
            }
            break;
        case PS_SKIP:
            if (x == FPGA_SPEC_MAGIC0) { /* la trama siguiente llegó antes de tiempo: se han perdido palabras */
                p->pending = false;
                p->skipped += (uint64_t)p->frame.n + FPGA_SPEC_HDR_WORDS;
                p->gaps++;
                p->st = PS_MAGIC1;
            } else if (--p->skip == 0) {
                p->pending = false;
                deliver(p);
                p->st = PS_MAGIC0;
            }
            break;
        }
    }
}

void fpga_lab_parser_feed_iq(fpga_lab_parser_t *p, const int16_t *iq, size_t nsamples)
{
    uint32_t buf[1024];
    size_t i = 0;
    while (i < nsamples) {
        size_t m = MIN(nsamples - i, ARRAY_LEN(buf));
        for (size_t j = 0; j < m; j++)
            buf[j] = (uint32_t)(uint16_t)iq[2 * (i + j)] | ((uint32_t)(uint16_t)iq[2 * (i + j) + 1] << 16);
        fpga_lab_parser_feed_words(p, buf, m);
        i += m;
    }
}

/* ---- Integración con el motor ---------------------------------------------- */

int fpga_lab_choose_params(int fft, double rate, int fps, int *log2n, int *k)
{
    if (rate <= 0 || fps < 1 || !log2n || !k) return -1;
    int l = 10;
    while (l < 13 && (1 << l) < fft) l++;
    const int n = 1 << l;
    /* Palabras por trama: cabecera + N, completado a múltiplos de 2048 (relleno de la FPGA,
     * necesario para que el último paquete USB no se quede retenido en el FX3). */
    const int total = ((n + FPGA_SPEC_HDR_WORDS + 2047) / 2048) * 2048;
    double kk = rate / ((double)fps * n);
    const double kmin = rate * total / ((double)n * FPGA_LAB_MAX_WORDS_PER_S);
    if (kk < kmin) kk = kmin;
    kk = ceil(kk - 1e-9);
    if (kk > 65535.0) kk = 65535.0;
    if (kk < 1.0) kk = 1.0;
    *log2n = l;
    *k = (int)kk;
    return total;
}

size_t spectrum_msg_pack(uint8_t *m, const float *db, int n, int src, double center, double rate, float dbmin,
                         float dbstep)
{
    memset(m, 0, 32);
    m[0] = 1;
    m[1] = (uint8_t)src;
    const uint32_t nn = (uint32_t)n;
    memcpy(m + 4, &nn, 4);
    memcpy(m + 8, &center, 8);
    memcpy(m + 16, &rate, 8);
    memcpy(m + 24, &dbmin, 4);
    memcpy(m + 28, &dbstep, 4);
    for (int i = 0; i < n; i++) {
        float v = (db[i] - dbmin) / dbstep;
        m[32 + i] = (uint8_t)(v < 0.f ? 0.f : v > 255.f ? 255.f : v);
    }
    return 32 + (size_t)n;
}


/* ---- Carga de imagen en RAM ---------------------------------------------- */

const char *fpga_load_state_name(fpga_load_state_t s)
{
    return s == FPGA_LOAD_OK ? "ok" : s == FPGA_LOAD_ERROR ? "error" : "omitido";
}

/* SHA-256 (FIPS 180-4): bloques de 512 bit; 64 rondas con Ch, Maj, Σ0, Σ1, σ0, σ1 sobre palabras de 32 bit. */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

int fpga_sha256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64 * 64];
    uint64_t total = 0;
    size_t have = 0;
    for (;;) {
        size_t r = fread(buf + have, 1, sizeof buf - have, f);
        if (r == 0 && ferror(f)) {
            fclose(f);
            return -1;
        }
        have += r;
        total += r;
        size_t off = 0;
        while (have - off >= 64 && (r > 0 || have - off >= 64)) {
            sha256_block(h, buf + off);
            off += 64;
        }
        memmove(buf, buf + off, have - off);
        have -= off;
        if (r == 0)
            break;
    }
    fclose(f);
    /* relleno: 0x80, ceros y la longitud en bits (64 bit, big-endian) */
    uint8_t tail[128] = {0};
    memcpy(tail, buf, have);
    tail[have] = 0x80;
    size_t nt = have + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = total * 8;
    for (int i = 0; i < 8; i++)
        tail[nt - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t o = 0; o < nt; o += 64)
        sha256_block(h, tail + o);
    for (int i = 0; i < 8; i++)
        snprintf(hex + 8 * i, 9, "%08x", h[i]);
    return 0;
}

int fpga_image_verify(const char *path, char hex[65], char *err, size_t errlen)
{
    char sumfile[600];
    snprintf(sumfile, sizeof sumfile, "%s.sha256", path);
    FILE *f = fopen(sumfile, "r");
    if (!f) {
        set_err(err, errlen, "falta %s: no se carga una imagen sin suma de comprobación", sumfile);
        return -1;
    }
    char line[256] = "";
    char *ok = fgets(line, sizeof line, f);
    fclose(f);
    char want[65] = "";
    size_t n = 0;
    while (ok && n < 64 && ((line[n] >= '0' && line[n] <= '9') || (line[n] >= 'a' && line[n] <= 'f') ||
                            (line[n] >= 'A' && line[n] <= 'F'))) {
        want[n] = (char)((line[n] >= 'A' && line[n] <= 'F') ? line[n] + 32 : line[n]);
        n++;
    }
    if (n != 64) {
        set_err(err, errlen, "%s no tiene una suma SHA-256 válida", sumfile);
        return -1;
    }
    want[64] = '\0';
    if (fpga_sha256_file(path, hex) < 0) {
        set_err(err, errlen, "no se puede leer %s", path);
        return -1;
    }
    if (strcmp(want, hex) != 0) {
        set_err(err, errlen, "la suma SHA-256 de %s no coincide (esperada %.12s…, calculada %.12s…)", path, want, hex);
        return -1;
    }
    return 0;
}

#include <dirent.h>
#include <sys/stat.h>

static bool path_is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int fpga_image_find(const char *dir, const char *model, const char *kind, char *out, size_t outlen, char *ver,
                    size_t verlen)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    char prefix[64];
    snprintf(prefix, sizeof prefix, "hosted-%s-%s-", model, kind);
    long best = -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        unsigned a, b, c;
        char tail[8] = "";
        if (strncmp(de->d_name, prefix, strlen(prefix)) != 0)
            continue;
        if (sscanf(de->d_name + strlen(prefix), "%u.%u.%u%7s", &a, &b, &c, tail) != 4 || strcmp(tail, ".rbf") != 0)
            continue;
        long key = (long)a * 1000000L + (long)b * 1000L + (long)c;
        if (key > best) {
            best = key;
            snprintf(out, outlen, "%s/%s", dir, de->d_name);
            snprintf(ver, verlen, "%u.%u.%u", a, b, c);
        }
    }
    closedir(d);
    return best >= 0 ? 0 : -1;
}

static void load_fail(fpga_load_result_t *r, fpga_load_state_t st, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void load_fail(fpga_load_result_t *r, fpga_load_state_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->msg, sizeof r->msg, fmt, ap);
    va_end(ap);
    r->state = st;
}

/* "hosted-xA9-lab-0.16.103.rbf" → modelo "xA9" y versión "0.16.103"; false si el nombre no sigue ese patrón. */
static bool image_name_parts(const char *path, char *model, size_t ml, char *ver, size_t vl)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char m[16], k[16];
    unsigned x, y, z;
    if (sscanf(b, "hosted-%15[^-]-%15[^-]-%u.%u.%u.rbf", m, k, &x, &y, &z) != 5)
        return false;
    snprintf(model, ml, "%s", m);
    snprintf(ver, vl, "%u.%u.%u", x, y, z);
    return true;
}

void fpga_lab_ensure(const fpga_loader_t *ld, const char *kind, const char *path, const char *const *dirs,
                     fpga_load_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->state = FPGA_LOAD_SKIPPED;
    const bool want_lab = strcmp(kind, "lab") == 0;
    if (!want_lab && strcmp(kind, "stock") != 0) {
        load_fail(r, FPGA_LOAD_ERROR, "imagen desconocida «%s» (use lab o stock)", kind);
        return;
    }
    if (!ld || !ld->fpga_kle || !ld->lib_version || !ld->is_lab || !ld->load || !ld->reopen) {
        load_fail(r, FPGA_LOAD_SKIPPED, "la fuente no es un bladeRF");
        return;
    }
    bool is_lab = false;
    if (ld->is_lab(ld->ctx, &is_lab) < 0) {
        load_fail(r, FPGA_LOAD_ERROR, "no se pudo detectar la imagen cargada");
        return;
    }
    if (is_lab == want_lab) {
        load_fail(r, FPGA_LOAD_SKIPPED, want_lab ? "la imagen lab ya está cargada" : "la imagen de serie ya está cargada");
        return;
    }
    int ma = 0, mi = 0, pa = 0;
    if (ld->lib_version(ld->ctx, &ma, &mi, &pa) < 0 || ma < FPGA_LIB_MIN_MAJOR ||
        (ma == FPGA_LIB_MIN_MAJOR && mi < FPGA_LIB_MIN_MINOR)) {
        load_fail(r, FPGA_LOAD_ERROR, "libbladeRF %d.%d.%d es demasiado antigua (se exige >= %d.%d)", ma, mi, pa,
                  FPGA_LIB_MIN_MAJOR, FPGA_LIB_MIN_MINOR);
        return;
    }
    int kle = 0;
    if (ld->fpga_kle(ld->ctx, &kle) < 0 || (kle != 301 && kle != 49)) {
        load_fail(r, FPGA_LOAD_ERROR, "modelo de FPGA desconocido (%d kLE): no se carga nada", kle);
        return;
    }
    const char *model = kle == 301 ? "xA9" : "xA4";
    if (kle != 301 && want_lab) {
        load_fail(r, FPGA_LOAD_ERROR, "la imagen lab solo existe para la xA9 (301 kLE); esta placa es una %s", model);
        return;
    }

    /* Localizar el fichero: ruta explícita (fichero o carpeta) o búsqueda en las carpetas candidatas */
    char file[512] = "", ver[32] = "";
    bool explicit_path = path && path[0];
    if (explicit_path && !path_is_dir(path)) {
        if (access(path, R_OK) != 0) {
            load_fail(r, FPGA_LOAD_ERROR, "no se encuentra la imagen %s", path);
            return;
        }
        snprintf(file, sizeof file, "%s", path);
    } else {
        const char *one[2] = {explicit_path ? path : NULL, NULL};
        const char *const *list = explicit_path ? one : dirs;
        bool found = false;
        for (int i = 0; list && list[i] && !found; i++)
            found = fpga_image_find(list[i], model, kind, file, sizeof file, ver, sizeof ver) == 0;
        if (!found) {
            load_fail(r, explicit_path ? FPGA_LOAD_ERROR : FPGA_LOAD_SKIPPED,
                      "no hay ninguna imagen hosted-%s-%s-*.rbf%s", model, kind,
                      explicit_path ? " en la carpeta indicada" : " (firmware/out o instalada)");
            return;
        }
    }
    snprintf(r->path, sizeof r->path, "%s", file);
    char pm[16] = "";
    char pv[32] = "";
    if (image_name_parts(file, pm, sizeof pm, pv, sizeof pv)) {
        snprintf(r->version, sizeof r->version, "%s", pv);
        if (strcmp(pm, model) != 0) {
            load_fail(r, FPGA_LOAD_ERROR, "la imagen es para la %s y esta placa es una %s: no se carga", pm, model);
            return;
        }
    } else {
        snprintf(r->version, sizeof r->version, "%s", ver);
    }
    char verr[300] = "";
    if (fpga_image_verify(file, r->sha256, verr, sizeof verr) < 0) {
        r->sha256[0] = '\0';
        load_fail(r, FPGA_LOAD_ERROR, "%s", verr);
        return;
    }
    char lerr[200] = "";
    int lrc = ld->load(ld->ctx, file, lerr, sizeof lerr);
    /* Siempre se cierra y reabre, también si la carga falló: el estado de la placa es incierto */
    char rerr[200] = "";
    int rrc = ld->reopen(ld->ctx, rerr, sizeof rerr);
    if (lrc < 0) {
        load_fail(r, FPGA_LOAD_ERROR, "la carga en RAM falló: %s%s%s", lerr, rrc < 0 ? "; además no se pudo reabrir: " : "",
                  rrc < 0 ? rerr : "");
        return;
    }
    if (rrc < 0) {
        load_fail(r, FPGA_LOAD_ERROR, "imagen cargada pero no se pudo reabrir la placa: %s", rerr);
        return;
    }
    r->state = FPGA_LOAD_OK;
    snprintf(r->msg, sizeof r->msg, "imagen %s %s cargada en RAM (sha256 %.16s…); la flash no se ha tocado", kind,
             r->version, r->sha256);
}

/* ---- Adaptador a libbladeRF ---------------------------------------------- */

#ifdef FPGA_LAB_WITH_LIBBLADERF
static int io_ver(void *c, uint32_t *ma, uint32_t *mi, uint32_t *pa)
{
    struct bladerf_version v;
    int r = bladerf_fpga_version((struct bladerf *)c, &v);
    if (r < 0) return r;
    *ma = v.major; *mi = v.minor; *pa = v.patch;
    return 0;
}
static int io_gr(void *c, uint32_t *v) { return bladerf_config_gpio_read((struct bladerf *)c, v); }
static int io_gw(void *c, uint32_t v) { return bladerf_config_gpio_write((struct bladerf *)c, v); }
static int io_wr(void *c, uint32_t a, uint32_t *v) { return bladerf_wishbone_master_read((struct bladerf *)c, a, v); }
static int io_ww(void *c, uint32_t a, uint32_t v) { return bladerf_wishbone_master_write((struct bladerf *)c, a, v); }

void fpga_lab_io_bladerf(fpga_lab_io_t *io, void *dev)
{
    io->ctx = dev;
    io->fpga_version = io_ver;
    io->gpio_read = io_gr;
    io->gpio_write = io_gw;
    io->wb_read = io_wr;
    io->wb_write = io_ww;
}
#endif
