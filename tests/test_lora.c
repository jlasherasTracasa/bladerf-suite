/*
 * test_lora.c — Decodificador LoRa / LoRaWAN.
 *
 * (1) Modulador → canal (AWGN con SNR medida en el ancho de banda LoRa, CFO de
 *     ±¼ BW, error de reloj de ±100 ppm) → decodificador a 500 kS/s, para SF7..12
 *     con BW 125 kHz y SF 7/9/11 con BW 250 kHz.
 * (2) SNR negativo (SF10 a −10 dB y SF12 a −15 dB, CR 4/8).
 * (3) Solo ruido → 0 eventos; coste de CPU por segundo de señal.
 * (4) Vectores publicados:
 *     - Secuencia de blanqueo LoRa: primeros 24 bytes de whitening_seq de
 *       gr-lora_sdr (EPFL, include/gnuradio/lora_sdr/tables.h), 255 en total.
 *     - Tramas LoRaWAN de brocaar/lorawan (phypayload_test.go,
 *       TestPHYPayloadMACPayloadLoRaWAN10 y ExamplePHYPayload_lorawan10Decode):
 *         40 04030201 80 0100 01 A6946426 15 D6C3B582 (UnconfirmedDataUp,
 *         DevAddr 01020304, FCnt 1, FPort 1) y base64 gAQDAgEDAAAGcwcK4mTU9+EX0sA=
 *         (ConfirmedDataUp, FOptsLen 3, FCnt 0, FPort 10).
 *     NO se dispone de capturas de un chip Semtech: la capa física se prueba
 *     contra el modulador propio (ver docs/protocolos/lora.md, limitaciones).
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/lora_phy.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_LORA;
extern const modulator_desc_t MOD_LORA;

static double cpu_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

/* Genera la señal, la pasa por el canal y devuelve n muestras a 500 kS/s. */
static cf32 *make_signal(const char *params, int bw, double snr_db, double cfo_hz, double ppm, uint32_t seed, int *n_out)
{
    iqbuf_t o = {0};
    char err[200] = "";
    if (MOD_LORA.modulate(params, &o, err, sizeof err) < 0) {
        printf("  modulador falló: %s\n", err);
        *n_out = 0;
        return NULL;
    }
    cf32 *x = malloc(sizeof(cf32) * (size_t)(o.n + o.n / 500 + 16));
    int n = o.n;
    if (ppm != 0)
        n = chan_resample(o.s, o.n, 1.0 + ppm * 1e-6, x);
    else
        memcpy(x, o.s, sizeof(cf32) * (size_t)n);
    iqbuf_free(&o);
    chan_cfo(x, n, 500000.0, cfo_hz);
    if (snr_db < 90) {
        /* SNR en el ancho de banda de la señal: P_n = P_s / SNR dentro de BW, ruido blanco a 500 kHz */
        double sigma = sqrt(0.64 * (500000.0 / bw) / pow(10.0, snr_db / 10.0) / 2.0);
        for (int i = 0; i < n; i++) {
            x[i].re += (float)(sigma * tu_gauss(&seed));
            x[i].im += (float)(sigma * tu_gauss(&seed));
        }
    }
    *n_out = n;
    return x;
}

static int run_case(const char *params, const char *opts, int bw, double snr, double cfo, double ppm, uint32_t seed,
                    evs_t *evs)
{
    int n;
    cf32 *x = make_signal(params, bw, snr, cfo, ppm, seed * 2654435761u + 12345u, &n);
    if (!x)
        return -1;
    run_narrow(&DEC_LORA, x, n, 868100000, opts, evs);
    free(x);
    return 0;
}

int main(void)
{
    /* ---- Blanqueo: tabla publicada de gr-lora_sdr ---- */
    static const uint8_t wh[24] = {0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E,
                                   0xBC, 0x78, 0xF1, 0xE3, 0xC6, 0x8D, 0x1A, 0x34, 0x68, 0xD0, 0xA0, 0x40};
    for (int i = 0; i < 24; i++)
        CHECK_EQ(lora_whitening_byte(i), wh[i]);
    CHECK_EQ(lora_whitening_byte(255), 0xFF); /* periodo 255 */

    /* ---- Cadena de bits: codificación → símbolos coherentes ---- */
    {
        uint8_t pl[5] = {1, 2, 3, 4, 5};
        uint16_t sy[200];
        for (int sf = 7; sf <= 12; sf++)
            for (int cr = 1; cr <= 4; cr++) {
                int n = lora_encode_symbols(pl, 5, sf, cr, true, false, sy, 200);
                CHECK_EQ(n, lora_frame_nsyms(5, sf, cr, true, false));
                for (int i = 0; i < n; i++)
                    CHECK(sy[i] < (1u << sf));
            }
        /* Fórmula del manual SX127x: SF7, CR4/5, 5 B + CRC, cabecera explícita → 8+10 = 18 símbolos más padding */
        CHECK(lora_frame_nsyms(5, 7, 1, true, false) >= 18);
    }

    /* ---- (1) Canal realista: SF7..12, BW 125 kHz, CFO ±BW/4, ±100 ppm, SNR +10 dB ---- */
    int ok_cases = 0, total_cases = 0;
    double cpu_sig = 0, sig_s = 0;
    for (int sf = 7; sf <= 12; sf++)
        for (int k = 0; k < 4; k++) {
            double cfo = (k & 1 ? -1 : 1) * 0.25 * 125000, ppm = (k & 2) ? -100 : 100;
            char p[300];
            snprintf(p, sizeof p, "{\"sf\":%d,\"bw\":125000,\"cr\":%d,\"payload\":\"400403020180010001A694642615D6C3B582\"}", sf,
                     1 + k);
            /* "sf" por defecto (todos) en la mitad de las corridas: se activan los 6 SF a la vez */
            evs_t ev = {0};
            int n = 0;
            cf32 *x = make_signal(p, 125000, 10.0, cfo, ppm, (uint32_t)(sf * 10 + k), &n);
            double t0 = cpu_s();
            run_narrow(&DEC_LORA, x, n, 868100000, (sf + k) % 2 ? "{\"bw\":125000,\"sf\":\"all\"}" : "{\"bw\":125000}", &ev);
            cpu_sig += cpu_s() - t0;
            sig_s += n / 500000.0;
            free(x);
            char needle[40];
            snprintf(needle, sizeof needle, "\"sf\":%d,", sf);
            bool good = evs_count(&ev) == 1 && ev_has(&ev, needle) && ev_has(&ev, "\"crc_ok\":true") &&
                        ev_has(&ev, "\"devaddr\":\"01020304\"") && ev_has(&ev, "\"fcnt\":1,") && ev_has(&ev, "\"fport\":1") &&
                        ev_has(&ev, "\"mtype\":\"UnconfirmedDataUp\"") && ev_has(&ev, "\"bw\":125000");
            total_cases++;
            ok_cases += good;
            if (!good)
                printf("  FALLO SF%d cfo %+.0f ppm %+.0f: %d eventos\n%s", sf, cfo, ppm, evs_count(&ev), ev.data ? ev.data : "");
            if (good && sf == 7 && k == 0)
                printf("  ejemplo: %.*s\n", (int)(strchr(ev.data, '\n') - ev.data), ev.data);
            evs_free(&ev);
        }
    CHECK_EQ(ok_cases, total_cases);

    /* ---- BW 250 kHz ---- */
    {
        static const int sfs[] = {7, 9, 11};
        for (int a = 0; a < 3; a++)
            for (int k = 0; k < 2; k++) {
                int sf = sfs[a];
                double cfo = (k ? -1 : 1) * 0.25 * 250000, ppm = k ? 100 : -100;
                char p[300];
                snprintf(p, sizeof p, "{\"sf\":%d,\"bw\":250000,\"cr\":3,\"payload\":\"400403020180010001A694642615D6C3B582\"}", sf);
                evs_t ev = {0};
                run_case(p, "{\"bw\":250000,\"sf\":\"all\"}", 250000, 10.0, cfo, ppm, (uint32_t)(100 + sf + k), &ev);
                char needle[40];
                snprintf(needle, sizeof needle, "\"sf\":%d,", sf);
                CHECK_EQ(evs_count(&ev), 1);
                CHECK(ev_has(&ev, needle) && ev_has(&ev, "\"bw\":250000") && ev_has(&ev, "\"devaddr\":\"01020304\""));
                evs_free(&ev);
            }
    }

    /* ---- Otro vector LoRaWAN: ConfirmedDataUp con FOpts (brocaar, base64 gAQDAgEDAAAGcwcK4mTU9+EX0sA=) ---- */
    {
        evs_t ev = {0};
        run_case("{\"sf\":8,\"payload\":\"80040302010300000673070AE264D4F7E117D2C0\"}", "{\"sf\":8}", 125000, 12.0, 5000, 0, 7, &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"mtype\":\"ConfirmedDataUp\"") && ev_has(&ev, "\"devaddr\":\"01020304\"") &&
              ev_has(&ev, "\"fopts_len\":3") && ev_has(&ev, "\"fopts\":\"067307\"") && ev_has(&ev, "\"fcnt\":0") &&
              ev_has(&ev, "\"fport\":10") && ev_has(&ev, "\"mic\":\"E117D2C0\""));
        evs_free(&ev);
    }

    /* ---- JoinRequest sintético (estructura del cap. 6.2.4 de LoRaWAN L2) ---- */
    {
        evs_t ev = {0};
        /* MHDR 00 | AppEUI LE | DevEUI LE | DevNonce LE | MIC */
        run_case("{\"sf\":9,\"payload\":\"00080706050403020118171615141312112211AABBCCDD\"}", "{\"sf\":9}", 125000, 12.0, 1000, 0, 3,
                 &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"mtype\":\"JoinRequest\"") && ev_has(&ev, "\"join_eui\":\"0102030405060708\"") &&
              ev_has(&ev, "\"dev_eui\":\"1112131415161718\"") && ev_has(&ev, "\"dev_nonce\":4386"));
        evs_free(&ev);
    }

    /* ---- Palabra de sincronía: lista, privada y "any" ---- */
    {
        evs_t ev = {0};
        run_case("{\"sf\":7,\"sync\":18,\"payload\":\"68656C6C6F20776F726C64\"}", "{\"sf\":7,\"sync\":\"0x34\"}", 125000, 15.0, 0, 0, 5,
                 &ev);
        CHECK_EQ(evs_count(&ev), 0); /* sync 0x12 con filtro 0x34: ignorada */
        evs_free(&ev);
        run_case("{\"sf\":7,\"sync\":18,\"payload\":\"68656C6C6F20776F726C64\"}", "{\"sf\":7,\"sync\":\"0x12\"}", 125000, 15.0, 0, 0, 5,
                 &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"sync\":\"0x12\"") && ev_has(&ev, "\"payload\":\"68656C6C6F20776F726C64\""));
        evs_free(&ev);
        run_case("{\"sf\":7,\"sync\":99,\"payload\":\"68656C6C6F\"}", "{\"sf\":7,\"sync\":\"any\"}", 125000, 15.0, 0, 0, 5, &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"sync\":\"0x63\""));
        evs_free(&ev);
    }

    /* ---- (2) SNR negativo: SF10 −10 dB y SF12 −15 dB, CR 4/8, CFO y reloj fuera de nominal ---- */
    {
        int good10 = 0, good12 = 0;
        for (int s = 1; s <= 5; s++) {
            evs_t ev = {0};
            run_case("{\"sf\":10,\"cr\":4,\"payload\":\"68656C6C6F204C6F52612D31302064422121\"}", "{\"sf\":10}", 125000, -10.0,
                     (s & 1 ? 1 : -1) * 20000.0, (s & 2) ? 80 : -80, (uint32_t)(40 + s), &ev);
            good10 += evs_count(&ev) == 1 && ev_has(&ev, "\"payload\":\"68656C6C6F204C6F52612D31302064422121\"") &&
                      ev_has(&ev, "\"crc_ok\":true");
            if (s == 1 && ev.data)
                printf("  SF10 -10 dB: %.*s\n", (int)(strchr(ev.data, '\n') - ev.data), ev.data);
            evs_free(&ev);
            run_case("{\"sf\":12,\"cr\":4,\"payload\":\"68656C6C6F204C6F52612D31352064422121\"}", "{\"sf\":12}", 125000, -15.0, 5000,
                     50, (uint32_t)(60 + s), &ev);
            good12 += evs_count(&ev) == 1 && ev_has(&ev, "\"crc_ok\":true");
            evs_free(&ev);
        }
        printf("  tasa de acierto: SF10 @-10 dB %d/5, SF12 @-15 dB %d/5\n", good10, good12);
        CHECK(good10 >= 4);
        CHECK(good12 >= 3);
    }

    /* ---- (2b) VECTORES REALES de un transmisor independiente (loratx de dxlaprs-lora,
     * instalado en el sistema; no es nuestro modulador): tests/data/lora_loratx_*.cs16,
     * 500 kS/s int16, carga "Hola desde loratx 123\n" (sin el salto: 21 B), SF7, CR 4/5,
     * CRC, red 0x34, BW 125 y 250 kHz. Generadas con
     *   loratx -i msg.txt -f i16 -r 500000 -C -s 7 -c 5 -b 7|250000 -p 8 -g 0.5 -N 34
     * Antes de esta validación fallaban (símbolos vecinos por la interpolación en el
     * salto del chirp). También se comprobó nuestro modulador con lorarx (SF7-12,
     * CR 1-4, BW 125/250): el CRC va SIN blanquear. ---- */
    {
        static const char *fs[2] = {"tests/data/lora_loratx_sf7_cr5_bw125.cs16", "tests/data/lora_loratx_sf7_cr5_bw250.cs16"};
        for (int i = 0; i < 2; i++) {
            int nn = 0;
            cf32 *x = vr_load_ci16(fs[i], &nn);
            if (!x) {
                printf("  (sin %s: se omite)\n", fs[i]);
                continue;
            }
            evs_t ev = {0};
            run_narrow(&DEC_LORA, x, nn, 868100000LL, i ? "{\"bw\":250000,\"sf\":7,\"sync\":\"0x34\"}" : "{\"sf\":7,\"sync\":\"0x34\"}", &ev);
            if (!ev_has(&ev, "\"crc_ok\":true"))
                printf("  loratx %s: sin trama (%s)\n", fs[i], ev.data ? ev.data : "-");
            CHECK(ev_has(&ev, "\"crc_ok\":true") &&
                  ev_has(&ev, "\"payload\":\"486F6C61206465736465206C6F7261747820313233\""));
            evs_free(&ev);
            free(x);
        }
    }

    /* ---- (3) Solo ruido: 0 eventos y coste de CPU ---- */
    {
        int n = 500000 * 20;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 987654321u;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t ev = {0};
        double t0 = cpu_s();
        run_narrow(&DEC_LORA, x, n, 868100000, "{\"bw\":125000,\"sf\":\"all\",\"sync\":\"any\"}", &ev);
        double dt = cpu_s() - t0;
        CHECK_EQ(evs_count(&ev), 0);
        printf("  solo ruido (20 s, 6 SF, sync any): %d eventos; CPU %.3f s por s de señal (%.3f por SF)\n", evs_count(&ev),
               dt / 20.0, dt / 20.0 / 6.0);
        CHECK(dt / 20.0 / 6.0 < VR_CPU(0.3));
        evs_free(&ev);
        free(x);
        printf("  con señal (6 SF activos, trama y huecos): CPU %.3f s por s de señal (%.3f por SF)\n", cpu_sig / sig_s,
               cpu_sig / sig_s / 6.0);
        CHECK(cpu_sig / sig_s / 6.0 < VR_CPU(0.3));
    }

    /* ---- state_json y opciones inválidas ---- */
    {
        char err[200] = "";
        dec_cfg_t cfg = {.rate = 500000, .freq_hz = 868100000, .center_hz = 868100000, .opts = "{\"bw\":500000}"};
        CHECK(DEC_LORA.create(&cfg, err, sizeof err) == NULL);
        CHECK(err[0] != 0);
        cfg.opts = "{\"sf\":13}";
        CHECK(DEC_LORA.create(&cfg, err, sizeof err) == NULL);
        cfg.opts = "{\"sf\":9}";
        void *st = DEC_LORA.create(&cfg, err, sizeof err);
        CHECK(st != NULL);
        int n;
        cf32 *x = make_signal("{\"sf\":9,\"lorawan\":{\"devaddr\":\"26011BDA\",\"fcnt\":77,\"fport\":2}}", 125000, 12.0, 3000, 0, 11, &n);
        evs_t ev = {0};
        for (int off = 0; off < n; off += 4096)
            DEC_LORA.process(st, x + off, n - off < 4096 ? n - off : 4096, 1000.0 + off / 500000.0, tu_collect, &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"devaddr\":\"26011BDA\"") && ev_has(&ev, "\"fcnt\":77"));
        sb_t sb = {0};
        DEC_LORA.state_json(st, &sb, 1001.0);
        CHECK(strstr(sb.data, "\"id\":\"26011BDA\"") && strstr(sb.data, "\"fcnt\":77"));
        sb_clear(&sb);
        DEC_LORA.state_json(st, &sb, 1000.0 + 2000.0); /* caducado */
        CHECK(strstr(sb.data, "\"id\"") == NULL);
        sb_free(&sb);
        DEC_LORA.destroy(st);
        evs_free(&ev);
        free(x);
    }
    return TEST_SUMMARY();
}
