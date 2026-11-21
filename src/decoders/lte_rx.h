/*
 * lte_rx.h — Receptor de la subtrama 5 de una celda LTE para leer SIB1.
 *
 * Entrada: muestras a la tasa N·15 kHz de la celda (N = 128, 256, 512, 1024 o 2048 según el
 * ancho de banda del MIB), ya sin desvío de portadora, y la posición (fraccionaria) del
 * primer símbolo de la subtrama. Hace lo que un móvil antes de leer SIB1: FFT de los 14
 * símbolos, estimación de canal con las CRS de todo el ancho (con la pendiente de fase del
 * retardo retirada antes de interpolar), PCFICH, búsqueda ciega de DCI 1A/1C con SI-RNTI en el
 * espacio común del PDCCH, PDSCH con SFBC y turbodecodificación. Teoría en lte_sib.h.
 *
 * Nunca se mira otro RNTI que el SI-RNTI (información de difusión): el resto de DCI se descarta
 * sin interpretarlo (AGENTS.md §0.12).
 */
#ifndef BLADERF_LAB_LTE_RX_H
#define BLADERF_LAB_LTE_RX_H

#include "lte_sib.h"

typedef struct {
    int pci, nrb, ports, phich_res;
    int fft;                    /* N: 128·2^m */
    int sf;                     /* subtrama (0..9) de la que se parte; 5 para SIB1 (0 = como 5 si no se indica sf_set) */
    bool sf_set;
    int sfn;                    /* nº de trama (para la RV de SIB1 con DCI 1C) */
} lte_rxcfg_t;

typedef struct {
    int cfi;                    /* 0 si el PCFICH no se decodificó */
    double pcfich_margin;       /* mejor / segunda correlación */
    int dci_fmt;                /* 0 ninguno, 1 = 1A, 2 = 1C */
    int dci_cce, dci_l;
    int mcs, rv, rb_start, nprb, nprb1a, tbs;
    bool distributed;
    int tbs_tried;
} lte_rx_info_t;

/* 1 si decodificó SIB1 (rellena *out), 0 si no, −1 si la subtrama no cabe en x. */
int lte_rx_sib1(const lte_rxcfg_t *cfg, const cf32 *x, int nx, double t0, lte_sib1_t *out, lte_rx_info_t *info);

#endif
