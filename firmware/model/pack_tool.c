/* pack_tool.c — Modelo de referencia (bit a bit) del empaquetado de muestras de RX hacia el FX3
 * (agente F11). Genera los vectores de entrada y la salida esperada del HDL y compara el volcado
 * de la simulación (palabras de 32 bit en hexadecimal, una por línea).
 *
 *   pack_tool vec   <semilla> <tramas> <modo 8|12> <shift> <in.txt> <exp.txt> <sat.txt>
 *   pack_tool check <exp.txt> <out.txt> <palabras_exigidas>
 *
 * Cada «trama» son 64 muestras (múltiplo de 4: el modo de 12 bit cierra su ciclo cada 4 muestras
 * = 3 palabras de 32 bit). Se mezclan tramas de ruido uniforme, señal débil, fondo de escala y
 * rampas, para ejercitar la saturación y los signos.
 *
 * Modo 8 (SC8_Q7 con ganancia digital, lab 1.3):
 *   i' = sat12(i · 2^shift);  byte = (i' >> 4) & 0xFF   (los 8 bit ALTOS de los 12)
 *   flujo de bytes I0 Q0 I1 Q1 …, cada palabra = 4 bytes en orden little-endian (así los lee
 *   libbladeRF como int8 consecutivos). 'sat' = nº de muestras con I o Q recortadas.
 * Modo 12 (SC16_Q11_PACKED, ya existente en la imagen de serie):
 *   flujo de bits LSB primero: I0(12) Q0(12) I1(12) Q1(12) … → 3 bytes por muestra IQ
 *   (4 muestras = 12 bytes = 3 palabras). Es la disposición que desempaqueta libbladeRF en
 *   streaming/sync.c (SYNC_STATE_USING_BUFFER) y firmware/host/unpack12.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME 64

static uint64_t rng_s;
static uint32_t rnd(void)
{   /* xorshift64*: determinista con la semilla */
    rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27;
    return (uint32_t)((rng_s * 0x2545F4914F6CDD1DULL) >> 32);
}

static int sat12(int v) { return v > 2047 ? 2047 : (v < -2048 ? -2048 : v); }

static int gen_sample(int kind, int idx)
{
    switch (kind) {
    case 0: return (int)(rnd() & 0xFFF) - 2048;               /* uniforme */
    case 1: return (int)(rnd() % 129) - 64;                   /* débil (±64) */
    case 2: { int r = rnd() & 3; return r == 0 ? 2047 : r == 1 ? -2048 : r == 2 ? 2046 : -2047; }
    default: return ((idx * 61) % 4096) - 2048;               /* rampa */
    }
}

static int cmd_vec(char **a)
{
    rng_s = strtoull(a[0], NULL, 10) * 0x9E3779B97F4A7C15ULL + 1;
    int nfr = atoi(a[1]), mode = atoi(a[2]), sh = atoi(a[3]);
    FILE *fi = fopen(a[4], "w"), *fe = fopen(a[5], "w"), *fs = fopen(a[6], "w");
    if (!fi || !fe || !fs) { perror("fopen"); return 1; }
    uint8_t bytes[FRAME * 3 + 8];
    unsigned long sat = 0;
    uint32_t pend = 0; int npend = 0; /* acumulador de bits LSB primero para el modo 12 */
    for (int f = 0; f < nfr; f++) {
        int kind = (int)(rnd() % 4), nb = 0;
        for (int n = 0; n < FRAME; n++) {
            int i = gen_sample(kind, n), q = gen_sample(kind, n + 1000);
            fprintf(fi, "%d %d\n", i, q);
            if (mode == 8) {
                int si = sat12(i * (1 << sh)), sq = sat12(q * (1 << sh));
                if (si != i * (1 << sh) || sq != q * (1 << sh)) sat++;
                bytes[nb++] = (uint8_t)((si >> 4) & 0xFF);
                bytes[nb++] = (uint8_t)((sq >> 4) & 0xFF);
            } else {
                int v[2] = { i & 0xFFF, q & 0xFFF };
                for (int k = 0; k < 2; k++) {
                    uint64_t acc = ((uint64_t)v[k] << npend) | pend; /* npend < 8 siempre */
                    int nbits = npend + 12;
                    while (nbits >= 8) { bytes[nb++] = (uint8_t)(acc & 0xFF); acc >>= 8; nbits -= 8; }
                    pend = (uint32_t)acc; npend = nbits;
                }
            }
        }
        for (int k = 0; k + 3 < nb; k += 4)
            fprintf(fe, "%08X\n", (unsigned)(bytes[k] | bytes[k+1] << 8 | bytes[k+2] << 16 | (uint32_t)bytes[k+3] << 24));
        if (nb % 4) { fprintf(stderr, "trama no alineada a palabra\n"); return 1; }
    }
    fprintf(fs, "%lu\n", sat);
    fclose(fi); fclose(fe); fclose(fs);
    return 0;
}

static int cmd_check(char **a)
{
    FILE *fe = fopen(a[0], "r"), *fo = fopen(a[1], "r");
    long need = atol(a[2]);
    if (!fe || !fo) { perror("fopen"); return 1; }
    char be[64], bo[64];
    long n = 0, bad = 0;
    while (n < need && fgets(be, sizeof be, fe)) {
        if (!fgets(bo, sizeof bo, fo)) break;
        if (strtoul(be, NULL, 16) != strtoul(bo, NULL, 16)) { if (bad < 5) printf("difiere en palabra %ld: esp %s obt %s", n, be, bo); bad++; }
        n++;
    }
    printf("palabras comparadas %ld, diferencias %ld (exigidas %ld)\n", n, bad, need);
    return (bad == 0 && n >= need) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 9 && !strcmp(argv[1], "vec")) return cmd_vec(argv + 2);
    if (argc >= 5 && !strcmp(argv[1], "check")) return cmd_check(argv + 2);
    fprintf(stderr, "uso: ver cabecera\n");
    return 2;
}
