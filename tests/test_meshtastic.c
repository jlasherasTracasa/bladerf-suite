/*
 * test_meshtastic.c — Decodificador Meshtastic.
 *
 * (1) Modulador (cuatro paquetes: nodeinfo, posición, texto, telemetría, con la
 *     clave pública por defecto) → canal (AWGN, CFO ±¼ BW = ±62,5 kHz, ±100 ppm)
 *     → decodificador a 500 kS/s → campos y protobuf correctos.
 * (2) Paquete cifrado con otra clave → se muestra como cifrado, sin contenido.
 * (3) Solo ruido → 0 eventos; coste de CPU.
 * (4) Referencias: hash de canal LongFast + clave por defecto = 8 y nonce
 *     (id u64 LE | from u32 LE | 0) y presets según la documentación y el
 *     firmware de Meshtastic (meshtastic.org/docs/overview/radio-settings,
 *     src/mesh/{CryptoEngine,Channels}.cpp, RadioInterface.h); AES-CTR contra
 *     NIST SP 800-38A en tests/test_aes.c. No hay capturas reales publicadas
 *     con bytes de ejemplo que se hayan podido verificar.
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/aes.h"
#include "decoders/lora_phy.h"
#include "decoders/meshtastic.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_MESHTASTIC;
extern const modulator_desc_t MOD_MESHTASTIC;

static double cpu_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void add_noise(cf32 *x, int n, int bw, double snr_db, uint32_t seed)
{
    double sigma = sqrt(0.64 * (500000.0 / bw) / pow(10.0, snr_db / 10.0) / 2.0);
    for (int i = 0; i < n; i++) {
        x[i].re += (float)(sigma * tu_gauss(&seed));
        x[i].im += (float)(sigma * tu_gauss(&seed));
    }
}

int main(void)
{
    /* ---- Referencias ---- */
    CHECK_EQ(mesh_channel_hash("LongFast", MESH_DEFAULT_KEY, 16), 8);
    const mesh_preset_t *lf = mesh_preset_find("LONG_FAST");
    CHECK(lf && lf->sf == 11 && lf->bw_hz == 250000 && lf->cr == 1);
    CHECK(mesh_preset_find("long_moderate")->bw_hz == 125000);
    CHECK(mesh_preset_find("NOPE") == NULL);
    {
        /* mesh_crypt es AES-CTR con nonce = id(8 LE) | from(4 LE) | 0 y contador de 4 bytes */
        uint8_t a[40], b[40];
        for (int i = 0; i < 40; i++)
            a[i] = b[i] = (uint8_t)(i * 7);
        mesh_crypt(MESH_DEFAULT_KEY, 0x11223344, 0xAABBCCDD, a, 40);
        aes_ctx_t c;
        aes_set_key(&c, MESH_DEFAULT_KEY, 16);
        uint8_t nonce[16] = {0xDD, 0xCC, 0xBB, 0xAA, 0, 0, 0, 0, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0}, ks[16];
        aes_encrypt_block(&c, nonce, ks);
        for (int i = 0; i < 16; i++)
            CHECK_EQ(a[i], b[i] ^ ks[i]);
        nonce[15] = 1; /* segundo bloque: contador gran-endian + 1 */
        aes_encrypt_block(&c, nonce, ks);
        for (int i = 0; i < 16; i++)
            CHECK_EQ(a[16 + i], b[16 + i] ^ ks[i]);
        mesh_crypt(MESH_DEFAULT_KEY, 0x11223344, 0xAABBCCDD, a, 40); /* simétrico */
        CHECK(memcmp(a, b, 40) == 0);
    }

    /* ---- (1) Cuatro paquetes por el canal ---- */
    const char *params = "{\"src\":\"!a1b2c3d4\",\"id\":1000,\"text\":\"Hola mesh desde BladeRF Lab\",\"kinds\":[\"nodeinfo\","
                         "\"position\",\"text\",\"telemetry\"],\"lat\":40.4168,\"lon\":-3.7038,\"alt\":667,"
                         "\"long_name\":\"BladeRF Lab\",\"short_name\":\"BLDR\",\"battery\":87,\"voltage\":4.01}";
    double cpu_sig = 0, sig_s = 0;
    for (int k = 0; k < 4; k++) {
        iqbuf_t o = {0};
        char err[200] = "";
        CHECK_EQ(MOD_MESHTASTIC.modulate(params, &o, err, sizeof err), 0);
        CHECK(o.rate == 500000.0);
        cf32 *x = malloc(sizeof(cf32) * (size_t)(o.n + o.n / 500 + 16));
        int n = o.n;
        double ppm = (k & 1) ? -100 : 100, cfo = (k & 2) ? -62500 : 62500;
        n = chan_resample(o.s, o.n, 1.0 + ppm * 1e-6, x);
        chan_cfo(x, n, 500000.0, cfo);
        add_noise(x, n, 250000, k < 2 ? 10.0 : 0.0, 77u + 2654435761u * (uint32_t)k);
        evs_t ev = {0};
        double t0 = cpu_s();
        run_narrow(&DEC_MESHTASTIC, x, n, 869525000, NULL, &ev);
        cpu_sig += cpu_s() - t0;
        sig_s += n / 500000.0;
        if (k == 0)
            printf("  duración de la señal: %.2f s, %d eventos\n%s", n / 500000.0, evs_count(&ev), ev.data ? ev.data : "");
        CHECK_EQ(evs_count(&ev), 4);
        CHECK(ev_has(&ev, "\"from\":\"!a1b2c3d4\""));
        CHECK(ev_has(&ev, "\"to\":\"^all\""));
        CHECK(ev_has(&ev, "\"encrypted\":false"));
        CHECK(ev_has(&ev, "\"hop_limit\":3,\"hop_start\":3"));
        CHECK(ev_has(&ev, "\"channel\":8"));
        CHECK(ev_has(&ev, "\"sf\":11,\"bw\":250000"));
        CHECK(ev_has(&ev, "\"port\":\"NODEINFO_APP\"") && ev_has(&ev, "\"long_name\":\"BladeRF Lab\"") &&
              ev_has(&ev, "\"short_name\":\"BLDR\""));
        CHECK(ev_has(&ev, "\"port\":\"POSITION_APP\"") && ev_has(&ev, "\"lat\":40.4168000") &&
              ev_has(&ev, "\"lon\":-3.7038000") && ev_has(&ev, "\"alt\":667"));
        CHECK(ev_has(&ev, "\"port\":\"TEXT_MESSAGE_APP\"") && ev_has(&ev, "\"msg\":\"Hola mesh desde BladeRF Lab\""));
        CHECK(ev_has(&ev, "\"port\":\"TELEMETRY_APP\"") && ev_has(&ev, "\"battery\":87") && ev_has(&ev, "\"voltage\":4.01"));
        CHECK(ev_has(&ev, "\"id\":1000,") && ev_has(&ev, "\"id\":1003,"));
        evs_free(&ev);
        free(x);
        iqbuf_free(&o);
    }

    /* ---- state_json y caducidad ---- */
    {
        iqbuf_t o = {0};
        char err[200] = "";
        MOD_MESHTASTIC.modulate(params, &o, err, sizeof err);
        add_noise(o.s, o.n, 250000, 10.0, 5);
        dec_cfg_t cfg = {.rate = 500000, .freq_hz = 869525000, .center_hz = 869525000, .opts = "{\"preset\":\"LONG_FAST\"}"};
        void *st = DEC_MESHTASTIC.create(&cfg, err, sizeof err);
        CHECK(st != NULL);
        evs_t ev = {0};
        for (int off = 0; off < o.n; off += 4096)
            DEC_MESHTASTIC.process(st, o.s + off, MIN(4096, o.n - off), 5000.0, tu_collect, &ev);
        CHECK_EQ(evs_count(&ev), 4);
        sb_t sb = {0};
        DEC_MESHTASTIC.state_json(st, &sb, 5010.0);
        CHECK(strstr(sb.data, "\"id\":\"!a1b2c3d4\"") && strstr(sb.data, "\"short_name\":\"BLDR\"") &&
              strstr(sb.data, "\"lat\":40.416800") && strstr(sb.data, "\"battery\":87") && strstr(sb.data, "\"packets\":4"));
        printf("  state: %s\n", sb.data);
        sb_clear(&sb);
        DEC_MESHTASTIC.state_json(st, &sb, 5000.0 + 8000.0); /* caducado */
        CHECK(strstr(sb.data, "\"nodes\":[]") != NULL);
        sb_free(&sb);
        /* el texto del evento usa el nombre corto una vez conocido */
        CHECK(ev_has(&ev, "BLDR → ^all: “Hola mesh"));
        DEC_MESHTASTIC.destroy(st);
        evs_free(&ev);
        iqbuf_free(&o);
    }

    /* ---- Paquete duplicado (reenvío de la malla): mismo from+id → "dup":true ---- */
    {
        iqbuf_t o = {0};
        char err[200] = "";
        MOD_MESHTASTIC.modulate("{\"src\":\"!00000042\",\"id\":7,\"text\":\"eco\"}", &o, err, sizeof err);
        int n = o.n * 2;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        memcpy(x, o.s, sizeof(cf32) * (size_t)o.n);
        memcpy(x + o.n, o.s, sizeof(cf32) * (size_t)o.n);
        add_noise(x, n, 250000, 12.0, 9);
        evs_t ev = {0};
        run_narrow(&DEC_MESHTASTIC, x, n, 869525000, "{}", &ev);
        CHECK_EQ(evs_count(&ev), 2);
        CHECK(ev_has(&ev, "\"dup\":true") && ev_has(&ev, "\"dup\":false"));
        evs_free(&ev);
        free(x);
        iqbuf_free(&o);
    }

    /* ---- (2) Otra clave → "cifrado" ---- */
    {
        uint8_t other[16], pkt[64];
        for (int i = 0; i < 16; i++)
            other[i] = (uint8_t)(0x55 + i * 3);
        uint32_t from = 0x0BADF00D, id = 4242;
        /* Data{portnum=1, payload="secreto"} cifrado con una clave PRIVADA */
        uint8_t data[] = {0x08, 0x01, 0x12, 0x07, 's', 'e', 'c', 'r', 'e', 't', 'o'};
        memset(pkt, 0, sizeof pkt);
        for (int i = 0; i < 4; i++) {
            pkt[i] = 0xFF;
            pkt[4 + i] = (uint8_t)(from >> (8 * i));
            pkt[8 + i] = (uint8_t)(id >> (8 * i));
        }
        pkt[12] = 0x63;
        pkt[13] = mesh_channel_hash("Privado", other, 16);
        memcpy(pkt + 16, data, sizeof data);
        mesh_crypt(other, from, id, pkt + 16, (int)sizeof data);
        uint16_t sy[400];
        int ns = lora_encode_symbols(pkt, 16 + (int)sizeof data, 11, 1, true, false, sy, 400);
        iqbuf_t o = {0};
        lora_push_silence(&o, 20000);
        lora_modulate_frame(&o, 500000, 250000, 11, 0x2B, 16, sy, ns, 0.8f);
        lora_push_silence(&o, 40000);
        add_noise(o.s, o.n, 250000, 10.0, 3);
        evs_t ev = {0};
        run_narrow(&DEC_MESHTASTIC, o.s, o.n, 869525000, NULL, &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"encrypted\":true") && ev_has(&ev, "\"from\":\"!0badf00d\"") && !ev_has(&ev, "secreto") &&
              !ev_has(&ev, "\"msg\""));
        evs_free(&ev);
        iqbuf_free(&o);
    }

    /* ---- Preset distinto y opciones inválidas ---- */
    {
        iqbuf_t o = {0};
        char err[200] = "";
        MOD_MESHTASTIC.modulate("{\"preset\":\"MEDIUM_FAST\",\"text\":\"m9\",\"id\":5}", &o, err, sizeof err);
        add_noise(o.s, o.n, 250000, 8.0, 21);
        evs_t ev = {0};
        run_narrow(&DEC_MESHTASTIC, o.s, o.n, 869525000, "{\"preset\":\"MEDIUM_FAST\"}", &ev);
        CHECK_EQ(evs_count(&ev), 1);
        CHECK(ev_has(&ev, "\"sf\":9,\"bw\":250000") && ev_has(&ev, "\"msg\":\"m9\""));
        evs_free(&ev);
        run_narrow(&DEC_MESHTASTIC, o.s, o.n, 869525000, NULL, &ev); /* LongFast no ve SF9 */
        CHECK_EQ(evs_count(&ev), 0);
        evs_free(&ev);
        iqbuf_free(&o);
        dec_cfg_t cfg = {.rate = 500000, .freq_hz = 869525000, .center_hz = 869525000, .opts = "{\"preset\":\"XX\"}"};
        CHECK(DEC_MESHTASTIC.create(&cfg, err, sizeof err) == NULL);
        CHECK(MOD_MESHTASTIC.modulate("{\"kinds\":[\"foo\"]}", &o, err, sizeof err) < 0);
    }

    /* ---- (3) Solo ruido ---- */
    {
        int n = 500000 * 20;
        cf32 *x = malloc(sizeof(cf32) * (size_t)n);
        uint32_t seed = 31337;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t ev = {0};
        double t0 = cpu_s();
        run_narrow(&DEC_MESHTASTIC, x, n, 869525000, NULL, &ev);
        double dt = cpu_s() - t0;
        CHECK_EQ(evs_count(&ev), 0);
        printf("  solo ruido (20 s): %d eventos; CPU %.3f s por s de señal (1 SF activo)\n", evs_count(&ev), dt / 20.0);
        CHECK(dt / 20.0 < VR_CPU(0.3));
        printf("  con señal: CPU %.3f s por s de señal\n", cpu_sig / sig_s);
        CHECK(cpu_sig / sig_s < VR_CPU(0.3));
        evs_free(&ev);
        free(x);
    }
    return TEST_SUMMARY();
}
