# INVESTIGACION: técnicas implementables en BladeRF Lab (A0)

Fecha: 2026-11-15. Autor: agente A0. Alcance: papers y documentos técnicos cuyas técnicas
se pueden **implementar desde cero** en C11 / CUDA (sin liquid-dsp, FFTW, cuFFT, GNU Radio;
AGENTS.md §0.3).

## Método y garantías de verificación

- Cada entrada lleva un enlace que **apareció en los resultados de las búsquedas** de esta
  sesión (alphaXiv, WebSearch, WebFetch de arXiv). Los autores/títulos de los arXiv marcados
  con (V) los leí en la ficha de arXiv; los demás vienen de la ficha de alphaXiv/buscador.
- **Normas** (ICAO Anexo 10, ETSI EN 300 401, IEC 62106, 3GPP TS 36.211/38.211,
  ETSI EN 300 744, ITU-R M.1371, ARINC 618/620, ETSI TS 102 361, IEEE 802.11, ETSI EN 300 744):
  no pude abrirlas aquí; las cito por nombre y las marco "norma (sin enlace verificado)". Las
  fórmulas que doy de ellas proceden de mi conocimiento del estándar y **deben contrastarse con
  el texto de la norma** antes de dar el módulo por bueno.
- Donde un paper solo se vio por su resumen, lo digo ("solo resumen"): el detalle de
  "qué implementar" entonces es la técnica estándar del campo, no una transcripción del paper.
- Se **excluyen** (por AGENTS.md §6/§0.12 y por encargo) trabajos de suplantación de
  ADS-B/AIS/GNSS, de extracción de identidades de abonado y de descifrado.
- Agentes: D1 rds/dab · D2 ais/dsc/aprs · D3 pocsag/flex/dmr · D4 acars/vdl2 · D5 ism/rs41 ·
  D6 lora/meshtastic · D7 lte/nr/gsm · D8 wifi/dvbt · C1 CUDA · H1 hardware/firmware ·
  T1 emisión · A1 base (host/DDC/TX) · A3 DSP común (fec/sync/pulse).

## Top 20 para implementar ya

| # | Paper / fuente | Técnica concreta | Módulo destino | Agente | Esf. | Impacto |
|---|---|---|---|---|---|---|
| 1 | [Landry ÉTS 2023](https://espace2.etsmtl.ca/id/eprint/26129/1/Landry-R-2023-26129.pdf) + [MDPI Appl. Sci. 13(1):493](https://mdpi.com/2076-3417/13/1/493) + [Gertz/MIT-LL](https://antena.fe.uni-lj.si/literatura/Razno/Avionika/modes/1090-WP19-02-NewADSB.pdf) | Confianza por bit con varias muestras + corrección por síndrome CRC-24 (1 y 2 bits, solo en bits poco fiables) | `src/decoders/adsb.c` | A1 (adsb) | M | Alto: +20-30 % de tramas en tráfico denso |
| 2 | [Stream1090](https://github.com/mgrone/stream1090/wiki) | Entramado por CRC deslizante en vez de preámbulo (recupera tramas solapadas) | `adsb.c` (modo alternativo) | A1 | M | Alto, con riesgo de falsos positivos |
| 3 | [Tapparel et al., arXiv:2002.08208](https://arxiv.org/abs/2002.08208) | Receptor LoRa completo: dechirp+FFT, whitening, Hamming, entrelazado, CRC | `src/decoders/lora*.c` | D6 | L | Alto (LoRa + Meshtastic) |
| 4 | [Tapparel & Burg, arXiv:2502.08485](https://arxiv.org/abs/2502.08485) (V) | Sincronía fina CFO/STO con corrección del SFO ya en el preámbulo | `lora_sync.c` | D6 | M | Alto: sin ella no hay LoRa fiable a ±100 ppm |
| 5 | [Nassralla et al., arXiv:1511.07316](https://arxiv.org/abs/1511.07316) (V) + [Schmidl-Cox 1997](https://doi.org/10.1109/26.650240) | Detección PSS LTE por correlación con 3 raíces ZC + CFO fraccionario por CP | `src/decoders/lte*.c` | D7 | M | Alto |
| 6 | [Bloessl et al., SRIF 2013](https://doi.org/10.1145/2491246.2491248) + [informe técnico](https://www.ccs-labs.org/bib/bloessl2013ofdm-tr/bloessl2013ofdm-tr.pdf) | 802.11a/g: Schmidl-Cox sobre STF, CFO, LTF, ecualización, SIGNAL, Viterbi | `src/decoders/wifi*.c` | D8 | L | Alto |
| 7 | [Merry, arXiv:2303.09886](https://arxiv.org/abs/2303.09886) (V) | Canalizador PFB en GPU (FIR polifásico + FFT por lotes) para N decodificadores | `src/gpu/pfb.cu` | C1 | L | Muy alto (arquitectura) |
| 8 | [Mohammadidoost & Hashemi, arXiv:2011.09337](https://arxiv.org/abs/2011.09337) (V) | Viterbi en GPU: núcleo unificado + traceback paralelo | `src/gpu/viterbi.cu` | C1 | M | Medio-alto (WiFi, DVB-T, DAB) |
| 9 | [Kanaan et al., arXiv:2503.01744](https://arxiv.org/html/2503.01744v1) | AIS: GMSK con Viterbi de lista (PLVA) + CRC para elegir candidata | `ais.c` | D2 | M | Medio-alto (colisiones) |
| 10 | [Pohl & Noack, URH, WOOT 2018](https://www.usenix.org/conference/woot18/presentation/pohl) | Analizador de pulsos OOK/FSK: umbral automático, tamaño de bit, agrupación de longitudes | `ism*.c` | D5 | M | Alto (decoder ISM genérico) |
| 11 | [Mueller & Müller 1976](https://doi.org/10.1109/TCOM.1976.1093326) + Gardner 1986 + [Farrow 1988](https://doi.org/10.1109/iscas.1988.15483) | Recuperación de reloj (TED M&M/Gardner, interpolador Farrow cúbico) | `src/dsp/sync.*` | A3 | S | Alto (base de 10 decodificadores) |
| 12 | [ITU-R M.584-2](https://www.itu.int/dms_pubrec/itu-r/rec/m/R-REC-M.584-2-199711-I!!PDF-E.pdf) + [multimon-ng pocsag.c](https://sources.debian.org/src/multimon-ng/1.3.1%2Bdfsg-1/pocsag.c) | POCSAG: BCH(31,21)+paridad, corrección de ≤2 errores, sincronía 0x7CD215D8 | `pocsag.c` | D3 | S | Alto |
| 13 | [bazjo RS41_Decoding](https://github.com/bazjo/RS41_Decoding) + [rs1729/rs](https://github.com/rs1729/rs/issues/10) | RS41: preámbulo, desaleatorizado XOR, RS(255,231) entrelazado, bloques GPS/PTU | `rs41.c` | D5 | M | Medio |
| 14 | [Roberts-Brown-Loomis 1991 / Brown-Loomis 1993](https://doi.org/10.1109/78.193211) + [Li-Wu-Leong, arXiv:2506.18003](https://arxiv.org/abs/2506.18003) (V) | FAM/SSCA: estimar la densidad de correlación espectral y detectar tasa de símbolos | `src/gpu/scd.cu` | C1 | L | Alto (clasificación automática) |
| 15 | [Hsu & Sheen (NYCU)](https://ir.lib.nycu.edu.tw/bitstream/11536/15650/1/000300426000040.pdf) + [US 8290458](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/8290458) | Calibración TX IQ/LO con bucle interno y tono desplazado | `src/tx/cal*.c` | T1 | M | Alto para TX limpia |
| 16 | [Nuand 2023.02](https://www.nuand.com/?p=17722) + [Estévez](https://destevez.net/2023/02/running-the-ad9361-at-122-88-msps) | Modo oversample 122,88 Msps SC8 y decimación en el host/GPU | `src/sdr/source.c` | H1 | M | Alto (ancho de banda) |
| 17 | [Xhonneux et al., arXiv:2012.08173](https://arxiv.org/abs/2012.08173) (V) | LoRa: detector ML de dos usuarios con colisión | `lora_coll.c` | D6 | L | Medio |
| 18 | [Kim et al., arXiv:2603.29341](https://arxiv.org/abs/2603.29341) + [1806.06898](https://arxiv.org/abs/1806.06898) | NR: PSS (m-secuencia 127) y búsqueda en rejilla GSCN con correlación por FFT | `nr.c` | D7 | M | Medio-alto |
| 19 | [Morgan et al., IEEE TSP 2006](https://doi.org/10.1109/TSP.2006.879264) | DPD con polinomio de memoria generalizado, ajuste por mínimos cuadrados | `src/tx/dpd.c` | T1 | M | Medio |
| 20 | [Merry/Oxford overlap-save GPU, arXiv:1910.01972](https://arxiv.org/abs/1910.01972) | Convolución rápida solape-guardado en memoria compartida (FIR largos de canal) | `src/gpu/` | C1 | M | Medio |

---

## 1. Decodificación

### 1.1 ADS-B / Mode S

**E1. Detección de preámbulo y corrección multi-bit con confianza por bit.**
Landry, tesis ÉTS 2023 ([PDF](https://espace2.etsmtl.ca/id/eprint/26129/1/Landry-R-2023-26129.pdf));
publicado como [MDPI Applied Sciences 13(1):493](https://mdpi.com/2076-3417/13/1/493) (listado Dec 2022
en [DOAJ](https://doaj.org/article/73e82947e7714ad488afc8bb250928db)).
- Aporta: preámbulo con interferencia Mode A/C, declaración de bit con confianza usando varias
  muestras por chip, y corrección de errores; reportan detección 100 % por encima de −86 dBm, probado con BladeRF.
- Implementar: (a) a 2 Msps hay 2 muestras por chip de 0,5 µs → por bit, comparar
  `m(1ª mitad)` vs `m(2ª mitad)` sumando las dos muestras de cada mitad; confianza = `|d|/(a+b)`;
  (b) marcar como "baja confianza" los bits con confianza < umbral (p. ej. los 8–10 peores de 112);
  (c) si el CRC-24 falla, calcular el síndrome `s` y probar patrones de 1 y 2 bits **solo sobre
  bits de baja confianza** (tabla precalculada de síndromes de 1 bit; para 2 bits, para cada
  bit `i` baja confianza buscar `s xor S[i]` en la tabla de 1 bit); (d) para DF17/18 exigir además que el
  ICAO corregido sea plausible (ya visto en tabla de aviones) para limitar falsos positivos.
- Encaja en: `src/decoders/adsb.c` (hoy trabaja a 2 Msps). Test: ruido + errores forzados.
- Riesgos: tasa de falsos positivos con 2 bits (el propio Gertz lo advierte, ver E3); limitar a DF17/18 con ICAO conocido.

**E2. Entramado por CRC sin preámbulo.** [Stream1090](https://github.com/mgrone/stream1090/wiki)
(proyecto abierto, no paper; vía [rtl-sdr.com](https://www.rtl-sdr.com/stream1090-a-new-approach-to-ads-b-demodulation-using-crc-based-framing-instead-of-preamble-detection/)).
- Aporta: registro de desplazamiento continuo de 112 (y 56) bits demodulados, sin preámbulo; una trama
  es válida si CRC-24 = 0 (o corrige 1 bit). Recupera tramas con preámbulo pisado. En un foro se
  informó de ~+19 % de mensajes frente a dump1090-fa (dato de foro, no verificado).
- Implementar: demodulación bit-a-bit en cada fase de muestra (2 fases a 2 Msps), CRC incremental
  O(1) por bit entrante (el CRC de una ventana deslizante se actualiza con el polinomio de ICAO), filtro por
  DF válido + ICAO conocido.
- Encaja en: segundo modo de `adsb.c`, ejecutable en GPU (una ventana por hilo).
- Riesgos: falsos positivos; exigir ICAO visto antes cuando la corrección sea de bits.

**E3. Técnicas históricas MIT Lincoln Laboratory.**
[Gertz, RTCA 1090 WP19-02](https://antena.fe.uni-lj.si/literatura/Razno/Avionika/modes/1090-WP19-02-NewADSB.pdf),
[Harman, DASC 1998](https://archive.ll.mit.edu/mission/aviation/publications/publication-files/ms-papers/Harman_1998_DASC_MS-13181_WW-18698.pdf),
[página MIT-LL](https://www.ll.mit.edu/r-d/publications/techniques-improved-reception-1090-mhz-ads-b-signals).
- Aporta: "bits de baja confianza dentro de una ventana de 24 bits que cubra el síndrome";
  recuperar un preámbulo tapado por una respuesta anterior localizando el último pulso de la respuesta.
- Implementar: algoritmo de corrección de E1 con ese criterio; el segundo punto (ancla en el final) es
  una variante del E2.
- Riesgos: la tasa de error no detectado "sigue en estudio" según el propio documento; medirla con simulación.

**E4. Preámbulo en presencia de solapes con KDE.** Estimación por densidad de núcleo para
sincronizar Mode S interferido ([ficha AMiner](https://www.aminer.cn/pub/627e2dc75aee126c0f955ef8/kernel-density-estimation-for-the-detection-and-synchronization-of-interfered-mode-s), ICNS 2022; solo resumen vía WebSearch).
- Aporta: argumenta que el "Enhanced Preamble Detection" de DO-260B falla con solapes de mayor amplitud,
  y que la correlación cruzada rinde mejor a SNR bajo pero peor en aeropuertos.
- Implementar: como mínimo, correlación cruzada normalizada del preámbulo (patrón 1010000101000000) como
  puntuación, con umbral adaptativo; el KDE es opcional (L).
- Riesgos: solo se leyó el resumen. Esfuerzo/beneficio dudoso; prioridad baja.

**E5. Referencia de CRC y síndromes.** Junzi Sun, [*The 1090 Megahertz Riddle*](https://www.github.com/junzis/the-1090mhz-riddle) (TU Delft OPEN, capítulo de control de errores); la tabla de síndromes de dump1090 también la describe
[mode_s.c](https://www.lip.pt/~pedjor/Offline/mode__s_8c_source.html).
- Implementar: polinomio generador ICAO `0xFFF409` (norma ICAO Anexo 10 Vol. IV, sin enlace verificado), tabla
  de 112 síndromes de un bit.
- Encaja en: `adsb.c`; ya existe parte. Riesgo: ninguno.

### 1.2 AIS

**E6. Viterbi de lista para AIS por satélite.** Kanaan, Amis, Guilloud, Chauvat,
[arXiv:2503.01744](https://arxiv.org/html/2503.01744v1) (autores según WebSearch).
- Aporta: demodulación GMSK con PLVA (parallel list Viterbi) en vez de Viterbi simple; clasifica colisiones en
  totales (misma ranura, celdas distintas) y parciales (desfase por retardo de propagación).
- Implementar: demodulador GMSK BT=0,4 a 9600 Bd (2 o 4 muestras/símbolo tras el DDC a 48 kHz), MLSE de 4 estados
  (memoria de 2 símbolos) con lista de K=4 caminos; para cada candidata, desNRZI + des-stuffing + CRC-16 HDLC
  (poly CCITT) y quedarse con la que valide.
- Encaja en: `ais.c` (D2). Reutiliza `fec` de A3 si se añade el Viterbi.
- Riesgos: en tierra las colisiones son menos frecuentes que en satélite; la ganancia será pequeña. Prioridad media.

**E7. Separación ciega de AIS por ICA.** Krishna et al., *Int. J. Satell. Commun. Netw.* 2020,
DOI 10.1002/sat.1362 ([ficha](https://www.x-mol.com/paper/1288533168742907904)); y
[Sharma et al., IJEAT](https://ijeat.org/?p=22828) (Viterbi duro de-colisión). Solo resumen.
- Aporta: FastICA requiere ≥ 2 antenas; el bladeRF 2.0 micro tiene 2 RX, así que sería factible.
- Implementar: FastICA complejo sobre 2 canales a 48 kHz tras DDC. Exige RX2 simultáneo.
- Riesgos: requiere dos antenas realmente distintas; solo experimental (L).

**E8. Norma AIS.** ITU-R M.1371 (norma, sin enlace verificado): GMSK, NRZI, bit-stuffing, mensajes 1-3, 4, 5, 18, 19, 21, 24, CRC-16/X.25. Implementar según AGENTS §5.

### 1.3 LoRa / Meshtastic

**E9. Receptor LoRa de referencia.** Tapparel, Afisiadis, Mayoraz, Balatsoukas-Stimming, Burg,
[arXiv:2002.08208](https://arxiv.org/abs/2002.08208), SPAWC 2020, DOI 10.1109/SPAWC48557.2020.9154273.
Código de los mismos autores: [gr-lora_sdr](https://github.com/gongmz/gr-lora_sdr) (enlace visto en búsqueda; ver también [EPFL](https://www.epfl.ch/labs/tcl/resources-and-sw/lora-phy)).
- Aporta: cadena PHY completa conforme al estándar (cabecera explícita, CRC) y resultados de BER/PER a SNR bajo.
- Implementar: (1) detección de preámbulo: N upchirps consecutivos (N ≥ 6) con `dechirp·FFT` en bin estable;
  (2) dechirp: multiplicar por el downchirp conjugado y FFT de `2^SF` puntos (a BW muestreado a fs = BW, o
  diezmar a BW); símbolo = `argmax |FFT|`; (3) sync word (2 upchirps modulados) y 2,25 downchirps de SFD;
  (4) CFO entero/fraccionario con la posición del pico en up y down (`cfo = (f_up + f_down)/2`,
  `sto = (f_up − f_down)/2`); (5) Gray inverso, desentrelazado diagonal, des-whitening (LFSR), Hamming CR 4/5..4/8,
  cabecera con CRC de 5 bits, CRC de carga CCITT. Valores de tablas exactos: tomar del paper y de gr-lora_sdr.
- Encaja en: `src/decoders/lora*.c` (D6), NARROW a 500 kS/s (cubre BW 125/250 con submuestreo). El detector de
  chirps comparte FFT de `src/dsp/fft.c`.
- Riesgos: el modo de bajo ritmo de datos (LDRO, SF11/12) y el modo de cabecera implícita; test con generación propia
  (modulador) más vectores de gr-lora_sdr.

**E10. Sincronía fina con SFO en el preámbulo.** Tapparel & Burg,
[arXiv:2502.08485](https://arxiv.org/abs/2502.08485) (V, EPFL, feb 2025).
- Aporta: estima y corrige el desvío de reloj de muestreo (SFO) ya en el preámbulo; mejora CFO y STO.
- Implementar: estimar el SFO como deriva del bin de pico a lo largo de los upchirps del preámbulo (pendiente de
  la fase entre chirps consecutivos), remuestrear con Farrow (E32) antes de estimar CFO/STO fraccionarios.
  El detalle de "dos pasadas" **no figura en la ficha**: leer la sección de método del PDF antes de codificar.
- Encaja en: `lora_sync.c`. Riesgos: sensibilidad al SFO (±100 ppm del test obligatorio de §7 es justo el caso).

**E11. Detector ML de dos usuarios.** Xhonneux, Tapparel, Afisiadis, Balatsoukas-Stimming, Burg,
[arXiv:2012.08173](https://arxiv.org/abs/2012.08173) (V, Asilomar 2020).
- Aporta: separa dos paquetes que colisionan usando el desfase temporal; incluye sincronía robusta a interferencia.
- Implementar: dechirp con el offset del usuario 1, resta de su contribución estimada y redemodulación del 2º
  (SIC), como aproximación; el ML completo es costoso (L).
- Riesgos: la ventaja solo aparece con colisiones; baja prioridad.

**E12. Robustez a baja SNR / colisiones (referencias para evaluar).**
- [SFusion, arXiv:2511.18484](https://arxiv.org/abs/2511.18484): fusión de energía y codificación para SNR bajo; solo resumen.
- [LZn, arXiv:2604.27672](https://www.alphaxiv.org/abs/2604.27672) (2026): sincronía de trama con colisiones del mismo SF; solo resumen.
- [Abboud et al., arXiv:1902.05295](https://arxiv.org/abs/1902.05295) (V): requiere retransmisión con mapas de bits, **no aplicable** a escucha pasiva; se descarta.
- Implementar: nada de momento; léelos si se quiere mejorar la sensibilidad por debajo de −20 dB SNR.

**E13. Ingeniería inversa original.** Knight & Seeber, "Decoding LoRa: Realizing a Modern LPWAN with SDR", GRCon 2016
([enlace](https://pubs.gnuradio.org/index.php/grcon/article/view/8)).
- Aporta: descripción de modulación y codificación (whitening, entrelazado) obtenida por análisis ciego.
- Implementar: usar como contraste de las tablas de E9. Riesgo: el original no cubre la cabecera tan bien como E9.

**E14. LR-FHSS** (Semtech): [arXiv:2010.00491](https://arxiv.org/abs/2010.00491). Descripción del nuevo PHY de saltos de frecuencia de LoRa; futuro módulo "lrfhss"; solo resumen. Prioridad baja.

**E15. Meshtastic.** Sin paper; es LoRa (E9) + AES-CTR con clave por defecto (AGENTS §0.12). Implementar AES-128/256 CTR propio en `aes.c` (D6).

### 1.4 POCSAG / FLEX / DMR

**E16. POCSAG.** [ITU-R M.584-2](https://www.itu.int/dms_pubrec/itu-r/rec/m/R-REC-M.584-2-199711-I!!PDF-E.pdf) (anexo "Radio-paging code No. 1") y
[multimon-ng pocsag.c](https://sources.debian.org/src/multimon-ng/1.3.1%2Bdfsg-1/pocsag.c) como código de referencia.
- Aporta: formato de palabra de 32 bits: 21 de información, 10 de BCH(31,21), 1 de paridad par; BCH puede corregir hasta 2 errores.
- Implementar: polinomio generador (octal 03551 según multimon-ng), tabla de síndromes de 1 y 2 errores en software
  (31×31/2 ≈ 465 entradas), sincronía `0x7CD215D8`, inactiva `0x7A89C197`, preámbulo 576 bits alternados,
  numérico BCD y alfanumérico de 7 bits LSB primero. A 512/1200/2400 bps elegir por búsqueda de sincronía en las tres tasas.
- Encaja en: `pocsag.c` + `fec.c` (A3, BCH).
- Riesgos: inversión de polaridad FM; probar ambos signos.

**E17. FLEX.** Norma ITU-R M.584-2 (anexo "FLEX-TD", el índice solo se vio, no el texto). Sin otra fuente verificada en esta sesión; buscar la especificación o implementar a partir de multimon-ng cuando D3 lo aborde. Riesgo: documentación pública escasa.

**E18. DMR (solo metadatos).** Norma ETSI TS 102 361-1 (sin enlace verificado). Implementar sincronía de 48 bits (BS/MS data/voice), CACH, slot type con Golay(20,8), LC con Hamming/BPTC(196,96) y CRC; **sin decodificar voz** (AGENTS §0.12). Demod 4FSK con 4 niveles, filtro RRC y recuperación de reloj (E32). Riesgo: BPTC es la parte larga.

### 1.5 ACARS / VDL2 / RDS / DAB

**E19. VDL Modo 2 (D8PSK 10 500 Bd).** Referencias: [dumpvdl2](https://github.com/szpajder/dumpvdl2) (decoder abierto); resumen de protocolo de [Wavecom](https://www.wavecom.ch/content/pdf/advanced_protocol_vdl-m2.pdf);
[Gim, Choi, Eun, KICS (coreano)](https://journal.kics.or.kr/digital-library/manuscript/file/33304/NODE01293632.pdf) sobre módem D8PSK; norma ICAO Anexo 10 Vol. III.
- Aporta: parámetros RRC α=0,6, mapeo Gray D8PSK diferencial, sincronía de 16 símbolos (según el resumen de búsqueda de las fuentes).
- Implementar: RRC adaptado, TED Gardner/M&M (E32) a 2-4 muestras/símbolo, demod diferencial `arg(x_k · conj(x_{k-1}))`
  cuantificado a 8 sectores, desaleatorizado, RS(255,249)/cabecera con FEC, AVLC/HDLC, CRC-16.
- Encaja en: `vdl2.c` (D4). Riesgos: ráfagas cortas con 100 ppm → TED rápido en el preámbulo.

**E20. ACARS.** Norma ARINC 618/620 (sin enlace). MSK 2400 sobre AM: demodulación AM → portadoras 1200/2400 Hz, bits con paridad impar y CRC-16 Kermit. Código de referencia no verificado en esta sesión. D4.

**E21. RDS.** [Ejemplo MathWorks RDS/RBDS](https://www.mathworks.com/help/comm/usrpradio/ug/rds-rbds-and-radiotext-plus-rt-fm-receiver.html) + norma IEC 62106 / NRSC-4 (sin enlace).
- Aporta (según el ejemplo): reloj 1187,5 Hz de la piloto de 19 kHz (19000/16), portadora 57 kHz = 3×piloto, Costas, integración bifase y decodificación diferencial.
- Implementar: filtro paso banda 57 kHz, mezcla con 3×piloto (PLL de la piloto, ya existe en `channel.c`), integrate-and-dump con reloj derivado, desdiferenciar, sincronía de bloque con síndrome de 10 bits y palabras de offset A,B,C,C',D. Grupos 0A/0B/2A/2B/4A.
- Encaja en: `rds.c` (D1), NARROW a 250 kS/s. Riesgos: desvío de reloj se resuelve usando la piloto.

**E22. DAB: sincronía y FIC.** Norma ETSI EN 300 401 (sin enlace). Para detección del símbolo nulo y del símbolo de fase de referencia:
patente [US 7729435](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/7729435) (detección de nulo por energía) y el paper
[IEEE 1999, "Joint synchronization in Eureka 147 DAB system based on abrupt phase change detection"](https://ir.lib.nycu.edu.tw/bitstream/11536/31059/1/000082918600007.pdf) (título leído de una ficha de catálogo; la coincidencia del PDF con esa ficha no está comprobada).
- Implementar (modo I): trama 96 ms (196 608 muestras a 2,048 Msps), nulo de 1,297 ms, símbolo PRS; detector de nulo por
  energía en ventana deslizante; sincronía fina por correlación del PRS (conocido) con la FFT de 2048; CFO por diferencia de fase entre portadoras; DQPSK
  diferencial; desentrelazado en frecuencia/tiempo, Viterbi K=7 con puncturing (FIC: 1/3 → tabla), desaleatorizado y CRC-16; decodificar FIB: ensemble, servicios, subcanales (FIG 0/0, 0/1, 0/2, 1/x).
- Encaja en: `dab*.c` (D1), WIDE a 2,048 Msps. Viterbi propio de A3 (también candidato a GPU, E36).

### 1.6 Celular: LTE, NR, GSM

**E23. PSS LTE de baja complejidad.** Nassralla, Mansour, Jalloul, [arXiv:1511.07316](https://arxiv.org/abs/1511.07316) (V, IEEE TVT). También
[EUSIPCO 2014 (Nasraoui et al.)](https://eurasip.org/Proceedings/Eusipco/Eusipco2014/HTML/papers/1569925485.pdf) sobre sector ID y sincronía conjuntos (solo resumen).
- Aporta: PSS con 8 multiplicaciones complejas por posición (frente a 24) mediante agrupación K-means; ligera pérdida.
- Implementar (versión robusta, no la K-means): correlación de la señal a 1,92 Msps con las 3 secuencias ZC (u = 25, 29, 34;
  longitud 62, norma 3GPP TS 36.211 sin enlace) por FFT en dominio de tiempo del PSS (decimado a 0,96 Msps tras filtro de 1,08 MHz),
  máximo entre 5 ms; CFO fraccionario con la fase de la correlación de la PSS y con el CP; SSS por correlación en frecuencia sobre
  las 168 hipótesis × 2 mitades (N_ID^(1) y número de media trama) → PCI = 3·N1 + N2; potencia RSRP desde CRS (opcional).
- Encaja en: `lte*.c` (D7). Riesgo: ±7,5 kHz de CFO deteriora la ZC; probar hipótesis de CFO en {−7,5, 0, +7,5} kHz.

**E24. NR: PSS/SSS y GSCN.** [Kim et al., arXiv:2603.29341](https://arxiv.org/abs/2603.29341) (estimador de timing en dos pasos con muestreo dual, −68 % de tiempo de búsqueda; solo resumen) y
[5G NR overview, arXiv:1806.06898](https://arxiv.org/abs/1806.06898) (PSS = m-secuencia BPSK de longitud 127; SSB 240 subportadoras × 4 símbolos).
- Implementar: SSB a 15,36 Msps (SCS 30 kHz → FFT 512 en el ancho de 7,2 MHz del SSB), barrido de GSCN (rejilla 3GPP TS 38.104: sin enlace);
  correlación con 3 PSS (N_ID^(2) ∈ {0,1,2}) mediante FFT masiva (E30), SSS con 336 hipótesis, PCI = 3·N1 + N2. CFO entero por simetría de la m-secuencia.
- Encaja en: `nr.c` (D7) con la correlación en GPU. Riesgo: dependencia del ancho que alcanza el hardware (30,72 Msps en n78).

**E25. GSM FCCH/SCH.** Referencia de implementación: [gr-gsm](https://gitea.osmocom.org/sdr/gr-gsm) (máquina de estados FCCH → SCH, MLSE con respuesta al impulso del SCH).
No hay paper verificado de un detector FCCH de baja complejidad.
- Implementar: FCCH = tono puro a +67,7 kHz (fs/4 de 270,833 kHz) durante 148 bits → detector por predicción lineal o autocorrelación a 1 símbolo (`arg(x_n·conj(x_{n−1}))` constante) en ventana de ≥ 100 símbolos; SCH: sincronía de 64 bits conocida, MLSE de 16 estados, convolucional 1/2 K=5, paridad 10 bits → BSIC y FN. Norma 3GPP TS 45.002/45.003 (sin enlace).
- Encaja en: `gsm*.c` (D7). Riesgo: SI3 (MCC/MNC/LAC/CI) requiere más de lo mínimo.

### 1.7 WiFi, DVB-T

**E26. WiFi OFDM 802.11a/g.** Bloessl, Segata, Sommer, Dressler, "An IEEE 802.11a/g/p OFDM Receiver for GNU Radio", SRIF 2013, DOI
[10.1145/2491246.2491248](https://doi.org/10.1145/2491246.2491248); [informe técnico CCS-2013-02](https://www.ccs-labs.org/bib/bloessl2013ofdm-tr/bloessl2013ofdm-tr.pdf); código gr-ieee802-11 (proyecto de CCS Labs, mencionado en la búsqueda).
- Aporta: receptor PHY→MAC con detección Schmidl-Cox sobre el STF, CFO grueso/fino, ecualización por LTF y seguimiento de piloto, decodificación de SIGNAL y datos.
- Implementar: a 20 Msps, autocorrelación con retardo 16 (STF) para detección y CFO grueso (`arg/(2π·16·T)`); FFT 64 sobre LTF (retardo 64) para CFO fino y canal;
  SIGNAL (BPSK 1/2) → Rate/Length; datos 6 Mb/s (BPSK 1/2), desentrelazado, Viterbi K=7, desaleatorizado, FCS CRC-32, trama de gestión Beacon: SSID, BSSID, canal.
- Encaja en: `wifi*.c` (D8) WIDE exacta a 20 Msps; Viterbi y FFT de A3/`fft.c`. Riesgos: ~L; empezar por 6 Mb/s.

**E27 (referencia cruzada)**: recuperación de reloj, referencia general para D2/D3/D4/D5/D8.

**E28. 802.11b DSSS 1 Mb/s.** Fuente: hilo [discuss-gnuradio, BBN 802.11b](https://lists.gnu.org/archive/html/discuss-gnuradio/2009-06/msg00155.html) y norma IEEE 802.11 cl. 16 (sin enlace).
- Implementar: Barker de 11 chips `+1 −1 +1 +1 −1 +1 +1 +1 −1 −1 −1` (patrón de la norma; nota del hilo: invertir en el tiempo para el filtro adaptado),
  correlador a 11 Mchip/s, DBPSK, sincronía de 128 bits con scrambler, SFD 0xF3A0, PLCP header con CRC-16, MPDU con FCS. Beacon a 1 Mb/s.
- Encaja en: `wifi*.c`. Riesgo: el hilo es de 2009; verificar el cálculo de scrambler con la norma.

**E29. DVB-T: modo y guarda por correlación del CP + TPS.** Referencias verificadas solo como resúmenes: tesis NYCU
[Liu 2006](https://ir.lib.nycu.edu.tw/bitstream/11536/78134/1/166301.pdf) (detección ciega de modo/guarda con NMC), patentes
[US 7729414](https://patents.google.com/patent/US7729414) y [US 9100226](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9100226) (TPS: 17 portadoras en 2K, 68 en 8K, DBPSK diferencial entre símbolos, palabra de sincronía). Norma ETSI EN 300 744 (sin enlace).
- Implementar: correlación `Σ x[n]·conj(x[n+N])` para N ∈ {2048, 8192} y 4 guardas (1/4, 1/8, 1/16, 1/32): el pico de módulo y su anchura dan modo y guarda; CFO = `arg(pico)/(2πN)`;
  FFT, TPS por DBPSK diferencial entre símbolos sucesivos, integrar 68 símbolos y localizar la palabra de sincronización (`0011010111101110` / `1100101000010001`, según la norma); BCH(67,53) para 53 bits TPS; leer constelación, tasa, guarda, cell ID.
- Encaja en: `dvbt*.c` (D8) a 9,142857 Msps. Riesgo: no se abrió ninguna fuente primaria; las constantes de la palabra TPS deben contrastarse con EN 300 744.

### 1.8 Señales ISM, radiosondas, recuperación de reloj, clasificación

**E30. Inferencia de protocolo OOK/FSK.** Pohl & Noack, "Universal Radio Hacker: A Suite for Analyzing and Attacking Stateful Wireless Protocols", WOOT 2018,
[USENIX](https://www.usenix.org/conference/woot18/presentation/pohl).
- Aporta: flujo de interpretación (demodulación con umbral automático, estimación del tamaño de bit, decodificación con codificaciones personalizadas), análisis de campos y simulación.
- Implementar: envolvente `|x|` suavizada, umbral por Otsu/centro de rango, histograma de longitudes de pulso y hueco → agrupar en 2–3 clústeres (PWM/PPM) o cuantizar con
  el bit corto (Manchester); extraer trama hasta un silencio largo, validar con CRC/suma de verificación de ≥ 10 sensores conocidos (meteorología, TPMS, mandos de 433/868). Salida de reglas de rtl_433 como referencia (no verificada aquí).
- Encaja en: `ism*.c` (D5). Riesgo: la variedad de protocolos; centrarse en 10 con tests con vectores públicos.

**E31. RS41.** [bazjo/RS41_Decoding](https://github.com/bazjo/RS41_Decoding) (documentación: 4800 Bd FSK, trama por segundo, 48 bytes de paridad RS en 0x008–0x037, byte de tipo en 0x038, XOR con tabla pseudoaleatoria de 64 bytes)
y el proyecto [rs1729/rs](https://github.com/rs1729/rs/issues/10) (decodificadores de referencia; aviso: algunas tramas SGM están cifradas y se descartan).
- Implementar: GFSK por discriminador `arg(x_n·conj(x_{n−1}))`, reloj M&M, búsqueda de cabecera `0x10B6CA11229612F8` (comprobar con el código de referencia), desaleatorizado, RS(255,231) con 24 de paridad entrelazados (2×), bloques GPSInfo/GPSPos/Meas; posición ECEF → lat/lon/alt. Constantes exactas desde el repositorio de rs1729 antes de implementar.
- Encaja en: `rs41.c` (D5). Riesgo: variantes SGP/SGM cifradas → ignorar.

**E32. Gardner, Mueller-Müller y Farrow (recuperación de reloj).**
K. H. Mueller & M. Müller, IEEE Trans. Commun. 24(5):516–531, 1976, DOI [10.1109/TCOM.1976.1093326](https://doi.org/10.1109/TCOM.1976.1093326) (1 muestra/símbolo, dirigido por decisión);
F. M. Gardner, "A BPSK/QPSK timing-error detector for sampled receivers", IEEE Trans. Commun. 34(5):423–429, 1986 (2 muestras/símbolo; sin DOI verificado, referencia comprobada por búsqueda);
C. W. Farrow, "A continuously variable digital delay element", ISCAS 1988, DOI [10.1109/iscas.1988.15483](https://doi.org/10.1109/iscas.1988.15483);
Erup, Gardner, Harris, "Interpolation in Digital Modems, Part II", IEEE Trans. Commun. 41(6):998–1008, 1993 ([ficha](https://typeset.io/papers/interpolation-in-digital-modems-ii-implementation-and-2270qgv1is); los autores varían entre fuentes).
- Implementar (A3, `sync.c`): TED Gardner `e = Re{(x[n] − x[n−2]) · conj(x[n−1])}`; M&M `e = Re{ conj(a[n-1])·x[n] − conj(a[n])·x[n-1] }`;
  interpolador Farrow cúbico (4 ramas, μ ∈ [0,1)) controlado por NCO con lazo PI (ancho de banda de lazo ≈ 1 % de la tasa de símbolo); control de fase fraccionaria "sin clics" al re-sintonizar.
- Encaja en: `src/dsp/sync.*` (A3) y todos los D*. Riesgo bajo; prueba obligatoria ±100 ppm.

**E33. Clasificación ligera de modulación.** Estadística de envolvente: [Revisiting R: Statistical Envelope Analysis for Lightweight RF Modulation Classification, arXiv:2506.19956](https://www.alphaxiv.org/abs/2506.19956) (solo resumen);
[G-AMC, arXiv:2604.06402](https://www.alphaxiv.org/abs/2604.06402) (método "verde" interpretable, solo resumen);
[Deep Meets Classical, arXiv:2611.05937](https://www.alphaxiv.org/abs/2611.05937) (fusión eficiente en CPU, solo resumen).
- Implementar sin redes neuronales: momentos de la envolvente (kurtosis, `E|x|^4/(E|x|^2)^2`), cumulantes C20/C21/C40/C42, picos de la FFT de `x^2`/`x^4` (tasa de símbolo/portadora), ancho de banda ocupado de la salida del detector OS-CFAR; reglas de decisión para etiquetar la señal como AM/FM/FSK/PSK/OFDM/CSS (LoRa: chirp).
- Encaja en: `detector.c` (etiqueta junto a cada señal) y sugerencia automática de decodificador. Riesgo: los resúmenes no dan la precisión; validar con nuestras señales sintéticas.

---

## 2. CUDA

**E34. Canalizador polifásico en GPU.** Bruce Merry, "Efficient channelization on a Graphics Processing Unit", [arXiv:2303.09886](https://arxiv.org/abs/2303.09886) (V; 4 antenas × 2 Gsps, límite el PCIe).
- Aporta: un F-engine optimizado, con el ancho de banda de memoria como cuello de botella. El detalle del kernel no está en la ficha: leer el PDF.
- Implementar: PFB de M canales (p. ej. 64 × 31,25 kHz a 2 Msps ... o 1024 a 122,88 Msps): prototipo Kaiser/RRC de longitud `M·K` (K=8..16), kernel que
  carga una ventana por bloque de hilos en memoria compartida, suma polifásica de K términos por rama, FFT de M puntos por lotes (nuestra FFT propia), desplazar canales con fase `exp(−j2π k n / M)` si es PFB desplazado. Salida en tamaño "canal" con tasa `fs/M` y overlap 2× para evitar el scalloping (ver [Overlap channel PFB de CCAT, arXiv:2608.13852](https://www.alphaxiv.org/abs/2608.13852), solo resumen).
- Encaja en: `src/gpu/`, integración en `host.c` (C1) para alimentar 20 decodificadores narrow sin 20 DDC. Alternativa por canal arbitrario: DDC por bloque + FIR por solape-guardado (E35).
- Riesgos: canales no alineados a la rejilla (cresamp posterior); sincronizar flujos CUDA con el hilo DSP sin bloquearlo.

**E35. Convolución rápida solape-guardado en GPU.** Oxford e-Research, [arXiv:1910.01972](https://arxiv.org/abs/1910.01972): FIR cortos/largos en memoria compartida; también aplicable al filtro de canal del DDC masivo.
- Implementar: bloques de `N_fft = 2^k` con `L = N_fft − M + 1` muestras útiles, FFT → producto con la respuesta precalculada → IFFT; uso para filtros de canal estrechos (miles de taps) donde el FIR directo es caro.
- Encaja en: `src/gpu/`. Riesgo: latencia por bloque; solo útil con anchuras muy estrechas.

**E36. Viterbi en GPU.** Mohammadidoost & Hashemi, [arXiv:2011.09337](https://arxiv.org/abs/2011.09337) (V; núcleo unificado + traceback paralelo);
[Tensor Cores, arXiv:2011.13579](https://arxiv.org/abs/2011.13579) (solo resumen);
[Gb/s PBVD, arXiv:1608.00066](https://arxiv.org/abs/1608.00066) (decodificador por bloques con solapes; solo resumen).
- Implementar: K=7 (64 estados, mariposas ACS), bloques solapados de longitud `L + 2·D` (D ≈ 5K = 35 de traceback) para paralelismo entre bloques; métricas `int16`.
- Encaja en: `src/gpu/viterbi.cu` y, para WiFi/DVB-T/DAB, un único núcleo. Riesgo: solo se justifica para flujos de decenas de Mb/s (WiFi en 20 Msps, DVB-T); para AIS/POCSAG la CPU basta.

**E37. FAM/SSCA cicloestacionario en GPU.** Roberts, Brown, Loomis, "Computationally efficient algorithms for cyclic spectral analysis", IEEE Signal Process. Mag., 1991 (referencia leída vía resumen de búsqueda, sin enlace propio);
Brown & Loomis, IEEE TSP 41(2):703–720, 1993, DOI [10.1109/78.193211](https://doi.org/10.1109/78.193211);
Li, Wu, Leong, [arXiv:2506.18003](https://arxiv.org/abs/2506.18003) (V; FAM y SSCA en FPGA Versal con ventanas hasta 2^20, comparados con una RTX 3090).
- Implementar FAM: canalizador de `Np` canales con ventana (Hamming), demodulación compleja, productos conjugados de todos los pares de canales, FFT de longitud `P` sobre los productos → `S_x^α(f)`; en GPU: kernel de productos (matriz `Np × Np` por bloque) + FFT por lotes. SSCA: correlación entre cada canal y la señal completa, FFT larga por tira.
- Uso: detectar tasa de baudios/ranuras (picos en α = fb), distinguir PSK/FSK/OFDM (α = 1/T_sym OFDM), alimentar E33.
- Encaja en: `src/gpu/scd.cu`, mapa α–f en la interfaz. Riesgo: memoria `O(Np²·P)`; usar `Np=64`, `P=256` como valores iniciales.

**E38. Correlación masiva por FFT en GPU.** Técnica general (sin paper específico verificado): correlación `ifft(FFT(x)·conj(FFT(p)))` por solape-guardado con **un solo FFT de la señal y N plantillas** (3 PSS LTE, 3 PSS NR, preámbulos LoRa/DAB PRS, FCCH). El valor está en compartir `FFT(x)` entre hipótesis y en un kernel de máximo con umbral relativo (CFAR). Referencias de apoyo para la GPU: E34/E35 y [DeepSig CUDA para 5G PHY, arXiv:2608.04338](https://www.alphaxiv.org/abs/2608.04338) (solo resumen, enseña la arquitectura residente en GPU de PHY completo).
- Encaja en: `src/gpu/corr.cu` para D7 (lte, nr, gsm), D6 (preámbulos).

**E39. LDPC en GPU (futuro NR/DVB-T2).** [Fast LDPC GPU Decoder for Cloud RAN, arXiv:2009.05534](https://arxiv.org/abs/2009.05534), solo resumen: se anota porque NR PBCH usa Polar, **no** LDPC; **no recomendada ahora**.

---

## 3. Emisión (TX)

**E40. Calibración TX IQ / fuga de LO con bucle interno.**
- [Hsu & Sheen, calibración conjunta TX/RX en transceptores de conversión directa con detector de envolvente y lazo interno](https://ir.lib.nycu.edu.tw/bitstream/11536/15650/1/000300426000040.pdf) (leído solo por ficha de búsqueda);
- patentes públicas: [US 8290458](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/8290458) (bucle con desvío de frecuencia; separa la imagen del TX y del RX),
  [US 9001875](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9001875) (búsqueda ciega de DC y tono de prueba), [US 7715836](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/7715836).
- Implementar en `src/tx/cal*.c` (T1) **solo en bucle `BLADERF_LB_RFIC_BIST`/RF, sin antena** (AGENTS §6): (1) fuga de LO: transmitir 0, medir potencia a LO en el RX, minimizar con búsqueda por coordenadas (I_dc, Q_dc) con el DC del TX; (2) desequilibrio: tono a `+f_t`; imagen en `−f_t`; `IRR = |X(−f_t)|/|X(+f_t)|`; ajustar `(g, φ)` de la matriz `[1 0; tanφ/…]` de corrección hasta minimizar la imagen; usar un desvío entre LO de TX y RX para que la imagen del RX no contamine (E: US 8290458); (3) repetir por rejilla de frecuencias y guardar en tabla.
- Riesgos: el bladeRF ya calibra DC/IQ en su firmware (`bladerf_cal_*`), así que primero medir lo que queda; nada de TX a la antena.

**E41. DPD asequible.** Morgan, Ma, Kim, Zierdt, Pastalan, "A generalized memory polynomial model for digital predistortion of RF power amplifiers", IEEE TSP 54(10):3852–3860, 2006,
DOI [10.1109/TSP.2006.879264](https://doi.org/10.1109/TSP.2006.879264). Banco de pruebas abierto: [OwnDPDLab, arXiv:2607.11612](https://www.alphaxiv.org/abs/2607.11612) (solo resumen); DPD+reciprocidad: [Lund, arXiv:2411.04689](https://www.alphaxiv.org/abs/2411.04689) (solo resumen).
- Implementar: `y[n] = Σ_k Σ_q a_{kq}·x[n−q]·|x[n−q]|^{k−1}`, K=5 (impares), Q=3; identificación por mínimos cuadrados indirecta (aprendizaje con realimentación del bucle interno), `N ≈ 10^4` muestras, resolución por ecuaciones normales regularizadas en doble precisión.
- Encaja en: `src/tx/dpd.c`. Riesgo: el AD9361 no es un PA; la ganancia real es pequeña sin PA externo. Prioridad baja; sirve para estudio y para OFDM de prueba a alta potencia.

**E42. Reducción de PAPR en OFDM.** [FFT-Free PAPR Reduction Methods for OFDM Signals, arXiv:2509.13851](https://arxiv.org/abs/2509.13851) (solo resumen: dos algoritmos de baja complejidad sin FFT);
[Tone injection por ranking de candidatos, arXiv:2604.18060](https://arxiv.org/abs/2604.18060) (solo resumen).
- Implementar lo clásico, medible y simple: *clipping and filtering* iterativo (recorte a `CR = 1,5–2`, FFT, anular fuera de banda, IFFT, repetir 3–5 veces) y TR (reserva de tonos). Útil para los moduladores `wifi`, `dvbt`, `dab`, `lte` solo en `file`/`loopback`.
- Riesgo: EVM vs ACPR; medir con nuestro propio receptor.

**E43. Emulador de canal para probar receptores.** [OpenAirLink, arXiv:2404.09660](https://arxiv.org/abs/2404.09660) (emulador de canal abierto sobre SDR, solo resumen).
- Implementar en `src/tx/` (o `tests/test_util.h`, A1): perfiles ITU/TDL: suma de `L` rayos con retardo fraccionario (Farrow, E32), Doppler de Jakes (suma de sinusoides) con ganancias complejas, ruido AWGN y CFO/desvío de reloj. Todos los tests de §7 ya piden `chan_awgn/cfo/resample/drift`; añadir `chan_multipath` y `chan_fading`.
- Riesgo mínimo; alto valor para validar WiFi/DVB-T/DAB/ADS-B.

**E44. Generación de formas de onda de prueba.** Cada modulador (`mod_<id>.c`) es la fuente; sumar un "rejilla de pruebas" SigMF con múltiples señales mezcladas (superposición de ADS-B + ruido de impulsos) para medir la tasa de colisión de E1/E2. Sin referencia externa.

---

## 4. Firmware/FPGA bladeRF y MIMO

**E45. Modo oversample 122,88 Msps.** [Nuand 2023.02](https://www.nuand.com/?p=17722), [Estévez](https://destevez.net/2023/02/running-the-ad9361-at-122-88-msps) y [issue #940](https://github.com/nuand/bladerf/issues/940).
- Aporta: formato SC8 (8 bit), configuración `feature=oversample` y un reporte de que en ese modo con 50 Msps un pulso de 48 µs se midió ~24 µs (¿tasa efectiva a la mitad?).
- Implementar (H1): abrir con `BLADERF_FORMAT_SC8_Q7`, habilitar `BLADERF_FEATURE_OVERSAMPLE`, tasas hasta 122,88 Msps; alimentar `engine` con un `engine_sanitize` para `int8`; en el host: decimación polifásica en GPU (E34) hacia las tasas de los decodificadores. Medir el caso del issue #940 antes de confiar en la tasa efectiva.
- Riesgo: USB 3 (≈ 350 MB/s con SC8 a 122,88 Msps ×2 = 245 MB/s: está al límite); el modo 8 bit reduce el rango dinámico (−48 dBFS de suelo).

**E46. Quick-tune y barrido rápido.** La release 2021.02 incluyó quick-tune para el bladeRF 2.0 micro (según búsqueda); no se encontró documentación del comportamiento en oversample.
- Implementar (H1): `bladerf_set_frequency` con *retune queue* (`bladerf_schedule_retune`) usando temporización por timestamp para barrer 70 MHz–6 GHz a saltos de 56 MHz; lectura de espectro en el hilo DSP con FFT propia; salida como cascada de banda ancha.
- Riesgo: `bladerf_schedule_retune` y la cola de retune requieren timestamps; verificar en la API 2.6.1 localmente (`~/bladerf-firmware/bladeRF/host/libraries/libbladeRF/include`).

**E47. MIMO 2×RX: correlación y dirección de llegada.**
[Werner et al., arXiv:2502.08592](https://www.arxiv.org/pdf/2502.08592) (dual-channel SDR con matriz de conmutación, MUSIC con suavizado espacial; título no confirmado, solo ficha de búsqueda);
[Xhafa et al., "Pathway to Coherent Phase Acquisition in Multi-channel USRP SDRs for DoA Estimation"](https://ddd.uab.cat/pub/caplli/2023/322164/SDS_2023_Pathway_to_Coherent_Phase_Acquisition_in_Multi.pdf) (trata la coherencia de fase entre canales RF; USRP, lista el bladeRF 2.0 micro como 2×2);
[A Self-Calibrating SDR for High Fidelity Beam- and Null-forming Arrays, arXiv:2604.02498](https://www.alphaxiv.org/abs/2604.02498) (solo resumen).
- Implementar (H1/C1): captura 2×RX con timestamp común (el bladeRF comparte LO entre canales RX); calibración de fase por tono de entrada común o ruido (offset de fase por frecuencia); correlación cruzada `R = E[x1·conj(x2)]` en banda estrecha; para una separación `d` ≈ λ/2 la fase `Δφ = 2π d sinθ / λ`; con 2 elementos solo ambigüedad ±θ y sin resolución de múltiples fuentes. Con 2 antenas, interferómetro de fase (no MUSIC).
- Riesgos: la fase relativa cambia con cada retune; calibrar en cada frecuencia; solo observación pasiva.

**E48. Detección ADS-B en FPGA (imagen `bladerf-adsb`).** Ver sección de firmware. Propuesta H1: **no** flashear (AGENTS §0.11); solo documentar en `docs/hardware/FIRMWARE.md` qué ahorraría (host recibe 128 bits por mensaje en vez de 2 Msps × 4 B = 8 MB/s) y por qué el decodificador del host (E1/E2) lo supera en tasa de mensajes recuperados.

---

## Firmware: imágenes FPGA disponibles en el árbol oficial y propuestas

Árbol: `~/bladerf-firmware/bladeRF/hdl/` (solo lectura). Las imágenes se definen en `hdl/fpga/platforms/<plataforma>/*.qip` y se compilan con `hdl/quartus/build_bladerf.sh`.

**bladeRF 2.0 micro (`platforms/bladerf-micro/`)**

| Imagen (`.qip`) | Qué hace (según el VHDL del árbol) |
|---|---|
| `bladerf.qip` | Imagen estándar: AD9361 + FIFOs RX/TX por FX3 (GPIF), Nios II con UART de órdenes, tamer de VCTCXO, `time_tamer` (timestamps), disparo (`trigger`), AGC ADI (`bladerf_agc_adi_drv.vhd`), modo 8 bit y sc16q11 empaquetado (CHANGELOG). Es la que corre hoy (FPGA 0.16.0). |
| `bladerf-hosted.qip` | La misma imagen en variantes de dispositivo (40, 115, A4, A5, A9; el CHANGELOG habla de compilación en paralelo de las variantes). Nuestro dispositivo es el "micro" con su FPGA concreta. |
| `bladerf-adsb.qip` | `rx-adsb.vhd`, `bladerf-adsb.vhd`: front-end `adsb_fe` (potencia de entrada), `adsb_decoder` con `NUM_DECODERS = 8` en paralelo (detección de preámbulo + 8 decodificadores), y entrega de **128 bits por mensaje** al host por un FIFO `adsbfifo` (modo paquetes). El IP `adsb/quartus/adsb_decoder.qip` se referencia desde `ip/nuand/adsb/`, que **no está en este árbol** (no hay fuentes de `adsb_decoder` ni `adsb_fe`; probablemente submódulo no descargado). |
| `bladerf-wlan.qip` | `bladerf-wlan.vhd` (1128 líneas): instancia `wlan.wlan_top` (módem 802.11 TX/RX, I/Q de 16 bit a/desde el AD9361; salida a DAC con 12 bit), más AGC y PLLs. Los fuentes de la biblioteca `wlan` tampoco están en el árbol. Tiene banco de pruebas (`wlan_fx3_gpif_tb`). |
| `bladerf-foxhunt.qip` | Demo de emisor OOK autónomo (`software/foxhunt/README.md`: TODO generador de tono, control desde Nios, introducir en el flujo TX). Incompleto. |

**bladeRF 1 (`platforms/bladerf/`)**, por completitud: `bladerf`, `bladerf-hosted`, `bladerf-headless`, `bladerf-adsb`, `bladerf-fsk_bridge`, `bladerf-atsc_tx`, `bladerf-qpsk_tx`. No aplican a nuestro 2.0 micro.

**Bloques de IP reutilizables del árbol** (`hdl/fpga/ip/nuand/synthesis/`): `fir_filter`, `nco`, `cordic`, `cos_table`, `iq_correction`, `fsk_modulator`/`fsk_demodulator`, `constellation_mapper`, `symbol_mapper`, `signal_generator` (con 8 bit), `atsc_tx`, `rx_packet_generator`, `bladerf_agc_adi_drv`; más `trigger`, `time_tamer`, `vctcxo_tamer`, `tone_generator`. Hay un `fsk_demodulator` y un `iq_correction` que sirven de modelo para DDC/FSK en FPGA.

**Propuestas (solo documentación; ninguna se flashea, AGENTS §0.11) para `docs/hardware/FIRMWARE.md` (H1)**

1. **Probar `bladerf-adsb` solo en simulación/estudio**: comparar su tasa de mensajes con nuestro E1/E2 sobre la misma grabación (el FPGA da mensajes sin CRC-corrección multi-bit). Conclusión esperada: la ventaja es el ancho de banda USB, no la sensibilidad.
2. **Imagen "DDC+FFT" propuesta (no existe en el árbol)**: 4–8 canales DDC con `nco` + `fir_filter` (CIC + FIR) y decimación por canal en la FPGA, devolviendo canales ya estrechos por USB cuando se trabaja a 61,44/122,88 Msps, que hoy ocupa casi todo el USB 3. Alternativa sin FPGA: PFB en GPU (E34), mucho más barato de desarrollar.
3. **AGC y calibración**: usar el AGC ADI (`bladerf_agc_adi_drv`) y la ganancia manual con tabla propia; añadir calibración de ganancia por frecuencia midiendo un tono (T1/H1) y guardarla en el perfil.
4. **MIMO 2×RX**: la imagen estándar ya entrega 2 canales con timestamp (corrección "MIMO RX timestamp" en el CHANGELOG): suficiente para E47 sin firmware nuevo.
5. **Quick-tune** (E46): solo host y Nios existente.

Dependencias sin resolver (para el orquestador): los fuentes de `adsb_decoder` y de la biblioteca `wlan` no están en `~/bladerf-firmware/bladeRF/hdl/fpga/ip/nuand/`; para estudiarlos habría que obtener los submódulos (no se hizo: AGENTS no permite instalar ni descargar fuera de lo asignado).

---

## Notas finales

- Recuento de entradas: ADS-B 5 (E1–E5), AIS 3 (E6–E8), LoRa/Meshtastic 7 (E9–E15), POCSAG/FLEX/DMR 3 (E16–E18),
  aviación/RDS/DAB 4 (E19–E22), celular 3 (E23–E25), WiFi/DVB-T 4 (E26, E28, E29; E27 apunta a E32), ISM/RS41/reloj/AMC 4 (E30–E33),
  CUDA 6 (E34–E39), TX 5 (E40–E44), firmware/MIMO 4 (E45–E48). Total: 48 (E27 es una referencia cruzada).
- Pendiente de verificar con el texto primario antes de codificar: constantes de DVB-T TPS (E29), cabecera RS41 (E31), "dos pasadas" de E10, detalles del kernel de E34, tablas de LoRa (E9).
