/*
 * dab.c — Decodificador DAB / DAB+ (modo I): sincronía OFDM y FIC.
 *
 * Alcance: obtiene del FIC el conjunto (EId, etiqueta) y la lista de servicios
 * (SId, etiqueta, subcanal, tasa, protección, tipo). No decodifica el MSC (audio).
 * Constantes y tablas: ver dab.h.
 *
 * Cadena (todo por trama de 96 ms, WIDE a 2,048 MS/s exactos):
 *
 *  1. Adquisición. Se acumulan ≥ 2 tramas y se busca el SÍMBOLO NULO: la suma
 *     móvil de |x|² en una ventana de 2304 muestras (< 2656) es mínima donde la
 *     ventana cae entera dentro del nulo. Se acepta si ese mínimo es < 40 % de la
 *     media (ruido solo: ≈ 100 %, así que no hay falsas detecciones).
 *  2. Sincronía fina por PREFIJO CÍCLICO. La guarda de 504 muestras repite el
 *     final del símbolo, así que C(t) = Σ x[t+i]·conj(x[t+i+2048]) (i < 504) tiene
 *     módulo máximo en el inicio de símbolo. Se suman 4 símbolos (PRS+FIC) para
 *     que el pico sea limpio y se busca en ±700 muestras (±48 en seguimiento).
 *  3. CFO FRACCIONARIO. Un desvío f0 gira la muestra x[n+2048] por e^{j2π f0/1kHz},
 *     luego ε = −arg(ΣC)/(2π) (en separaciones de portadora, módulo 1) con la suma
 *     de los 76 símbolos de la trama (≈ 38 000 productos: error de pocos Hz).
 *     Se corrige con un rotor sobre los 4 símbolos que se demodulan.
 *  4. CFO ENTERO con el símbolo de referencia de fase (PRS): el espectro del PRS
 *     (EN 300 401 §14.3.2: φ_k = π/2·(h[i][k−k'] + n)) es conocido. Se evalúa
 *     Σ_k R[k+m]·conj(R[k+m−1])·conj(P_k)·P_{k−1} para m = −300..300 (la correlación
 *     diferencial entre portadoras vecinas elimina la pendiente de fase debida al
 *     desfase de tiempo de la ventana de FFT) y se elige el máximo.
 *  5. FFT de 2048 (la ventana arranca 40 muestras antes de la parte útil, dentro
 *     de la guarda: robusto a multitrayecto), D-QPSK  d_k = R_l[k]·conj(R_{l−1}[k]),
 *     corrección de fase común por símbolo (arg Σ d⁴ = π), decisión blanda
 *     (−Re d, −Im d) y desentrelazado de frecuencia (dab_carrier_map).
 *  6. FIC: despunzonado (ceros = borrado), Viterbi K=7 r1/4 blando de fec.h,
 *     dispersión de energía (lfsr_init_dab), 3 FIB con CRC-16.
 *  7. FIG: 0/0 (EId), 0/1 (subcanales: UEP corto o EEP largo), 0/2 (servicios y
 *     componentes), 0/17 (tipo de programa), 1/0 (etiqueta del conjunto), 1/1
 *     (etiqueta de servicio de programa), 1/5 (de servicio de datos).
 *
 * Eventos: {"kind":"ensemble","eid","label","text"} al conocerse/cambiar el
 * conjunto y {"kind":"service","sid","label","subch","kbps","prot","type","text"}
 * cuando un servicio queda completo (etiqueta + componente + subcanal) o cambia.
 * state_json: {"ensemble":{"id","label"},"services":[{"sid","label","subch",
 * "kbps","prot","type","pty"}],"sync":bool,"frames","fib_ok","fib_bad","cfo_hz"}.
 * Caducidad: sin FIB válido 30 s → todo vacío; servicio no visto en 120 s → baja.
 *
 * Limitaciones: solo modo I; sin MSC/audio; sin FIG 0/5,0/8,0/13,0/14… (no se
 * necesitan para la lista de servicios); sin reconfiguración (flags C/N ignorados,
 * se usa la configuración actual); un transmisor (sin análisis de multitrayecto).
 */
#include "dab.h"

#include "rds.h" /* tabla de caracteres EBU Latin */

#include "decoder.h"

#include "../dsp/bits.h"
#include "../dsp/crc.h"
#include "../dsp/fft.h"
#include "../dsp/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/*  Tablas y bloques comunes                                                */
/* ======================================================================== */
static int16_t g_map[DAB_K];
static bool g_map_ok;

/* Π(0) = 0; Π(i) = (13·Π(i−1) + 511) mod 2048. Se descartan los valores fuera de
 * 256..1792 y el 1024 (portadora central); el n-ésimo superviviente es la
 * portadora k = Π − 1024 (EN 300 401 §14.6). */
const int16_t *dab_carrier_map(void)
{
    if (!g_map_ok) {
        int tmp = 0, n = 0;
        for (int i = 0; i < DAB_TU; i++) {
            if (i > 0)
                tmp = (13 * tmp + 511) % DAB_TU;
            if (tmp == 1024 || tmp < 256 || tmp > 1792)
                continue;
            g_map[n++] = (int16_t)(tmp - 1024);
        }
        g_map_ok = n == DAB_K;
    }
    return g_map;
}

/* Tabla h_{i,j} (EN 300 401 tabla 23) y parámetros (k', i, n) del modo I (tabla 24). */
static const int8_t H_TAB[4][32] = {
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1, 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0, 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3, 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2, 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2}};
static const int16_t PRS_KMIN[48] = {-768, -736, -704, -672, -640, -608, -576, -544, -512, -480, -448, -416,
                                     -384, -352, -320, -288, -256, -224, -192, -160, -128, -96,  -64,  -32,
                                     1,    33,   65,   97,   129,  161,  193,  225,  257,  289,  321,  353,
                                     385,  417,  449,  481,  513,  545,  577,  609,  641,  673,  705,  737};
static const int8_t PRS_I[48] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3,
                                 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1};
static const int8_t PRS_N[48] = {1, 2, 0, 1, 3, 2, 2, 3, 2, 1, 2, 3, 1, 2, 3, 3, 2, 2, 2, 1, 1, 3, 1, 2,
                                 3, 1, 1, 1, 2, 2, 1, 0, 2, 2, 3, 3, 0, 2, 1, 3, 3, 3, 3, 0, 3, 0, 1, 1};

void dab_prs_spectrum(cf32 *bins)
{
    memset(bins, 0, sizeof(cf32) * DAB_TU);
    for (int blk = 0; blk < 48; blk++)
        for (int j = 0; j < 32; j++) {
            int k = PRS_KMIN[blk] + j;
            double ph = M_PI / 2.0 * (H_TAB[PRS_I[blk]][j] + PRS_N[blk]);
            bins[k >= 0 ? k : DAB_TU + k] = (cf32){(float)cos(ph), (float)sin(ph)};
        }
}

/* ---- CRC del FIB ------------------------------------------------------- */
static crc_t g_crc;
static bool g_crc_ok;

static const crc_t *fib_crc(void)
{
    if (!g_crc_ok) {
        crc_init(&g_crc, crc_preset("CRC-16/GENIBUS")); /* x^16+x^12+x^5+1, init 0xFFFF, ~ */
        g_crc_ok = true;
    }
    return &g_crc;
}

bool dab_fib_crc_ok(const uint8_t fib[32])
{
    uint32_t c = crc_compute(fib_crc(), fib, 30);
    return (fib[30] << 8 | fib[31]) == (int)c;
}

void dab_fib_set_crc(uint8_t fib[32])
{
    uint32_t c = crc_compute(fib_crc(), fib, 30);
    fib[30] = (uint8_t)(c >> 8);
    fib[31] = (uint8_t)c;
}

/* ---- Punzonado del FIC (EN 300 401 §11.2/§11.3) ------------------------ */
static const uint8_t PI_16[32] = {1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0,
                                  1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0};
static const uint8_t PI_15[32] = {1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0,
                                  1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0};
static const uint8_t PI_X[24] = {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};

#define MOTHER_BITS 3096 /* (768 + 6 de cola) × 4 */
static uint8_t g_keep[MOTHER_BITS];
static bool g_keep_ok;

/* Los 3072 primeros bits del código madre se dividen en 24 bloques de 128: los 21
 * primeros con PI_16, los 3 siguientes con PI_15; los 24 de cola con PI_X. */
static const uint8_t *keep_table(void)
{
    if (!g_keep_ok) {
        for (int i = 0; i < MOTHER_BITS; i++)
            g_keep[i] = i < 21 * 128 ? PI_16[i % 32] : i < 3072 ? PI_15[i % 32] : PI_X[i - 3072];
        g_keep_ok = true;
    }
    return g_keep;
}

void dab_fic_encode(const uint8_t fib[3][32], uint8_t out[2304])
{
    uint8_t info[768], mother[MOTHER_BITS + 16];
    for (int f = 0; f < 3; f++)
        bits_unpack_msb(fib[f], 256, info + 256 * f);
    lfsr_t l;
    lfsr_init_dab(&l);
    lfsr_process(&l, info, 768, info, 0);
    conv_code_t code = conv_dab_k7();
    int n = conv_encode(&code, info, 768, mother, CONV_TERM_ZERO, NULL);
    const uint8_t *keep = keep_table();
    int w = 0;
    for (int i = 0; i < n && i < MOTHER_BITS; i++)
        if (keep[i])
            out[w++] = mother[i];
}

int dab_fic_decode(viterbi_t *v, const int8_t soft[2304], uint8_t fib[3][32])
{
    int8_t mother[MOTHER_BITS];
    const uint8_t *keep = keep_table();
    int r = 0;
    for (int i = 0; i < MOTHER_BITS; i++)
        mother[i] = keep[i] ? soft[r++] : 0;
    uint8_t info[768];
    int errs = viterbi_decode_i8(v, mother, MOTHER_BITS, 768, CONV_TERM_ZERO, NULL, info);
    if (errs < 0)
        return -1;
    lfsr_t l;
    lfsr_init_dab(&l);
    lfsr_process(&l, info, 768, info, 0);
    for (int f = 0; f < 3; f++)
        bits_pack_msb(info + 256 * f, 256, fib[f]);
    return errs;
}

/* ---- Tabla UEP (tabla 8): {tamaño en CU, nivel, kbps} ------------------- */
static const int16_t UEP[64][3] = {
    {16, 5, 32},  {21, 4, 32},  {24, 3, 32},  {29, 2, 32},  {35, 1, 32},  {24, 5, 48},  {29, 4, 48},
    {35, 3, 48},  {42, 2, 48},  {52, 1, 48},  {29, 5, 56},  {35, 4, 56},  {42, 3, 56},  {52, 2, 56},
    {32, 5, 64},  {42, 4, 64},  {48, 3, 64},  {58, 2, 64},  {70, 1, 64},  {40, 5, 80},  {52, 4, 80},
    {58, 3, 80},  {70, 2, 80},  {84, 1, 80},  {48, 5, 96},  {58, 4, 96},  {70, 3, 96},  {84, 2, 96},
    {104, 1, 96}, {58, 5, 112}, {70, 4, 112}, {84, 3, 112}, {104, 2, 112}, {64, 5, 128}, {84, 4, 128},
    {96, 3, 128}, {116, 2, 128}, {140, 1, 128}, {80, 5, 160}, {104, 4, 160}, {116, 3, 160}, {140, 2, 160},
    {168, 1, 160}, {96, 5, 192}, {116, 4, 192}, {140, 3, 192}, {168, 2, 192}, {208, 1, 192}, {116, 5, 224},
    {140, 4, 224}, {168, 3, 224}, {208, 2, 224}, {232, 1, 224}, {128, 5, 256}, {168, 4, 256}, {192, 3, 256},
    {232, 2, 256}, {280, 1, 256}, {160, 5, 320}, {208, 4, 320}, {280, 2, 320}, {192, 5, 384}, {280, 3, 384},
    {416, 1, 384}};

bool dab_uep_info(int index, int *size_cu, int *level, int *kbps)
{
    if (index < 0 || index > 63)
        return false;
    *size_cu = UEP[index][0];
    *level = UEP[index][1];
    *kbps = UEP[index][2];
    return true;
}

/* ======================================================================== */
/*  Decodificador                                                           */
/* ======================================================================== */
#define MAX_SVC 48
#define MAX_COMP 4
#define SEARCH_CP 700
#define TRACK_CP 48
#define INT_CFO_MAX 300
#define WIN_BACK 40
#define EXPIRE_FIB_S 30.0
#define EXPIRE_SVC_S 120.0

typedef struct {
    bool valid;
    int start, size; /* CU */
    int kind;        /* 0 UEP, 1 EEP-A, 2 EEP-B */
    int level, kbps;
} subch_t;

typedef struct {
    int tmid;  /* 0 audio, 1 datos en flujo, 3 datos en paquetes */
    int type;  /* ASCTy / DSCTy */
    int subch; /* −1 si no aplica */
    bool ps;
} comp_t;

typedef struct {
    bool used, data;
    uint32_t sid;
    char label[80];
    int ncomp, pty;
    comp_t comp[MAX_COMP];
    double seen;
    char last_ev[260];
} svc_t;

typedef struct {
    viterbi_t *vit;
    fft_plan_t *fft;
    cf32 prs[DAB_TU];
    /* búfer de muestras (índices absolutos desde el arranque) */
    cf32 *buf;
    int64_t base, len_abs; /* buf[0] es la muestra 'base'; len_abs = base + nº de muestras */
    size_t cap;
    bool tracking;
    int64_t prs_nom; /* inicio nominal (absoluto) del PRS de la próxima trama */
    int fail, fic_bad_frames;
    double now;
    /* estadísticas */
    int64_t frames, fib_ok, fib_bad;
    double cfo_hz, last_fib_t;
    bool sync;
    /* conjunto */
    bool eid_ok;
    uint16_t eid;
    char elabel[80];
    bool elabel_ok;
    char last_elabel_ev[80];
    subch_t sub[64];
    svc_t svc[MAX_SVC];
    /* memoria de trabajo (en la instancia: puede haber varias en hilos distintos) */
    double w_e[DAB_FRAME / 64 + 40], w_sm[DAB_FRAME / 64];
    cf32 w_x[4][DAB_TU], w_dref[DAB_TU], w_d[DAB_K];
    int8_t w_soft[3 * 2 * DAB_K];
    float w_sf[3 * 2 * DAB_K];
    dec_emit_fn emit;
    void *ctx;
} dab_t;

/* ---- tablas de datos de FIG --------------------------------------------- */
static uint32_t getb(const uint8_t *d, int bit, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | ((d[(bit + i) >> 3] >> (7 - ((bit + i) & 7))) & 1);
    return v;
}

static svc_t *svc_find(dab_t *r, uint32_t sid, bool create)
{
    svc_t *free_s = NULL;
    for (int i = 0; i < MAX_SVC; i++) {
        if (r->svc[i].used && r->svc[i].sid == sid)
            return &r->svc[i];
        if (!r->svc[i].used && !free_s)
            free_s = &r->svc[i];
    }
    if (!create || !free_s)
        return NULL;
    memset(free_s, 0, sizeof *free_s);
    free_s->used = true;
    free_s->sid = sid;
    free_s->pty = -1;
    return free_s;
}

static void label_from(const uint8_t *src, int charset, char *dst)
{
    if (charset == 15) { /* UTF-8 */
        memcpy(dst, src, 16);
        dst[16] = 0;
        int w = 16;
        while (w > 0 && dst[w - 1] == ' ')
            dst[--w] = 0;
    } else {
        ebu_latin_string(src, 16, false, dst); /* juego 0: EBU Latin; otros: aproximación */
    }
}

static void clear_ensemble(dab_t *r)
{
    memset(r->sub, 0, sizeof r->sub);
    memset(r->svc, 0, sizeof r->svc);
    r->elabel[0] = 0;
    r->elabel_ok = false;
    r->last_elabel_ev[0] = 0;
}

static void fig0_0(dab_t *r, const uint8_t *f, int len)
{
    if (len < 5)
        return;
    uint16_t eid = (uint16_t)getb(f, 16, 16);
    if (!r->eid_ok || r->eid != eid) {
        clear_ensemble(r);
        r->eid = eid;
        r->eid_ok = true;
    }
}

static void fig0_1(dab_t *r, const uint8_t *f, int len)
{
    if ((f[1] >> 6) & 1)
        return; /* OE = otro conjunto */
    int pos = 2; /* byte */
    while (pos + 3 <= len + 1) {
        int id = f[pos] >> 2;
        subch_t s = {0};
        s.start = ((f[pos] & 3) << 8) | f[pos + 1];
        if (!(f[pos + 2] & 0x80)) { /* forma corta: UEP */
            int idx = f[pos + 2] & 0x3F;
            if (f[pos + 2] & 0x40 || !dab_uep_info(idx, &s.size, &s.level, &s.kbps))
                return; /* "table switch" ≠ 0 no definido */
            s.kind = 0;
            pos += 3;
        } else { /* forma larga: EEP */
            if (pos + 4 > len + 1)
                return;
            int opt = (f[pos + 2] >> 4) & 7;
            int lvl = ((f[pos + 2] >> 2) & 3) + 1;
            s.size = ((f[pos + 2] & 3) << 8) | f[pos + 3];
            if (opt > 1 || s.size == 0)
                return;
            s.kind = 1 + opt;
            s.level = lvl;
            static const int dA[4] = {12, 8, 6, 4}, dB[4] = {27, 21, 18, 15};
            s.kbps = opt == 0 ? s.size / dA[lvl - 1] * 8 : s.size / dB[lvl - 1] * 32;
            pos += 4;
        }
        s.valid = true;
        r->sub[id] = s;
    }
}

static void fig0_2(dab_t *r, const uint8_t *f, int len)
{
    if ((f[1] >> 6) & 1)
        return;
    bool pd = (f[1] >> 5) & 1;
    int pos = 2;
    while (pos < len + 1) {
        uint32_t sid;
        int idlen = pd ? 4 : 2;
        if (pos + idlen + 1 > len + 1)
            return;
        sid = getb(f, pos * 8, idlen * 8);
        pos += idlen;
        int nc = f[pos] & 15;
        pos++;
        if (pos + 2 * nc > len + 1)
            return;
        svc_t *s = svc_find(r, sid, true);
        if (s) {
            s->data = pd;
            s->ncomp = MIN(nc, MAX_COMP);
            s->seen = r->now;
        }
        for (int c = 0; c < nc; c++, pos += 2) {
            if (!s || c >= MAX_COMP)
                continue;
            int tm = f[pos] >> 6;
            comp_t *cc = &s->comp[c];
            cc->tmid = tm;
            cc->ps = (f[pos + 1] >> 1) & 1;
            if (tm == 3) {
                cc->subch = -1;
                cc->type = 0;
            } else {
                cc->type = f[pos] & 0x3F;
                cc->subch = f[pos + 1] >> 2;
            }
        }
    }
}

static void fig0_17(dab_t *r, const uint8_t *f, int len)
{
    int pos = 2;
    while (pos + 4 <= len + 1) {
        uint32_t sid = getb(f, pos * 8, 16);
        bool lflag = (f[pos + 2] >> 5) & 1, cc = (f[pos + 2] >> 4) & 1;
        int p = pos + 3;
        if (lflag)
            p++;
        if (p >= len + 1)
            return;
        int pty = f[p] & 0x1F;
        svc_t *s = svc_find(r, sid, false);
        if (s)
            s->pty = pty;
        pos = p + 1 + (cc ? 1 : 0);
    }
}

static void fig1(dab_t *r, const uint8_t *f, int len)
{
    int charset = f[1] >> 4, oe = (f[1] >> 3) & 1, ext = f[1] & 7;
    if (oe)
        return;
    char lab[80];
    if (ext == 0 && len >= 21) {
        uint16_t eid = (uint16_t)getb(f, 16, 16);
        if (r->eid_ok && eid == r->eid) {
            label_from(f + 4, charset, lab);
            snprintf(r->elabel, sizeof r->elabel, "%s", lab);
            r->elabel_ok = true;
        }
    } else if (ext == 1 && len >= 21) {
        svc_t *s = svc_find(r, getb(f, 16, 16), true);
        if (s) {
            label_from(f + 4, charset, s->label);
            s->seen = r->now;
        }
    } else if (ext == 5 && len >= 23) {
        svc_t *s = svc_find(r, getb(f, 16, 32), true);
        if (s) {
            s->data = true;
            label_from(f + 6, charset, s->label);
            s->seen = r->now;
        }
    }
}

static void parse_fib(dab_t *r, const uint8_t *fib)
{
    int pos = 0;
    while (pos < 30) {
        if (fib[pos] == 0xFF)
            break; /* marcador de fin */
        int type = fib[pos] >> 5, len = fib[pos] & 31;
        if (len == 0 || pos + 1 + len > 30)
            break;
        const uint8_t *f = fib + pos; /* f[0] = cabecera, f[1..len] = datos */
        if (type == 0) {
            switch (f[1] & 31) {
            case 0: fig0_0(r, f, len); break;
            case 1: fig0_1(r, f, len); break;
            case 2: fig0_2(r, f, len); break;
            case 17: fig0_17(r, f, len); break;
            default: break;
            }
        } else if (type == 1) {
            fig1(r, f, len);
        }
        pos += 1 + len;
    }
}

/* ---- descripciones y eventos ------------------------------------------- */
static void prot_str(const subch_t *s, char *out, size_t n)
{
    if (s->kind == 0)
        snprintf(out, n, "UEP %d", s->level);
    else
        snprintf(out, n, "EEP %d-%c", s->level, s->kind == 1 ? 'A' : 'B');
}

static const char *type_str(const svc_t *s, char *tmp, size_t n)
{
    if (s->ncomp < 1)
        return "";
    const comp_t *c = &s->comp[0];
    if (c->tmid == 0)
        return c->type == 63 ? "DAB+" : c->type == 0 ? "DAB" : (snprintf(tmp, n, "audio %d", c->type), tmp);
    if (c->tmid == 1)
        return (snprintf(tmp, n, "data %d", c->type), tmp);
    return "packet data";
}

static void svc_json(sb_t *sb, const svc_t *s, const dab_t *r)
{
    char prot[24] = "", tmp[24];
    int subch = -1, kbps = 0;
    if (s->ncomp > 0 && s->comp[0].subch >= 0 && r->sub[s->comp[0].subch].valid) {
        const subch_t *sc = &r->sub[s->comp[0].subch];
        subch = s->comp[0].subch;
        kbps = sc->kbps;
        prot_str(sc, prot, sizeof prot);
    } else if (s->ncomp > 0) {
        subch = s->comp[0].subch;
    }
    char sid[12];
    snprintf(sid, sizeof sid, s->data ? "%08X" : "%04X", s->sid);
    sb_printf(sb, "{\"sid\":\"%s\",\"label\":", sid);
    sb_json_str(sb, s->label);
    sb_printf(sb, ",\"subch\":%d,\"kbps\":%d,\"prot\":", subch, kbps);
    sb_json_str(sb, prot);
    sb_puts(sb, ",\"type\":");
    sb_json_str(sb, type_str(s, tmp, sizeof tmp));
    sb_printf(sb, ",\"pty\":%d}", s->pty < 0 ? 0 : s->pty);
}

/* Emite eventos de lo que haya cambiado. */
static void emit_changes(dab_t *r)
{
    if (r->eid_ok && r->elabel_ok && strcmp(r->elabel, r->last_elabel_ev) != 0) {
        snprintf(r->last_elabel_ev, sizeof r->last_elabel_ev, "%s", r->elabel);
        sb_t sb = {0};
        sb_printf(&sb, "{\"kind\":\"ensemble\",\"eid\":\"%04X\",\"label\":", r->eid);
        sb_json_str(&sb, r->elabel);
        char t[120];
        snprintf(t, sizeof t, "Ensemble %04X \"%s\"", r->eid, r->elabel);
        sb_puts(&sb, ",\"text\":");
        sb_json_str(&sb, t);
        sb_puts(&sb, "}");
        r->emit(r->ctx, sb.data, sb.len);
        sb_free(&sb);
    }
    for (int i = 0; i < MAX_SVC; i++) {
        svc_t *s = &r->svc[i];
        if (!s->used || !s->label[0] || s->ncomp < 1)
            continue;
        const comp_t *c = &s->comp[0];
        if (c->subch >= 0 && !r->sub[c->subch].valid)
            continue; /* faltan los datos del subcanal */
        sb_t js = {0};
        svc_json(&js, s, r);
        if (strcmp(js.data, s->last_ev) != 0) {
            snprintf(s->last_ev, sizeof s->last_ev, "%s", js.data);
            sb_t sb = {0};
            sb_puts(&sb, "{\"kind\":\"service\",");
            sb_append(&sb, js.data + 1, js.len - 1); /* reutiliza los campos de svc_json */
            sb.len -= 1;                            /* quita la '}' final */
            sb.data[sb.len] = 0;
            char prot[24] = "", tmp[24];
            int kbps = 0, subch = c->subch;
            if (c->subch >= 0) {
                prot_str(&r->sub[c->subch], prot, sizeof prot);
                kbps = r->sub[c->subch].kbps;
            }
            char t[200];
            snprintf(t, sizeof t, "Service %04X \"%s\": subch %d, %d kbps, %s, %s", s->sid, s->label, subch, kbps,
                     prot, type_str(s, tmp, sizeof tmp));
            sb_puts(&sb, ",\"text\":");
            sb_json_str(&sb, t);
            sb_puts(&sb, "}");
            r->emit(r->ctx, sb.data, sb.len);
            sb_free(&sb);
        }
        sb_free(&js);
    }
}

/* ---- sincronía y demodulación ------------------------------------------- */
static const cf32 *at(const dab_t *r, int64_t abs) { return r->buf + (abs - r->base); }

static void buf_append(dab_t *r, const int16_t *iq, int n)
{
    size_t need = (size_t)(r->len_abs - r->base) + (size_t)n;
    if (need > r->cap) {
        size_t nc = need * 2 + 65536;
        cf32 *nb = realloc(r->buf, nc * sizeof(cf32));
        if (!nb)
            return;
        r->buf = nb;
        r->cap = nc;
    }
    cf32 *d = r->buf + (r->len_abs - r->base);
    for (int i = 0; i < n; i++) {
        d[i].re = (float)iq[2 * i] * (1.0f / 2048.0f);
        d[i].im = (float)iq[2 * i + 1] * (1.0f / 2048.0f);
    }
    r->len_abs += n;
}

static void buf_drop_before(dab_t *r, int64_t abs)
{
    if (abs <= r->base)
        return;
    if (abs > r->len_abs)
        abs = r->len_abs;
    memmove(r->buf, r->buf + (abs - r->base), (size_t)(r->len_abs - abs) * sizeof(cf32));
    r->base = abs;
}

/* Σ x[i]·conj(x[i+2048]) sobre la guarda de 504 muestras que empieza en p. */
static cf32 cp_corr(const cf32 *p)
{
    double re = 0, im = 0;
    for (int i = 0; i < DAB_GUARD; i++) {
        re += (double)p[i].re * p[i + DAB_TU].re + (double)p[i].im * p[i + DAB_TU].im;
        im += (double)p[i].im * p[i + DAB_TU].re - (double)p[i].re * p[i + DAB_TU].im;
    }
    return (cf32){(float)re, (float)im};
}

/* Busca el símbolo nulo en las primeras DAB_FRAME muestras del búfer; fija prs_nom. */
static bool find_null(dab_t *r)
{
    enum { B = 64, NB = DAB_FRAME / B, W = 36 };
    double *e = r->w_e;
    int nblk = NB + W;
    for (int i = 0; i < nblk; i++) {
        const cf32 *p = r->buf + (int64_t)i * B;
        double s = 0;
        for (int j = 0; j < B; j++)
            s += (double)p[j].re * p[j].re + (double)p[j].im * p[j].im;
        e[i] = s;
    }
    double sum = 0, mean = 0, mn = 1e300;
    int imin = 0;
    double *sm = r->w_sm;
    for (int j = 0; j < W; j++)
        sum += e[j];
    for (int i = 0; i < NB; i++) {
        sm[i] = sum;
        mean += sum;
        if (sum < mn) {
            mn = sum;
            imin = i;
        }
        sum += e[i + W] - e[i];
    }
    mean /= NB;
    if (mean < 1e-9 || mn > 0.4 * mean)
        return false;
    /* centro de la meseta del mínimo */
    double thr = mn + 0.15 * (mean - mn);
    int lo = imin, hi = imin;
    while (lo > 0 && sm[lo - 1] <= thr)
        lo--;
    while (hi < NB - 1 && sm[hi + 1] <= thr)
        hi++;
    double center = ((lo + hi) / 2.0) * B + (W * B) / 2.0; /* centro del nulo, relativo al búfer */
    int64_t null_start = r->base + (int64_t)(center - DAB_NULL / 2.0);
    r->prs_nom = null_start + DAB_NULL;
    return true;
}

static void fft_window(dab_t *r, int64_t start, double eps, int64_t ref, cf32 *out)
{
    /* x[n]·e^{−j2π ε (n − ref)/2048}: quita el CFO fraccional con fase continua */
    const cf32 *p = at(r, start);
    for (int i = 0; i < DAB_TU; i++) {
        double a = -2.0 * M_PI * eps * (double)(start + i - ref) / DAB_TU;
        float c = (float)cos(a), s = (float)sin(a);
        out[i].re = p[i].re * c - p[i].im * s;
        out[i].im = p[i].re * s + p[i].im * c;
    }
    fft_exec(r->fft, out);
}

/* Procesa una trama con PRS nominal en prs_nom. Devuelve true si la sincronía es válida. */
static bool process_frame(dab_t *r, int search)
{
    int64_t nom = r->prs_nom;
    /* 2. temporización fina por prefijo cíclico (4 símbolos) */
    double best = -1;
    int bd = 0;
    for (int d = -search; d <= search; d++) {
        cf32 c = {0, 0};
        for (int l = 0; l < 4; l++) {
            cf32 q = cp_corr(at(r, nom + d + (int64_t)l * DAB_SYM));
            c.re += q.re;
            c.im += q.im;
        }
        double m = (double)c.re * c.re + (double)c.im * c.im;
        if (m > best) {
            best = m;
            bd = d;
        }
    }
    int64_t t0 = nom + bd;
    /* 3. CFO fraccional con los 76 símbolos; calidad = coherencia normalizada */
    cf32 ct = {0, 0};
    double e1 = 0, e2 = 0;
    for (int l = 0; l < DAB_NSYM; l++) {
        const cf32 *p = at(r, t0 + (int64_t)l * DAB_SYM);
        cf32 q = cp_corr(p);
        ct.re += q.re;
        ct.im += q.im;
        for (int i = 0; i < DAB_GUARD; i += 4) {
            e1 += (double)p[i].re * p[i].re + (double)p[i].im * p[i].im;
            e2 += (double)p[i + DAB_TU].re * p[i + DAB_TU].re + (double)p[i + DAB_TU].im * p[i + DAB_TU].im;
        }
    }
    double coh = sqrt((double)ct.re * ct.re + (double)ct.im * ct.im) / (4.0 * sqrt(e1 * e2) + 1e-30);
    if (coh < 0.25)
        return false;
    double eps = -atan2((double)ct.im, (double)ct.re) / (2.0 * M_PI);
    /* 4. FFT de los 4 primeros símbolos con ventana adelantada WIN_BACK muestras */
    cf32(*X)[DAB_TU] = r->w_x;
    for (int l = 0; l < 4; l++)
        fft_window(r, t0 + (int64_t)l * DAB_SYM + DAB_GUARD - WIN_BACK, eps, t0, X[l]);
    /* CFO entero: correlación diferencial con el PRS conocido */
    cf32 *dref = r->w_dref;
    for (int k = -767; k <= 768; k++) {
        if (k == 0 || k == 1)
            continue;
        int b = k >= 0 ? k : DAB_TU + k, bp = (k - 1) >= 0 ? k - 1 : DAB_TU + k - 1;
        cf32 a = r->prs[b], c = r->prs[bp];
        dref[b] = (cf32){a.re * c.re + a.im * c.im, a.im * c.re - a.re * c.im}; /* P_k·conj(P_{k−1}) */
    }
    /* Etapa A: correlación DIFERENCIAL para cada m. Su ángulo es la pendiente de
     * fase por portadora (desfase de tiempo de la ventana). El PRS tiene estructura
     * periódica (h se repite cada 16 portadoras) y m = ±16, ±32… también correlan,
     * por eso la etapa B confirma los mejores candidatos con la correlación
     * COHERENTE (quitando esa pendiente), donde el verdadero gana por un factor ≥ 2
     * en amplitud. */
    enum { NCAND = 8 };
    int cand[NCAND];
    double cand_slope[NCAND], cand_mag[NCAND];
    int ncand = 0;
    for (int m = -INT_CFO_MAX; m <= INT_CFO_MAX; m++) {
        double re = 0, im = 0;
        for (int k = -767; k <= 768; k++) {
            if (k == 0 || k == 1)
                continue;
            int b = k >= 0 ? k : DAB_TU + k;
            int i1 = (b + m) & (DAB_TU - 1), i0 = (b - 1 + m) & (DAB_TU - 1);
            cf32 a = X[0][i1], c = X[0][i0];
            double dr = (double)a.re * c.re + (double)a.im * c.im, di = (double)a.im * c.re - (double)a.re * c.im;
            /* (R_k·conj(R_{k−1}))·conj(P_k·conj(P_{k−1})) */
            re += dr * dref[b].re + di * dref[b].im;
            im += di * dref[b].re - dr * dref[b].im;
        }
        double mag = re * re + im * im;
        /* mantiene los NCAND mejores */
        int pos = ncand < NCAND ? ncand : NCAND - 1;
        if (ncand == NCAND && mag <= cand_mag[NCAND - 1])
            continue;
        if (ncand < NCAND)
            ncand++;
        while (pos > 0 && cand_mag[pos - 1] < mag) {
            cand[pos] = cand[pos - 1];
            cand_mag[pos] = cand_mag[pos - 1];
            cand_slope[pos] = cand_slope[pos - 1];
            pos--;
        }
        cand[pos] = m;
        cand_mag[pos] = mag;
        cand_slope[pos] = atan2(im, re);
    }
    double bestm = -1, second = 0, best_slope = 0;
    int bm = 0;
    for (int c = 0; c < ncand; c++) {
        int m = cand[c];
        double re = 0, im = 0;
        for (int k = -768; k <= 768; k++) {
            if (k == 0)
                continue;
            int b = k >= 0 ? k : DAB_TU + k;
            cf32 a = X[0][(b + m) & (DAB_TU - 1)], pk = r->prs[b];
            double ph = -cand_slope[c] * (double)k, cs = cos(ph), sn = sin(ph);
            double cr = (double)a.re * pk.re + (double)a.im * pk.im, ci = (double)a.im * pk.re - (double)a.re * pk.im;
            re += cr * cs - ci * sn;
            im += cr * sn + ci * cs;
        }
        double mag = re * re + im * im;
        if (mag > bestm) {
            second = MAX(second, bestm > 0 ? bestm : 0);
            bestm = mag;
            bm = m;
            best_slope = cand_slope[c];
        } else if (mag > second) {
            second = mag;
        }
    }
    (void)best_slope;
    if (bestm < 2.5 * second)
        return false; /* el PRS no aparece: no es una trama DAB sincronizada */
    r->cfo_hz = (bm + eps) * 1000.0;
    /* 5. D-QPSK de los símbolos 1..3 → bits blandos desentrelazados */
    const int16_t *map = dab_carrier_map();
    int8_t *soft = r->w_soft;
    cf32 *d = r->w_d;
    double avg = 0;
    float *sf = r->w_sf;
    const double rot = -2.0 * M_PI * (double)bm * DAB_SYM / DAB_TU;
    const float rc = (float)cos(rot), rs = (float)sin(rot);
    for (int l = 1; l <= 3; l++) {
        cf32 a4 = {0, 0};
        for (int n = 0; n < DAB_K; n++) {
            int k = map[n];
            int b = ((k >= 0 ? k : DAB_TU + k) + bm) & (DAB_TU - 1);
            cf32 x = X[l][b], y = X[l - 1][b];
            d[n] = (cf32){x.re * y.re + x.im * y.im, x.im * y.re - x.re * y.im};
            /* el desvío entero de m portadoras gira cada símbolo 2π·m·2552/2048: se quita aquí
             * (la corrección de fase común de abajo solo resuelve ±45°) */
            cf32 dd = d[n];
            d[n] = (cf32){dd.re * rc - dd.im * rs, dd.re * rs + dd.im * rc};
            float p2r = d[n].re * d[n].re - d[n].im * d[n].im, p2i = 2 * d[n].re * d[n].im;
            a4.re += p2r * p2r - p2i * p2i; /* d⁴ */
            a4.im += 2 * p2r * p2i;
        }
        /* d ideal = e^{j(π/4 + nπ/2)} → d⁴ = −1: arg Σ d⁴ = π + 4·(error de fase) */
        double perr = (atan2((double)a4.im, (double)a4.re) - M_PI) / 4.0;
        if (perr < -M_PI / 4)
            perr += M_PI / 2;
        float cr = (float)cos(-perr), ci = (float)sin(-perr);
        for (int n = 0; n < DAB_K; n++) {
            float re = d[n].re * cr - d[n].im * ci, im = d[n].re * ci + d[n].im * cr;
            sf[(l - 1) * 2 * DAB_K + n] = -re;
            sf[(l - 1) * 2 * DAB_K + DAB_K + n] = -im;
            avg += sqrt((double)re * re + (double)im * im);
        }
    }
    avg /= 3.0 * DAB_K;
    float scale = (float)(60.0 / (avg + 1e-12));
    for (int i = 0; i < 3 * 2 * DAB_K; i++)
        soft[i] = (int8_t)CLAMP(lrintf(sf[i] * scale), -127, 127);
    /* 6-7. FIC: 4 bloques de 2304 bits */
    int ok = 0;
    for (int q = 0; q < 4; q++) {
        uint8_t fib[3][32];
        if (dab_fic_decode(r->vit, soft + q * 2304, fib) < 0)
            continue;
        for (int f = 0; f < 3; f++) {
            if (dab_fib_crc_ok(fib[f])) {
                ok++;
                r->fib_ok++;
                parse_fib(r, fib[f]);
            } else {
                r->fib_bad++;
            }
        }
    }
    r->frames++;
    if (ok > 0)
        r->last_fib_t = r->now;
    r->fic_bad_frames = ok > 0 ? 0 : r->fic_bad_frames + 1;
    r->prs_nom = t0; /* el PRS real; la siguiente trama es t0 + DAB_FRAME */
    return r->fic_bad_frames < 20;
}

static void dab_raw(void *st, const int16_t *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    dab_t *r = st;
    r->emit = emit;
    r->ctx = ctx;
    r->now = now;
    buf_append(r, iq, n);
    for (;;) {
        int64_t have = r->len_abs - r->base;
        if (!r->tracking) {
            if (have < 2 * DAB_FRAME + 4000)
                return;
            if (find_null(r)) {
                r->tracking = true;
                r->fail = 0;
            } else {
                buf_drop_before(r, r->base + DAB_FRAME); /* sin señal DAB: descarta una trama */
                continue;
            }
        }
        int64_t need = r->prs_nom + (int64_t)DAB_NSYM * DAB_SYM + SEARCH_CP + 2 * DAB_TU + 64;
        if (r->len_abs < need)
            return;
        bool ok = process_frame(r, r->fail == 0 && r->frames > 0 ? TRACK_CP : SEARCH_CP);
        r->sync = ok;
        if (ok) {
            r->fail = 0;
            emit_changes(r);
        } else {
            r->fail++;
            r->prs_nom += DAB_FRAME;
            if (r->fail >= 3) {
                r->tracking = false;
                r->frames = 0;
                r->fic_bad_frames = 0;
            }
        }
        if (ok)
            r->prs_nom += DAB_FRAME;
        buf_drop_before(r, r->prs_nom - DAB_NULL - 2 * SEARCH_CP);
        if (!r->tracking && r->base < r->prs_nom - DAB_FRAME)
            buf_drop_before(r, r->prs_nom - DAB_FRAME);
    }
}

/* ---- state_json -------------------------------------------------------- */
static void dab_state(void *st, sb_t *sb, double now)
{
    dab_t *r = st;
    bool valid = r->eid_ok && now - r->last_fib_t < EXPIRE_FIB_S;
    sb_puts(sb, "{\"ensemble\":{\"id\":");
    char id[8] = "";
    if (valid)
        snprintf(id, sizeof id, "%04X", r->eid);
    sb_json_str(sb, id);
    sb_puts(sb, ",\"label\":");
    sb_json_str(sb, valid && r->elabel_ok ? r->elabel : "");
    sb_puts(sb, "},\"services\":[");
    int cnt = 0;
    for (int i = 0; valid && i < MAX_SVC; i++) {
        const svc_t *s = &r->svc[i];
        if (!s->used || now - s->seen > EXPIRE_SVC_S || !s->label[0])
            continue;
        if (cnt++)
            sb_puts(sb, ",");
        svc_json(sb, s, r);
    }
    sb_printf(sb, "],\"sync\":%s,\"frames\":%lld,\"fib_ok\":%lld,\"fib_bad\":%lld,\"cfo_hz\":", valid && r->sync ? "true" : "false",
              (long long)r->frames, (long long)r->fib_ok, (long long)r->fib_bad);
    sb_json_num(sb, r->cfo_hz, 0);
    sb_puts(sb, "}");
}

static void dab_destroy(void *st)
{
    dab_t *r = st;
    if (!r)
        return;
    viterbi_destroy(r->vit);
    fft_plan_destroy(r->fft);
    free(r->buf);
    free(r);
}

static void *dab_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - DAB_RATE) > 0.5) {
        snprintf(err, errlen, "DAB requiere exactamente 2,048 Msps (recibido %.0f)", cfg->rate);
        return NULL;
    }
    dab_t *r = calloc(1, sizeof *r);
    if (!r) {
        snprintf(err, errlen, "sin memoria");
        return NULL;
    }
    conv_code_t code = conv_dab_k7();
    r->vit = viterbi_create(&code, 768);
    r->fft = fft_plan_create(DAB_TU);
    if (!r->vit || !r->fft || !dab_carrier_map()) {
        snprintf(err, errlen, "no se pudo inicializar la cadena DAB");
        dab_destroy(r);
        return NULL;
    }
    dab_prs_spectrum(r->prs);
    r->last_fib_t = -1e18;
    return r;
}

const decoder_desc_t DEC_DAB = {
    .id = "dab",
    .name = "DAB / DAB+",
    .group = "Radiodifusión",
    .desc = "Radio digital DAB: sincronía OFDM y FIC (conjunto, servicios).",
    .kind = DEC_WIDE,
    .rate = 0,
    .bw_hz = 0,
    .wide_rate = 2048000,
    .wide_min_rate = 0,
    .freqs = {227360000LL, 0LL, 0LL, 0LL},
    .create = dab_create,
    .destroy = dab_destroy,
    .process_raw = dab_raw,
    .state_json = dab_state,
};
