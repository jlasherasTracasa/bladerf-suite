/*
 * pfb_plan.c — Diseño (C puro, siempre compilado) del filtro prototipo del
 * canalizador PFB. Ver pfb_gpu.h.
 */
#include "pfb_gpu.h"

#include "../dsp/filter.h"

void pfb_design_proto(float *h, int M, int P)
{
    /* P·M coeficientes: Kaiser de P·M−1 (impar) y un cero final. Corte a fs/M,
     * es decir a ±1 canal: −6 dB en el centro del vecino (ver pfb_gpu.h). */
    const int L = P * M;
    fir_design_lowpass(h, L - 1, 1.0 / M, kaiser_beta(80.0));
    h[L - 1] = 0.f;
}
