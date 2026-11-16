/* ber.c (DOC2b) — BER de nuestro código convolucional K=7 (0171,0133) con el
 * Viterbi de src/dsp/fec.c sobre BPSK en canal AWGN.
 * Series: sin codificar, Viterbi duro (signo), blando float, blando int8 y 3/4
 * punzonado (DVB) blando. Eb/N0 de 0 a 7 dB. CSV: ebn0_db,serie,bits,errores */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "dsp/fec.h"

static uint64_t s = 88172645463325252ull;
static double urand(void)
{
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return ((s >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand()); }

#define NB 2000 /* bits de datos por bloque */

int main(void)
{
    conv_code_t code = conv_nasa_k7();
    viterbi_t *v = viterbi_create(&code, NB);
    const conv_punct_t *p34 = conv_punct_dvb("3/4");
    static uint8_t in[NB], enc[(NB + 8) * 4], out[NB];
    static float sf[(NB + 8) * 4];
    static int8_t si[(NB + 8) * 4];
    printf("ebn0_db,serie,bits,errores\n");
    for (double eb = 0; eb <= 7.01; eb += 0.5) {
        long bits = 0, e_unc = 0, e_hard = 0, e_soft = 0, e_i8 = 0, e_p = 0;
        int blocks = eb < 3 ? 400 : (eb < 5 ? 1500 : 6000);
        double lin = pow(10.0, eb / 10.0);
        for (int b = 0; b < blocks; b++) {
            for (int i = 0; i < NB; i++) in[i] = urand() < 0.5;
            bits += NB;
            double sg = sqrt(0.5 / lin); /* BPSK ±1, Es = Eb */
            for (int i = 0; i < NB; i++) e_unc += ((in[i] ? 1.0 : -1.0) + sg * gauss() > 0) != in[i];
            int nc = conv_encode(&code, in, NB, enc, CONV_TERM_ZERO, NULL);
            double r12 = (double)NB / nc;
            double sg2 = sqrt(1.0 / (2.0 * r12 * lin));
            for (int i = 0; i < nc; i++) sf[i] = (enc[i] ? 1.f : -1.f) + (float)(sg2 * gauss());
            viterbi_decode_f(v, sf, nc, NB, CONV_TERM_ZERO, NULL, out);
            for (int i = 0; i < NB; i++) e_soft += out[i] != in[i];
            for (int i = 0; i < nc; i++) {
                int q = (int)lrintf(sf[i] * 40.f);
                si[i] = (int8_t)(q > 127 ? 127 : (q < -127 ? -127 : q));
            }
            viterbi_decode_i8(v, si, nc, NB, CONV_TERM_ZERO, NULL, out);
            for (int i = 0; i < NB; i++) e_i8 += out[i] != in[i];
            for (int i = 0; i < nc; i++) si[i] = sf[i] > 0 ? 127 : -127;
            viterbi_decode_i8(v, si, nc, NB, CONV_TERM_ZERO, NULL, out);
            for (int i = 0; i < NB; i++) e_hard += out[i] != in[i];
            int np = conv_encode(&code, in, NB, enc, CONV_TERM_ZERO, p34);
            double r34 = (double)NB / np;
            double sg3 = sqrt(1.0 / (2.0 * r34 * lin));
            for (int i = 0; i < np; i++) sf[i] = (enc[i] ? 1.f : -1.f) + (float)(sg3 * gauss());
            viterbi_decode_f(v, sf, np, NB, CONV_TERM_ZERO, p34, out);
            for (int i = 0; i < NB; i++) e_p += out[i] != in[i];
        }
        printf("%.1f,sin codificar,%ld,%ld\n", eb, bits, e_unc);
        printf("%.1f,Viterbi duro 1/2,%ld,%ld\n", eb, bits, e_hard);
        printf("%.1f,Viterbi blando float 1/2,%ld,%ld\n", eb, bits, e_soft);
        printf("%.1f,Viterbi blando int8 1/2,%ld,%ld\n", eb, bits, e_i8);
        printf("%.1f,Viterbi blando 3/4 (DVB),%ld,%ld\n", eb, bits, e_p);
        fflush(stdout);
    }
    viterbi_destroy(v);
    return 0;
}
