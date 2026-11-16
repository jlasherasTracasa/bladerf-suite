/*
 * mod_wifi.c — Modulador de balizas (beacons) WiFi 802.11 a 20 Msps.
 *
 * Construye una trama de gestión Beacon (§9.3.3.3: control de trama, duración,
 * DA = difusión, SA = BSSID, BSSID, secuencia, marca de tiempo, intervalo,
 * capacidades, elementos de información SSID / tasas / DS / TIM / RSN…, y FCS
 * CRC-32) y la modula como la emitiría un punto de acceso:
 *
 *  - OFDM 802.11a/g (cláusula 17): STF + LTF + SIGNAL + datos a 6, 9, 12 o
 *    18 Mb/s (BPSK/QPSK, K=7 1/2 o 3/4). Pasos del transmisor: SERVICE (16
 *    ceros) + PSDU (LSB primero) + cola (6 ceros) + relleno → aleatorizador
 *    x⁷+x⁴+1 (con los 6 bits de cola puestos a cero después) → convolucional
 *    → punzonado → entrelazado → constelación → 48 datos + 4 pilotos (polaridad
 *    p_n) → IFFT de 64 → prefijo cíclico de 16.
 *  - DSSS 802.11b (cláusula 16): preámbulo largo (128 unos + SFD), cabecera
 *    PLCP con CRC-16, PSDU a 1 Mb/s: aleatorizador autosincronizado, DBPSK,
 *    ensanchado Barker-11, chips a 11 Mchip/s conformados con coseno alzado
 *    (α = 0,5) evaluado analíticamente a 20 Msps.
 *
 * Parámetros JSON (todos opcionales; sin parámetros emite dos redes de ejemplo,
 * una OFDM 6 Mb/s y una DSSS 1 Mb/s):
 *   {"ssid":"BladeRF-Lab","bssid":"02:00:5e:10:00:01","channel":6,
 *    "std":"g"|"b"|"gb","security":"open|wep|wpa|wpa2|wpa3","rate":6,
 *    "ht":false,"repeat":2,"gap_us":300,"frame":"beacon|probe_resp|data"}
 * ("frame" solo sirve para probar el decodificador: probe_resp pone una MAC de
 * cliente ficticia en la dirección 1 y data genera una trama de datos de relleno).
 *
 * air_ok = false (AGENTS.md §6): las bandas WiFi no están en la lista blanca.
 * Las tramas NO llevan ninguna dirección de cliente: DA = ff:ff:ff:ff:ff:ff.
 */
#include "mod.h"

#include "../decoders/wifi_phy.h"
#include "../dsp/bits.h"
#include "../dsp/crc.h"
#include "../dsp/fft.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int channel;
    char std;           /* 'g' OFDM, 'b' DSSS */
    char security[8];
    int rate;
    bool ht;
    char frame;         /* 'b' baliza, 'p' respuesta de sondeo, 'd' datos (solo para pruebas) */
} ap_t;

typedef struct {
    ap_t ap[2];
    int nap, repeat, gap_us;
} params_t;

/* ---- Trama MAC ----------------------------------------------------------- */
static int ie_put(uint8_t *p, int id, const void *data, int len)
{
    p[0] = (uint8_t)id;
    p[1] = (uint8_t)len;
    memcpy(p + 2, data, (size_t)len);
    return len + 2;
}

/* Devuelve la longitud del PSDU (con FCS). */
static int build_beacon(const ap_t *a, uint8_t *f, uint16_t seq)
{
    int n = 0;
    f[n++] = a->frame == 'p' ? 0x50 : a->frame == 'd' ? 0x08 : 0x80; /* FC: gestión/8 baliza, gestión/5 probe-resp, datos */
    f[n++] = 0x00;
    f[n++] = 0x00; /* duración */
    f[n++] = 0x00;
    if (a->frame == 'p') {
        /* DA = un cliente ficticio (02:AA:BB:CC:DD:EE): sirve para comprobar que el decodificador NUNCA lo emite */
        const uint8_t cl[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
        memcpy(f + n, cl, 6);
    } else {
        memset(f + n, 0xFF, 6);      /* DA: difusión */
    }
    n += 6;
    memcpy(f + n, a->bssid, 6);      /* SA */
    n += 6;
    memcpy(f + n, a->bssid, 6);      /* BSSID */
    n += 6;
    f[n++] = (uint8_t)((seq << 4) & 0xF0);
    f[n++] = (uint8_t)(seq >> 4);
    memset(f + n, 0, 8);             /* marca de tiempo */
    f[n + 0] = (uint8_t)(seq * 3);
    n += 8;
    f[n++] = 0x64; /* intervalo: 100 TU */
    f[n++] = 0x00;
    bool open = !strcmp(a->security, "open");
    uint16_t cap = 0x0001 | 0x0020 | (open ? 0 : 0x0010); /* ESS, preámbulo corto, privacidad */
    f[n++] = (uint8_t)cap;
    f[n++] = (uint8_t)(cap >> 8);

    if (a->frame == 'd') { /* trama de datos sintética: relleno determinista, sin significado */
        for (int i = 0; i < 40; i++)
            f[n++] = (uint8_t)(i * 37 + 11);
        goto fcs;
    }
    n += ie_put(f + n, 0, a->ssid, (int)strlen(a->ssid));
    static const uint8_t rb[] = {0x82, 0x84, 0x8B, 0x96};
    static const uint8_t rg[] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    if (a->std == 'b')
        n += ie_put(f + n, 1, rb, 4);
    else
        n += ie_put(f + n, 1, rg, 8);
    uint8_t ch = (uint8_t)a->channel;
    n += ie_put(f + n, 3, &ch, 1);
    static const uint8_t tim[] = {0, 1, 0, 0};
    n += ie_put(f + n, 5, tim, 4);
    if (!strcmp(a->security, "wpa2") || !strcmp(a->security, "wpa3")) {
        /* RSN: versión 1, grupo CCMP (00-0F-AC:4), 1 cifrador pareado CCMP, 1 AKM
         * (2 = PSK, 8 = SAE), capacidades 0. */
        uint8_t akm = a->security[3] == '3' ? 8 : 2;
        const uint8_t rsn[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                               1, 0, 0x00, 0x0F, 0xAC, akm, 0, 0};
        n += ie_put(f + n, 48, rsn, (int)sizeof rsn);
    } else if (!strcmp(a->security, "wpa")) {
        /* IE de proveedor Microsoft WPA (00-50-F2:1), TKIP + PSK. */
        const uint8_t wpa[] = {0x00, 0x50, 0xF2, 1, 1, 0, 0x00, 0x50, 0xF2, 2, 1, 0, 0x00, 0x50, 0xF2, 2,
                               1, 0, 0x00, 0x50, 0xF2, 2};
        n += ie_put(f + n, 221, wpa, (int)sizeof wpa);
    }
    if (a->ht) {
        uint8_t htc[26] = {0x2C, 0x01, 0x17};
        n += ie_put(f + n, 45, htc, 26);
    }
fcs:;
    static crc_t c;
    static bool ci;
    if (!ci) {
        crc_init(&c, crc_preset("CRC-32"));
        ci = true;
    }
    uint32_t fcs = crc_compute(&c, f, (size_t)n);
    for (int i = 0; i < 4; i++)
        f[n++] = (uint8_t)(fcs >> (8 * i));
    return n;
}

static void gap(iqbuf_t *o, int n)
{
    iqbuf_reserve(o, n);
    memset(o->s + o->n, 0, sizeof(cf32) * (size_t)n);
    o->n += n;
}

/* ---- OFDM ---------------------------------------------------------------- */
/* IFFT de 64 puntos con la identidad ifft(X) = conj(fft(conj(X)))/N. */
static void ifft64(const fft_plan_t *pl, const cf32 *X, cf32 *x)
{
    cf32 t[64];
    for (int i = 0; i < 64; i++)
        t[i] = (cf32){X[i].re, -X[i].im};
    fft_exec(pl, t);
    for (int i = 0; i < 64; i++)
        x[i] = (cf32){t[i].re / 64.0f * 2.0f, -t[i].im / 64.0f * 2.0f};
}

static void push_sym(iqbuf_t *o, const cf32 *x, int gi)
{
    for (int i = 64 - gi; i < 64; i++)
        iqbuf_push(o, x[i]);
    for (int i = 0; i < 64; i++)
        iqbuf_push(o, x[i]);
}

static void put_symbol(iqbuf_t *o, const fft_plan_t *pl, const float *bitsoft, const wifi_rate_t *r, int polarity)
{
    /* bitsoft: ncbps bits (0/1) ya entrelazados de este símbolo. */
    cf32 X[64] = {{0}}, x[64];
    for (int i = 0; i < 48; i++) {
        int k = wifi_data_carrier(i);
        float re, im = 0;
        if (r->nbpsc == 1) {
            re = bitsoft[i] ? 1.0f : -1.0f;
        } else {
            re = (bitsoft[2 * i] ? 1.0f : -1.0f) * 0.70710678f;
            im = (bitsoft[2 * i + 1] ? 1.0f : -1.0f) * 0.70710678f;
        }
        X[wifi_bin(k)] = (cf32){re, im};
    }
    for (int p = 0; p < 4; p++)
        X[wifi_bin(WIFI_PILOT_K[p])] = (cf32){(float)(WIFI_PILOT_V[p] * polarity), 0};
    ifft64(pl, X, x);
    push_sym(o, x, 16);
}

static void ofdm_frame(iqbuf_t *o, const uint8_t *psdu, int len, const wifi_rate_t *r)
{
    fft_plan_t *pl = fft_plan_create(64);
    cf32 X[64], x[64];
    /* STF: 160 muestras = 2,5 repeticiones del símbolo de 64 (periodo 16). */
    wifi_stf_bins(X);
    ifft64(pl, X, x);
    for (int i = 0; i < 160; i++)
        iqbuf_push(o, x[i & 63]);
    /* LTF: GI2 (32) + 2 símbolos largos. */
    float L[64];
    wifi_ltf_bins(L);
    for (int i = 0; i < 64; i++)
        X[i] = (cf32){L[i], 0};
    ifft64(pl, X, x);
    push_sym(o, x, 32);
    for (int i = 0; i < 64; i++)
        iqbuf_push(o, x[i]);

    /* SIGNAL: RATE, reservado, LENGTH (LSB primero), paridad par, cola. */
    const wifi_rate_t *r6 = wifi_rate_by_mbps(6);
    uint8_t sig[24] = {0}, coded[48], sbits[48];
    for (int i = 0; i < 4; i++)
        sig[i] = (uint8_t)((r->code >> (3 - i)) & 1);
    for (int i = 0; i < 12; i++)
        sig[5 + i] = (uint8_t)((len >> i) & 1);
    int par = 0;
    for (int i = 0; i < 17; i++)
        par ^= sig[i];
    sig[17] = (uint8_t)par;
    conv_code_t cc = wifi_conv_code();
    conv_encode(&cc, sig, 24, coded, CONV_TERM_NONE, NULL);
    int jk[288];
    wifi_interleave_table(r6, jk);
    for (int k = 0; k < 48; k++)
        sbits[jk[k]] = coded[k];
    float fb[288];
    for (int i = 0; i < 48; i++)
        fb[i] = sbits[i];
    put_symbol(o, pl, fb, r6, wifi_pilot_polarity(0));

    /* DATA */
    int nsym = (16 + 8 * len + 6 + r->ndbps - 1) / r->ndbps;
    int nbits = nsym * r->ndbps;
    uint8_t *d = calloc((size_t)nbits, 1), *cb = malloc((size_t)nbits * 2 + 16);
    for (int i = 0; i < 8 * len; i++)
        d[16 + i] = (uint8_t)((psdu[i >> 3] >> (i & 7)) & 1);
    lfsr_t l;
    lfsr_init_wifi(&l, 0x5D);
    lfsr_process(&l, d, nbits, d, 0);
    for (int i = 0; i < 6; i++)
        d[16 + 8 * len + i] = 0; /* la cola vuelve el codificador al estado 0 */
    const conv_punct_t *pu = wifi_punct(r);
    int nc = conv_encode(&cc, d, nbits, cb, CONV_TERM_NONE, pu);
    wifi_interleave_table(r, jk);
    for (int s = 0; s < nsym && (s + 1) * r->ncbps <= nc; s++) {
        for (int k = 0; k < r->ncbps; k++)
            fb[jk[k]] = cb[s * r->ncbps + k];
        put_symbol(o, pl, fb, r, wifi_pilot_polarity(s + 1));
    }
    free(d);
    free(cb);
    fft_plan_destroy(pl);
}

/* ---- DSSS ---------------------------------------------------------------- */
static void put_bits_lsb(uint8_t *b, int *n, uint32_t v, int nbits)
{
    for (int i = 0; i < nbits; i++)
        b[(*n)++] = (uint8_t)((v >> i) & 1);
}

/* Pulso de coseno alzado (α=0,5) con τ en periodos de chip. */
static double rc_pulse(double t)
{
    const double a = 0.5;
    double sc = fabs(t) < 1e-9 ? 1.0 : sin(M_PI * t) / (M_PI * t);
    double den = 1.0 - 4.0 * a * a * t * t;
    if (fabs(den) < 1e-6)
        return sc * (M_PI / 4.0) / 1.0; /* límite en |t| = 1/(2α) */
    return sc * cos(M_PI * a * t) / den;
}

static void dsss_frame(iqbuf_t *o, const uint8_t *psdu, int len)
{
    int cap = 128 + 16 + 48 + 8 * len + 8, nb = 0;
    uint8_t *bits = malloc((size_t)cap);
    for (int i = 0; i < 128; i++)
        bits[nb++] = 1;
    for (int i = 0; i < 16; i++)
        bits[nb++] = (uint8_t)((WIFI_SFD_BITS >> (15 - i)) & 1);
    int h0 = nb;
    put_bits_lsb(bits, &nb, 0x0A, 8);        /* SIGNAL: 1 Mb/s en unidades de 100 kb/s */
    put_bits_lsb(bits, &nb, 0x04, 8);        /* SERVICE: relojes enganchados */
    put_bits_lsb(bits, &nb, (uint32_t)(8 * len), 16); /* LENGTH en µs */
    uint16_t crc = wifi_plcp_crc16(bits + h0, 32);
    for (int i = 15; i >= 0; i--)
        bits[nb++] = (uint8_t)((crc >> i) & 1);
    for (int i = 0; i < 8 * len; i++)
        bits[nb++] = (uint8_t)((psdu[i >> 3] >> (i & 7)) & 1);
    lfsr_t l;
    lfsr_init(&l, 7, 0x48, 0x1B, LFSR_MULTIPLICATIVE);
    lfsr_process(&l, bits, nb, bits, 0);
    /* DBPSK (1 = giro de π) y ensanchado Barker. */
    int nchip = nb * 11;
    float *chip = malloc(sizeof(float) * (size_t)nchip);
    float a = 1.0f;
    for (int m = 0; m < nb; m++) {
        if (bits[m])
            a = -a;
        for (int i = 0; i < 11; i++)
            chip[m * 11 + i] = a * WIFI_BARKER[i];
    }
    int ns = (int)((double)nchip * 20.0 / 11.0) + 8;
    iqbuf_reserve(o, ns);
    for (int n = 0; n < ns; n++) {
        double t = (double)n * 11.0 / 20.0; /* en chips */
        int k0 = (int)floor(t);
        double acc = 0;
        for (int k = k0 - 4; k <= k0 + 5; k++)
            if (k >= 0 && k < nchip)
                acc += chip[k] * rc_pulse(t - k);
        iqbuf_push(o, (cf32){(float)(acc * 0.5), 0.0f});
    }
    free(chip);
    free(bits);
}

/* ---- Parámetros ---------------------------------------------------------- */
static void ap_default(ap_t *a, int idx)
{
    memset(a, 0, sizeof *a);
    snprintf(a->ssid, sizeof a->ssid, idx ? "LegacyAP" : "BladeRF-Lab");
    const uint8_t b[6] = {0x02, 0x00, 0x5E, 0x10, 0x00, (uint8_t)(idx + 1)};
    memcpy(a->bssid, b, 6);
    a->channel = 6;
    a->std = idx ? 'b' : 'g';
    snprintf(a->security, sizeof a->security, idx ? "open" : "wpa2");
    a->rate = 6;
    a->frame = 'b';
}

static int parse_params(const char *js, params_t *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    p->nap = 2;
    ap_default(&p->ap[0], 0);
    ap_default(&p->ap[1], 1);
    p->repeat = 2;
    p->gap_us = 300;
    if (!js || !js[0])
        return 0;
    jtok_t t[64];
    int nt = json_parse(js, strlen(js), t, 64);
    if (nt < 1 || t[0].type != J_OBJ) {
        snprintf(err, errlen, "params no es un objeto JSON válido");
        return -1;
    }
    char s[64];
    double v;
    int i;
    ap_t a;
    ap_default(&a, 0);
    char std[4] = "g";
    if ((i = json_obj_get(js, t, 0, "ssid")) >= 0 && json_str(js, t, i, s, sizeof s))
        snprintf(a.ssid, sizeof a.ssid, "%.32s", s);
    if ((i = json_obj_get(js, t, 0, "bssid")) >= 0 && json_str(js, t, i, s, sizeof s)) {
        unsigned b[6];
        if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
            for (int k = 0; k < 6; k++)
                a.bssid[k] = (uint8_t)b[k];
    }
    if ((i = json_obj_get(js, t, 0, "channel")) >= 0 && json_num(js, t, i, &v))
        a.channel = (int)CLAMP(v, 1, 233);
    if ((i = json_obj_get(js, t, 0, "std")) >= 0 && json_str(js, t, i, s, sizeof s))
        snprintf(std, sizeof std, "%.2s", s);
    if ((i = json_obj_get(js, t, 0, "security")) >= 0 && json_str(js, t, i, s, sizeof s))
        snprintf(a.security, sizeof a.security, "%.7s", s);
    if ((i = json_obj_get(js, t, 0, "rate")) >= 0 && json_num(js, t, i, &v))
        a.rate = (int)v;
    if ((i = json_obj_get(js, t, 0, "frame")) >= 0 && json_str(js, t, i, s, sizeof s))
        a.frame = s[0] == 'p' ? 'p' : s[0] == 'd' ? 'd' : 'b';
    bool bv;
    if ((i = json_obj_get(js, t, 0, "ht")) >= 0 && json_bool(js, t, i, &bv))
        a.ht = bv;
    if ((i = json_obj_get(js, t, 0, "repeat")) >= 0 && json_num(js, t, i, &v))
        p->repeat = (int)CLAMP(v, 1, 100);
    if ((i = json_obj_get(js, t, 0, "gap_us")) >= 0 && json_num(js, t, i, &v))
        p->gap_us = (int)CLAMP(v, 50, 100000);
    const wifi_rate_t *r = wifi_rate_by_mbps(a.rate);
    if (!r || r->nbpsc > 2) {
        snprintf(err, errlen, "rate debe ser 6, 9, 12 o 18");
        return -1;
    }
    p->nap = 0;
    for (const char *c = std; *c && p->nap < 2; c++) {
        if (*c != 'g' && *c != 'b')
            continue;
        p->ap[p->nap] = a;
        p->ap[p->nap].std = *c;
        if (p->nap == 1) { /* segunda red: BSSID y SSID distintos */
            p->ap[1].bssid[5] ^= 0x10;
            snprintf(p->ap[1].ssid, sizeof p->ap[1].ssid, "%.28s-b", a.ssid);
        }
        p->nap++;
    }
    if (!p->nap) {
        snprintf(err, errlen, "std debe ser g, b o gb");
        return -1;
    }
    return 0;
}

static int wifi_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    params_t p;
    if (parse_params(params, &p, err, errlen) < 0)
        return -1;
    out->rate = WIFI_FS;
    gap(out, 4000);
    uint8_t psdu[512];
    uint16_t seq = 100;
    for (int rep = 0; rep < p.repeat; rep++)
        for (int k = 0; k < p.nap; k++) {
            int len = build_beacon(&p.ap[k], psdu, seq++);
            if (p.ap[k].std == 'b')
                dsss_frame(out, psdu, len);
            else
                ofdm_frame(out, psdu, len, wifi_rate_by_mbps(p.ap[k].rate));
            gap(out, p.gap_us * 20);
        }
    gap(out, 2000);
    for (int i = 0; i < out->n; i++) { /* margen de seguridad frente al recorte del DAC */
        out->s[i].re = CLAMP(out->s[i].re, -0.9f, 0.9f);
        out->s[i].im = CLAMP(out->s[i].im, -0.9f, 0.9f);
    }
    return 0;
}

const modulator_desc_t MOD_WIFI = {
    .id = "wifi",
    .name = "WiFi beacons",
    .air_ok = false,
    .modulate = wifi_modulate,
};
