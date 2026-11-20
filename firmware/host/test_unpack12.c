/* test_unpack12.c — Prueba de unpack12.c (no se enlaza al binario).
 *   gcc -std=c11 -O2 -Wall -Wextra -I. test_unpack12.c unpack12.c -o /tmp/t && /tmp/t
 * Comprueba: (a) contra la fórmula literal de libbladeRF (sync.c, SC16_Q11_PACKED) con datos
 * aleatorios; (b) contra un empaquetador de flujo de bits LSB primero (el del modelo
 * firmware/model/pack_tool.c); (c) SC8 con ganancia: ida y vuelta con el modelo de la FPGA. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unpack12.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* copia literal de libbladeRF (host/.../streaming/sync.c) */
static void libbladerf_unpack(const uint8_t *meta_sample_ptr, int16_t *dest_ptr, size_t samples_to_copy)
{
    size_t zz, jj;
    for (zz = 0, jj = 0; zz < 2*samples_to_copy; zz+=4, jj+=3) {
        dest_ptr[zz+0] = (int16_t)((((uint16_t*)(meta_sample_ptr))[jj+0] & 0x0FFF) << 4) >> 4;
        dest_ptr[zz+1] = (int16_t)((((uint16_t*)(meta_sample_ptr))[jj+1] & 0x00FF) << 8) >> 4
            | ((((uint16_t*)(meta_sample_ptr))[jj+0] & 0xF000) >> 12);
        dest_ptr[zz+2] = (int16_t)((((uint16_t*)(meta_sample_ptr))[jj+2] & 0x000F) << 12) >> 4
            | ((((uint16_t*)(meta_sample_ptr))[jj+1] & 0xFF00)) >> 8;
        dest_ptr[zz+3] = (int16_t)((((uint16_t*)(meta_sample_ptr))[jj+2] & 0xFFF0)) >> 4;
    }
}

static uint32_t s = 12345;
static uint32_t rnd(void) { s = s * 1664525u + 1013904223u; return s >> 8; }

int main(void)
{
    enum { N = 4096 };
    static uint8_t raw[N * 3 + 16] __attribute__((aligned(4)));
    static int16_t a[2 * N], b[2 * N], iq[2 * N];
    for (int it = 0; it < 200; it++) {
        for (int i = 0; i < 2 * N; i++) iq[i] = (int16_t)((int)(rnd() & 0xFFF) - 2048);
        /* empaquetado LSB primero: I0 Q0 I1 Q1 … */
        memset(raw, 0, sizeof raw);
        size_t bit = 0;
        for (int i = 0; i < 2 * N; i++)
            for (int k = 0; k < 12; k++, bit++)
                if ((iq[i] >> k) & 1) raw[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        unpack_sc12_to_sc16(raw, a, N);
        libbladerf_unpack(raw, b, N);
        CHECK(memcmp(a, b, sizeof a) == 0);
        CHECK(memcmp(a, iq, sizeof a) == 0);
    }
    /* SC8 con ganancia: la FPGA hace sat12(v<<s) y se queda con los 8 bit altos */
    for (unsigned sh = 0; sh < 4; sh++) {
        uint8_t by[2 * 2048]; int16_t v[2 * 2048], o[2 * 2048];
        for (int i = 0; i < 2 * 2048; i++) {
            int x = (int)(rnd() % 513) - 256;            /* señal débil (±256 de 2048) */
            v[i] = (int16_t)x;
            int g = x * (1 << sh); if (g > 2047) g = 2047; if (g < -2048) g = -2048;
            by[i] = (uint8_t)((g >> 4) & 0xFF);
        }
        unpack_sc8_to_sc16(by, o, 2048, sh);
        int maxerr = 0;
        for (int i = 0; i < 2 * 2048; i++) { int e = abs(o[i] - v[i]); if (e > maxerr) maxerr = e; }
        /* error de cuantificación: ±8 (medio LSB de 8 bit) dividido por 2^s, +1 de redondeo */
        CHECK(maxerr <= 8 / (1 << sh) + 1);
        printf("SC8 shift=%u: error máximo %d (límite %d)\n", sh, maxerr, 8 / (1 << sh) + 1);
    }
    printf(fails ? "unpack12: FALLO (%d)\n" : "unpack12: OK\n", fails);
    return fails != 0;
}
