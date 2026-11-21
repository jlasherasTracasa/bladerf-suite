/*
 * wifi.c — Decodificador de balizas WiFi 802.11b/g/a (solo difusión).
 *
 * ALCANCE ÉTICO (AGENTS.md §0.12): solo se analizan tramas de gestión de
 * DIFUSIÓN: Beacon (subtipo 8) y Probe Response (subtipo 5), de las que solo se
 * leen el BSSID (dirección 3) y los elementos de información públicos (SSID,
 * tasas, canal, RSN/WPA). El receptor decodifica primero ÚNICAMENTE el prefijo
 * del PSDU (unos 8 bytes) para leer el campo "tipo/subtipo" del control de
 * trama; si la trama no es una baliza o una respuesta de sondeo (datos,
 * control, peticiones de sondeo…) se descarta ahí mismo, sin desentrelazar ni
 * decodificar el resto. Nunca se lee, guarda ni emite la dirección de destino
 * de una respuesta de sondeo (es la MAC de un cliente), ni ninguna dirección
 * que no sea el BSSID; no se descifra nada.
 *
 * Cadena de recepción (20 Msps exactos, muestras Q11):
 *
 *  0. Detector de energía por bloques de 1 µs (20 muestras). El suelo de ruido es una
 *     media móvil EXPONENCIAL (no un mínimo: un seguidor "baja rápido, sube lento" se
 *     clava en el cuantil ~1 % de la potencia y sesga el suelo ~2 dB hacia abajo, ver
 *     docs/protocolos/wifi.md §Detector) que solo se actualiza con bloques sin señal.
 *     Se abre una ráfaga con DOS criterios sobre medias de bloques consecutivas:
 *      - rápido: media de 8 µs > 1,8× el suelo (+2,6 dB; σ de la media = 1/√160 = 8 %,
 *        o sea > 9σ en solo ruido gaussiano) para señales fuertes;
 *      - lento: media de 64 µs > 1,35× (+1,3 dB; σ = 2,8 %, > 12σ) para señales débiles:
 *        una baliza DSSS a 1 Mb/s se decodifica con SNR por muestra de ~0 dB (Barker
 *        da 10,4 dB de ganancia), muy por debajo del antiguo umbral de +6 dB.
 *     Se cierra tras 64 µs con la media de 16 µs bajo 1,2× el suelo (una baliza débil
 *     fluctúa ±40 % y se partía en trozos) o, si la ráfaga era fuerte, tras 3 bloques
 *     bajo el 25 % de su nivel. Las ráfagas de < 24 µs se
 *     descartan (no caben ni un preámbulo OFDM más SIGNAL) y las que llegan a
 *     BURST_MAX no se procesan: se re-aprende el suelo (cambio de ganancia, interferencia
 *     continua) en vez de disparar cientos de miles de ráfagas falsas. Todo lo demás
 *     solo se hace SOBRE LA RÁFAGA (pre-roll de 2048 muestras = 102 µs, por el retardo
 *     de la ventana lenta), lo que mantiene el coste bajo aunque el canal esté ocupado.
 *  1. OFDM 802.11a/g (Bloessl et al., "An IEEE 802.11a/g/p OFDM Receiver for GNU
 *     Radio", SRIF 2013), 6-54 Mb/s:
 *      a. Schmidl-Cox sobre el STF: P(n)=Σ x[n+k]·x*[n+k+16], M=|P|²/R²; el
 *         umbral M>0,5 sostenido marca el inicio. CFO grueso
 *         f = −arg(P)·fs/(2π·16) (rango ±625 kHz).
 *      b. Correlación cruzada con el símbolo largo (LTF) conocido → inicio de
 *         símbolo; CFO fino con el desfase entre los dos LTF (retardo 64,
 *         arg(·)·fs/(2π·64)).
 *      c. Estimación de canal H(k) = media(Y₁,Y₂)·L(k); ecualización
 *         Z = Y·conj(H)/(|H|^2) y seguimiento por pilotos en cada símbolo: ajuste por
 *         mínimos cuadrados de fase = θ₀ + b·k (θ₀ = error de fase común/CFO
 *         residual, b = deriva de muestreo). Las ventanas de FFT arrancan 4
 *         muestras antes del final del prefijo cíclico para tolerar eco.
 *      d. SIGNAL (BPSK 1/2): Viterbi, paridad par, RATE, LENGTH.
 *      e. Prefijo del PSDU → control de trama; si es baliza/probe-resp se
 *         decodifica entero: desentrelazado, reinserción de borrados (2/3 y 3/4), Viterbi K=7
 *         (133,171) con cola, desaleatorizado (la semilla sale de los 7 primeros
 *         bits del campo SERVICE, que son ceros), FCS = CRC-32.
 *  2. DSSS 802.11b a 1 Mb/s (si no hay STF): Barker-11 con filtro adaptado al chip
 *     (coseno alzado, 6 muestras por chip) en las posiciones p + i·20/11; fase de símbolo
 *     por búsqueda en ventanas con señal; seguimiento de reloj temprano/tardío (±100 ppm =
 *     1 muestra/500 símbolos); DBPSK diferencial con CFO por arg(Σd²)/2; desaleatorizador
 *     autosincronizado z⁻⁷+z⁻⁴+1; SFD 0xF3A0 precedido de unos; cabecera PLCP con CRC-16;
 *     PSDU con FCS CRC-32. Sensibilidad medida: balizas reales a −3,4 dB de SNR por muestra.
 *
 * Una red genera un evento al verse por primera vez o cuando cambian su SSID,
 * canal, seguridad o estándar. state_json lista las redes vistas (caducan a los
 * 300 s). air_ok=false para el modulador.
 *
 * LIMITACIONES: OFDM 6-54 Mb/s y DSSS 1 Mb/s; no 2/5,5/11 Mb/s (CCK); no se
 * recombinan ráfagas pegadas (SIFS); sin 802.11n/ac/ax salvo identificar su IE.
 * Validado con IQ real DSSS (sigidwiki y captura propia de Pamplona); OFDM solo sintético:
 * ver docs/protocolos/wifi.md.
 */
#include "decoder.h"

#include "wifi_phy.h"
#include "../dsp/bits.h"
#include "../dsp/crc.h"
#include "../dsp/fec.h"
#include "../dsp/fft.h"
#include "../util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIFI_DEBUG
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif
#define BLK 20                 /* 1 µs */
#define PRE 2048               /* pre-roll (potencia de 2) */
#define BURST_MAX 200000       /* 10 ms */
#define MAX_NETS 64
#define NET_TTL 300.0
#define MAX_PSDU 1500
#define SOFT_CAP (520 * 288)
#define PMIN 1e-8f
#define MIN_BLK 24             /* ráfaga mínima (µs) */
#define DELTA 4                /* adelanto de la ventana FFT dentro del GI */

typedef struct {
    bool used;
    uint8_t bssid[6];
    char ssid[40];
    int channel;
    char sec[28], std[8], phy[16], rates[64], country[4];
    double rssi, snr, first, last, interval_ms;
    uint32_t sig, count;
} wnet_t;

typedef struct {
    double fs;
    int64_t center_hz;
    fft_plan_t *fft;
    viterbi_t *vit;
    conv_code_t code;
    crc_t crc32;
    /* detector de ráfagas */
    cf32 ring[PRE];
    int rpos;
    cf32 *buf, *work;
    int n;
    bool inburst;
    int bfill, lo, warm, hold, quiet;
    float psum, noise;
    float pw[64];                /* potencias de los últimos 64 bloques */
    int pwi;
    double s8, s16, s64;         /* sumas móviles de 8, 16 y 64 bloques */
    float level;                 /* mayor media de 8 µs de la ráfaga en curso */
    float *bp;                   /* potencia de cada bloque de la ráfaga (para la mediana) */
    int burst_blocks;
    uint64_t short_bursts, overlong;
    double t_now;                /* hora de la muestra siguiente */
    int64_t abs_n;               /* muestras consumidas desde el principio (índice absoluto) */
    int64_t call0;               /* abs_n al empezar la llamada en curso a process_raw */
    int sp0, sp1;                /* inicio y fin de la trama dentro de buf (muestras; −1 = ?) */
    double snr_db;               /* SNR de la ráfaga en curso */
    float dcr, dci;              /* estimación de la continua del receptor (se resta) */
    /* DSSS */
    cf32 *z;
    float *soft;
    uint8_t *bits;
    /* ráfagas y contadores */
    wnet_t nets[MAX_NETS];
    uint64_t bursts, ofdm_hdr, dsss_hdr, frames, fcs_err, skipped;
    dec_emit_fn emit;
    void *ctx;
} wifi_t;

/* ======================================================================== */
/*  Utilidades                                                              */
/* ======================================================================== */
static void *wifi_create(const dec_cfg_t *cfg, char *err, size_t errlen)
{
    if (fabs(cfg->rate - WIFI_FS) > 1.0) {
        snprintf(err, errlen, "requiere exactamente 20 Msps");
        return NULL;
    }
    wifi_t *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->fs = cfg->rate;
    w->center_hz = cfg->center_hz;
    w->fft = fft_plan_create(64);
    w->code = wifi_conv_code();
    w->vit = viterbi_create(&w->code, 8 * (MAX_PSDU + 4) + 64);
    crc_init(&w->crc32, crc_preset("CRC-32"));
    w->buf = malloc(sizeof(cf32) * (BURST_MAX + PRE + 64));
    w->work = malloc(sizeof(cf32) * (BURST_MAX + PRE + 64));
    w->bp = malloc(sizeof(float) * ((BURST_MAX + PRE) / BLK + 64));
    w->z = malloc(sizeof(cf32) * (BURST_MAX / 20 + 64));
    w->soft = malloc(sizeof(float) * SOFT_CAP);
    w->bits = malloc((size_t)(BURST_MAX / 20 + 64));
    if (!w->fft || !w->vit || !w->buf || !w->work || !w->bp || !w->z || !w->soft || !w->bits) {
        snprintf(err, errlen, "sin memoria");
        free(w->buf); free(w->work); free(w->bp); free(w->z); free(w->soft); free(w->bits);
        if (w->fft) fft_plan_destroy(w->fft);
        if (w->vit) viterbi_destroy(w->vit);
        free(w);
        return NULL;
    }
    return w;
}

static void wifi_destroy(void *st)
{
    wifi_t *w = st;
    if (!w)
        return;
    fft_plan_destroy(w->fft);
    viterbi_destroy(w->vit);
    free(w->buf); free(w->work); free(w->bp); free(w->z); free(w->soft); free(w->bits);
    free(w);
}

/* Copia sanea a UTF-8 válido: los bytes ilegibles se sustituyen por '?'. */
static void utf8_clean(const uint8_t *s, int n, char *out, size_t cap)
{
    size_t o = 0;
    for (int i = 0; i < n && o + 5 < cap;) {
        uint8_t c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n && !(c < 0x20 || c == 0x7F);
        for (int k = 1; ok && k < len; k++)
            ok = (s[i + k] & 0xC0) == 0x80;
        if (ok) {
            for (int k = 0; k < len; k++)
                out[o++] = (char)s[i + k];
            i += len;
        } else {
            out[o++] = '?';
            i++;
        }
    }
    out[o] = '\0';
}

static int freq_to_channel(int64_t f)
{
    if (f >= 2400000000LL && f <= 2500000000LL)
        return f == 2484000000LL ? 14 : (int)((f - 2407000000LL) / 5000000LL);
    if (f >= 5000000000LL && f <= 5900000000LL)
        return (int)((f - 5000000000LL) / 5000000LL);
    return 0;
}

/* ======================================================================== */
/*  MAC: baliza / respuesta de sondeo                                       */
/* ======================================================================== */
static void parse_rsn(const uint8_t *p, int len, bool *sae, bool *eap, bool *psk)
{
    /* versión(2) grupo(4) nº pareados(2) lista(4·n) nº AKM(2) lista(4·n) */
    if (len < 10)
        return;
    int n = p[6] | (p[7] << 8), o = 8 + 4 * n;
    if (o + 2 > len)
        return;
    int m = p[o] | (p[o + 1] << 8);
    o += 2;
    for (int i = 0; i < m && o + 4 <= len; i++, o += 4)
        if (p[o] == 0x00 && p[o + 1] == 0x0F && p[o + 2] == 0xAC) {
            int t = p[o + 3];
            if (t == 8 || t == 9)
                *sae = true;
            else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12)
                *eap = true;
            else if (t == 2 || t == 4 || t == 6)
                *psk = true;
        }
}

static void net_json(wifi_t *w, sb_t *sb, const wnet_t *n, double now, bool full_event)
{
    char mac[20];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", n->bssid[0], n->bssid[1], n->bssid[2], n->bssid[3],
             n->bssid[4], n->bssid[5]);
    sb_puts(sb, "\"ssid\":");
    sb_json_str(sb, n->ssid);
    sb_printf(sb, ",\"bssid\":\"%s\",\"channel\":%d,\"band\":\"%s\",\"security\":", mac, n->channel,
              w->center_hz > 4000000000LL ? "5" : "2.4");
    sb_json_str(sb, n->sec);
    sb_puts(sb, ",\"std\":");
    sb_json_str(sb, n->std);
    sb_puts(sb, ",\"phy\":");
    sb_json_str(sb, n->phy);
    sb_puts(sb, ",\"rates\":");
    sb_json_str(sb, n->rates);
    sb_printf(sb, ",\"freq\":%lld,\"rssi\":%.1f,\"snr\":%.1f,\"beacon_ms\":%.1f,\"count\":%u",
              (long long)w->center_hz, n->rssi, n->snr, n->interval_ms, n->count);
    if (n->country[0])
        sb_printf(sb, ",\"country\":\"%s\"", n->country);
    if (!full_event)
        sb_printf(sb, ",\"last\":%.3f,\"age\":%.0f", n->last, now - n->last);
}

static void mac_frame(wifi_t *w, const uint8_t *f, int len, const char *phy, double rssi_db)
{
    /* Solo llegan aquí tramas con FCS correcto y subtipo baliza / probe-resp. */
    if (len < 24 + 12 + 4)
        return;
    w->frames++;
    bool beacon = ((f[0] >> 4) == 8);
    wnet_t t;
    memset(&t, 0, sizeof t);
    memcpy(t.bssid, f + 16, 6); /* dirección 3 = BSSID; la dirección 1 NO se lee */
    int cap = f[34] | (f[35] << 8);
    t.interval_ms = (f[32] | (f[33] << 8)) * 1.024;
    bool have_ssid = false, rsn = false, wpa = false, sae = false, eap = false, psk = false, ht = false,
         vht = false, he = false, ds = false;
    int ch_ht = 0;
    char *rp = t.rates;
    for (int i = 36; i + 2 <= len - 4;) {
        int id = f[i], l = f[i + 1];
        const uint8_t *d = f + i + 2;
        if (i + 2 + l > len - 4)
            break;
        switch (id) {
        case 0:
            utf8_clean(d, MIN(l, 32), t.ssid, sizeof t.ssid);
            have_ssid = true;
            break;
        case 1:
        case 50:
            for (int k = 0; k < l && rp < t.rates + sizeof t.rates - 8; k++) {
                int r = d[k] & 0x7F;
                if (r == 0x7F)
                    continue; /* selector BSS membership */
                rp += snprintf(rp, 8, "%s%g", rp == t.rates ? "" : ",", r * 0.5);
            }
            break;
        case 7:
            if (l >= 2 && d[0] >= 'A' && d[0] <= 'Z' && d[1] >= 'A' && d[1] <= 'Z') {
                t.country[0] = (char)d[0];
                t.country[1] = (char)d[1];
            }
            break;
        case 3:
            if (l >= 1) {
                t.channel = d[0];
                ds = true;
            }
            break;
        case 45: ht = true; break;
        case 61: if (l >= 1) ch_ht = d[0]; break;
        case 191: vht = true; break;
        case 255: if (l >= 1 && d[0] == 35) he = true; break;
        case 48:
            rsn = true;
            parse_rsn(d, l, &sae, &eap, &psk);
            break;
        case 221:
            if (l >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 1)
                wpa = true;
            break;
        default: break;
        }
        i += 2 + l;
    }
    if (!ds)
        t.channel = ch_ht ? ch_ht : freq_to_channel(w->center_hz);
    if (!have_ssid)
        return;
    if (!t.ssid[0])
        snprintf(t.ssid, sizeof t.ssid, "(oculta)");
    if (rsn) {
        const char *s = sae && (psk || !eap) ? (psk ? "WPA2/WPA3" : "WPA3") : eap ? "WPA2-EAP" : "WPA2";
        snprintf(t.sec, sizeof t.sec, "%s%s", s, wpa ? "+WPA" : "");
    } else if (wpa) {
        snprintf(t.sec, sizeof t.sec, "WPA");
    } else {
        snprintf(t.sec, sizeof t.sec, "%s", (cap & 0x10) ? "WEP" : "open");
    }
    bool ofdm = phy[0] == 'O';
    snprintf(t.std, sizeof t.std, "%s",
             he ? "ax" : vht ? "ac" : ht ? "n" : !ofdm ? "b" : w->center_hz > 4000000000LL ? "a" : "g");
    snprintf(t.phy, sizeof t.phy, "%s", phy);
    t.rssi = rssi_db;
    t.snr = w->snr_db;
    /* hash FNV-1a de lo que, si cambia, merece un evento nuevo */
    uint32_t h = 2166136261u;
    for (const char *c = t.ssid; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    for (const char *c = t.sec; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    for (const char *c = t.std; *c; c++)
        h = (h ^ (uint8_t)*c) * 16777619u;
    h = (h ^ (uint32_t)t.channel) * 16777619u;
    t.sig = h;

    wnet_t *n = NULL, *slot = NULL;
    for (int i = 0; i < MAX_NETS; i++) {
        wnet_t *c = &w->nets[i];
        if (c->used && !memcmp(c->bssid, t.bssid, 6)) {
            n = c;
            break;
        }
        /* hueco libre o, si no hay, la red más antigua */
        if (!slot || (slot->used && (!c->used || c->last < slot->last)))
            slot = c;
    }
    wnet_t *old = slot;
    bool is_new = false, changed = false;
    if (!n) {
        n = old;
        is_new = true;
        *n = t;
        n->used = true;
        n->first = w->t_now;
        n->count = 0;
    } else {
        changed = n->sig != t.sig;
        double c = n->rssi, snr_old = n->snr;
        uint32_t cnt = n->count;
        double first = n->first;
        *n = t;
        n->used = true;
        n->first = first;
        n->count = cnt;
        n->rssi = 0.7 * c + 0.3 * rssi_db;
        n->snr = 0.7 * snr_old + 0.3 * w->snr_db;
    }
    n->count++;
    n->last = w->t_now;
    if (is_new || changed) {
        char text[200];
        snprintf(text, sizeof text, "%s %02X:%02X:%02X:%02X:%02X:%02X canal %d %s %s %.0f dBFS", n->ssid,
                 n->bssid[0], n->bssid[1], n->bssid[2], n->bssid[3], n->bssid[4], n->bssid[5], n->channel, n->sec,
                 n->std, n->rssi);
        sb_t sb = {0};
        sb_puts(&sb, "{\"text\":");
        sb_json_str(&sb, text);
        sb_printf(&sb, ",\"kind\":\"%s\",\"event\":\"%s\",", beacon ? "beacon" : "probe_resp",
                  is_new ? "new" : "changed");
        net_json(w, &sb, n, w->t_now, true);
        sb_puts(&sb, "}");
        int64_t base = w->abs_n - w->n - w->call0; /* buf[0] respecto al inicio de la llamada */
        if (w->sp0 >= 0)
            dec_emit_span(w->emit, w->ctx, base + w->sp0, w->sp1 >= w->sp0 ? base + w->sp1 : -1, sb.data, sb.len);
        else
            w->emit(w->ctx, sb.data, sb.len);
        sb_free(&sb);
    }
}

/* Tipo/subtipo del primer byte del control de trama: ¿baliza o probe-resp? */
static bool fc_is_broadcast_mgmt(uint8_t fc0)
{
    int version = fc0 & 3, type = (fc0 >> 2) & 3, sub = fc0 >> 4;
    return version == 0 && type == 0 && (sub == 8 || sub == 5);
}

/* Desaleatoriza (802.11a/g): la semilla sale de los 7 primeros bits de SERVICE. */
static void ofdm_descramble(uint8_t *b, int n)
{
    if (n < 8)
        return;
    uint32_t st = 0;
    for (int i = 0; i < 7; i++)
        st |= (uint32_t)(b[i] & 1) << (6 - i); /* bit (k-1) = muestra retrasada k */
    lfsr_t l;
    lfsr_init(&l, 7, 0x48, st, LFSR_ADDITIVE);
    for (int i = 0; i < 7; i++)
        b[i] = 0;
    for (int i = 7; i < n; i++)
        b[i] = (uint8_t)lfsr_bit(&l, b[i], 0);
}

/* ======================================================================== */
/*  OFDM                                                                    */
/* ======================================================================== */
typedef struct {
    cf32 hinv[64];  /* conj(H)/|H|^2 (con regularización) */
    float wgt[64];  /* |H|²/media */
} chan_t;

/* Demodula un símbolo: devuelve I/Q ecualizados y rotados de los 48 datos y el peso de
 * fiabilidad de cada uno (|H|²/media: una subportadora en un nulo del canal vale poco). */
static void ofdm_symbol(wifi_t *w, const cf32 *y, int start, int pol, const chan_t *ch, float *si, float *sq,
                        float *wt)
{
    cf32 X[64];
    memcpy(X, y + start, sizeof X);
    fft_exec(w->fft, X);
    cf32 q[4];
    cf32 sum = {0, 0};
    for (int p = 0; p < 4; p++) {
        int b = wifi_bin(WIFI_PILOT_K[p]);
        cf32 z = {X[b].re * ch->hinv[b].re - X[b].im * ch->hinv[b].im,
                  X[b].re * ch->hinv[b].im + X[b].im * ch->hinv[b].re};
        float s = (float)(WIFI_PILOT_V[p] * pol);
        q[p] = (cf32){z.re * s, z.im * s};
        sum.re += q[p].re;
        sum.im += q[p].im;
    }
    float th0 = atan2f(sum.im, sum.re), c0 = cosf(th0), s0 = sinf(th0), num = 0;
    for (int p = 0; p < 4; p++) {
        float re = q[p].re * c0 + q[p].im * s0, im = q[p].im * c0 - q[p].re * s0; /* q·e^{-jθ0} */
        num += (float)WIFI_PILOT_K[p] * atan2f(im, re);
    }
    float bsl = num / 980.0f; /* Σk² = 2(21²+7²) */
    for (int i = 0; i < 48; i++) {
        int k = wifi_data_carrier(i), b = wifi_bin(k);
        cf32 z = {X[b].re * ch->hinv[b].re - X[b].im * ch->hinv[b].im,
                  X[b].re * ch->hinv[b].im + X[b].im * ch->hinv[b].re};
        float a = th0 + bsl * (float)k, c = cosf(a), s = sinf(a);
        si[i] = z.re * c + z.im * s;
        sq[i] = z.im * c - z.re * s;
        wt[i] = ch->wgt[b];
    }
}

/* Bits blandos de un nivel PAM x (ya escalado a niveles enteros ±1, ±3, ±5, ±7) con la
 * constelación de Gray de §17.3.5.8 (aproximación max-log por tramos): el primer bit
 * es el signo, el segundo vale 1 cerca del centro y el tercero distingue {±3,±5} de {±1,±7}.
 * Convención de fec.h: signo positivo = bit 1. */
static void pam_soft(float x, int m, float *o)
{
    o[0] = x;
    if (m > 1)
        o[1] = (m == 2 ? 2.0f : 4.0f) - fabsf(x);
    if (m > 2)
        o[2] = 2.0f - fabsf(fabsf(x) - 4.0f);
}

/* Escribe en out[ncbps] los bits blandos desentrelazados de un símbolo. La
 * subportadora i aporta los bits [i·nbpsc, (i+1)·nbpsc): mitad para I y mitad para Q. */
static void soft_symbol(const wifi_rate_t *r, const int *jk, const float *si, const float *sq, const float *wt,
                        float *out)
{
    static const float INV[7] = {0, 1.0f, 1.41421356f, 0, 3.16227766f, 0, 6.48074070f}; /* 1, √2, √10, √42 */
    float in[288];
    int m = r->nbpsc == 1 ? 1 : r->nbpsc / 2;
    float g = INV[r->nbpsc];
    for (int i = 0; i < 48; i++) {
        pam_soft(si[i] * g, m, in + i * r->nbpsc);
        if (r->nbpsc > 1)
            pam_soft(sq[i] * g, m, in + i * r->nbpsc + m);
        /* el peso se aplica DESPUÉS de las no linealidades (umbrales de 16/64-QAM) */
        for (int j = 0; j < r->nbpsc; j++)
            in[i * r->nbpsc + j] *= wt[i];
    }
    for (int k = 0; k < r->ncbps; k++)
        out[k] = in[jk[k]];
}

static void rotate(const cf32 *x, cf32 *y, int n, double f, double fs, int from)
{
    double w = -2.0 * M_PI * f / fs;
    cf32 st = {(float)cos(w), (float)sin(w)}, ph = {(float)cos(w * from), (float)sin(w * from)};
    for (int i = 0; i < n; i++) {
        float re = x[i].re * ph.re - x[i].im * ph.im, im = x[i].re * ph.im + x[i].im * ph.re;
        y[i] = (cf32){re, im};
        float pr = ph.re * st.re - ph.im * st.im, pi = ph.re * st.im + ph.im * st.re;
        ph = (cf32){pr, pi};
        if ((i & 255) == 255) {
            float m = 1.0f / sqrtf(ph.re * ph.re + ph.im * ph.im);
            ph.re *= m;
            ph.im *= m;
        }
    }
}

static cf32 cdot_conj(const cf32 *a, const cf32 *b, int n) /* Σ a·conj(b) */
{
    float re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        re += a[i].re * b[i].re + a[i].im * b[i].im;
        im += a[i].im * b[i].re - a[i].re * b[i].im;
    }
    return (cf32){re, im};
}

/* Decodifica bits (scrambled) con Viterbi y devuelve los bytes del PSDU. */
static int ofdm_viterbi(wifi_t *w, const wifi_rate_t *r, int ninfo, conv_term_t term, uint8_t *out_bits)
{
    const conv_punct_t *pu = wifi_punct(r);
    int ncod = wifi_coded_bits(ninfo + (term == CONV_TERM_ZERO ? 6 : 0), pu);
    return viterbi_decode_f(w->vit, w->soft, ncod, ninfo, term, pu, out_bits);
}

/* Devuelve 1 si había un preámbulo OFDM (aunque la trama no se aceptase). */
static int ofdm_try(wifi_t *w, const cf32 *x, int n, double rssi_db)
{
    const int L = 16, W = 48;
    if (n < 700)
        return 0;
    int smax = MIN(n - 520, PRE + 200);
    /* ---- a. Schmidl-Cox sobre el STF ---- */
    cf32 P = {0, 0};
    float R = 0;
    for (int k = 0; k < W; k++) {
        P.re += x[k].re * x[k + L].re + x[k].im * x[k + L].im;
        P.im += x[k].im * x[k + L].re - x[k].re * x[k + L].im;
        R += x[k + L].re * x[k + L].re + x[k + L].im * x[k + L].im;
    }
    int run = 0, s_star = -1;
    for (int s = 0; s <= smax; s++) {
        float m = (P.re * P.re + P.im * P.im) / (R * R + 1e-30f);
        if (m > 0.5f && R > 1e-12f) {
            if (++run >= 24) {
                s_star = s - 23;
                break;
            }
        } else {
            run = 0;
        }
        const cf32 *a = &x[s], *b = &x[s + L];
        const cf32 *a2 = &x[s + W], *b2 = &x[s + W + L];
        P.re += a2->re * b2->re + a2->im * b2->im - (a->re * b->re + a->im * b->im);
        P.im += a2->im * b2->re - a2->re * b2->im - (a->im * b->re - a->re * b->im);
        R += b2->re * b2->re + b2->im * b2->im - (b->re * b->re + b->im * b->im);
    }
    DBG("burst n=%d s_star=%d\n", n, s_star);
    if (s_star < 0)
        return 0;
    w->ofdm_hdr++;
    int sm = s_star + 8;
    if (sm + 64 + L > n)
        return 1;
    P = cdot_conj(&x[sm], &x[sm + L], 64);
    double f1 = -atan2((double)P.im, (double)P.re) * w->fs / (2.0 * M_PI * L);
    /* ---- b. CFO grueso, LTF → inicio de símbolo, CFO fino ---- */
    cf32 *y = w->work;
    rotate(x, y, n, f1, w->fs, 0);
    static cf32 lref[64];
    static bool lref_ok;
    if (!lref_ok) {
        float Lb[64];
        wifi_ltf_bins(Lb);
        fft_plan_t *pl = fft_plan_create(64);
        cf32 t[64];
        for (int i = 0; i < 64; i++)
            t[i] = (cf32){Lb[i], 0};
        fft_exec(pl, t);
        for (int i = 0; i < 64; i++)
            lref[i] = (cf32){t[i].re, -t[i].im}; /* IFFT = conj(FFT(conj(L))), L real */
        fft_plan_destroy(pl);
        lref_ok = true;
    }
    int t1 = -1;
    float best = 0;
    int lo = MAX(0, s_star + 192 - 12), hi = MIN(n - 128 - 80 * 2, s_star + 192 + 48);
    for (int m = lo; m <= hi; m++) {
        cf32 c1 = cdot_conj(&y[m], lref, 64), c2 = cdot_conj(&y[m + 64], lref, 64);
        float v = sqrtf(c1.re * c1.re + c1.im * c1.im) + sqrtf(c2.re * c2.re + c2.im * c2.im);
        if (v > best) {
            best = v;
            t1 = m;
        }
    }
    DBG("f1=%.0f t1=%d (esperado ~%d)\n", f1, t1, s_star + 192);
    if (t1 < 0 || t1 < DELTA || t1 + 128 + 80 * 2 + 16 > n)
        return 1;
    cf32 pf = cdot_conj(&y[t1], &y[t1 + 64], 64);
    double f2 = -atan2((double)pf.im, (double)pf.re) * w->fs / (2.0 * M_PI * 64.0);
    rotate(y, y, n, f2, w->fs, 0);
    /* ---- c. Canal con los dos LTF ---- */
    chan_t ch;
    cf32 Y1[64], Y2[64];
    memcpy(Y1, y + t1 - DELTA, sizeof Y1);
    memcpy(Y2, y + t1 + 64 - DELTA, sizeof Y2);
    fft_exec(w->fft, Y1);
    fft_exec(w->fft, Y2);
    float Lb[64], hsum = 0;
    wifi_ltf_bins(Lb);
    cf32 H[64];
    for (int b = 0; b < 64; b++) {
        H[b] = (cf32){0.5f * (Y1[b].re + Y2[b].re) * Lb[b], 0.5f * (Y1[b].im + Y2[b].im) * Lb[b]};
        hsum += H[b].re * H[b].re + H[b].im * H[b].im;
    }
    float hmean = hsum / 52.0f + 1e-30f;
    /* SNR por subportadora con los dos LTF idénticos: E|Y1−Y2|² = 2N (solo ruido) y
     * E|Y1+Y2|²/4 = |H|² + N/2, de modo que SNR = (E|Y1+Y2|²/4 − N/2)/N. */
    {
        double dn = 0, sg = 0;
        for (int k = -26; k <= 26; k++) {
            if (!k)
                continue;
            int b = wifi_bin(k);
            float dr = Y1[b].re - Y2[b].re, di = Y1[b].im - Y2[b].im, sr = Y1[b].re + Y2[b].re, si2 = Y1[b].im + Y2[b].im;
            dn += (double)dr * dr + (double)di * di;
            sg += ((double)sr * sr + (double)si2 * si2) / 4.0;
        }
        double N = dn / 2.0 / 52.0, S = sg / 52.0 - N / 2.0;
        w->snr_db = 10.0 * log10((S > 1e-12 ? S : 1e-12) / (N > 1e-12 ? N : 1e-12));
    }
    for (int b = 0; b < 64; b++) {
        float p = H[b].re * H[b].re + H[b].im * H[b].im;
        float inv = 1.0f / (p + 1e-3f * hmean);
        ch.hinv[b] = (cf32){H[b].re * inv, -H[b].im * inv};
        ch.wgt[b] = p / hmean;
    }
    /* ---- d. SIGNAL ---- */
    float si[48], sq[48], wt[48];
    const wifi_rate_t *r6 = wifi_rate_by_mbps(6);
    int jk6[288], jk[288];
    wifi_interleave_table(r6, jk6);
    ofdm_symbol(w, y, t1 + 128 + 16 - DELTA, wifi_pilot_polarity(0), &ch, si, sq, wt);
    soft_symbol(r6, jk6, si, sq, wt, w->soft);
    uint8_t sg[24];
    if (viterbi_decode_f(w->vit, w->soft, 48, 18, CONV_TERM_ZERO, NULL, sg) < 0)
        return 1;
    int par = 0;
    for (int i = 0; i < 18; i++)
        par ^= sg[i];
    DBG("f2=%.0f SIGNAL bits:", f2);
    for (int i = 0; i < 18; i++) DBG("%d", sg[i]);
    DBG("\n");
    int code = sg[0] << 3 | sg[1] << 2 | sg[2] << 1 | sg[3], len = 0;
    for (int i = 0; i < 12; i++)
        len |= sg[5 + i] << i;
    const wifi_rate_t *r = wifi_rate_by_code(code);
    if (par || sg[4] || !r || len < 30 || len > MAX_PSDU)
        return 1;
    wifi_interleave_table(r, jk);
    int nsym = (16 + 8 * len + 6 + r->ndbps - 1) / r->ndbps;
    if (t1 + 128 + 80 * (nsym + 1) + 16 > n)
        return 1; /* ráfaga truncada */
    /* ---- e. Prefijo → control de trama → resto ---- */
    int nsym1 = (16 + 64 + r->ndbps - 1) / r->ndbps;
    for (int s = 0; s < nsym1; s++) {
        ofdm_symbol(w, y, t1 + 128 + 80 * (s + 1) + 16 - DELTA, wifi_pilot_polarity(s + 1), &ch, si, sq, wt);
        soft_symbol(r, jk, si, sq, wt, w->soft + s * r->ncbps);
    }
    uint8_t *bits = w->bits;
    if (ofdm_viterbi(w, r, 80, CONV_TERM_NONE, bits) < 0)
        return 1;
    ofdm_descramble(bits, 80);
    uint8_t fc0 = 0;
    for (int i = 0; i < 8; i++)
        fc0 |= (uint8_t)(bits[16 + i] << i);
    if (!fc_is_broadcast_mgmt(fc0)) {
        w->skipped++; /* datos, control, probe-request…: NO se decodifica más */
        return 1;
    }
    for (int s = nsym1; s < nsym; s++) {
        ofdm_symbol(w, y, t1 + 128 + 80 * (s + 1) + 16 - DELTA, wifi_pilot_polarity(s + 1), &ch, si, sq, wt);
        soft_symbol(r, jk, si, sq, wt, w->soft + s * r->ncbps);
    }
    int ninfo = 16 + 8 * len;
    uint8_t *full = malloc((size_t)ninfo + 8);
    if (!full)
        return 1;
    if (ofdm_viterbi(w, r, ninfo, CONV_TERM_ZERO, full) >= 0) {
        ofdm_descramble(full, ninfo);
        uint8_t *psdu = malloc((size_t)len + 1);
        if (psdu) {
            bits_pack_lsb(full + 16, 8 * len, psdu);
            uint32_t fcs = crc_compute(&w->crc32, psdu, (size_t)len - 4);
            uint32_t rx = psdu[len - 4] | psdu[len - 3] << 8 | psdu[len - 2] << 16 | (uint32_t)psdu[len - 1] << 24;
            if (fcs == rx) {
                char phy[16];
                snprintf(phy, sizeof phy, "OFDM %dM", r->mbps);
                w->sp0 = t1 - 192;                           /* primer símbolo modulado: inicio del STF */
                w->sp1 = t1 + 128 + 80 * (nsym + 1);          /* fin del último símbolo de datos */
                mac_frame(w, psdu, len, phy, rssi_db);
            } else {
                w->fcs_err++;
                DBG("OFDM fcs_err t=%.4f rate=%d len=%d snr=%.1f\n", (double)w->abs_n / w->fs, r->mbps, len, w->snr_db);
            }
            free(psdu);
        }
    }
    free(full);
    return 1;
}

/* ======================================================================== */
/*  DSSS 1 Mb/s                                                             */
/* ======================================================================== */
/* Filtro adaptado al chip. Antes cada chip se leía interpolando UNA sola muestra
 * (Catmull-Rom en la posición del chip): con 20 Msps y 11 Mchip/s eso toma solo 11 de las
 * 20 muestras por símbolo y pierde 10·log10(20/11) = 2,6 dB de relación Es/N0 frente al
 * correlador ideal (ruido blanco: Es/N0 = SNR_muestra + 10·log10(20)). A SNR por muestra
 * de −3,4 dB (balizas reales de la captura de Pamplona) esos 2,6 dB son la diferencia entre
 * un BER del descodificador de ~1e-3 y ~1e-4 y, con 3700 bits por baliza, entre 0 y
 * ~50 % de tramas con FCS correcto. Aquí cada chip se integra con el pulso de coseno
 * alzado (α = BK_ALPHA) que usa el transmisor, evaluado en las 6 muestras que rodean a la
 * posición del chip p0 + i·20/11 (índices idx-2…idx+3, ±1,8 chips: el lóbulo principal). Los
 * índices y pesos son los MISMOS en todos los símbolos (20 muestras = entero). Los pesos
 * se normalizan a suma 1 para que la escala de salida no cambie. */
#define BK_ALPHA 0.5
#define BK_TAPS 6
typedef struct {
    int idx[11];             /* índice de la muestra anterior a la posición del chip */
    float wt[11][BK_TAPS];   /* pesos de las muestras idx-2…idx+3 (ya multiplicados por el Barker) */
} bk_t;

static double rc_pulse(double u, double alpha)   /* u en periodos de chip */
{
    double d = 1.0 - 4.0 * alpha * alpha * u * u;
    double sc = fabs(u) < 1e-9 ? 1.0 : sin(M_PI * u) / (M_PI * u);
    if (fabs(d) < 1e-6)
        return M_PI / 4.0 * sc;
    return sc * cos(M_PI * alpha * u) / d;
}

/* Las posiciones de chip son p0 + i·20/11 con p0 múltiplo de 0,25: su parte fraccionaria es
 * siempre un múltiplo de 1/44, así que los pesos del pulso se calculan UNA vez para esas 44
 * fracciones (sin, cos y divisiones por chip costaban más que la correlación). */
#define BK_FR 44
static float g_bkw[BK_FR][BK_TAPS];
static bool g_bkw_ok;

static void bk_tables(void)
{
    const double tc = 20.0 / 11.0;
    for (int k = 0; k < BK_FR; k++) {
        double f = (double)k / BK_FR, h[BK_TAPS], sum = 0;
        for (int j = 0; j < BK_TAPS; j++) {
            h[j] = rc_pulse(((j - 2) - f) / tc, BK_ALPHA);
            sum += h[j];
        }
        for (int j = 0; j < BK_TAPS; j++)
            g_bkw[k][j] = (float)(h[j] / sum);
    }
    g_bkw_ok = true;
}

static void bk_prepare(bk_t *t, double p0)
{
    if (!g_bkw_ok)
        bk_tables();
    const int p44 = (int)lrint(p0 * BK_FR); /* p0 en 1/44 de muestra (exacto para múltiplos de 0,25) */
    for (int i = 0; i < 11; i++) {
        int pos44 = p44 + i * 80; /* i·20/11 = i·80/44 */
        int ix = pos44 / BK_FR, k = pos44 % BK_FR;
        if (pos44 < 0) {
            ix = -((-pos44 + BK_FR - 1) / BK_FR);
            k = pos44 - ix * BK_FR;
        }
        t->idx[i] = ix;
        for (int j = 0; j < BK_TAPS; j++)
            t->wt[i][j] = (float)WIFI_BARKER[i] * g_bkw[k][j];
    }
}

/* Correlación con el Barker-11 del símbolo que empieza 'base' muestras después de p0. */
static cf32 bk_corr(const bk_t *t, const cf32 *x, int base)
{
    float re = 0, im = 0;
    for (int i = 0; i < 11; i++) {
        const cf32 *q = x + base + t->idx[i] - 2;
        for (int k = 0; k < BK_TAPS; k++) {
            re += t->wt[i][k] * q[k].re;
            im += t->wt[i][k] * q[k].im;
        }
    }
    return (cf32){re, im};
}

static inline float mag2(cf32 a) { return a.re * a.re + a.im * a.im; }

static int dsss_try(wifi_t *w, const cf32 *x0, int n, double rssi_db)
{
    if (n < 4600)
        return 0;
    /* El filtro del chip lee hasta 2 muestras antes de la posición: se desplaza el origen 4
     * muestras (las posiciones relativas a x se convierten con +XOFF al final). */
    const int XOFF = 4;
    const cf32 *x = x0 + XOFF;
    n -= XOFF;
    /* ---- fase de símbolo: 80 candidatos de 0,25 muestras sobre 50 símbolos ----
     * La ráfaga lleva 2048 muestras de pre-roll (102 µs) y la trama empieza en cualquier punto
     * de [0, ~PRE+100]: se prueban ventanas de 50 símbolos cada 25 y solo las que tienen
     * potencia > 1,15× el suelo (ruido puro: σ = 3 %), y se elige la de mayor contraste
     * (S_máx / media de las 20 fases gruesas; con ruido u OFDM ronda 1,2-2,1, con Barker alineado > 3). */
    bk_t tb;
    double bestC = 0, p_best = 0, Cw[40], pbw[40];
    int ms = -1, nw = 0, m0w[40];
    int nwin_ok = 0;
    for (int m0 = 20; m0 + 50 + 4 < (n - 64) / 20 && m0 < PRE / 20 + 130 && nwin_ok < 3; m0 += 25) {
        double pw = 0;
        for (int i = 20 * m0; i < 20 * (m0 + 50); i++)
            pw += mag2(x[i]);
        if (pw / (20.0 * 50.0) < 1.15 * w->noise)
            continue;
        /* Como mucho 3 ventanas con señal, la 2.ª y la 3.ª a +50 y +100 símbolos de la primera
         * (el preámbulo dura 128: alguna cae entera dentro). Las ráfagas OFDM débiles que no
         * se pudieron decodificar llegan aquí y no deben costar 7 ventanas cada una. */
        if (nwin_ok++ > 0)
            m0 += 25;
        /* búsqueda gruesa de fase (paso de 1 muestra, 20 candidatos) y afinado de ±0,75 en
         * pasos de 0,25: 26 evaluaciones en vez de 80 (el coste de este bloque dominaba) */
        double bestS = 0, sum = 0, pb = 0;
        for (int c = 0; c < 20; c++) {
            double p0 = 2.0 + c, S = 0;
            bk_prepare(&tb, p0);
            for (int m = m0; m < m0 + 50; m += 2) /* símbolos alternos: 25 de cada ventana */
                S += mag2(bk_corr(&tb, x, 20 * m));
            sum += S;
            if (S > bestS) {
                bestS = S;
                pb = p0;
            }
        }
        double coarse_mean = sum / 20.0;
        for (int c = -3; c <= 3; c++) {
            if (!c)
                continue;
            double p0 = pb + 0.25 * c, S = 0;
            if (p0 < 1.5)
                continue;
            bk_prepare(&tb, p0);
            for (int m = m0; m < m0 + 50; m += 2)
                S += mag2(bk_corr(&tb, x, 20 * m));
            if (S > bestS) {
                bestS = S;
                pb = p0;
            }
        }
        double C = bestS / (coarse_mean + 1e-30);
        if (nw < 40) {
            Cw[nw] = C;
            pbw[nw] = pb;
            m0w[nw++] = m0;
        }
        DBG("win m0=%d C=%.2f\n", m0, C);
        bestC = MAX(bestC, C);
        if (C > 3.0) /* alineado inequívoco: no hace falta mirar más ventanas */
            break;
    }
    if (bestC < 2.3) /* ruido o OFDM: 1,2-2,1; balizas reales a −3 dB: 3,5-6; limpias: 9 */
        return 0;
    /* la ventana válida MÁS TEMPRANA (≥ 85 % del mejor contraste): el SFD cae 128 símbolos
     * tras el inicio de la trama y la búsqueda de SFD arranca 36 símbolos después de ms */
    for (int i = 0; i < nw; i++)
        if (Cw[i] >= 0.85 * bestC) {
            ms = m0w[i];
            p_best = pbw[i];
            break;
        }
    int M = (n - 64) / 20;
    if (M > BURST_MAX / 20)
        M = BURST_MAX / 20;
    /* ---- despreading con seguimiento temprano/tardío (desde m = 20) ---- */
    const double step = 0.25, dl = 0.5;
    double p0 = p_best, acc = 0;
    bk_t tp, te, tl;
    bk_prepare(&tp, p0);
    bk_prepare(&te, p0 - dl);
    bk_prepare(&tl, p0 + dl);
    for (int m = ms; m < M; m++) {
        cf32 z0 = bk_corr(&tp, x, 20 * m);
        w->z[m] = z0;
        if (m >= ms + 20 && ((m - ms) & 3) == 0) {
            /* El reloj deriva ≤ 1 muestra cada ~500 símbolos (±100 ppm): basta mirar temprano/
             * tardío cada 4 símbolos (con ventana de 16 en vez de 64 = misma constante de
             * tiempo en símbolos, umbral 10/4) y el coste de DSSS baja un tercio. */
            float e = sqrtf(mag2(bk_corr(&tl, x, 20 * m))) - sqrtf(mag2(bk_corr(&te, x, 20 * m)));
            acc = acc * (1.0 - 1.0 / 16) + e / (sqrtf(mag2(z0)) + 1e-9);
            if (acc > 2.5 || acc < -2.5) {
                p0 += acc > 0 ? step : -step;
                acc = 0;
                bk_prepare(&tp, p0);
                bk_prepare(&te, p0 - dl);
                bk_prepare(&tl, p0 + dl);
            }
        }
    }
    /* ---- DBPSK: CFO con d² y decisión diferencial ---- */
    cf32 s2 = {0, 0};
    for (int m = ms + 4; m < MIN(M, ms + 80); m++) {
        cf32 d = {w->z[m].re * w->z[m - 1].re + w->z[m].im * w->z[m - 1].im,
                  w->z[m].im * w->z[m - 1].re - w->z[m].re * w->z[m - 1].im};
        s2.re += d.re * d.re - d.im * d.im;
        s2.im += 2 * d.re * d.im;
    }
    double th = 0.5 * atan2((double)s2.im, (double)s2.re);
    float ct = (float)cos(th), st = (float)sin(th);
    uint8_t *bits = w->bits;
    lfsr_t l;
    lfsr_init(&l, 7, 0x48, 0, LFSR_MULTIPLICATIVE);
    bits[ms] = 0;
    for (int m = ms + 1; m < M; m++) {
        cf32 d = {w->z[m].re * w->z[m - 1].re + w->z[m].im * w->z[m - 1].im,
                  w->z[m].im * w->z[m - 1].re - w->z[m].re * w->z[m - 1].im};
        float re = d.re * ct + d.im * st; /* d·e^{-jθ} */
        bits[m] = (uint8_t)lfsr_bit(&l, re < 0, 1);
    }
    /* ---- SFD tras unos de sincronía ---- */
    int sfd = -1;
    for (int m = ms + 36; m + 64 < M; m++) {
        int err = 0;
        for (int i = 0; i < 16 && err < 2; i++)
            err += bits[m + i] != ((WIFI_SFD_BITS >> (15 - i)) & 1);
        if (err > 1)
            continue;
        int ones = 0;
        for (int i = 1; i <= 32; i++)
            ones += bits[m - i];
        if (ones >= 26) {
            sfd = m;
            break;
        }
    }
    DBG("dsss ms=%d t=%.4f n=%d p_best=%.2f M=%d th=%.3f sfd=%d\n", ms, (double)w->abs_n / w->fs, n, p_best, M, th, sfd);
    if (sfd < 0)
        return 0;
    DBG("dsss sfd=%d M=%d th=%.3f\n", sfd, M, th);
    w->dsss_hdr++;
    const uint8_t *h = bits + sfd + 16;
    uint16_t crc = wifi_plcp_crc16(h, 32), rx = 0;
    for (int i = 0; i < 16; i++)
        rx = (uint16_t)(rx << 1 | h[32 + i]);
    if (crc != rx)
        return 1;
    int sig = 0, lus = 0;
    for (int i = 0; i < 8; i++)
        sig |= h[i] << i;
    for (int i = 0; i < 16; i++)
        lus |= h[16 + i] << i;
    int len = lus / 8;
    if (sig != 0x0A || len < 30 || len > MAX_PSDU || sfd + 64 + 8 * len > M)
        return 1;
    const uint8_t *pb = bits + sfd + 64;
    uint8_t fc0 = 0;
    for (int i = 0; i < 8; i++)
        fc0 |= (uint8_t)(pb[i] << i);
    if (!fc_is_broadcast_mgmt(fc0)) {
        w->skipped++;
        return 1;
    }
    uint8_t *psdu = malloc((size_t)len + 1);
    if (!psdu)
        return 1;
    bits_pack_lsb(pb, 8 * len, psdu);
    uint32_t fcs = crc_compute(&w->crc32, psdu, (size_t)len - 4);
    uint32_t r4 = psdu[len - 4] | psdu[len - 3] << 8 | psdu[len - 2] << 16 | (uint32_t)psdu[len - 1] << 24;
    if (fcs == r4)
    {
        w->sp0 = MAX(0, (int)(20.0 * (sfd - 128) + p0) + XOFF); /* SYNC de 128 símbolos de 1 µs antes del SFD */
        w->sp1 = (int)(20.0 * (sfd + 64 + 8 * len) + p0) + XOFF;
        mac_frame(w, psdu, len, "DSSS 1M", rssi_db);
    }
    else {
        w->fcs_err++;
        DBG("DSSS fcs_err t=%.4f len=%d fc0=%02x th=%.3f M=%d snr=%.1f p0=%.2f\n", (double)w->abs_n / w->fs, len, fc0, th, M, w->snr_db, p0);
    }
    free(psdu);
    return 1;
}

/* ======================================================================== */
/*  Detector de ráfagas                                                     */
/* ======================================================================== */
static int cmp_f(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Cierra la ráfaga: la potencia se estima con la MEDIANA de los bloques de 1 µs (robusta
 * frente al retardo de apertura de la ventana lenta y a la cola de cierre, que son ruido). */
static void burst_done(wifi_t *w)
{
    w->bursts++;
    int nb = w->burst_blocks;
    qsort(w->bp, (size_t)nb, sizeof(float), cmp_f);
    double med = w->bp[nb / 2];
    double noise = w->noise > 1e-12f ? w->noise : 1e-12f;
    double pw = med - noise;
    double rssi = 10.0 * log10(pw > 1e-12 ? pw : 1e-12);
    w->snr_db = 10.0 * log10((pw > 1e-12 ? pw : 1e-12) / noise);
    w->sp0 = w->sp1 = -1;
    /* Un tono estrecho (p. ej. un interferente a +9 MHz) también da correlación periódica a
     * retardo 16: si el camino OFDM no produjo nada (ni trama, ni descarte por subtipo, ni
     * FCS malo) se prueba también DSSS. */
    uint64_t before = w->frames + w->skipped + w->fcs_err;
    if (ofdm_try(w, w->buf, w->n, rssi) == 0 || w->frames + w->skipped + w->fcs_err == before)
        dsss_try(w, w->buf, w->n, rssi);
}

static void burst_open(wifi_t *w)
{
    for (int i = 0; i < PRE; i++)
        w->buf[i] = w->ring[(w->rpos + i) & (PRE - 1)];
    w->n = PRE;
    DBG("open t=%.5f nz=%.0f\n", (double)w->abs_n / w->fs, w->noise * 4194304.0);
    w->inburst = true;
    w->lo = 0;
    w->quiet = 0;
    w->burst_blocks = 0;
    w->level = 0;
}

static void block_end(wifi_t *w)
{
    float P = w->psum / BLK;
    w->psum = 0;
    /* medias móviles de 8, 16 y 64 bloques (sumas incrementales) */
    float o64 = w->pw[w->pwi & 63], o16 = w->pw[(w->pwi - 16) & 63], o8 = w->pw[(w->pwi - 8) & 63];
    w->s64 += P - o64;
    w->s16 += P - o16;
    w->s8 += P - o8;
    w->pw[w->pwi & 63] = P;
    w->pwi++;
    if ((w->pwi & 0xFFF) == 0) { /* evita la deriva del acumulador en coma flotante */
        w->s8 = w->s16 = w->s64 = 0;
        for (int i = 0; i < 64; i++) {
            w->s64 += w->pw[i];
            if (i < 16) w->s16 += w->pw[(w->pwi - 1 - i) & 63];
            if (i < 8) w->s8 += w->pw[(w->pwi - 1 - i) & 63];
        }
    }
    float m8 = (float)(w->s8 / 8), m16 = (float)(w->s16 / 16), m64 = (float)(w->s64 / 64);
    if (w->warm < 64) { /* arranque: se guardan 64 µs y el suelo es su mediana */
        w->bp[w->warm++] = P;
        if (w->warm == 64) {
            qsort(w->bp, 64, sizeof(float), cmp_f);
            w->noise = w->bp[32]; /* si había señal, m64 < 0,9·suelo lo corrige enseguida */
        }
        return;
    }
    float nz = MAX(w->noise, PMIN);
    if (w->hold > 0)
        w->hold--;
    if (!w->inburst) {
        /* El suelo se actualiza justo cuando NO se abre ráfaga (complemento de la condición de
         * apertura): un suelo bajo que se quedaba sin actualizar por una puerta más estricta
         * dejaba abierta la ráfaga durante ms de ruido puro. Fuera durante la cola de la
         * ráfaga anterior (hold), que aún contiene señal en las ventanas de 64 µs. */
        if (w->hold == 0 && m8 <= 1.8f * nz && m64 <= 1.35f * nz)
            w->noise += (P - w->noise) * (1.0f / 1024.0f);
        if (m64 < 0.9f * nz) /* el suelo baja de golpe (cambio de ganancia) */
            w->noise += (m64 - w->noise) * (1.0f / 64.0f);
        if (w->hold == 0 && w->pwi >= 64 && (m8 > 1.8f * nz || m64 > 1.35f * nz))
            burst_open(w);
        return;
    }
    w->bp[w->burst_blocks++] = P;
    if (m8 > w->level)
        w->level = m8;
    bool strong = w->level >= 4.0f * nz;
    w->lo = (strong && P < 0.25f * w->level) ? w->lo + 1 : 0;
    /* Ráfaga débil: la potencia de ventanas de 16 µs fluctúa ±40 % incluso sin señal (el ruido
     * real no es blanco ni gaussiano), así que una baliza a SNR < 0 dB se partía en 2-7 trozos.
     * Solo se cierra tras 64 µs seguidos bajo el umbral; si la señal vuelve, sigue la misma. */
    w->quiet = m16 < 1.2f * nz ? w->quiet + 1 : 0;
    bool end = (w->lo >= 3) || (w->quiet >= 64);
    if (w->n + BLK > BURST_MAX + PRE) {
        /* demasiado larga: no es una trama; re-aprende el suelo y no procesa */
        w->overlong++;
        w->noise = m64;
        w->inburst = false;
        w->hold = 256;
    } else if (end) {
        DBG("close t=%.5f nz=%.0f lo=%d quiet=%d lvl=%.0f m16=%.0f\n", (double)w->abs_n / w->fs, w->noise * 4194304.0, w->lo, w->quiet, w->level * 4194304.0, m16 * 4194304.0);
        w->inburst = false;
        w->hold = 64; /* la ventana de 64 µs aún contiene la ráfaga: no reabrir con su cola */
        if (w->burst_blocks >= MIN_BLK)
            burst_done(w);
        else
            w->short_bursts++;
    }
}

static void wifi_process_raw(void *st, const int16_t *iq, int n, double now, dec_emit_fn emit, void *ctx)
{
    wifi_t *w = st;
    w->emit = emit;
    w->ctx = ctx;
    w->t_now = now;
    w->call0 = w->abs_n;
    const float k = 1.0f / 2048.0f;
    int i = 0;
    while (i < n) {
        int m = MIN(n - i, BLK - w->bfill);
        float ps = 0;
        /* Se resta la continua del receptor (pico del LO en la subportadora 0): seguidor de
         * un polo (τ ≈ 4096 muestras = 0,2 ms), mucho más lento que cualquier trama útil. */
        const float ad = 1.0f / 4096.0f;
        float dr = w->dcr, di = w->dci;
        cf32 *d = w->inburst ? w->buf + w->n : NULL;
        for (int j = 0; j < m; j++) {
            float xr = (float)iq[2 * (i + j)] * k, xi = (float)iq[2 * (i + j) + 1] * k;
            dr += (xr - dr) * ad;
            di += (xi - di) * ad;
            float re = xr - dr, im = xi - di;
            if (d)
                d[j] = (cf32){re, im};
            w->ring[(w->rpos + j) & (PRE - 1)] = (cf32){re, im};
            ps += re * re + im * im;
        }
        w->dcr = dr;
        w->dci = di;
        if (d)
            w->n += m;
        w->rpos = (w->rpos + m) & (PRE - 1);
        w->psum += ps;
        w->bfill += m;
        i += m;
        w->abs_n = w->call0 + i;
        w->t_now = now + (double)i / w->fs;
        if (w->bfill == BLK) {
            w->bfill = 0;
            block_end(w);
        }
    }
}

static void wifi_state_json(void *st, sb_t *sb, double now)
{
    wifi_t *w = st;
    (void)now;
    double t = w->t_now;
    sb_puts(sb, "{\"nets\":[");
    bool first = true;
    for (int i = 0; i < MAX_NETS; i++) {
        wnet_t *n = &w->nets[i];
        if (n->used && t - n->last > NET_TTL)
            n->used = false;
        if (!n->used)
            continue;
        sb_puts(sb, first ? "{" : ",{");
        first = false;
        net_json(w, sb, n, t, false);
        sb_puts(sb, "}");
    }
    sb_printf(sb, "],\"bursts\":%llu,\"short\":%llu,\"overlong\":%llu,\"ofdm_hdr\":%llu,\"dsss_hdr\":%llu,\"frames\":%llu,\"fcs_err\":%llu,\"skipped\":%llu,\"noise_dbfs\":%.1f}",
              (unsigned long long)w->bursts, (unsigned long long)w->short_bursts, (unsigned long long)w->overlong, (unsigned long long)w->ofdm_hdr,
              (unsigned long long)w->dsss_hdr,
              (unsigned long long)w->frames, (unsigned long long)w->fcs_err,
              (unsigned long long)w->skipped, 10.0 * log10(w->noise + 1e-12));
}

const decoder_desc_t DEC_WIFI = {
    .id = "wifi",
    .name = "WiFi beacons",
    .group = "Redes",
    .desc = "Balizas 802.11b/g/a: SSID, BSSID, canal.",
    .kind = DEC_WIDE,
    .rate = 0,
    .bw_hz = 0,
    .wide_rate = 20000000,
    .wide_min_rate = 0,
    .freqs = {2437000000LL, 0LL, 0LL, 0LL},
    .create = wifi_create,
    .destroy = wifi_destroy,
    .process_raw = wifi_process_raw,
    .state_json = wifi_state_json,
};
