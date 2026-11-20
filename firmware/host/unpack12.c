/* unpack12.c — Desempaquetado en el host de SC16_Q11_PACKED y de SC8 con ganancia (F11).
 *
 * 1) Formato de 12 bit empaquetado (ya existente en la imagen de serie 0.16, bit 21 de
 *    config_gpio = BLADERF_GPIO_HIGHLY_PACKED_MODE; fifo_writer.vhd, pack_sc12q11). Los 8
 *    componentes (I0 Q0 I1 Q1 I2 Q2 I3 Q3) de 12 bit de 4 muestras ocupan 96 bit = 3 palabras
 *    de 32 bit; el flujo de bits va LSB primero:
 *
 *      w0 = [ c2[7:0] | c1[11:0] | c0[11:0] ]
 *      w1 = [ c5[3:0] | c4[11:0] | c3[11:0] | c2[11:8] ]
 *      w2 = [ c7[11:0] | c6[11:0] | c5[11:4] ]
 *
 *    con c0=I0 c1=Q0 c2=I1 c3=Q1 c4=I2 c5=Q2 c6=I3 c7=Q3 (complemento a dos de 12 bit, ±2048
 *    = fondo de escala del ADC). Es la misma aritmética que libbladeRF en
 *    host/libraries/libbladeRF/src/streaming/sync.c (SYNC_STATE_USING_BUFFER, ramas de
 *    SC16_Q11_PACKED): allí se hace dentro de bladerf_sync_rx() y entrega SC16_Q11. Por tanto el
 *    host NO necesita una función propia salvo para rendimiento (esta versión usa 3 lecturas de
 *    32 bit por 4 muestras en lugar de 12 accesos de 16 bit con máscaras).
 *    Limitaciones del formato en libbladeRF 2.6.1: solo bladerf_sync_rx (el asíncrono lo
 *    rechaza), sin metadatos ni marca de tiempo, buffer_size múltiplo de 3·8192 bytes.
 *    Caudal: 3 B/muestra → a 45 MB/s (USB 2) 15,0 Msps (SC16: 11,25 Msps; SC8: 22,5 Msps).
 *
 * 2) SC8_Q7 con ganancia digital (lab 1.3, registro GAIN8 = índice 23): la FPGA multiplica
 *    I y Q por 2^s (s = 0..3), satura a 12 bit y envía los 8 bit altos. Para devolver la
 *    escala original: v12 = ((b << 4) + 8) / 2^s (+8 = medio LSB, pues la FPGA trunca). El error de cuantificación
 *    baja a ±8/2^s; el precio es el recorte de las señales fuertes (contador CNT_SAT8).
 */
#include "unpack12.h"

static inline int16_t sext12(uint32_t v) { return (int16_t)((int16_t)(v << 4) >> 4); }

static inline uint32_t ld32le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void unpack_sc12_to_sc16(const uint8_t *in, int16_t *out, size_t nsamples)
{
    for (size_t n = 0; n + 4 <= nsamples; n += 4, in += 12, out += 8) {
        uint32_t w0 = ld32le(in), w1 = ld32le(in + 4), w2 = ld32le(in + 8);
        out[0] = sext12(w0);
        out[1] = sext12(w0 >> 12);
        out[2] = sext12((w0 >> 24) | (w1 << 8));
        out[3] = sext12(w1 >> 4);
        out[4] = sext12(w1 >> 16);
        out[5] = sext12((w1 >> 28) | (w2 << 4));
        out[6] = sext12(w2 >> 8);
        out[7] = sext12(w2 >> 20);
    }
}

void unpack_sc8_to_sc16(const uint8_t *in, int16_t *out, size_t nsamples, unsigned shift)
{
    int32_t rnd = shift ? (1 << (shift - 1)) : 0;
    for (size_t k = 0; k < 2 * nsamples; k++) {
        int32_t v = (int32_t)(int8_t)in[k] * 16 + 8;   /* +½ LSB: la FPGA trunca (>>4), sin sesgo */
        out[k] = (int16_t)((v + rnd) >> shift);
    }
}
