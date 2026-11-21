/*
 * test_lte.c — Decodificador LTE (búsqueda de celda, MIB y SIB1) con el modulador gemelo.
 *
 * VECTORES REALES:
 *  - tests/data/lte_sigid_vf14_1p92M.cs8 = 50 ms (8 bit con signo) de la grabación pública
 *    "LTE_VF_1.4_downlink.zip" de sigidwiki.com (celda de 1,4 MHz, 1,25 MS/s remuestreada a 1,92 MS/s):
 *    PCI 65 (N_ID1 21, N_ID2 2), FDD, CP normal, 1 puerto, MIB de 6 RB; el CRC del PBCH solo pasa si
 *    codificación, aleatorizado y adaptación de velocidad son los de TS 36.212 §5.3.1. Con SINR ≈ 3 dB el
 *    PDCCH no se decodifica (sin SIB1).
 *  - tests/data/real/lte_or10.sigmf-data y lte_tk10.sigmf-data = 50 ms de "LTE_OR_10_downlink.zip" y
 *    "LTE_TK_10_downlink.zip" (sigidwiki.com, CC BY-SA 3.0; 10 MHz a 10 MS/s remuestreados a 15,36 MS/s):
 *    dos celdas reales de 50 RB y 2 puertos con SIB1 completo. OR: PCI 44, PLMN 231-01, TAC 1807, ECI
 *    2854175, banda 3, DCI 1C con asignación distribuida. TK: PCI 348, PLMN 231-02, TAC 1, ECI 98641,
 *    banda 20, DCI 1A localizado. Que el CRC-24A del DL-SCH cuadre en ambas valida a la vez turbocódigo,
 *    adaptación de velocidad, aleatorización, SFBC, entrelazado del PDCCH, tabla de TBS y mapeo VRB→PRB.
 * Además se comprueban propiedades de TS 36.211 (PSS: |d| = 1 y raíces 25/29/34 de la tabla 6.11.1.1; SSS ±1,
 * N_ID1 y subtrama 0/5 dan secuencias distintas y casi ortogonales), la codificación del PBCH (§5.3.1), el
 * turbocódigo + adaptación de velocidad (§5.1.3), los DCI 1A/1C, el RIV, el mapeo VRB distribuido y el
 * ASN.1 de SIB1 por ida y vuelta con errores de canal. La prueba extremo a extremo es modulador → canal →
 * decodificador (ver docs/protocolos/lte.md).
 */
#include "test_util.h"
#include "vec_real.h"

#include "decoders/lte_phy.h"
#include "decoders/lte_sib.h"
#include "tx/mod.h"

#include <time.h>

extern const decoder_desc_t DEC_LTE;
extern const modulator_desc_t MOD_LTE;

/* Genera, recorta 'crop' muestras al principio y aplica el canal. Devuelve la tasa en *rate. */
static int make_signal(const char *params, int crop, double snr_db, double cfo_hz, double ppm, uint32_t seed,
                       cf32 **out, double *rate)
{
    iqbuf_t b = {0};
    char err[128] = "";
    if (MOD_LTE.modulate(params, &b, err, sizeof err) < 0) {
        printf("  modulate: %s\n", err);
        return 0;
    }
    int n = b.n - crop;
    cf32 *x = malloc(sizeof(cf32) * (size_t)n);
    memcpy(x, b.s + crop, sizeof(cf32) * (size_t)n);
    *rate = b.rate;
    iqbuf_free(&b);
    if (ppm != 0) {
        cf32 *y = malloc(sizeof(cf32) * (size_t)(n + n / 500 + 64));
        int m = chan_resample(x, n, 1.0 + ppm * 1e-6, y);
        free(x);
        x = y;
        n = MIN(n, m);
    }
    if (cfo_hz != 0)
        chan_cfo(x, n, *rate, cfo_hz);
    chan_awgn(x, n, snr_db, &seed);
    *out = x;
    return n;
}

/* Corre el decodificador (WIDE) sobre la señal y devuelve los eventos. */
static void decode(const cf32 *x, int n, double rate, const char *opts, evs_t *evs)
{
    int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
    cf32_to_q11(x, n, 0.5, q);
    run_wide(&DEC_LTE, q, n, rate, 800000000, opts, evs);
    free(q);
}

typedef struct {
    const char *mcc, *mnc;
    int tac, band;
    unsigned cell_id;
} want_sib_t;

static void case_run(const char *name, const char *params, int crop, double snr, double cfo, double ppm, int want_pci,
                     int want_ports, int want_bw, const want_sib_t *ws)
{
    cf32 *x;
    double rate;
    int n = make_signal(params, crop, snr, cfo, ppm, 1234u + (uint32_t)crop, &x, &rate);
    evs_t evs = {0};
    decode(x, n, rate, "{\"period\":0}", &evs);
    printf("  [%s] %.2f Msps, eventos=%d\n", name, rate / 1e6, evs_count(&evs));
    if (evs.data)
        printf("    %.*s\n", (int)(strchr(evs.data, '\n') - evs.data), evs.data);
    char key[96];
    snprintf(key, sizeof key, "\"pci\":%d,", want_pci);
    CHECK(ev_has(&evs, key));
    if (want_ports) {
        snprintf(key, sizeof key, "\"ports\":%d", want_ports);
        CHECK(ev_has(&evs, key));
    }
    CHECK(ev_has(&evs, "\"mib\":true"));
    snprintf(key, sizeof key, "\"bw_rb\":%d", want_bw);
    CHECK(ev_has(&evs, key));
    CHECK(ev_has(&evs, "\"phich_resource\":2"));
    if (ws) {
        CHECK(ev_has(&evs, "\"sib1\":true"));
        snprintf(key, sizeof key, "\"mcc\":\"%s\",\"mnc\":\"%s\"", ws->mcc, ws->mnc);
        CHECK(ev_has(&evs, key));
        snprintf(key, sizeof key, "\"tac\":%d,\"cell_id\":%u,", ws->tac, ws->cell_id);
        CHECK(ev_has(&evs, key));
        snprintf(key, sizeof key, "\"band\":%d,", ws->band);
        CHECK(ev_has(&evs, key));
        CHECK(ev_has(&evs, "\"sched\":[{\"period_rf\":16,\"sibs\":[3]},{\"period_rf\":32,\"sibs\":[4,5]}]"));
    }
    if (evs.data && cfo != 0) {
        const char *c = strstr(evs.data, "\"cfo_hz\":");
        if (c)
            CHECK_NEAR(atof(c + 9), cfo, 400.0);
    }
    evs_free(&evs);
    free(x);
}

/* Decodifica un fichero real (8 bit con signo) a la tasa dada. */
static evs_t real_file(const char *path, double rate, bool *found)
{
    evs_t evs = {0};
    int nn = 0;
    cf32 *x = vr_load_cs8(path, &nn);
    *found = x != NULL;
    if (!x) {
        printf("  (sin %s: se omite)\n", path);
        return evs;
    }
    decode(x, nn, rate, "{\"period\":0}", &evs);
    free(x);
    return evs;
}

int main(void)
{
    /* ---- Vectores de la norma / propiedades ---- */
    int8_t s0[62];
    /* N_ID1 = 0 → (m0,m1) = (0,1), N_ID2 = 0: d(0) = s̃(0)·c̃(0) = +1 */
    lte_sss_seq(0, 0, 0, s0);
    CHECK_EQ(s0[0], 1);      /* s̃(0) = 1−2x(0) = +1 y c̃(0) = +1 */
    for (int u = 0; u < 3; u++) {
        cf32 d[62];
        lte_pss_seq(u, d);
        double mx = 0, mn = 9;
        for (int n = 0; n < 62; n++) {
            double m = hypot(d[n].re, d[n].im);
            mx = fmax(mx, m);
            mn = fmin(mn, m);
        }
        CHECK_NEAR(mx, 1.0, 1e-5);
        CHECK_NEAR(mn, 1.0, 1e-5);
    }
    CHECK_EQ(lte_pss_root(0), 25);
    CHECK_EQ(lte_pss_root(1), 29);
    CHECK_EQ(lte_pss_root(2), 34);
    /* Las SSS de N_ID1 distintos y de subtramas 0/5 son distintas pero con correlación baja */
    {
        int8_t a[62], b[62];
        lte_sss_seq(29, 1, 0, a);
        lte_sss_seq(30, 1, 0, b);   /* (29,30) frente a (0,2): secuencias distintas */
        int dot = 0;
        for (int i = 0; i < 62; i++)
            dot += a[i] * b[i];
        CHECK(abs(dot) < 40);
        lte_sss_seq(29, 1, 0, b);
        dot = 0;
        for (int i = 0; i < 62; i++)
            dot += a[i] * b[i];
        CHECK_EQ(dot, 62);
        lte_sss_seq(29, 1, 1, b);
        dot = 0;
        for (int i = 0; i < 62; i++)
            dot += a[i] * b[i];
        CHECK(abs(dot) < 40);
    }
    /* PBCH: ida y vuelta de la codificación, con 5 bits erróneos en la ráfaga */
    for (int ext = 0; ext < 2; ext++)
        for (int ports = 1; ports <= 4; ports += (ports == 1 ? 1 : 2)) {
            lte_mib_t m = {.bw_rb = 25, .phich_ext = 1, .phich_res = 1, .sfn8 = 0xA5}, r;
            uint8_t bits[1920];
            int E = lte_pbch_encode(&m, ports, 77, ext, bits);
            int eseg = E / 4;
            float soft[480];
            for (int seg = 0; seg < 4; seg += 3) {
                for (int i = 0; i < eseg; i++)
                    soft[i] = bits[seg * eseg + i] ? -1.0f : 1.0f;
                for (int k = 0; k < 5; k++)
                    soft[(k * 97) % eseg] *= -1;
                int pr = lte_pbch_decode(soft, seg, 77, ext, &r);
                CHECK_EQ(pr, ports);
                CHECK_EQ(r.bw_rb, 25);
                CHECK_EQ(r.sfn8, 0xA5);
                CHECK_EQ(r.phich_res, 1);
            }
        }

    /* ---- CRC, turbocódigo, DCI, RIV, VRB y SIB1 por ida y vuelta ---- */
    {
        /* CRC-24A de un mensaje de un bit: el resto de x^24·(1) es el polinomio generador (sin el x^24) */
        uint8_t one[1] = {1};
        CHECK_EQ((long long)lte_crc24a(one, 1), 0x864CFBLL);
        CHECK_EQ(lte_tbs_qpsk_get(0, 1), 16);       /* 36.213 tabla 7.1.7.2.1-1 */
        CHECK_EQ(lte_tbs_qpsk_get(2, 3), 144);
        CHECK_EQ(lte_tbs_qpsk_get(2, 5), 208);
        CHECK_EQ(lte_tbs_1c_get(0), 40);
        CHECK_EQ(lte_tbs_1c_get(31), 1736);
        CHECK_EQ(lte_dci1a_size(6), 21);
        CHECK_EQ(lte_dci1a_size(50), 27);
        CHECK_EQ(lte_dci1c_size(6), 8);
        CHECK_EQ(lte_dci1c_size(50), 13);
        uint32_t sd = 99;
        for (int tbs = 40; tbs <= 1000; tbs += 160) {
            uint8_t tb[1100], dd[1100], out[8000];
            float soft[8000];
            for (int i = 0; i < tbs; i++)
                tb[i] = (uint8_t)(tu_rand(&sd) & 1);
            for (int rv = 0; rv < 4; rv++) {
                int E = 2 * tbs + 400;
                if (lte_dlsch_encode(tb, tbs, E, rv, out) < 0)
                    continue;
                for (int i = 0; i < E; i++)
                    soft[i] = (out[i] ? -1.f : 1.f) + (float)(0.9 * tu_gauss(&sd));   /* ≈ 1 dB de Eb/N0 por bit */
                CHECK(lte_dlsch_decode(soft, E, tbs, rv, 8, dd));
                CHECK(memcmp(dd, tb, (size_t)tbs) == 0);
            }
        }
        /* DCI: ida y vuelta con SI-RNTI y un RNTI distinto (no debe devolver 0xFFFF) */
        for (int nrb = 6; nrb <= 100; nrb += (nrb == 6 ? 9 : (nrb == 15 ? 10 : (nrb == 25 ? 25 : 25)))) {
            lte_dci1a_t d = {.mcs = 5, .rv = 2, .nprb1a = 3, .rb_start = 1, .nprb = 3, .harq = 0}, r;
            uint8_t db[64], back[64];
            int a = lte_dci1a_pack(&d, nrb, db);
            CHECK_EQ(a, lte_dci1a_size(nrb));
            uint8_t e[576];
            float sf[576];
            lte_dci_encode(db, a, LTE_SI_RNTI, 288, e);
            for (int i = 0; i < 288; i++)
                sf[i] = e[i] ? -1.f : 1.f;
            CHECK_EQ(lte_dci_decode(sf, 288, a, back), LTE_SI_RNTI);
            CHECK(lte_dci1a_unpack(back, a, nrb, &r) == 0);
            CHECK(r.mcs == 5 && r.rv == 2 && r.nprb1a == 3 && r.rb_start == 1 && r.nprb == 3);
            lte_dci_encode(db, a, 0x1234, 288, e);
            for (int i = 0; i < 288; i++)
                sf[i] = e[i] ? -1.f : 1.f;
            CHECK(lte_dci_decode(sf, 288, a, back) != LTE_SI_RNTI);
            lte_dci1a_t c = {.mcs = 9, .riv = lte_riv_encode(nrb < 50 ? lte_type2_nvrb(nrb, false) / 2 : lte_type2_nvrb(nrb, false) / 4, 0, 2)};
            int ac = lte_dci1c_pack(&c, nrb, db);
            CHECK_EQ(ac, lte_dci1c_size(nrb));
            CHECK(lte_dci1c_unpack(db, ac, nrb, &r) == 0);
            CHECK(r.mcs == 9 && r.riv == c.riv);
        }
        /* RIV */
        for (int len = 1; len <= 6; len++)
            for (int st = 0; st + len <= 6; st++) {
                int s2, l2;
                CHECK(lte_riv_decode(6, lte_riv_encode(6, st, len), &s2, &l2) && s2 == st && l2 == len);
            }
        /* VRB distribuidos: nº de PRB = longitud, sin repetidos, y la 2ª ranura difiere de la 1ª */
        for (int nrb = 25; nrb <= 100; nrb += 25) {
            int step = nrb < 50 ? 2 : 4, nn = lte_type2_nvrb(nrb, false) / step;
            uint8_t mask[2][110];
            int cnt = lte_type2_prb(nrb, true, false, step, lte_riv_encode(nn, 0, 2), mask);
            CHECK_EQ(cnt, 2 * step);
            int c1 = 0;
            for (int i = 0; i < nrb; i++)
                c1 += mask[1][i];
            CHECK_EQ(c1, 2 * step);
            /* un VRB suelto de la 1A (step 1) cae en un PRB distinto en cada ranura */
            cnt = lte_type2_prb(nrb, true, false, 1, lte_riv_encode(lte_type2_nvrb(nrb, false), 1, 1), mask);
            int same = 0;
            for (int i = 0; i < nrb; i++)
                same += mask[0][i] & mask[1][i];
            CHECK_EQ(cnt, 1);
            CHECK_EQ(same, 0);
        }
        /* CRS: potencia unitaria y 2·N_RB pilotos; subportadoras de los puertos 0/1 desplazadas 3 */
        cf32 rr[200];
        lte_crs_full(301, 10, 0, 100, false, rr);
        CHECK_NEAR(rr[0].re * rr[0].re + rr[0].im * rr[0].im, 1.0, 1e-5);
        CHECK_EQ((lte_crs_k0(301, 1, 0) - lte_crs_k0(301, 0, 0) + 6) % 6, 3);
        /* Capa de control: nº de REG por CFI para 1,4 MHz con 2 puertos y PHICH 1 (36.211 §6.2.4) */
        lte_ctrl_t ct;
        CHECK_EQ(lte_ctrl_init(&ct, 65, 6, 2, 2), 0);
        CHECK_EQ(ct.nregs, 6 * (2 + 3 + 3 + 3));
        CHECK(ct.npdcch[2] % 9 == 0 && ct.npdcch[2] / 9 >= 4);
        lte_ctrl_free(&ct);
        /* SIB1: ida y vuelta con 2 PLMN (MNC de 2 y 3 cifras) */
        lte_sib1_t a = {0}, b;
        a.nplmn = 2;
        a.plmn[0] = (lte_plmn_t){{2, 1, 4}, {0, 7, 0}, 2, false};
        a.plmn[1] = (lte_plmn_t){{3, 1, 0}, {2, 6, 0}, 3, true};
        a.tac = 0xABCD;
        a.cell_id = 0xFEDCBA9;
        a.barred = true;
        a.q_rxlevmin = -60;
        a.has_pmax = true;
        a.p_max = -3;
        a.band = 64;
        a.nsched = 3;
        a.sched[0] = (lte_sched_t){0, 0, 0, {0}};
        a.sched[1] = (lte_sched_t){0, 6, 2, {3, 4}};
        a.sched[2] = (lte_sched_t){0, 2, 1, {13}};
        a.si_window_idx = 6;
        a.value_tag = 31;
        uint8_t bits[400];
        int nb = lte_sib1_encode(&a, bits, 400);
        CHECK(nb > 100);
        CHECK_EQ(lte_sib1_decode(bits, nb, &b), 0);
        CHECK(b.nplmn == 2 && b.plmn[1].mnc_len == 3 && b.plmn[1].mnc[0] == 2 && b.plmn[1].mcc[0] == 3 &&
              b.plmn[1].reserved && !b.plmn[0].reserved);
        CHECK(b.tac == 0xABCD && b.cell_id == 0xFEDCBA9u && b.barred && b.q_rxlevmin == -60 && b.p_max == -3);
        CHECK(b.band == 64 && b.nsched == 3 && b.sched[1].nsib == 2 && b.sched[2].sib[0] == 13);
        CHECK(b.si_window_idx == 6 && b.value_tag == 31);
        CHECK_EQ(lte_si_window_ms(6), 40);
        CHECK_EQ(lte_si_period_rf(6), 512);
        bits[0] ^= 1;
        CHECK(lte_sib1_decode(bits, nb, &b) < 0);       /* no es BCCH-DL-SCH c1/SIB1 */
    }

    /* ---- Señales sintéticas ---- */
    const want_sib_t W = {"001", "01", 4660, 20, 0x1234567};
    const char *p1 = "{\"pci\":301,\"ports\":1,\"sfn\":100,\"frames\":12,\"bw_rb\":6}";
    case_run("1,4 MHz 1p, SNR 10", p1, 5001, 10, 0, 0, 301, 1, 6, &W);
    case_run("1,4 MHz 1p, CFO +7,5 kHz", p1, 7003, 10, 7500, 0, 301, 1, 6, &W);
    case_run("1,4 MHz 1p, CFO -7,5 kHz, +100 ppm", p1, 2222, 10, -7500, 100, 301, 1, 6, &W);
    case_run("1,4 MHz 1p, CFO +3 kHz, -100 ppm", p1, 9999, 10, 3000, -100, 301, 1, 6, &W);
    case_run("1,4 MHz 2p, DCI 1C",
             "{\"pci\":2,\"ports\":2,\"sfn\":4,\"frames\":12,\"bw_rb\":6,\"dci\":\"1c\"}", 1500, 10, 4000, 0, 2, 2, 6, &W);
    case_run("3 MHz (15 RB) 1p, DCI 1C",
             "{\"pci\":77,\"ports\":1,\"sfn\":8,\"frames\":12,\"bw_rb\":15,\"dci\":\"1c\"}", 3333, 10, -2000, 50, 77, 1, 15, &W);
    case_run("5 MHz (25 RB) 2p", "{\"pci\":501,\"ports\":2,\"sfn\":300,\"frames\":12,\"bw_rb\":25}", 4321, 10, 6000, 0, 501, 2, 25, &W);
    case_run("10 MHz (50 RB) 2p, DCI 1A",
             "{\"pci\":44,\"ports\":2,\"sfn\":100,\"frames\":12,\"bw_rb\":50}", 8888, 10, -5000, 100, 44, 2, 50, &W);
    case_run("10 MHz (50 RB) 2p, DCI 1C distribuido",
             "{\"pci\":503,\"ports\":2,\"sfn\":102,\"frames\":12,\"bw_rb\":50,\"dci\":\"1c\"}", 1234, 10, 2500, -80, 503, 2, 50, &W);
    case_run("15 MHz (75 RB) 1p", "{\"pci\":9,\"ports\":1,\"sfn\":100,\"frames\":12,\"bw_rb\":75}", 777, 10, 1000, 0, 9, 1, 75, &W);
    case_run("20 MHz (100 RB) 2p, DCI 1C",
             "{\"pci\":300,\"ports\":2,\"sfn\":100,\"frames\":12,\"bw_rb\":100,\"dci\":\"1c\"}", 4000, 10, 3000, 0, 300, 2, 100, &W);
    case_run("10 MHz capturado a 30,72 Msps (sobremuestreo 2)",
             "{\"pci\":44,\"ports\":2,\"sfn\":100,\"frames\":12,\"bw_rb\":50,\"os\":2}", 2468, 10, 2000, 0, 44, 2, 50, &W);
    {
        const want_sib_t W2 = {"310", "260", 7, 12, 0xABCDE01};
        case_run("MNC de 3 cifras, otra banda",
                 "{\"pci\":10,\"ports\":1,\"sfn\":20,\"frames\":12,\"bw_rb\":25,\"mcc\":\"310\",\"mnc\":\"260\",\"tac\":7,"
                 "\"band\":12,\"cell_id\":180149761}", 555, 10, 0, 0, 10, 1, 25, &W2);
    }
    /* 10 MHz capturado con menos tasa de la que SIB1 exige: celda y MIB sí, SIB1 no, con el aviso */
    {
        cf32 *x;
        double rate;
        int n = make_signal("{\"pci\":44,\"ports\":2,\"sfn\":100,\"frames\":12,\"bw_rb\":50}", 0, 10, 0, 0, 5, &x, &rate);
        /* se diezma a 1,92 Msps con un promedio de 8 muestras (la banda útil de 1,4 MHz basta para PSS/SSS/PBCH) */
        int m = n / 8;
        for (int i = 0; i < m; i++) {
            cf32 a = {0, 0};
            for (int j = 0; j < 8; j++) {
                a.re += x[8 * i + j].re;
                a.im += x[8 * i + j].im;
            }
            x[i] = (cf32){a.re / 8, a.im / 8};
        }
        evs_t evs = {0};
        decode(x, m, rate / 8, "{\"period\":0}", &evs);
        CHECK(ev_has(&evs, "\"pci\":44,") && ev_has(&evs, "\"mib\":true") && ev_has(&evs, "\"sib1\":false"));
        CHECK(ev_has(&evs, "\"sib1_min_rate_msps\":15.36"));
        evs_free(&evs);
        free(x);
    }
    /* tasa de captura no admitida: create() debe negarse con un mensaje */
    {
        char err[160] = "";
        dec_cfg_t cfg = {.rate = 2400000, .freq_hz = 800000000, .center_hz = 800000000};
        void *st = DEC_LTE.create(&cfg, err, sizeof err);
        CHECK(st == NULL && strstr(err, "1,92") != NULL);
        if (st)
            DEC_LTE.destroy(st);
        cfg.rate = 20000000;
        st = DEC_LTE.create(&cfg, err, sizeof err);
        CHECK(st == NULL);
        if (st)
            DEC_LTE.destroy(st);
    }
    /* TDD y CP extendido: celda y MIB, sin SIB1 (no soportado) */
    case_run("TDD normal 2p",
             "{\"pci\":503,\"duplex\":\"tdd\",\"ports\":2,\"sfn\":40,\"frames\":12,\"bw_rb\":6}", 8888, 10, -5000, 50, 503, 2, 6, NULL);
    case_run("FDD extendido 1p",
             "{\"pci\":57,\"cp\":\"extended\",\"ports\":1,\"sfn\":8,\"frames\":12,\"bw_rb\":6}", 4321, 10, 2000, 0, 57, 1, 6, NULL);
    /* detección de duplexado y CP en el texto */
    {
        cf32 *x;
        double rate;
        int n = make_signal("{\"pci\":503,\"duplex\":\"tdd\",\"cp\":\"extended\",\"frames\":12,\"bw_rb\":6}", 3000, 10, 1000, 0, 99,
                            &x, &rate);
        evs_t evs = {0};
        decode(x, n, rate, "{\"period\":0}", &evs);
        CHECK(ev_has(&evs, "\"duplex\":\"TDD\""));
        CHECK(ev_has(&evs, "\"cp\":\"extended\""));
        CHECK(ev_has(&evs, "\"pci\":503,"));
        evs_free(&evs);
        free(x);
    }
    /* ---- Solo ruido: 0 eventos ---- */
    {
        int n = 300000;
        cf32 *x = calloc((size_t)n, sizeof(cf32));
        uint32_t seed = 4242;
        for (int i = 0; i < n; i++) {
            x[i].re = (float)(0.1 * tu_gauss(&seed));
            x[i].im = (float)(0.1 * tu_gauss(&seed));
        }
        evs_t evs = {0};
        decode(x, n, 1920000, "{\"period\":0}", &evs);
        CHECK_EQ(evs_count(&evs), 0);
        evs_free(&evs);
        free(x);
    }
    /* ---- Grabaciones reales (sigidwiki) ---- */
    {
        bool f;
        evs_t evs = real_file("tests/data/lte_sigid_vf14_1p92M.cs8", 1920000, &f);
        if (f) {
            CHECK(ev_has(&evs, "\"pci\":65") && ev_has(&evs, "\"n_id2\":2") && ev_has(&evs, "\"duplex\":\"FDD\""));
            CHECK(ev_has(&evs, "\"mib\":true") && ev_has(&evs, "\"bw_rb\":6") && ev_has(&evs, "\"ports\":1"));
        }
        evs_free(&evs);
        evs = real_file("tests/data/real/lte_or10.sigmf-data", 15360000, &f);
        if (f) {
            if (!ev_has(&evs, "\"sib1\":true"))
                printf("  real OR: sin SIB1 (%s)\n", evs.data ? evs.data : "-");
            CHECK(ev_has(&evs, "\"pci\":44,") && ev_has(&evs, "\"bw_rb\":50") && ev_has(&evs, "\"ports\":2"));
            CHECK(ev_has(&evs, "\"sib1\":true") && ev_has(&evs, "\"mcc\":\"231\",\"mnc\":\"01\""));
            CHECK(ev_has(&evs, "\"tac\":1807,\"cell_id\":2854175,\"enb_id\":11149,\"cell_local\":31,\"band\":3,"));
        }
        evs_free(&evs);
        evs = real_file("tests/data/real/lte_tk10.sigmf-data", 15360000, &f);
        if (f) {
            if (!ev_has(&evs, "\"sib1\":true"))
                printf("  real TK: sin SIB1 (%s)\n", evs.data ? evs.data : "-");
            CHECK(ev_has(&evs, "\"pci\":348,") && ev_has(&evs, "\"bw_rb\":50") && ev_has(&evs, "\"ports\":2"));
            CHECK(ev_has(&evs, "\"sib1\":true") && ev_has(&evs, "\"mcc\":\"231\",\"mnc\":\"02\""));
            CHECK(ev_has(&evs, "\"tac\":1,\"cell_id\":98641,\"enb_id\":385,\"cell_local\":81,\"band\":20,"));
        }
        evs_free(&evs);
    }

    /* ---- Coste: s de CPU por s de señal con la configuración por defecto (1 ventana por s) ---- */
    {
        cf32 *x;
        double rate;
        int n = make_signal("{\"pci\":301,\"ports\":2,\"frames\":110,\"bw_rb\":50}", 0, 10, 2000, 0, 5, &x, &rate);
        int16_t *q = malloc(sizeof(int16_t) * 2 * (size_t)n);
        cf32_to_q11(x, n, 0.5, q);
        evs_t evs = {0};
        clock_t t0 = clock();
        run_wide(&DEC_LTE, q, n, rate, 800000000, NULL, &evs);
        double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC, sig = n / rate;
        printf("  coste (10 MHz a %.2f Msps, ventana de 48 ms por s): %.3f s de CPU por s de señal (%.2f s de señal, %d eventos)\n",
               rate / 1e6, cpu / sig, sig, evs_count(&evs));
        CHECK(cpu / sig < VR_CPU(0.3));
        CHECK(evs_count(&evs) >= 1);
        evs_free(&evs);
        free(q);
        free(x);
    }
    return TEST_SUMMARY();
}
