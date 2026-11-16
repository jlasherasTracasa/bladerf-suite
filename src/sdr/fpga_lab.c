/*
 * fpga_lab.c — Cliente de la imagen de FPGA "lab". Ver fpga_lab.h y docs/FIRMWARE.md.
 */
#include "fpga_lab.h"

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
    return 1;
}

int fpga_lab_reg_read(const fpga_lab_t *lab, int index, uint32_t *val)
{
    if (!lab || !lab->present || index < 0 || index > 15) return -1;
    return lab->io.wb_read(lab->io.ctx, 4u * (uint32_t)index, val);
}

int fpga_lab_reg_write(const fpga_lab_t *lab, int index, uint32_t val)
{
    if (!lab || !lab->present || index < 0 || index > 15 || !lab->io.wb_write) return -1;
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
    return lab->io.gpio_write(lab->io.ctx, g);
}

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

double fpga_lab_window_sum(int n)
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
    for (int i = 0; i < f->n; i++)
        db[i] = (float)(10.0 * log10(fpga_lab_power_value(f->power[i]) + 1e-30) + off);
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
