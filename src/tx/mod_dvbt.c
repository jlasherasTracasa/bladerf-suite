/*
 * mod_dvbt.c — Modulador DVB-T completo (ETSI EN 300 744) en banda base a 64/7 MHz.
 *
 * TS → aleatorizador → RS(204,188) → entrelazado de Forney → convolucional + perforado →
 * demux/entrelazado de bits → entrelazado de símbolo → constelación → tramas OFDM con
 * pilotos continuos y dispersos (4/3), portadoras TPS DBPSK y prefijo cíclico. Los bloques
 * de codificación son los mismos que usa el receptor (src/tv/dvbt_tab.c), de modo que un
 * error de constantes común a ambos lo detectaría solo un vector externo (ver
 * tests/test_dvbt_rx.c y docs/protocolos/dvbt.md: validado contra GNU Radio gr-dtv).
 *
 * Jerarquía: el flujo HP ocupa y0,y1 y el LP el resto de bits; α = 1, 2, 4 (campo TPS).
 *
 * Parámetros JSON del catálogo (todos opcionales):
 *   {"mode":"8k","guard":"1/8","constellation":"64qam","hierarchy":0,"hp":"2/3","lp":"2/3",
 *    "cell_id":4660,"frames":4,"frame0":0,"service":"BladeRF Lab","ts_file":"ruta.ts"}
 * air_ok = false (§6): la banda de televisión no está en la lista blanca.
 */
#include "mod_dvbt.h"

#include "../decoders/dvbt_phy.h"
#include "../dsp/fec.h"
#include "../dsp/fft.h"
#include "../tv/dvbt_tab.h"
#include "../util/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Cadena de un flujo de transporte hasta bits codificados (tras el perforado).
 * ------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *ts;
    int npkt, next;         /* paquete de entrada siguiente */
    int rate;
    rs_t rs;
    /* entrelazador de Forney: rama j con retardo 17·j bytes */
    uint8_t *fifo[12];
    int fpos[12];
    long nbytes;            /* bytes ya entrelazados (rama = nbytes mod 12) */
    uint8_t *q;             /* cola de bytes entrelazados */
    int qn, qcap, qbit;     /* bytes en cola; bit ya consumido del primero */
    uint32_t cstate;
    uint8_t *tmpbits;
    int tmpcap;
} chain_t;

static chain_t *chain_create(const uint8_t *ts, int npkt, int rate)
{
    chain_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->ts = ts;
    c->npkt = npkt;
    c->rate = rate;
    rs_init_dvb(&c->rs);
    for (int j = 1; j < 12; j++)
        c->fifo[j] = calloc((size_t)17 * (size_t)j, 1);
    return c;
}

static void chain_free(chain_t *c)
{
    if (!c)
        return;
    for (int j = 1; j < 12; j++)
        free(c->fifo[j]);
    free(c->q);
    free(c->tmpbits);
    free(c);
}

/* Genera 8 paquetes: aleatoriza, protege y entrelaza, y los añade a la cola. */
static void chain_group(chain_t *c)
{
    const uint8_t *prbs = dvbt_prbs1503();
    uint8_t blk[8][204];
    for (int g = 0; g < 8; g++) {
        const uint8_t *src = c->ts + (size_t)(c->next % c->npkt) * 188;
        c->next++;
        blk[g][0] = g == 0 ? 0xB8 : 0x47;
        for (int i = 1; i < 188; i++)
            blk[g][i] = src[i];
    }
    /* PRBS: byte p (1..1503 del grupo) usa prbs[p−1]; los sincronismos no se tocan */
    for (int p = 1; p < 1504; p++) {
        if (p % 188 == 0)
            continue;
        blk[p / 188][p % 188] ^= prbs[p - 1];
    }
    for (int g = 0; g < 8; g++) {
        rs_encode(&c->rs, blk[g], 188, blk[g] + 188);
        for (int i = 0; i < 204; i++) {
            int j = (int)(c->nbytes % 12);
            uint8_t o = blk[g][i];
            if (j) {
                int len = 17 * j;
                o = c->fifo[j][c->fpos[j]];
                c->fifo[j][c->fpos[j]] = blk[g][i];
                c->fpos[j] = c->fpos[j] + 1 == len ? 0 : c->fpos[j] + 1;
            }
            c->nbytes++;
            if (c->qn + 1 > c->qcap) {
                c->qcap = c->qcap ? c->qcap * 2 : 8192;
                c->q = realloc(c->q, (size_t)c->qcap);
            }
            c->q[c->qn++] = o;
        }
    }
}

/* Produce 'nbits' bits codificados (nbits múltiplo de la salida del perforado). */
static void chain_pull(chain_t *c, uint8_t *bits, int nbits)
{
    const dvbt_punct_t *pu = dvbt_punct(c->rate);
    int nin = nbits / pu->nout * pu->period;
    int need = (nin + c->qbit + 7) / 8;
    while (c->qn < need)
        chain_group(c);
    if (c->tmpcap < nin) {
        c->tmpcap = nin;
        c->tmpbits = realloc(c->tmpbits, (size_t)nin);
    }
    int bi = c->qbit;
    for (int i = 0; i < nin; i++) {
        c->tmpbits[i] = (uint8_t)((c->q[(bi + i) / 8] >> (7 - (bi + i) % 8)) & 1);
    }
    int adv = c->qbit + nin;
    int bytes = adv / 8;
    c->qbit = adv % 8;
    memmove(c->q, c->q + bytes, (size_t)(c->qn - bytes));
    c->qn -= bytes;
    int no = dvbt_conv_encode(&c->cstate, c->tmpbits, nin, c->rate, bits);
    if (no != nbits)
        fprintf(stderr, "mod_dvbt: perforado inconsistente %d != %d\n", no, nbits);
}

/* ---------------------------------------------------------------------------
 * TS de demostración (PAT, NIT, SDT, PMT).
 * ------------------------------------------------------------------------- */
static uint32_t mpeg_crc32(const uint8_t *d, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= (uint32_t)d[i] << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

/* Empaqueta una sección PSI en un paquete de 188 B (cabe en un paquete). */
static void put_section(uint8_t *pkt, int pid, int cc, const uint8_t *sec, int len)
{
    memset(pkt, 0xFF, 188);
    pkt[0] = 0x47;
    pkt[1] = (uint8_t)(0x40 | (pid >> 8));
    pkt[2] = (uint8_t)pid;
    pkt[3] = (uint8_t)(0x10 | (cc & 15));
    pkt[4] = 0; /* pointer_field */
    memcpy(pkt + 5, sec, (size_t)len);
}

static int sec_finish(uint8_t *s, int body_len /* hasta antes de CRC, sin cabecera de 3 B */)
{
    int total = 3 + body_len + 4;
    s[1] = (uint8_t)(0xB0 | (((total - 3) >> 8) & 0x0F));
    s[2] = (uint8_t)((total - 3) & 0xFF);
    uint32_t crc = mpeg_crc32(s, total - 4);
    s[total - 4] = (uint8_t)(crc >> 24);
    s[total - 3] = (uint8_t)(crc >> 16);
    s[total - 2] = (uint8_t)(crc >> 8);
    s[total - 1] = (uint8_t)crc;
    return total;
}

void dvbt_demo_ts(uint8_t *out, int npkt, const char *service_name)
{
    uint8_t pat[64] = {0}, sdt[160] = {0}, nit[96] = {0}, pmt[64] = {0};
    const char *nm = service_name && *service_name ? service_name : "BladeRF Lab";
    int nl = (int)strlen(nm);
    if (nl > 60)
        nl = 60;
    /* PAT: tsid 1, programa 1 → PMT 0x100 */
    pat[0] = 0x00;
    pat[3] = 0x00; pat[4] = 0x01; pat[5] = 0xC1; pat[6] = 0; pat[7] = 0;
    pat[8] = 0x00; pat[9] = 0x01; pat[10] = 0xE1; pat[11] = 0x00;
    int patn = sec_finish(pat, 5 + 4);
    /* NIT (actual, 0x40): network_id 0x3001, nombre de red; sin bucles de TS */
    nit[0] = 0x40; nit[3] = 0x30; nit[4] = 0x01; nit[5] = 0xC1;
    int p = 8;
    nit[p++] = 0xF0; nit[p++] = (uint8_t)(2 + 8); /* network_descriptors_length */
    nit[p++] = 0x40; nit[p++] = 8;                /* network_name_descriptor */
    memcpy(nit + p, "BladeRF", 7); p += 7; nit[p++] = '!';
    nit[p++] = 0xF0; nit[p++] = 0;                /* transport_stream_loop_length = 0 */
    int nitn = sec_finish(nit, p - 3);
    /* SDT (actual, 0x42): servicio 1 con nombre */
    sdt[0] = 0x42; sdt[3] = 0x00; sdt[4] = 0x01; sdt[5] = 0xC1;
    sdt[8] = 0x30; sdt[9] = 0x01; sdt[10] = 0xFF;
    p = 11;
    sdt[p++] = 0x00; sdt[p++] = 0x01; sdt[p++] = 0xFC;
    int dl = 2 + 1 + 1 + 11 + 1 + nl; /* cabecera descriptor + tipo + len proveedor + proveedor + len nombre + nombre */
    sdt[p++] = (uint8_t)(0x80 | ((dl >> 8) & 15)); sdt[p++] = (uint8_t)dl;
    sdt[p++] = 0x48; sdt[p++] = (uint8_t)(dl - 2);
    sdt[p++] = 0x01; /* digital television service */
    sdt[p++] = 11; memcpy(sdt + p, "BladeRF Lab", 11); p += 11;
    sdt[p++] = (uint8_t)nl; memcpy(sdt + p, nm, (size_t)nl); p += nl;
    int sdtn = sec_finish(sdt, p - 3);
    /* PMT: PCR 0x101, vídeo H.264 0x101 */
    pmt[0] = 0x02; pmt[3] = 0x00; pmt[4] = 0x01; pmt[5] = 0xC1;
    pmt[8] = 0xE1; pmt[9] = 0x01; pmt[10] = 0xF0; pmt[11] = 0x00;
    pmt[12] = 0x1B; pmt[13] = 0xE1; pmt[14] = 0x01; pmt[15] = 0xF0; pmt[16] = 0x00;
    int pmtn = sec_finish(pmt, 14);
    int cc[4] = {0, 0, 0, 0}, cnull = 0;
    for (int i = 0; i < npkt; i++) {
        uint8_t *o = out + (size_t)i * 188;
        int k = i % 32;
        if (k == 0)
            put_section(o, 0x0000, cc[0]++, pat, patn);
        else if (k == 8)
            put_section(o, 0x0010, cc[1]++, nit, nitn);
        else if (k == 16)
            put_section(o, 0x0011, cc[2]++, sdt, sdtn);
        else if (k == 24)
            put_section(o, 0x0100, cc[3]++, pmt, pmtn);
        else {
            memset(o, 0xA5 ^ (i & 0xFF), 188);
            o[0] = 0x47; o[1] = 0x1F; o[2] = 0xFF; o[3] = (uint8_t)(0x10 | (cnull++ & 15));
            for (int j = 4; j < 188; j++)
                o[j] = (uint8_t)((i * 31 + j * 7) ^ (i >> 3));
        }
    }
}

/* ---------------------------------------------------------------------------
 * OFDM.
 * ------------------------------------------------------------------------- */
void dvbt_mod_defaults(dvbt_mod_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->n = 8192;
    c->guard = 3;
    c->constellation = 2;
    c->hp = c->lp = 1;
    c->cell_id = 0x1234;
    c->frames = 4;
}

int dvbt_mod_run(const dvbt_mod_cfg_t *c, const uint8_t *ts_hp, int npkt_hp, const uint8_t *ts_lp, int npkt_lp,
                 iqbuf_t *out, char *err, size_t errlen)
{
    const int N = c->n, K = dvbt_ncar(N), L = N / dvbt_guard_div(c->guard), nd = dvbt_ndata(N);
    const int v = 2 * (c->constellation + 1);
    const bool hier = c->hierarchy != 0 && v > 2;
    const int alpha = hier ? (c->hierarchy == 1 ? 1 : c->hierarchy == 2 ? 2 : 4) : 1;
    if (!ts_hp || npkt_hp < 1) {
        snprintf(err, errlen, "sin flujo de transporte");
        return -1;
    }
    chain_t *ch[2] = {chain_create(ts_hp, npkt_hp, c->hp), NULL};
    if (hier)
        ch[1] = chain_create(ts_lp ? ts_lp : ts_hp, ts_lp ? npkt_lp : npkt_hp, c->lp);
    fft_plan_t *pl = fft_plan_create(N);
    int cp[200], tps[80];
    int ncp = dvbt_cp_list(N, cp), ntps = dvbt_tps_list(N, tps);
    uint8_t *kind = calloc((size_t)K, 1); /* 1 = continuo, 2 = TPS */
    float *tpsval = calloc((size_t)K, sizeof(float));
    cf32 *X = malloc(sizeof(cf32) * (size_t)N);
    int nb0 = dvbt_stream_bits(N, v, hier, 0), nb1 = hier ? dvbt_stream_bits(N, v, hier, 1) : 0;
    uint8_t *b0 = malloc((size_t)nb0 + 8), *b1 = malloc((size_t)nb1 + 8);
    if (!pl || !kind || !tpsval || !X || !b0 || !b1 || !ch[0] || (hier && !ch[1])) {
        snprintf(err, errlen, "sin memoria");
        chain_free(ch[0]); chain_free(ch[1]);
        free(kind); free(tpsval); free(X); free(b0); free(b1);
        if (pl) fft_plan_destroy(pl);
        return -1;
    }
    for (int i = 0; i < ncp; i++)
        kind[cp[i]] = 1;
    for (int i = 0; i < ntps; i++)
        kind[tps[i]] = 2;
    out->rate = DVBT_FS;
    iqbuf_reserve(out, c->frames * 68 * (N + L));
    int base = out->n;
    for (int f = 0; f < c->frames; f++) {
        dvbt_tps_t t = {.frame = (c->frame0 + f) & 3, .constellation = c->constellation, .hierarchy = c->hierarchy,
                        .hp = c->hp, .lp = c->lp, .guard = c->guard, .mode = N == 8192 ? 1 : 0,
                        .length = c->cell_id >= 0 ? 31 : 23};
        t.cell_byte = c->cell_id < 0 ? 0 : (t.frame & 1) ? (c->cell_id & 0xFF) : (c->cell_id >> 8);
        uint8_t s[68];
        dvbt_tps_build(&t, s);
        for (int l = 0; l < 68; l++) {
            const int par = l & 1;
            chain_pull(ch[0], b0, nb0);
            if (hier)
                chain_pull(ch[1], b1, nb1);
            const uint32_t *m0 = dvbt_bitmap(N, v, hier, par, 0);
            const uint32_t *m1 = hier ? dvbt_bitmap(N, v, hier, par, 1) : NULL;
            memset(X, 0, sizeof(cf32) * (size_t)N);
            int dp = 0;
            for (int k = 0; k < K; k++) {
                cf32 val;
                float wk = 2.0f * (0.5f - (float)dvbt_prbs(k));
                if (kind[k] == 2) {
                    tpsval[k] = l == 0 ? wk : (s[l] ? -tpsval[k] : tpsval[k]);
                    val = (cf32){tpsval[k], 0};
                } else if (kind[k] == 1 || dvbt_is_scattered(k, l)) {
                    val = (cf32){wk * 4.0f / 3.0f, 0};
                } else {
                    unsigned word = 0;
                    for (int i = 0; i < v; i++) {
                        uint32_t a = m0[dp * v + i];
                        uint8_t bit;
                        if (a != 0xFFFFFFFFu)
                            bit = b0[a];
                        else
                            bit = b1[m1[dp * v + i]];
                        word |= (unsigned)bit << i;
                    }
                    val = dvbt_qam_point(v, alpha, word);
                    if (c->dbg_data)
                        c->dbg_data[((size_t)f * 68 + (size_t)l) * (size_t)nd + (size_t)dp] = val;
                    dp++;
                }
                X[((k - (K - 1) / 2) % N + N) % N] = val;
            }
            /* IFFT = conj(FFT(conj(X)))/N */
            for (int i = 0; i < N; i++)
                X[i].im = -X[i].im;
            fft_exec(pl, X);
            for (int i = 0; i < N; i++)
                X[i] = (cf32){X[i].re / (float)N, -X[i].im / (float)N};
            for (int i = N - L; i < N; i++)
                iqbuf_push(out, X[i]);
            for (int i = 0; i < N; i++)
                iqbuf_push(out, X[i]);
        }
    }
    /* Normaliza para que el pico quede en 'peak' (PAPR ≈ 11-13 dB). */
    float gain = 0.0f;
    for (int i = base; i < out->n; i++)
        gain = fmaxf(gain, fmaxf(fabsf(out->s[i].re), fabsf(out->s[i].im)));
    gain = (c->peak > 0 ? c->peak : 0.85f) / gain;
    for (int i = base; i < out->n; i++) {
        out->s[i].re *= gain;
        out->s[i].im *= gain;
    }
    chain_free(ch[0]);
    chain_free(ch[1]);
    free(kind); free(tpsval); free(X); free(b0); free(b1);
    fft_plan_destroy(pl);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Interfaz de catálogo.
 * ------------------------------------------------------------------------- */
static int parse_rate(const char *s)
{
    static const char *r[] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(s, r[i]))
            return i;
    return -1;
}

static int dvbt_modulate(const char *params, iqbuf_t *out, char *err, size_t errlen)
{
    dvbt_mod_cfg_t c;
    dvbt_mod_defaults(&c);
    char service[64] = "BladeRF Lab", tsfile[256] = "";
    if (params && params[0]) {
        jtok_t t[64];
        int nt = json_parse(params, strlen(params), t, 64);
        if (nt < 1 || t[0].type != J_OBJ) {
            snprintf(err, errlen, "params no es un objeto JSON válido");
            return -1;
        }
        char s[256];
        double v;
        int i;
        if ((i = json_obj_get(params, t, 0, "mode")) >= 0 && json_str(params, t, i, s, sizeof s))
            c.n = !strcmp(s, "2k") ? 2048 : 8192;
        if ((i = json_obj_get(params, t, 0, "guard")) >= 0 && json_str(params, t, i, s, sizeof s))
            c.guard = !strcmp(s, "1/32") ? 0 : !strcmp(s, "1/16") ? 1 : !strcmp(s, "1/8") ? 2 : 3;
        if ((i = json_obj_get(params, t, 0, "constellation")) >= 0 && json_str(params, t, i, s, sizeof s))
            c.constellation = !strcmp(s, "qpsk") ? 0 : !strcmp(s, "16qam") ? 1 : 2;
        if ((i = json_obj_get(params, t, 0, "hierarchy")) >= 0 && json_num(params, t, i, &v))
            c.hierarchy = v == 1 ? 1 : v == 2 ? 2 : v == 4 ? 3 : 0;
        if ((i = json_obj_get(params, t, 0, "hp")) >= 0 && json_str(params, t, i, s, sizeof s) && parse_rate(s) >= 0)
            c.hp = parse_rate(s);
        if ((i = json_obj_get(params, t, 0, "lp")) >= 0 && json_str(params, t, i, s, sizeof s) && parse_rate(s) >= 0)
            c.lp = parse_rate(s);
        if ((i = json_obj_get(params, t, 0, "cell_id")) >= 0 && json_num(params, t, i, &v))
            c.cell_id = (int)CLAMP(v, -1, 65535);
        if ((i = json_obj_get(params, t, 0, "frames")) >= 0 && json_num(params, t, i, &v))
            c.frames = (int)CLAMP(v, 1, 16);
        if ((i = json_obj_get(params, t, 0, "frame0")) >= 0 && json_num(params, t, i, &v))
            c.frame0 = (int)CLAMP(v, 0, 3);
        if ((i = json_obj_get(params, t, 0, "service")) >= 0)
            json_str(params, t, i, service, sizeof service);
        if ((i = json_obj_get(params, t, 0, "ts_file")) >= 0)
            json_str(params, t, i, tsfile, sizeof tsfile);
    }
    uint8_t *ts = NULL;
    int npkt = 0;
    if (tsfile[0]) {
        FILE *f = fopen(tsfile, "rb");
        if (!f) {
            snprintf(err, errlen, "no se puede abrir ts_file");
            return -1;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        npkt = (int)(sz / 188);
        ts = malloc((size_t)npkt * 188 + 1);
        if (!ts || npkt < 1 || fread(ts, 188, (size_t)npkt, f) != (size_t)npkt) {
            fclose(f);
            free(ts);
            snprintf(err, errlen, "ts_file vacío o ilegible");
            return -1;
        }
        fclose(f);
    } else {
        npkt = 1024;
        ts = malloc((size_t)npkt * 188);
        if (!ts)
            return -1;
        dvbt_demo_ts(ts, npkt, service);
    }
    int r = dvbt_mod_run(&c, ts, npkt, NULL, 0, out, err, errlen);
    free(ts);
    return r;
}

const modulator_desc_t MOD_DVBT = {
    .id = "dvbt",
    .name = "DVB-T",
    .air_ok = false,
    .modulate = dvbt_modulate,
};
