# Decodificador 5G NR (`nr`)

<!-- vision-rapida:inicio (generada por docs/img/src/doc_decoders.py) -->

## Visión rápida

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/decoders/nr-cadena-dark.png">
  <img src="../img/decoders/nr-cadena-light.png" alt="Cadena de procesado de NR">
</picture>

*Figura. Cadena de procesado de NR, dibujada a partir del código de `src/` y de la sección «Cadena DSP» de este documento.*

### Así es la señal

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/decoders/nr-pss-dark.png">
  <img src="../img/decoders/nr-pss-light.png" alt="Espectro de la grabación real y correlación con la PSS en una señal generada.">
</picture>

*Figura. Espectro de la grabación real y correlación con la PSS en una señal generada.*

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../img/decoders/nr-estructura-dark.png">
  <img src="../img/decoders/nr-estructura-light.png" alt="Bloque SSB e identidad de celda.">
</picture>

*Figura. Bloque SSB e identidad de celda.*

La procedencia de cada señal (grabación real o sintética) y qué parte del procesado es Python de ilustración y no el código C se indican en el pie de cada imagen.

### Ejemplo de evento y de estado

Procedencia: grabación IQ real (sigidwiki «NR 10 MHz»): no aparece ninguna celda, así que no hay evento. Sacado de ejecutar el decodificador, no escrito a mano (`bladerf-lab --replay nr.sigmf-meta --decode nr --print`).

Esta grabación no produce ningún evento `dec`.

Último `dec_state` recibido por WebSocket (`state`, listas recortadas):

```json
{
  "windows": 2,
  "cells": []
}
```

### En la web

![NR en la interfaz web](../img/decoders/nr-web.png)

*Captura de la interfaz con un servidor `--replay` propio mostrando NR.*

<!-- vision-rapida:fin -->

Búsqueda de bloques SS/PBCH (SSB) en la rejilla GSCN que cae dentro de la captura: PSS/SSS → PCI (0–1007),
SCS (15/30 kHz), CFO, potencia, y **MIB del PBCH** (DMRS → índice de SSB, polar SCL L = 8 con CRC-24C, SFN, subCarrierSpacingCommon,
ssb-SubcarrierOffset, dmrs-TypeA-Position, pdcch-ConfigSIB1, cellBarred; 38.211 §7.3.3/§7.4.1.4, 38.212 §5.3.1/§7.1, 38.331 §6.2.2). Código:
`src/decoders/nr.c`, secuencias, GSCN, DMRS y polar/MIB en `nr_phy.{h,c}` (tablas en `nr_tables.h`), correlación en `nr_corr.{h,c}` (aislada para C1), modulador
`src/tx/mod_nr.c`, test `tests/test_nr.c`.

Descriptor: `W`, tasa libre ≥ 15 360 000 pero **múltiplo entero de 7,68 Msps** (15,36 … 61,44; otras tasas →
`create` falla con un texto claro). Opción JSON: `{"period":s}` (por defecto `max(2, 0,4·nº de GSCN)` s).

## Teoría y cadena DSP

Referencias: 3GPP TS 38.211 §7.4.2 (PSS, SSS, estructura del SSB), TS 38.104 §5.4.3 (GSCN), TS 38.213 §4.1;
Kim et al., arXiv:2603.29341 (búsqueda por GSCN, solo resumen); *5G NR overview*, arXiv:1806.06898 (PSS: m-secuencia
de 127; SSB: 240 subportadoras × 4 símbolos); técnica de correlación masiva por FFT compartiendo la FFT de la señal
(E38 de `docs/investigacion/INVESTIGACION.md`).

* PSS: `d(n) = 1 − 2x((n + 43·N_ID2) mod 127)`, `x(i+7) = x(i+4) ⊕ x(i)`, `x(0..6) = 0110111` (listado
  `x(6)…x(0) = 1110110`). SSS: `d(n) = [1 − 2x0((n+m0) mod 127)]·[1 − 2x1((n+m1) mod 127)]`, `m0 = 15⌊N_ID1/112⌋ +
  5N_ID2`, `m1 = N_ID1 mod 112`, `x0(i+7) = x0(i+4)⊕x0(i)`, `x1(i+7) = x1(i+1)⊕x1(i)`, `x(0) = 1`. **PCI = 3·N_ID1 +
  N_ID2.** Ambas ocupan las subportadoras 56…182 del SSB, centradas en la 120 = SS_REF = frecuencia del GSCN.
* GSCN: `N·1,2 MHz + {50,150,250} kHz` (< 3 GHz), `3 GHz + N·1,44 MHz` (3–24,25 GHz), `24,25008 GHz + N·17,28 MHz`.

Por cada posición candidata f_g dentro de `centro ± (fs/2 − 2,6 MHz)`:
1. Mezclado a banda base y diezmado a 7,68 Msps (FIR de Kaiser, paso ±2,4 MHz, 70 dB).
2. `nr_corr_run()`: correlación normalizada por FFT (solape-guardado, 8192 puntos) con **6 plantillas** = PSS
   N_ID2 {0,1,2} × SCS {30, 15 kHz} (longitudes 256 y 512). Estadístico `R = pico / mediana` de la NCC (mediana
   submuestreada). Umbral `R ≥ 45`: en ruido blanco el máximo de ≈ 2·10^5 muestras llega a ≈ 17×.
3. CFO grueso: barrido ±15 kHz (paso 2,5 kHz, parábola) de la correlación con la plantilla rotada.
4. SSS: FFT de 256/512 puntos de los símbolos 0 y 2 (separados 2·(N+CP), CP = N·9/128), equalización con el canal
   de la PSS y correlación real con las 336 hipótesis. Umbral 0,45 (ruido ≲ 0,27). El SSS valida de verdad la celda
   (los GSCN vecinos que ven parte de la PSS no pasan).
5. CFO fino: `arg Σ H_sss·conj(H_pss)` entre ambos símbolos (separados 2(N+CP)/7,68 M s); el barrido grueso resuelve
   la ambigüedad de ±7 kHz (30 kHz) o ±3,5 kHz (15 kHz).
6. Potencia: `mean|Y_sss|²/N²` menos el ruido por RE (percentil 20 de la energía por bloques de 256 muestras, /0,95).
   `rsrp_dbfs` es por RE y aproximado; `snr_db` subestima (el ruido medido a 7,68 Msps incluye la caída del FIR).

7. **PBCH → MIB** (`pbch_try` en `nr.c`, codificación y decodificación en `nr_phy.c`). Con la posición de la PSS y el CFO se hacen las FFT
   de los 4 símbolos del SSB (para SCS 30 kHz el SSB ocupa ±3,6 MHz: se vuelve a canalizar solo el trozo del SSB con paso de ±3,65 MHz y se
   corrige la diferencia de retardo de los dos filtros). Para cada una de las 8 hipótesis de ī_SSB:
   * *DMRS* (38.211 §7.4.1.4.1): QPSK de 144 elementos, `c_init = 2^11(ī+1)(⌊N_ID/4⌋+1) + 2^6(ī+1) + N_ID mod 4`, en las subportadoras
     `k ≡ N_ID mod 4`: símbolo 1 (60), símbolo 2 (12 en 0..47 y 12 en 192..239) y símbolo 3 (60). Se elige ī por la autocoherencia de
     `Y·conj(r)` entre pilotos contiguos (con la secuencia buena el canal es suave; con la mala es ruido) y se prueban las tres mejores.
   * *Canal*: por símbolo y región, con la pendiente de fase del retardo retirada, suavizado (1,2,1) e interpolación lineal; MRC.
   * *Demapeo y descodificación*: 432 RE de datos → 864 valores blandos; desaleatorizado con Gold `c_init = N_ID` desplazada `v·864` (v = i_SSB mod 8,
     o mod 4 con L_max = 4); **adaptación de velocidad** E = 864 > N = 512: repetición, entrelazado de subbloques de 32 (38.212 §5.4.1.1); **polar**
     N = 512, K = 56 (32 bits de carga + 24 de CRC), secuencia de fiabilidad Q de la tabla 5.3.1.2-1, sin bits de paridad; decodificador de cancelación
     sucesiva con lista **SCL L = 8** (métrica de camino en LLR, recursión por mitades con f/g, copia completa de estado por bifurcación); desentrelazado de
     los 56 bits con el patrón de 164 de la tabla 5.3.1.1-1; **CRC-24C** (`0xB2B117`); desaleatorizado del contenido (§7.1.2: M = 29, v = 2·SFN₃ + SFN₂,
     sin tocar SFN₂/SFN₃ ni HRF); reordenado por la tabla 7.1.1-1 (G) en MIB de 24 bits + 4 LSB del SFN + HRF + MSB de k_SSB.
   * L_max por frecuencia: < 3 GHz → 4 (ī = i_SSB + 4·n_hf; el HRF decodificado debe coincidir con ī/4), si no 8 (ī = i_SSB; FR1).
   El MIB se refresca cada 8 detecciones de una celda ya conocida.

### Interfaz de correlación para GPU (C1)

```c
nr_corr_t *nr_corr_create(int nfft, int nt, const cf32 *const *tmpl, const int *tlen);
int nr_corr_run(nr_corr_t *c, const cf32 *x, int n, float *out);   /* out[t*nout+i], nout = n − max(tlen) + 1 */
```
Una señal, `nt` ≤ 8 plantillas de energía unidad; salida `|Σ x[i+k]·conj(p_t[k])|² / (Σ|x|² + ε)`. Basta
reimplementar estas dos funciones en `src/gpu/` (una FFT de la señal por bloque, `nt` IFFT, kernel de módulo²/energía
con la suma acumulada de |x|²). El resto (`channelize`, `verify_ssb`) es barato.

## Eventos y estado

Un evento por (PCI, GSCN) nuevo, cuyo SCS cambia o cuyo MIB se decodifica por primera vez:

```json
{"text":"5G NR PCI 321 (N_ID1 107, N_ID2 0) en 3499.68 MHz, SCS 30 kHz, CFO -0.02 kHz, RSRP -41.0 dBFS, SNR 8.8 dB, MIB: SFN 2, SSB 1, k_SSB 0, CORESET0 5, SS0 0",
 "pci":321,"n_id1":107,"n_id2":0,"freq_hz":3499680000,"scs_khz":30,"cfo_hz":-24,"rsrp_dbfs":-41.0,"snr_db":8.8,
 "sss_corr":0.88,"mib":true,"sfn":2,"ssb_idx":1,"l_max":8,"half_frame":0,"scs_common_khz":30,"k_ssb":0,
 "dmrs_type_a_pos":2,"coreset0":5,"search_space0":0,"cell_barred":false,"intra_freq_reselection":"allowed",
 "has_sib1":true,"hits":1}
```
`sfn` = 6 bits del MIB + 4 de la capa física (0–1023); `k_ssb` = 5 bits (el MSB viaja fuera del MIB); `has_sib1` = `k_ssb < 24`
(en FR1, k_SSB ≥ 24 indica que no hay CORESET 0 y, por tanto, SIB1 no está en este SSB; 38.213 §13); `coreset0` y `search_space0` son los
índices de `pdcch-ConfigSIB1` (sus tablas 13-1…13-10 dan el ancho, los símbolos y el offset del CORESET 0). Sin MIB: `"mib":false`.
`state_json`: `{"windows":N,"cells":[{...,"age":s}]}` (caduca a 120 s, 32 celdas). `air_ok = false`.

## Modulador

`{"pci":321,"scs":30|15,"rate":15360000,"nssb":2,"periods":3,"period_ms":20,"start_ms":1.5,"seed":1,"sfn":0,"lmax":8,
"k_ssb":0,"coreset0":5,"ss0":0,"dmrs_pos":2,"barred":false}`: SSB con PSS, SSS y **PBCH real** (MIB de 38.331, DMRS, CRC-24C, polar N = 512,
E = 864, aleatorización y QPSK) con `i_SSB` = índice del SSB en la ráfaga; el SFN avanza 2 por periodo de 20 ms; ráfagas cada 20 ms, SSB
centrado en 0 Hz (SS_REF). Ejemplo de extremo a extremo:
`bladerf-lab --gen nr --gen-params '{"pci":321,"periods":3}' --gen-freq 3499680000 --gen-center 3500000000 --gen-rate 15360000 --out x`
y `bladerf-lab --replay x.sigmf-meta --decode nr@3500000000 --print --duration 5`.

## Pruebas y coste

AWGN 10 dB, CFO (0, ±7,5, +4, −3, +2,5 kHz), deriva ±100 ppm, SCS 30 y 15 kHz, 15,36 y 30,72 Msps: PCI, GSCN, SCS y CFO (±600 Hz) correctos;
solo ruido → 0 eventos; tasa 20 Msps rechazada. Propiedades de las secuencias y GSCN. **PBCH**: DMRS de módulo 1 y distintas por ī, posiciones
(144 + 432 RE), ida y vuelta de codificador y decodificador SCL (60/60 a SNR 0 dB por símbolo, sin contenidos erróneos; un v equivocado nunca
pasa el CRC; SFN 0…1023 y HRF), y extremo a extremo SFN, ssb_idx, k_SSB de 5 bits, dmrs-TypeA, coreset0/ss0, cellBarred, SCS común, SCS 15 kHz a
30,72 Msps, ráfaga de 4 SSB y banda baja con L_max = 4. Coste a 15,36 Msps ≈ 0,06 s de CPU por posición y ventana más ≈ 0,03 s por PBCH decodificado
(solo la primera vez y cada 8 detecciones): ≈ 0,12–0,2 s/s con el periodo automático. **No contrastado con grabaciones reales**: el PBCH solo
está comprobado frente a nuestro modulador y a las tablas de la norma (secuencia Q de 512, patrón de entrelazado de 164, tabla G de 7.1.1-1,
CRC-24C); los valores de las tablas se transcribieron de la norma y se contrastaron con srsRAN como dato.

## Limitaciones

Sin SIB1 (solo se dan los índices de `pdcch-ConfigSIB1`; no se decodifica el CORESET 0 ni el PDCCH de SIB1); L_max = 64 (FR2) sin tratar; SSB con
periodicidad > 20 ms pueden perderse en una ventana de 25 ms; solo FR1 y SCS 15/30 kHz; tasas múltiplo de 7,68 Msps; a 61,44 Msps y GSCN < 3 GHz
(≈ 40 posiciones) el periodo automático es de 16 s. Solo se decodifica el SSB más fuerte de la ráfaga por posición y ventana.
Las constantes de PSS/SSS (estados iniciales de los LFSR, m0/m1) son de memoria de TS 38.211 y se validan por las propiedades de m-secuencia y por
coherencia con el modulador. Los pesos del suavizado y el umbral de ī son heurísticos (no hay SNR < 0 dB probada de extremo a extremo).

## Cómo capturar un SSB de n78 (3,5 GHz)

El centro debe quedar a ≲ 3,5 MHz de un GSCN (3 GHz + N·1,44 MHz) y la tasa ser múltiplo de 7,68 Msps. Con USB 3 se puede usar 30,72 Msps (cubre ≈ 20
posiciones de GSCN) o 61,44 Msps (≈ 40, decodificador con periodo automático largo):

```
bladerf-lab --freq 3500000000 --rate 30720000 --decode nr --print --duration 60
```

Los SSB de n78 en España (3,4–3,8 GHz) suelen ir con SCS 30 kHz y L_max = 8; ráfaga de 5 ms cada 20 ms.

## Intento de validación (V2, 2026-11-15) — sin validar

Se probaron sigidwiki.com `NR_OR_10_downlink.zip` (10 MHz FDD, 10 MS/s) y `NR_4ka_40_downlink.zip` (18 MS/s). No se conoce la frecuencia central
real; con 6 centros candidatos (1800 MHz + k·150 kHz, remuestreo a 15,36 MS/s) no apareció ninguna celda, y una búsqueda independiente de PSS por
correlación no fue concluyente. **PSS/SSS de NR siguen sin contrastar con aire real.**
