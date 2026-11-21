/*
 * profiles.c — Tabla de perfiles (AGENTS.md §5) y su aplicación.
 */
#include "profiles.h"

#include "dsp/channel.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MHZ(x) ((int64_t)((x) * 1e6 + 0.5))

/* El 'spectrum_only' se calcula en profiles_json: sin audio ni decodificadores. */
const profile_t PROFILES[] = {
    {"fm", "FM comercial", "Radiodifusión", MHZ(100.0), 2400000, "wfm", MHZ(100.3), 0,
     {{"rds", MHZ(100.3), true, false}}, 1, false},
    {"airband", "Banda aérea", "Aviación", MHZ(127.2), 2400000, "am", MHZ(127.5), 0, {{0}}, 0, false},
    {"adsb", "ADS-B", "Aviación", MHZ(1090.0), 2000000, NULL, 0, 0, {{"adsb", 0, false, false}}, 1, false},
    {"maritimo", "VHF marina", "Marítimo", MHZ(159.3), 8000000, "nfm", MHZ(156.8), 0,
     {{"ais", MHZ(161.975), false, false}, {"ais", MHZ(162.025), false, false}, {"dsc", MHZ(156.525), false, false}},
     3, false},
    {"acars", "ACARS", "Aviación", MHZ(131.3), 1000000, "am", MHZ(131.725), 0,
     {{"acars", MHZ(131.525), false, false}, {"acars", MHZ(131.725), false, false},
      {"acars", MHZ(131.825), false, false}},
     3, false},
    {"vdl2", "VDL2", "Aviación", MHZ(136.6), 1000000, NULL, 0, 0,
     {{"vdl2", MHZ(136.975), false, false}, {"vdl2", MHZ(136.875), false, false},
      {"vdl2", MHZ(136.775), false, false}, {"vdl2", MHZ(136.725), false, false}},
     4, false},
    {"dab", "DAB / DAB+", "Radiodifusión", MHZ(227.36), 2048000, NULL, 0, 0, {{"dab", 0, false, false}}, 1, false},
    {"busca", "Buscapersonas", "Buscapersonas", MHZ(466.3), 1000000, "nfm", MHZ(466.075), 0,
     {{"pocsag", MHZ(466.075), false, false}, {"flex", MHZ(466.075), false, false}}, 2, false},
    {"aprs", "APRS 2 m", "Radioafición", MHZ(145.0), 1000000, "nfm", MHZ(144.8), 0,
     {{"aprs", MHZ(144.8), false, false}}, 1, false},
    {"ham2m", "2 m", "Radioafición", MHZ(145.0), 2400000, "nfm", MHZ(145.5), 0,
     {{"aprs", MHZ(144.8), false, false}}, 1, false},
    {"ham70", "70 cm", "Radioafición", MHZ(433.0), 2400000, "nfm", MHZ(433.5), 0, {{0}}, 0, false},
    {"dmr", "DMR 70 cm", "Radio digital", MHZ(438.5), 4000000, NULL, 0, 0, {{0}}, 0, false},
    {"ism433", "ISM 433", "Sensores", MHZ(433.42), 2000000, NULL, 0, 0, {{"ism", MHZ(433.92), false, false}}, 1,
     false},
    {"ism868", "ISM 868", "Sensores", MHZ(868.0), 2000000, NULL, 0, 0,
     {{"ism", MHZ(868.3), false, false}, {"ism", MHZ(868.95), false, false}}, 2, false},
    {"lora", "LoRaWAN EU868", "IoT", MHZ(867.7), 2000000, NULL, 0, 0,
     {{"lora", MHZ(868.1), false, false}, {"lora", MHZ(868.3), false, false}, {"lora", MHZ(868.5), false, false}}, 3,
     false},
    {"meshtastic", "Meshtastic EU", "IoT", MHZ(869.2), 1000000, NULL, 0, 0,
     {{"meshtastic", MHZ(869.525), false, false}}, 1, false},
    {"sonda", "Radiosondas", "Meteorología", MHZ(403.0), 6000000, NULL, 0, 0, {{"rs41", 0, false, true}}, 1, false},
    /* LTE es WIDE: el decodificador exige 1,92·2^n Msps (decoders/lte.c, create). 20 Msps no es
     * múltiplo entero de 1,92 y el alta fallaría. 15,36 Msps = 1,92·8 es válido. Ojo: la captura
     * abarca 15,36 MHz, menos que los ~18 MHz ocupados por una portadora de 20 MHz (el SIB1 de
     * banda 20 puede necesitar más tasa: lo indica 'sib1_min_rate_msps' en el estado). */
    {"lte", "LTE banda 20/3", "Celular", MHZ(806.0), 15360000, NULL, 0, 0, {{0}}, 0, false},
    {"nr", "5G n78", "Celular", MHZ(3500.0), 30720000, NULL, 0, 0, {{"nr", 0, false, false}}, 1, false},
    {"gsm", "GSM 900", "Celular", MHZ(942.5), 20000000, NULL, 0, 0, {{0}}, 0, false},
    {"wifi", "WiFi 2,4 GHz", "Redes", MHZ(2437.0), 20000000, NULL, 0, 0, {{"wifi", 0, false, false}}, 1, false},
    {"dvbt", "TDT", "Televisión", MHZ(474.0), 20000000, NULL, 0, 0, {{"dvbt", MHZ(474.0), false, false}}, 1, false},
};
const int PROFILE_COUNT = (int)(sizeof PROFILES / sizeof PROFILES[0]);

const profile_t *profile_find(const char *id)
{
    if (!id)
        return NULL;
    for (int i = 0; i < PROFILE_COUNT; i++)
        if (strcmp(PROFILES[i].id, id) == 0)
            return &PROFILES[i];
    return NULL;
}

void profiles_json(sb_t *sb)
{
    sb_puts(sb, "{\"type\":\"profiles\",\"list\":[");
    for (int i = 0; i < PROFILE_COUNT; i++) {
        const profile_t *p = &PROFILES[i];
        int bw = 0;
        if (p->mode) {
            int m = mode_from_name(p->mode);
            bw = p->bw ? p->bw : (m >= 0 ? mode_default_bw((demod_mode_t)m) : 0);
        }
        sb_puts(sb, i ? ",{\"id\":" : "{\"id\":");
        sb_json_str(sb, p->id);
        sb_puts(sb, ",\"name\":");
        sb_json_str(sb, p->name);
        sb_puts(sb, ",\"group\":");
        sb_json_str(sb, p->group);
        sb_printf(sb, ",\"center\":%lld,\"rate\":%u,\"mode\":", (long long)p->center, p->rate);
        if (p->mode)
            sb_json_str(sb, p->mode);
        else
            sb_puts(sb, "null");
        sb_printf(sb, ",\"chan_freq\":%lld,\"bw\":%d,\"decoders\":[", (long long)p->chan_freq, bw);
        for (int k = 0; k < p->ndec; k++) {
            sb_puts(sb, k ? ",{\"proto\":" : "{\"proto\":");
            sb_json_str(sb, p->dec[k].proto);
            sb_printf(sb, ",\"freq\":%lld,\"follow\":%s,\"auto\":%s}",
                      (long long)(p->dec[k].freq ? p->dec[k].freq : p->center), p->dec[k].follow ? "true" : "false",
                      p->dec[k].autodec ? "true" : "false");
        }
        sb_printf(sb, "],\"spectrum_only\":%s}", (!p->mode && p->ndec == 0) ? "true" : "false");
    }
    sb_puts(sb, "]}");
}

/* Elige la frecuencia real del canal de audio. Si 'chan_freq' cae dentro de la banda capturada
 * [centro ± tasa/2] se respeta. Si no (fuente de fichero: el centro no se puede mover), un canal
 * fuera de banda solo daría ruido y "FUERA -150 dBFS": se coloca en la señal detectada más cercana
 * dentro de banda (el 90 % central, para evitar el filtro analógico) o, sin señales, en
 * centro + tasa/4, lejos del pico de continua del centro. Devuelve true si hubo que moverlo. */
bool profile_pick_audio_freq(int64_t chan_freq, int64_t center, double rate, const int64_t *sig_hz, int nsig,
                             int64_t *out)
{
    double half = rate / 2.0;
    if (chan_freq >= center - half && chan_freq <= center + half) {
        *out = chan_freq;
        return false;
    }
    int64_t best = center + (int64_t)(rate / 4.0);
    int64_t dmin = INT64_MAX;
    for (int i = 0; sig_hz && i < nsig; i++) {
        if (sig_hz[i] < center - 0.45 * rate || sig_hz[i] > center + 0.45 * rate)
            continue;
        int64_t d = llabs(sig_hz[i] - chan_freq);
        if (d < dmin) {
            dmin = d;
            best = sig_hz[i];
        }
    }
    *out = best;
    return true;
}

int profile_apply(const profile_t *p, engine_t *e, dec_host_t *h, int client, char *err, size_t errlen)
{
    err[0] = '\0';
    int ret = 0;
    dec_host_auto_clear(h); /* antes de quitar: que el hilo auto no repueble */
    dec_host_remove_owner(h, 1);

    engine_cfg_t c;
    engine_get_config(e, &c);
    c.center_hz = p->center;
    c.rate = p->rate;
    c.rf_bw = 0; /* ancho de banda analógico automático para la nueva tasa */
    engine_set_profile(e, p->id);
    engine_request(e, &c, CFG_CENTER | CFG_RATE | CFG_RFBW);

    chan_params_t cp;
    if (p->mode && client >= 0 && engine_get_channel(e, client, &cp)) {
        int m = mode_from_name(p->mode);
        if (m >= 0) {
            cp.mode = (demod_mode_t)m;
            /* Banda real tras sanear: con fichero es la del fichero, no la del perfil. */
            engine_cfg_t real;
            engine_get_config(e, &real);
            int64_t f;
            /* Señales que el detector ya ve (fuente de fichero: la banda no cambia, así que valen) */
            det_signal_t dsg[96];
            int64_t sc = real.center_hz;
            int nd = engine_get_signals(e, dsg, 96, &sc);
            int64_t sig_hz[96];
            for (int i = 0; i < nd; i++)
                sig_hz[i] = sc + (int64_t)llround(dsg[i].offset_hz);
            if (profile_pick_audio_freq(p->chan_freq, real.center_hz, real.rate, nd ? sig_hz : NULL, nd, &f)) {
                snprintf(err, errlen, "el canal de audio del perfil (%.3f MHz) queda fuera de la banda capturada; se coloca en %.3f MHz",
                         (double)p->chan_freq / 1e6, (double)f / 1e6);
                LOG_W("Perfil %s: %s", p->id, err);
                ret = 1; /* aviso, no error */
            }
            cp.freq_hz = f;
            cp.bw_hz = p->bw ? p->bw : mode_default_bw(cp.mode);
            engine_set_channel(e, client, &cp);
        }
    }
    for (int k = 0; k < p->ndec; k++) {
        const prof_dec_t *d = &p->dec[k];
        if (d->autodec) {
            /* Sobre las señales que detecte el detector dentro de la banda útil del
             * perfil (80 % central: los bordes caen en el filtro analógico). */
            int64_t half = (int64_t)(0.4 * p->rate);
            if (dec_host_auto_add_rule(h, d->proto, p->center - half, p->center + half) < 0)
                LOG_W("Perfil %s: no se pudo activar auto %s", p->id, d->proto);
            continue;
        }
        char derr[160];
        int64_t f = d->freq ? d->freq : p->center;
        if (dec_host_add(h, d->proto, f, NULL, 1, derr, sizeof derr) < 0)
            LOG_W("Perfil %s: no se pudo crear %s: %s", p->id, d->proto, derr);
    }
    return ret;
}
