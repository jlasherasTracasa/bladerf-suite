/*
 * source.c — Implementación de las fuentes de muestras. Ver source.h.
 */
#include <stdatomic.h>
#include "source.h"

#include "../util/json.h"

#include <libbladeRF.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef enum { FMT_CS16_Q11, FMT_CS16_FULL, FMT_CS8, FMT_CU8 } file_fmt_t;

/* Perfiles de sintonía rápida: el 2.0 micro admite 256 por sesión (NUM_BBP_FASTLOCK_PROFILES). */
#define QT_MAX 256

typedef struct {
    int64_t freq;
    struct bladerf_quick_tune qt;
} qt_entry_t;

/* Nombres de los modos de ganancia (ver source.h). El primero de cada fila es el canónico. */
static const struct {
    const char *name, *alias;
    gain_mode_t mode;
} GAIN_NAMES[] = {
    {"auto", "agc", GAIN_AUTO},         {"manual", "mgc", GAIN_MANUAL},
    {"fast", "agc_fast", GAIN_FAST},    {"slow", "agc_slow", GAIN_SLOW},
    {"hybrid", "agc_hybrid", GAIN_HYBRID},
};

const char *gain_mode_name(gain_mode_t m)
{
    for (size_t i = 0; i < ARRAY_LEN(GAIN_NAMES); i++)
        if (GAIN_NAMES[i].mode == m)
            return GAIN_NAMES[i].name;
    return "auto";
}

bool gain_mode_parse(const char *str, gain_mode_t *m)
{
    for (size_t i = 0; i < ARRAY_LEN(GAIN_NAMES); i++)
        if (strcasecmp(str, GAIN_NAMES[i].name) == 0 || strcasecmp(str, GAIN_NAMES[i].alias) == 0) {
            *m = GAIN_NAMES[i].mode;
            return true;
        }
    return false;
}

/* Copia con truncado explícito (evita -Wformat-truncation de snprintf("%s")). */
static void scopy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

struct source {
    source_info_t info;
    char err[256];
    int64_t freq;
    uint32_t rate;
    bool streaming;
    /* bladeRF */
    struct bladerf *dev;
    bool tx_on;
    rxfmt_t fmt_req;      /* formato pedido (RXFMT_AUTO por defecto) */
    rxfmt_t fmt_eff;      /* formato concreto sobre el USB: SC16, PACKED12 o SC8G (nunca AUTO) */
    fpga_lab_t lab;       /* imagen lab detectada al abrir (lab.present) */
    bool need_meta;       /* el barrido exige metadatos: fuerza sc16 */
    int g8_cfg;           /* desplazamiento GAIN8 pedido: 0..3, o -1 = automático */
    _Atomic unsigned g8_shift;  /* desplazamiento en uso (lo lee el hilo lector al convertir) */
    int g8_hw;            /* último valor escrito en la FPGA (-1 = desconocido) */
    _Atomic unsigned g8_peak;   /* pico |SC8| desde la última sondeo */
    _Atomic uint64_t rx_count; /* muestras entregadas desde source_start (marca de tiempo sintética) */
    uint64_t sat8_n0;     /* rx_count en el último sondeo de CNT_SAT8 */
    uint32_t sat8_c0;
    int g8_calm;          /* sondeos seguidos que piden subir (histéresis) */
    bool oversample;      /* feature OVERSAMPLE activa: SC8_Q7 a > 61,44 Msps */
    bool low_latency;     /* búferes pequeños (barrido) */
    bool spec_stream;     /* flujo de espectro de la FPGA lab: poco caudal, búferes pequeños */
    unsigned rx_timeout_ms; /* tiempo de espera de sync_rx (0 = SYNC_TIMEOUT_MS) */
    bool fast;            /* fichero: sin ritmo real ni bucle, acaba con SOURCE_EOF (--replay-fast) */
    qt_entry_t qt[QT_MAX];
    int qt_n;
    /* fichero */
    FILE *fp;
    file_fmt_t fmt;
    double t0;            /* instante de inicio de la reproducción */
    uint64_t delivered;   /* muestras entregadas desde t0 */
    uint8_t *raw;
    size_t raw_cap;
};

#define SETERR(s, ...) snprintf((s)->err, sizeof (s)->err, __VA_ARGS__)

const source_info_t *source_info(const source_t *s) { return &s->info; }
const char *source_error(const source_t *s) { return s->err; }
int64_t source_frequency(const source_t *s) { return s->freq; }
uint32_t source_rate(const source_t *s) { return s->rate; }
bool source_oversample(const source_t *s) { return s->oversample; }
void source_set_low_latency(source_t *s, bool on) { s->low_latency = on; }

/* Flujo del motor de espectro de la FPGA lab: ~0,1..0,5 Mpalabras/s en vez de decenas de Msps.
 * Búferes de 2048 palabras (= el múltiplo al que la FPGA rellena cada trama, así que cada trama
 * llena transferencias enteras y el FX3 no retiene paquetes) y poco colchón; el tiempo de espera
 * se ajusta al periodo de una imagen (4 periodos, entre 1 y 10 s) para no esperar 3,5 s por un
 * timeout ni cortar una cascada lenta. Se aplica en el siguiente source_start(). */
void source_set_spectrum_stream(source_t *s, bool on, double frame_period_s)
{
    s->spec_stream = on;
    double ms = on ? 4000.0 * frame_period_s : 0.0;
    s->rx_timeout_ms = on ? (unsigned)(ms < 1000.0 ? 1000.0 : ms > 10000.0 ? 10000.0 : ms) : 0;
}
void source_set_fast(source_t *s, bool on) { s->fast = on && !s->dev; }

const char *source_format(const source_t *s)
{
    if (s->dev) {
        if (s->fmt_eff == RXFMT_SC8G)
            return "sc8g";
        if (s->oversample)
            return "sc8q7";
        return s->fmt_eff == RXFMT_PACKED12 ? "packed12" : "sc16q11";
    }
    switch (s->fmt) {
    case FMT_CS8: return "cs8";
    case FMT_CU8: return "cu8";
    default: return "cs16";
    }
}

/* ======================================================================= */
/*  bladeRF                                                                */
/* ======================================================================= */
#define RX0 BLADERF_CHANNEL_RX(0)
#define TX0 BLADERF_CHANNEL_TX(0)

/* Las funciones de calibración de ganancia existen desde libbladeRF 2.6. En este
 * equipo hay dos copias (/usr/lib, anterior, y /usr/local/lib, 2.6.1) y el enlazador
 * puede elegir la vieja: con referencias débiles el programa enlaza igual y la
 * calibración se desactiva sola si la biblioteca cargada en ejecución no las tiene. */
extern int bladerf_load_gain_calibration(struct bladerf *, bladerf_channel, const char *) __attribute__((weak));
extern int bladerf_enable_gain_calibration(struct bladerf *, bladerf_channel, bool) __attribute__((weak));
extern int bladerf_get_gain_target(struct bladerf *, bladerf_channel, int *) __attribute__((weak));

/* Parámetros del flujo síncrono: 32 búferes de 16 Ki muestras con 16
 * transferencias USB en vuelo. A 61 Msps eso son ~8 ms por búfer y ~270 ms
 * de colchón total, suficiente para absorber pausas del sistema. */
#define SYNC_BUFFERS 32
#define SYNC_BUFLEN 16384
#define SYNC_XFERS 16
#define SYNC_TIMEOUT_MS 3500

static int64_t range_min(const struct bladerf_range *r) { return (int64_t)(r->min * r->scale); }
static int64_t range_max(const struct bladerf_range *r) { return (int64_t)(r->max * r->scale); }

/* Tabla de calibración de ganancia de libbladeRF (<serie>_rx_gain_cal.tbl/.csv en
 * ~/.config/Nuand/bladeRF, /etc/Nuand/bladeRF, etc.). Si existe se activa: así
 * set_gain compensa la respuesta de cada unidad y dBm = dBFS − gain_target.
 * Se silencia el registro porque, sin fichero, libbladeRF lo cuenta como error. */
static void load_gain_cal(source_t *s)
{
    if (!bladerf_load_gain_calibration || !bladerf_enable_gain_calibration || !bladerf_get_gain_target)
        return;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_SILENT);
    int st = bladerf_load_gain_calibration(s->dev, RX0, NULL);
    if (st == 0)
        st = bladerf_enable_gain_calibration(s->dev, RX0, true);
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);
    s->info.gain_cal = st == 0;
    if (st == 0)
        LOG_I("Calibración de ganancia del bladeRF cargada: se informará de dbm_offset");
}

source_t *source_open_bladerf(const char *serial, char *err, size_t errlen)
{
    source_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_WARNING);

    char devstr[96] = "";
    if (serial && *serial)
        snprintf(devstr, sizeof devstr, "*:serial=%s", serial);
    int st = bladerf_open(&s->dev, devstr[0] ? devstr : NULL);
    if (st < 0) {
        const char *hint = "";
        if (st == BLADERF_ERR_NODEV)
            hint = " — ¿está conectado? Comprueba con 'bladeRF-cli -p'";
        else if (st == BLADERF_ERR_IO || st == -10 /* BLADERF_ERR_PERMISSION */)
            hint = " — puede que otro programa (p. ej. OpenWebRX) lo esté usando, o faltan permisos udev";
        snprintf(err, errlen, "no se pudo abrir el bladeRF: %s%s", bladerf_strerror(st), hint);
        free(s);
        return NULL;
    }

    source_info_t *in = &s->info;
    scopy(in->kind, sizeof in->kind, "bladerf");
    scopy(in->board, sizeof in->board, bladerf_get_board_name(s->dev));
    char sn[BLADERF_SERIAL_LENGTH] = "";
    bladerf_get_serial(s->dev, sn);
    scopy(in->serial, sizeof in->serial, sn);
    struct bladerf_version v;
    if (bladerf_fw_version(s->dev, &v) == 0)
        snprintf(in->fw_version, sizeof in->fw_version, "%u.%u.%u", v.major, v.minor, v.patch);
    if (bladerf_fpga_version(s->dev, &v) == 0)
        snprintf(in->fpga_version, sizeof in->fpga_version, "%u.%u.%u", v.major, v.minor, v.patch);
    bladerf_version(&v);
    snprintf(in->lib_version, sizeof in->lib_version, "%u.%u.%u", v.major, v.minor, v.patch);

    const struct bladerf_range *r;
    if (bladerf_get_frequency_range(s->dev, RX0, &r) == 0) {
        in->freq_min = range_min(r);
        in->freq_max = range_max(r);
    }
    if (bladerf_get_sample_rate_range(s->dev, RX0, &r) == 0) {
        in->rate_min = (uint32_t)range_min(r);
        in->rate_max = (uint32_t)range_max(r);
    }
    if (bladerf_get_bandwidth_range(s->dev, RX0, &r) == 0) {
        in->bw_min = (uint32_t)range_min(r);
        in->bw_max = (uint32_t)range_max(r);
    }
    if (bladerf_get_gain_range(s->dev, RX0, &r) == 0) {
        in->gain_min = (int)range_min(r);
        in->gain_max = (int)range_max(r);
    }
    /* Oversample: solo bladeRF 2.0 con FPGA >= 0.15 (Nuand 2023.02). Se descubre
     * de verdad al activarlo; si falla, source_set_rate() lo comunica. */
    if (strcmp(bladerf_get_board_name(s->dev), "bladerf2") == 0) {
        unsigned fv = 0;
        if (bladerf_fpga_version(s->dev, &v) == 0)
            fv = v.major * 10000 + v.minor * 100 + v.patch;
        if (fv >= 1500)
            in->rate_max_oversample = SOURCE_RATE_OVERSAMPLE_MAX;
        load_gain_cal(s);
        /* SC16_Q11_PACKED existe en la imagen de serie desde la 0.16 (fifo_writer: pack_sc12q11) */
        in->fmt_packed12 = fv >= 1600;
    }
    switch (bladerf_device_speed(s->dev)) {
    case BLADERF_DEVICE_SPEED_HIGH: in->usb_speed = 2; break;
    case BLADERF_DEVICE_SPEED_SUPER: in->usb_speed = 3; break;
    default: in->usb_speed = 0; break;
    }
    /* Imagen lab >= 1.3 con GAIN8: sc8g. La detección solo lee la versión y config_gpio (segura en la de serie). */
    {
        fpga_lab_io_t io;
        char perr[160];
        if (source_fpga_lab_io(s, &io) == 0 && fpga_lab_probe(&io, &s->lab, perr, sizeof perr) > 0)
            in->fmt_sc8g = fpga_lab_has_gain8(&s->lab);
    }
    s->fmt_req = RXFMT_AUTO;
    s->fmt_eff = RXFMT_SC16;
    s->g8_cfg = -1;
    s->g8_hw = -1;
    in->can_tune = true;
    bool bt;
    in->has_biastee = bladerf_get_bias_tee(s->dev, RX0, &bt) == 0;
    in->can_tx = true;
    return s;
}

/* ======================================================================= */
/*  Formato de RX por el USB (F11c)                                        */
/* ======================================================================= */
/* Teoría: el enlace USB limita las muestras/s según los bytes por muestra: SC16 = 4 B, packed12 = 3 B (12 bit
 * por componente, 8 componentes en 3 palabras de 32 bit), SC8 = 2 B. En USB 2 Hi-Speed el caudal útil medido es
 * ≈ 45 MB/s: 11,25 Msps en SC16, 15 Msps en packed12 y 22,5 Msps en SC8. */
const char *rxfmt_name(rxfmt_t f)
{
    switch (f) {
    case RXFMT_SC16: return "sc16";
    case RXFMT_PACKED12: return "packed12";
    case RXFMT_SC8G: return "sc8g";
    default: return "auto";
    }
}

bool rxfmt_parse(const char *s, rxfmt_t *f)
{
    static const rxfmt_t all[] = {RXFMT_AUTO, RXFMT_SC16, RXFMT_PACKED12, RXFMT_SC8G};
    for (size_t i = 0; s && i < sizeof all / sizeof all[0]; i++)
        if (strcasecmp(s, rxfmt_name(all[i])) == 0) {
            *f = all[i];
            return true;
        }
    return false;
}

double source_usb_capacity(int usb_speed) { return usb_speed == 2 ? 45e6 : 360e6; }

static int rxfmt_bytes(rxfmt_t f) { return f == RXFMT_SC16 ? 4 : f == RXFMT_PACKED12 ? 3 : 2; }

rxfmt_t source_choose_format(rxfmt_t req, uint32_t rate, int usb_speed, bool packed_ok, bool sc8g_ok)
{
    const bool ok[4] = {false, true, packed_ok, sc8g_ok};
    if (req != RXFMT_AUTO) {
        if (ok[req])
            return req;
        return packed_ok ? RXFMT_PACKED12 : RXFMT_SC16; /* explícito pero no disponible: el más próximo */
    }
    const double cap = source_usb_capacity(usb_speed);
    rxfmt_t best = RXFMT_SC16;
    for (int f = RXFMT_SC16; f <= RXFMT_SC8G; f++) {
        if (!ok[f])
            continue;
        best = (rxfmt_t)f; /* acaba en el de menos bytes disponible si ninguno cabe */
        if ((double)rate * rxfmt_bytes((rxfmt_t)f) <= cap)
            break;
    }
    return best;
}

unsigned sc8g_auto_shift(unsigned cur, double sat_pct, int peak)
{
    if (cur > 3)
        cur = 3;
    if (sat_pct > 0.01)
        return cur > 0 ? cur - 1 : 0;
    if (cur < 3 && peak > 0 && peak * 2 < 112)
        return cur + 1;
    return cur;
}

int source_sc8g_to_q11(const int8_t *in, int16_t *out, size_t n_values, unsigned shift)
{
    const int32_t rnd = shift ? (1 << (shift - 1)) : 0;
    int pk = 0;
    for (size_t i = 0; i < n_values; i++) {
        const int32_t b = in[i];
        out[i] = (int16_t)((b * 16 + 8 + rnd) >> shift);
        const int a = b < 0 ? -b : b;
        pk = a > pk ? a : pk;
    }
    return pk;
}

static inline int16_t sext12(uint32_t v) { return (int16_t)((int16_t)(v << 4) >> 4); }

void source_packed12_to_q11(const uint8_t *in, int16_t *out, size_t nsamples)
{
    for (size_t n = 0; n + 4 <= nsamples; n += 4, in += 12, out += 8) {
        uint32_t w[3];
        for (int k = 0; k < 3; k++)
            w[k] = (uint32_t)in[4 * k] | (uint32_t)in[4 * k + 1] << 8 | (uint32_t)in[4 * k + 2] << 16 |
                   (uint32_t)in[4 * k + 3] << 24;
        out[0] = sext12(w[0]);
        out[1] = sext12(w[0] >> 12);
        out[2] = sext12((w[0] >> 24) | (w[1] << 8));
        out[3] = sext12(w[1] >> 4);
        out[4] = sext12(w[1] >> 16);
        out[5] = sext12((w[1] >> 28) | (w[2] << 4));
        out[6] = sext12(w[2] >> 8);
        out[7] = sext12(w[2] >> 20);
    }
}

rxfmt_t source_format_request(const source_t *s) { return s->fmt_req; }
rxfmt_t source_format_effective(const source_t *s) { return s->dev ? s->fmt_eff : RXFMT_SC16; }
int source_sc8g_shift(const source_t *s) { return (int)atomic_load(&s->g8_shift); }

bool source_has_timestamps(const source_t *s)
{
    if (!s->dev)
        return true;
    return s->oversample ? s->fmt_eff != RXFMT_SC8G : s->fmt_eff == RXFMT_SC16;
}

/* Fija fmt_eff según el formato pedido, la tasa y el enlace. En oversample (> 61,44 Msps) libbladeRF ya usa
 * SC8: ahí fmt_eff solo es SC8G si se pidió y la FPGA lo ofrece; si no, SC16 significa «SC8 de serie». */
static void resolve_format(source_t *s)
{
    rxfmt_t e;
    if (s->need_meta && !s->oversample)
        e = RXFMT_SC16;
    else if (s->oversample)
        e = (s->fmt_req == RXFMT_SC8G && s->info.fmt_sc8g) ? RXFMT_SC8G : RXFMT_SC16;
    else
        e = source_choose_format(s->fmt_req, s->rate, s->info.usb_speed, s->info.fmt_packed12, s->info.fmt_sc8g);
    if (e != s->fmt_eff)
        LOG_I("Formato de RX: %s (pedido %s, %.2f Msps, USB %d)", rxfmt_name(e), rxfmt_name(s->fmt_req),
              s->rate / 1e6, s->info.usb_speed);
    s->fmt_eff = e;
}

/* Escribe GAIN8 si cambió (y si la FPGA lo tiene). */
static void sync_gain8(source_t *s, int shift)
{
    if (!fpga_lab_has_gain8(&s->lab) || s->g8_hw == shift)
        return;
    if (fpga_lab_gain8_set(&s->lab, (unsigned)shift) == 0)
        s->g8_hw = shift;
}

static int bladerf_stream_config(source_t *s)
{
    /* Formato: Q11 de 16 bit normalmente; en oversample libbladeRF solo admite 8 bit.
     * Con "META" cada lectura devuelve la marca de tiempo (necesaria para el barrido). */
    bladerf_format fmt = s->oversample ? BLADERF_FORMAT_SC8_Q7_META : BLADERF_FORMAT_SC16_Q11_META;
    /* sc8g y packed12 no admiten metadatos (packed12 por limitación de libbladeRF; sc8g porque la ganancia y
     * el desempaquetado se hacen aquí): sin marca de tiempo del FPGA ni aviso de overrun. */
    if (s->fmt_eff == RXFMT_SC8G)
        fmt = BLADERF_FORMAT_SC8_Q7;
    else if (s->fmt_eff == RXFMT_PACKED12 && !s->oversample)
        fmt = BLADERF_FORMAT_SC16_Q11_PACKED;
    unsigned nbuf = SYNC_BUFFERS, len = SYNC_BUFLEN, xfers = SYNC_XFERS;
    unsigned tmo = s->rx_timeout_ms ? s->rx_timeout_ms : SYNC_TIMEOUT_MS;
    if (s->spec_stream) {
        nbuf = 16;
        len = 2048;
        xfers = 4;
    } else if (s->low_latency) {
        /* Barrido: 4096 muestras por transferencia (~0,1 ms a 40 Msps) para que el
         * retraso lector↔FPGA sea mínimo y los retunes programados lleguen a tiempo. */
        nbuf = 64;
        len = 4096;
    } else if (s->rate > 30720000u) {
        /* > 30 Msps: transferencias de 64 Ki muestras (128 KiB en SC8, 256 KiB en SC16)
         * y ~4 M muestras de colchón (34 ms a 122,88 Msps) frente a pausas del sistema. */
        nbuf = 64;
        len = 65536;
        xfers = 32;
    }
    /* GAIN8 se escribe ANTES de habilitar RX y solo actúa en modo de 8 bit: fuera de sc8g se deja a 0 para que
     * el oversample de serie (SC8 puro) no se vea afectado por un valor residual. */
    sync_gain8(s, s->fmt_eff == RXFMT_SC8G ? (int)atomic_load(&s->g8_shift) : 0);
    int st = bladerf_sync_config(s->dev, BLADERF_RX_X1, fmt, nbuf, len, xfers, tmo);
    if (st < 0 && fmt == BLADERF_FORMAT_SC16_Q11_PACKED) {
        /* Biblioteca antigua sin SC16_Q11_PACKED: se desactiva y se vuelve a SC16 */
        LOG_W("packed12 no disponible en esta libbladeRF (%s): vuelvo a sc16", bladerf_strerror(st));
        s->info.fmt_packed12 = false;
        s->fmt_eff = RXFMT_SC16;
        return bladerf_stream_config(s);
    }
    if (st < 0) {
        SETERR(s, "sync_config: %s", bladerf_strerror(st));
        return st;
    }
    s->rx_count = 0;
    s->sat8_n0 = 0;
    s->sat8_c0 = 0;
    if (s->fmt_eff == RXFMT_SC8G)
        fpga_lab_read_sat8(&s->lab, &s->sat8_c0);
    st = bladerf_enable_module(s->dev, RX0, true);
    if (st < 0)
        SETERR(s, "enable_module: %s", bladerf_strerror(st));
    return st;
}

/* ======================================================================= */
/*  Fichero                                                                */
/* ======================================================================= */
static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

/* Lee los metadatos SigMF mínimos: tipo de dato, tasa y frecuencia. */
static int read_sigmf_meta(const char *meta_path, file_fmt_t *fmt, uint32_t *rate, int64_t *freq, char *err,
                           size_t errlen)
{
    FILE *f = fopen(meta_path, "rb");
    if (!f) {
        snprintf(err, errlen, "no se puede abrir %s: %s", meta_path, strerror(errno));
        return -1;
    }
    char *buf = malloc(1 << 20);
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = '\0';
    jtok_t *t = malloc(sizeof(jtok_t) * 4096);
    int nt = json_parse(buf, n, t, 4096);
    int rc = -1;
    if (nt < 0) {
        snprintf(err, errlen, "%s no es JSON válido", meta_path);
        goto out;
    }
    int g = json_obj_get(buf, t, 0, "global");
    char dt[32] = "";
    json_str(buf, t, json_obj_get(buf, t, g, "core:datatype"), dt, sizeof dt);
    double v;
    /* Q3 (fuzz): un meta corrupto puede traer tasa negativa, NaN o 1e300; convertirlos a
     * entero sin acotar es comportamiento indefinido. Solo se acepta lo representable. */
    if (json_num(buf, t, json_obj_get(buf, t, g, "core:sample_rate"), &v) && v >= 1 && v <= 4294967295.0)
        *rate = (uint32_t)v;
    int caps = json_obj_get(buf, t, 0, "captures");
    int c0 = json_arr_get(t, caps, 0);
    if (json_num(buf, t, json_obj_get(buf, t, c0, "core:frequency"), &v) && fabs(v) <= 1e13)
        *freq = (int64_t)v;
    /* Nuestra app guarda la escala nativa Q11 en un campo propio */
    double scale = 0;
    json_num(buf, t, json_obj_get(buf, t, g, "bladerf_lab:full_scale"), &scale);
    if (strcmp(dt, "ci16_le") == 0 || strcmp(dt, "ci16") == 0)
        *fmt = (scale > 0 && scale < 4096) ? FMT_CS16_Q11 : FMT_CS16_FULL;
    else if (strcmp(dt, "ci8") == 0)
        *fmt = FMT_CS8;
    else if (strcmp(dt, "cu8") == 0)
        *fmt = FMT_CU8;
    else {
        snprintf(err, errlen, "tipo de dato SigMF no soportado: '%s' (se admiten ci16_le, ci8, cu8)", dt);
        goto out;
    }
    rc = 0;
out:
    free(t);
    free(buf);
    return rc;
}

source_t *source_open_file(const char *path, uint32_t rate, int64_t center_hz, char *err, size_t errlen)
{
    source_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    char data_path[1024];
    if (strlen(path) >= sizeof data_path - 16) { /* hueco para cambiar la extensión */
        snprintf(err, errlen, "ruta demasiado larga (máx. %zu caracteres)", sizeof data_path - 17);
        free(s);
        return NULL;
    }
    scopy(data_path, sizeof data_path, path);
    s->fmt = FMT_CS16_Q11;

    if (ends_with(path, ".sigmf-meta") || ends_with(path, ".sigmf-data")) {
        char meta[1024];
        size_t base = strlen(path) - strlen(".sigmf-meta");
        memcpy(meta, path, base);
        memcpy(meta + base, ".sigmf-meta", sizeof ".sigmf-meta");
        memcpy(data_path, path, base);
        memcpy(data_path + base, ".sigmf-data", sizeof ".sigmf-data");
        if (read_sigmf_meta(meta, &s->fmt, &rate, &center_hz, err, errlen) < 0) {
            free(s);
            return NULL;
        }
    } else if (ends_with(path, ".cu8")) {
        s->fmt = FMT_CU8;
    } else if (ends_with(path, ".cs8")) {
        s->fmt = FMT_CS8;
    }
    if (rate == 0) {
        snprintf(err, errlen, "falta la tasa de muestreo del fichero (usa --rate)");
        free(s);
        return NULL;
    }
    s->fp = fopen(data_path, "rb");
    if (!s->fp) {
        snprintf(err, errlen, "no se puede abrir %s: %s", data_path, strerror(errno));
        free(s);
        return NULL;
    }
    s->rate = rate;
    s->freq = center_hz;
    source_info_t *in = &s->info;
    scopy(in->kind, sizeof in->kind, "file");
    const char *base = strrchr(data_path, '/');
    scopy(in->board, sizeof in->board, "Fichero ");
    size_t pre = strlen(in->board);
    scopy(in->board + pre, sizeof in->board - pre, base ? base + 1 : data_path);
    in->freq_min = in->freq_max = center_hz;
    in->rate_min = in->rate_max = rate;
    in->can_tune = false;
    return s;
}

/* ======================================================================= */
/*  Operaciones comunes                                                    */
/* ======================================================================= */
int source_set_frequency(source_t *s, int64_t hz)
{
    if (!s->dev) {
        SETERR(s, "la reproducción de ficheros no permite cambiar la frecuencia central");
        return -1;
    }
    int st = bladerf_set_frequency(s->dev, RX0, (bladerf_frequency)hz);
    if (st < 0) {
        SETERR(s, "set_frequency(%lld): %s", (long long)hz, bladerf_strerror(st));
        return st;
    }
    s->freq = hz;
    return 0;
}

/* Re-resuelve el formato y, si hay flujo, lo reconfigura (llamar con el lector en pausa). */
static int reapply_format(source_t *s)
{
    const bool was = s->streaming;
    const rxfmt_t before = s->fmt_eff;
    resolve_format(s);
    if (!was || before == s->fmt_eff)
        return 0;
    bladerf_enable_module(s->dev, RX0, false);
    return bladerf_stream_config(s);
}

int source_require_timestamps(source_t *s, bool on)
{
    if (!s->dev)
        return 0;
    s->need_meta = on;
    return reapply_format(s);
}

int source_set_format(source_t *s, rxfmt_t req)
{
    if (!s->dev) {
        SETERR(s, "el formato de RX no aplica con un fichero");
        return -1;
    }
    if (req == RXFMT_PACKED12 && !s->info.fmt_packed12) {
        SETERR(s, "packed12 exige FPGA >= 0.16 y una libbladeRF con SC16_Q11_PACKED");
        return -1;
    }
    if (req == RXFMT_SC8G && !s->info.fmt_sc8g) {
        SETERR(s, "sc8g exige la imagen lab >= 1.3 (FPGA 0.16.103) cargada en la placa");
        return -1;
    }
    s->fmt_req = req;
    return reapply_format(s);
}

int source_set_sc8g_shift(source_t *s, int shift)
{
    if (shift < -1 || shift > 3) {
        SETERR(s, "desplazamiento GAIN8: use -1 (automático) o 0..3");
        return -1;
    }
    s->g8_cfg = shift;
    if (shift >= 0) {
        atomic_store(&s->g8_shift, (unsigned)shift);
        if (s->dev && s->fmt_eff == RXFMT_SC8G)
            sync_gain8(s, shift);
    }
    return 0;
}

/* Sondeo de CNT_SAT8 (+ ajuste automático). El cambio de GAIN8 y el de la conversión no son simultáneos: las
 * muestras que ya estaban en vuelo por el USB (hasta unas decenas de ms) se convierten con el valor nuevo y
 * salen mal escaladas una vez; se descarta el pico de ese intervalo. */
bool source_sc8g_poll(source_t *s, double *sat_pct, int *shift)
{
    if (!s->dev || s->fmt_eff != RXFMT_SC8G)
        return false;
    uint32_t c;
    if (fpga_lab_read_sat8(&s->lab, &c) < 0)
        return false;
    const uint64_t n = atomic_load(&s->rx_count);
    const uint64_t dn = n - s->sat8_n0;
    if (dn == 0)
        return false;
    const double pct = 100.0 * (double)(uint32_t)(c - s->sat8_c0) / (double)dn;
    s->sat8_n0 = n;
    s->sat8_c0 = c;
    const unsigned peak = atomic_exchange(&s->g8_peak, 0);
    unsigned cur = atomic_load(&s->g8_shift);
    if (s->g8_cfg < 0) {
        unsigned nx = sc8g_auto_shift(cur, pct, (int)peak);
        if (nx > cur) {
            if (++s->g8_calm < 2) /* histéresis: subir solo tras dos intervalos seguidos pidiéndolo */
                nx = cur;
            else
                s->g8_calm = 0;
        } else {
            s->g8_calm = 0;
        }
        if (nx != cur) {
            sync_gain8(s, (int)nx);
            atomic_store(&s->g8_shift, nx);
            atomic_store(&s->g8_peak, 0);
            cur = nx;
        }
    }
    if (sat_pct)
        *sat_pct = pct;
    if (shift)
        *shift = (int)cur;
    return true;
}

/* Activa/desactiva OVERSAMPLE. Debe hacerse con el flujo parado y seguirse de un
 * set_sample_rate (libbladeRF reescribe entonces los registros del AD9361). */
static int set_oversample(source_t *s, bool on)
{
    int st = bladerf_enable_feature(s->dev, BLADERF_FEATURE_OVERSAMPLE, on);
    if (st < 0)
        SETERR(s, "oversample %s: %s", on ? "on" : "off", bladerf_strerror(st));
    else
        s->oversample = on;
    return st;
}

int source_set_rate(source_t *s, uint32_t rate, uint32_t *actual)
{
    if (!s->dev) {
        if (actual)
            *actual = s->rate;
        return rate == s->rate ? 0 : -1;
    }
    bool want_os = rate > SOURCE_RATE_NATIVE_MAX;
    if (want_os && !s->info.rate_max_oversample) {
        SETERR(s, "tasa %u Hz: el modo oversample (hasta 122,88 Msps) no está disponible en este equipo", rate);
        return -1;
    }
    bool was = s->streaming;
    if (was)
        bladerf_enable_module(s->dev, RX0, false);
    bool was_os = s->oversample;
    int st = 0;
    if (want_os != was_os)
        st = set_oversample(s, want_os);
    if (st == 0) {
        bladerf_sample_rate act = 0;
        st = bladerf_set_sample_rate(s->dev, RX0, rate, &act);
        if (st < 0) {
            SETERR(s, "set_sample_rate(%u): %s", rate, bladerf_strerror(st));
            if (want_os != was_os)
                set_oversample(s, was_os); /* volver al modo anterior */
        } else {
            s->rate = act;
            if (actual)
                *actual = act;
        }
    }
    resolve_format(s); /* la tasa (o el modo oversample) puede cambiar el formato automático */
    if (was)
        bladerf_stream_config(s);
    return st;
}

int source_set_bandwidth(source_t *s, uint32_t bw, uint32_t *actual)
{
    if (!s->dev)
        return 0;
    if (s->oversample) {
        /* libbladeRF avisa de que el ancho de banda en oversample da "resultados
         * desconocidos": se deja el filtro analógico como está. */
        if (actual)
            *actual = s->info.bw_max;
        return 0;
    }
    bladerf_bandwidth act = 0;
    int st = bladerf_set_bandwidth(s->dev, RX0, bw, &act);
    if (st < 0)
        SETERR(s, "set_bandwidth(%u): %s", bw, bladerf_strerror(st));
    else if (actual)
        *actual = act;
    return st;
}

int source_set_gain(source_t *s, gain_mode_t mode, int gain_db)
{
    if (!s->dev)
        return 0;
    bladerf_gain_mode m = BLADERF_GAIN_DEFAULT;
    switch (mode) {
    case GAIN_AUTO: m = BLADERF_GAIN_DEFAULT; break;
    case GAIN_MANUAL: m = BLADERF_GAIN_MGC; break;
    case GAIN_FAST: m = BLADERF_GAIN_FASTATTACK_AGC; break;
    case GAIN_SLOW: m = BLADERF_GAIN_SLOWATTACK_AGC; break;
    case GAIN_HYBRID: m = BLADERF_GAIN_HYBRID_AGC; break;
    }
    int st = bladerf_set_gain_mode(s->dev, RX0, m);
    if (st < 0) {
        SETERR(s, "set_gain_mode(%s): %s", gain_mode_name(mode), bladerf_strerror(st));
        return st;
    }
    if (mode == GAIN_MANUAL) {
        st = bladerf_set_gain(s->dev, RX0, gain_db);
        if (st < 0)
            SETERR(s, "set_gain(%d): %s", gain_db, bladerf_strerror(st));
    }
    return st;
}

int source_dbm_offset(source_t *s, double *off_db)
{
    /* La tabla se midió con el flujo normal de 12 bit; en oversample (8 bit, otro
     * reloj del RFIC) no se garantiza, así que no se ofrece. */
    if (!s->dev || !s->info.gain_cal || s->oversample)
        return -1;
    int target = 0;
    if (bladerf_get_gain_target(s->dev, RX0, &target) < 0)
        return -1;
    *off_db = -(double)target;
    return 0;
}

int source_get_gain(source_t *s, int *gain_db)
{
    if (!s->dev)
        return -1;
    bladerf_gain g;
    int st = bladerf_get_gain(s->dev, RX0, &g);
    if (st == 0)
        *gain_db = g;
    return st;
}

int source_set_biastee(source_t *s, bool on)
{
    if (!s->dev || !s->info.has_biastee)
        return -1;
    int st = bladerf_set_bias_tee(s->dev, RX0, on);
    if (st < 0)
        SETERR(s, "set_bias_tee: %s", bladerf_strerror(st));
    return st;
}

int source_get_temperature(source_t *s, float *c)
{
    if (!s->dev)
        return -1;
    return bladerf_get_rfic_temperature(s->dev, c);
}

int source_start(source_t *s)
{
    if (s->dev) {
        int st = bladerf_stream_config(s);
        if (st < 0)
            return st;
    } else {
        s->t0 = now_monotonic();
        s->delivered = 0;
    }
    s->streaming = true;
    return 0;
}

/* Adaptador de fpga_lab a libbladeRF (aquí y no en fpga_lab.c para no obligar a las
 * pruebas de fpga_lab a enlazar libbladeRF). */
static int lab_ver(void *c, uint32_t *ma, uint32_t *mi, uint32_t *pa)
{
    struct bladerf_version v;
    int r = bladerf_fpga_version((struct bladerf *)c, &v);
    if (r < 0)
        return r;
    *ma = v.major;
    *mi = v.minor;
    *pa = v.patch;
    return 0;
}
static int lab_gr(void *c, uint32_t *v) { return bladerf_config_gpio_read((struct bladerf *)c, v); }
static int lab_gw(void *c, uint32_t v) { return bladerf_config_gpio_write((struct bladerf *)c, v); }
static int lab_wr(void *c, uint32_t a, uint32_t *v) { return bladerf_wishbone_master_read((struct bladerf *)c, a, v); }
static int lab_ww(void *c, uint32_t a, uint32_t v) { return bladerf_wishbone_master_write((struct bladerf *)c, a, v); }

int source_fpga_lab_io(source_t *s, fpga_lab_io_t *io)
{
    if (!s->dev)
        return -1;
    io->ctx = s->dev;
    io->fpga_version = lab_ver;
    io->gpio_read = lab_gr;
    io->gpio_write = lab_gw;
    io->wb_read = lab_wr;
    io->wb_write = lab_ww;
    return 0;
}

int source_stop(source_t *s)
{
    if (s->dev && s->streaming)
        bladerf_enable_module(s->dev, RX0, false);
    s->streaming = false;
    return 0;
}

static int file_read(source_t *s, int16_t *iq, unsigned n)
{
    size_t bps = (s->fmt == FMT_CS8 || s->fmt == FMT_CU8) ? 2 : 4; /* bytes por muestra compleja */
    size_t need = (size_t)n * bps;
    if (need > s->raw_cap) {
        s->raw = realloc(s->raw, need);
        s->raw_cap = need;
    }
    size_t got = 0, pass_start = 0;
    int empty_passes = 0; /* vueltas seguidas sin muestras completas: fichero vacío o < 1 muestra */
    while (got < need) {
        size_t r = fread(s->raw + got, 1, need - got, s->fp);
        got += r;
        if (got < need) {
            if (feof(s->fp)) {
                /* Q3 (fuzz): un fichero truncado puede acabar a mitad de una muestra; al
                 * volver al principio esos bytes sueltos desplazarían I/Q para siempre
                 * (I↔Q intercambiados). Se descarta la muestra incompleta del final. */
                got -= got % bps;
                if (s->fast) {
                    /* Una sola pasada: el último bloque se rellena con 'silencio' (cero de la
                     * fuente) y la lectura siguiente devuelve SOURCE_EOF. */
                    if (got == 0)
                        return SOURCE_EOF;
                    if (s->fmt == FMT_CU8)
                        memset(s->raw + got, 128, need - got);
                    else
                        memset(s->raw + got, 0, need - got);
                    got = need;
                    break;
                }
                empty_passes = got > pass_start ? 0 : empty_passes + 1;
                if (empty_passes < 2) {
                    pass_start = got;
                    rewind(s->fp); /* reproducción en bucle (aunque el bloque dé varias vueltas al fichero) */
                    continue;
                }
            }
            SETERR(s, "fichero vacío o ilegible");
            return -1;
        }
    }
    /* Conversión a Q11 */
    switch (s->fmt) {
    case FMT_CS16_Q11: memcpy(iq, s->raw, need); break;
    case FMT_CS16_FULL: {
        const int16_t *in = (const int16_t *)s->raw;
        for (unsigned i = 0; i < 2 * n; i++)
            iq[i] = (int16_t)(in[i] / 16);
        break;
    }
    case FMT_CS8: {
        const int8_t *in = (const int8_t *)s->raw;
        for (unsigned i = 0; i < 2 * n; i++)
            iq[i] = (int16_t)(in[i] * 16);
        break;
    }
    case FMT_CU8:
        for (unsigned i = 0; i < 2 * n; i++)
            iq[i] = (int16_t)(((int)s->raw[i] - 128) * 16);
        break;
    }
    /* Ritmo real: dormir hasta que "toque" entregar estas muestras */
    s->delivered += n;
    if (s->fast)
        return 0; /* sin ritmo real */
    double due = s->t0 + (double)s->delivered / s->rate;
    double wait = due - now_monotonic();
    if (wait > 0) {
        struct timespec ts = {(time_t)wait, (long)((wait - (time_t)wait) * 1e9)};
        nanosleep(&ts, NULL);
    } else if (wait < -1.0) {
        s->t0 = now_monotonic(); /* nos quedamos atrás: reiniciar referencia */
        s->delivered = 0;
    }
    return 0;
}

/* SC8_Q7 → Q11: Q7 va de −128 a 127 (±1,0 = 128) y Q11 de −2048 a 2047 (±1,0 = 2048),
 * así que basta multiplicar por 16. Se pierden 4 bits de resolución (suelo de
 * cuantización a ≈ −48 dBFS por muestra, ver docs/hardware/HARDWARE.md), no de escala:
 * los dBFS del espectro siguen siendo comparables con los del modo normal.
 * Elegimos convertir a Q11 (y no llevar int8 por todo el programa) porque
 * decodificadores, grabador y espectro CPU/GPU están escritos para int16; la
 * conversión es un bucle trivial que GCC vectoriza (~0,1 ns/muestra). */
void source_sc8_to_q11(const int8_t *in, int16_t *out, size_t n_values)
{
    for (size_t i = 0; i < n_values; i++)
        out[i] = (int16_t)(in[i] * 16);
}

static int bladerf_read(source_t *s, int16_t *iq, unsigned n, uint64_t *ts, unsigned *overruns)
{
    unsigned got = 0;
    bool first = true;
    const bool raw8 = s->oversample || s->fmt_eff == RXFMT_SC8G; /* llega SC8 y se convierte aquí */
    const bool meta_on = s->oversample ? s->fmt_eff != RXFMT_SC8G : s->fmt_eff == RXFMT_SC16;
    if (raw8 && (size_t)n * 2 > s->raw_cap) {
        s->raw = realloc(s->raw, (size_t)n * 2);
        if (!s->raw) {
            s->raw_cap = 0;
            SETERR(s, "sin memoria para el búfer SC8");
            return -1;
        }
        s->raw_cap = (size_t)n * 2;
    }
    while (got < n) {
        struct bladerf_metadata meta;
        memset(&meta, 0, sizeof meta);
        meta.flags = UINT32_C(1) << 31; /* = BLADERF_META_FLAG_RX_NOW; la macro (1 << 31) es UB con int */
        /* En oversample se lee SC8 al búfer auxiliar y se convierte; si no, se
         * lee directamente a iq. (void*: la API es la misma para ambos formatos.) */
        void *dst = raw8 ? (void *)(s->raw + 2 * (size_t)got) : (void *)(iq + 2 * (size_t)got);
        int st = bladerf_sync_rx(s->dev, dst, n - got, &meta, s->rx_timeout_ms ? s->rx_timeout_ms : SYNC_TIMEOUT_MS);
        if (st < 0) {
            SETERR(s, "sync_rx: %s", bladerf_strerror(st));
            return st;
        }
        if (meta_on && (meta.status & BLADERF_META_STATUS_OVERRUN) && overruns)
            (*overruns)++;
        if (first && ts)
            *ts = meta_on ? meta.timestamp : s->rx_count; /* sin metadatos: cuenta propia */
        first = false;
        got += meta.actual_count ? meta.actual_count : (n - got);
    }
    s->rx_count += n;
    if (s->fmt_eff == RXFMT_SC8G) {
        const unsigned sh = atomic_load(&s->g8_shift);
        const unsigned pk = (unsigned)source_sc8g_to_q11((const int8_t *)s->raw, iq, 2 * (size_t)n, sh);
        if (pk > atomic_load(&s->g8_peak))
            atomic_store(&s->g8_peak, pk);
    } else if (s->oversample) {
        source_sc8_to_q11((const int8_t *)s->raw, iq, 2 * (size_t)n);
    }
    return 0;
}

int source_read_ts(source_t *s, int16_t *iq, unsigned n, uint64_t *ts, unsigned *overruns)
{
    if (!s->dev) {
        int r = file_read(s, iq, n);
        if (ts)
            *ts = s->delivered - n;
        return r;
    }
    return bladerf_read(s, iq, n, ts, overruns);
}

int source_read(source_t *s, int16_t *iq, unsigned n, unsigned *overruns)
{
    return source_read_ts(s, iq, n, NULL, overruns);
}

/* ======================================================================= */
/*  Quick-tune y retune programado                                         */
/* ======================================================================= */
int source_qt_free(const source_t *s) { return s->dev ? QT_MAX - s->qt_n : 0; }

int source_qt_prepare(source_t *s, int64_t freq_hz)
{
    if (!s->dev)
        return -1;
    for (int i = 0; i < s->qt_n; i++)
        if (s->qt[i].freq == freq_hz)
            return i; /* ya hay perfil para esta frecuencia */
    if (s->qt_n >= QT_MAX) {
        SETERR(s, "agotados los %d perfiles de sintonía rápida de esta sesión", QT_MAX);
        return -1;
    }
    int st = bladerf_set_frequency(s->dev, RX0, (bladerf_frequency)freq_hz);
    if (st == 0)
        st = bladerf_get_quick_tune(s->dev, RX0, &s->qt[s->qt_n].qt);
    if (st < 0) {
        SETERR(s, "quick_tune(%lld): %s", (long long)freq_hz, bladerf_strerror(st));
        return -1;
    }
    s->freq = freq_hz;
    s->qt[s->qt_n].freq = freq_hz;
    return s->qt_n++;
}

int source_retune_at(source_t *s, uint64_t ts, int qt_id)
{
    if (!s->dev || qt_id < 0 || qt_id >= s->qt_n)
        return -1;
    int st = bladerf_schedule_retune(s->dev, RX0, ts, (bladerf_frequency)s->qt[qt_id].freq, &s->qt[qt_id].qt);
    if (st == BLADERF_ERR_QUEUE_FULL)
        return SOURCE_QUEUE_FULL;
    if (st < 0) {
        SETERR(s, "schedule_retune: %s", bladerf_strerror(st));
        return st;
    }
    s->freq = s->qt[qt_id].freq;
    return 0;
}

int source_retune_cancel(source_t *s)
{
    if (!s->dev)
        return -1;
    return bladerf_cancel_scheduled_retunes(s->dev, RX0);
}

int source_timestamp(source_t *s, uint64_t *ts)
{
    if (!s->dev)
        return -1;
    int st = bladerf_get_timestamp(s->dev, BLADERF_RX, ts);
    if (st < 0)
        SETERR(s, "get_timestamp: %s", bladerf_strerror(st));
    return st;
}

/* ======================================================================= */
/*  Transmisión                                                            */
/* ======================================================================= */
int source_tx_configure(source_t *s, int64_t freq_hz, uint32_t rate, int gain_db, bool rfic_loopback)
{
    if (!s->dev) {
        SETERR(s, "la transmisión requiere el bladeRF");
        return -1;
    }
    if (s->oversample) {
        SETERR(s, "la transmisión no está disponible mientras el modo oversample (> 61,44 Msps) esté activo");
        return -1;
    }
    if (s->fmt_eff != RXFMT_SC16) {
        /* sync_config de TX reescribe config_gpio (bits de 8 bit y empaquetado) y rompería el flujo de RX */
        SETERR(s, "la transmisión no está disponible con el formato de RX %s: use format=sc16", rxfmt_name(s->fmt_eff));
        return -1;
    }
    int st = bladerf_set_loopback(s->dev, rfic_loopback ? BLADERF_LB_RFIC_BIST : BLADERF_LB_NONE);
    if (st < 0) {
        SETERR(s, "set_loopback: %s", bladerf_strerror(st));
        return st;
    }
    if ((st = bladerf_set_frequency(s->dev, TX0, (bladerf_frequency)freq_hz)) < 0 ||
        (st = bladerf_set_sample_rate(s->dev, TX0, rate, NULL)) < 0 ||
        (st = bladerf_set_bandwidth(s->dev, TX0, MAX(rate * 4 / 5, 200000u), NULL)) < 0 ||
        (st = bladerf_set_gain(s->dev, TX0, gain_db)) < 0) {
        SETERR(s, "configuración de TX: %s", bladerf_strerror(st));
        return st;
    }
    st = bladerf_sync_config(s->dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11, 16, 8192, 8, SYNC_TIMEOUT_MS);
    if (st < 0) {
        SETERR(s, "sync_config TX: %s", bladerf_strerror(st));
        return st;
    }
    st = bladerf_enable_module(s->dev, TX0, true);
    if (st < 0)
        SETERR(s, "enable TX: %s", bladerf_strerror(st));
    s->tx_on = st == 0;
    return st;
}

int source_tx_write(source_t *s, const int16_t *iq, unsigned n)
{
    if (!s->dev || !s->tx_on)
        return -1;
    int st = bladerf_sync_tx(s->dev, (void *)iq, n, NULL, SYNC_TIMEOUT_MS);
    if (st < 0)
        SETERR(s, "sync_tx: %s", bladerf_strerror(st));
    return st;
}

int source_tx_stop(source_t *s)
{
    if (!s->dev)
        return -1;
    if (s->tx_on)
        bladerf_enable_module(s->dev, TX0, false);
    s->tx_on = false;
    bladerf_set_loopback(s->dev, BLADERF_LB_NONE);
    return 0;
}

void source_close(source_t *s)
{
    if (!s)
        return;
    if (s->dev) {
        source_tx_stop(s);
        source_stop(s);
        bladerf_close(s->dev);
    }
    if (s->fp)
        fclose(s->fp);
    free(s->raw);
    free(s);
}
