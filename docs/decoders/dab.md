# DAB / DAB+ (modo I) — `src/decoders/dab.c`, `dab.h`, `src/tx/mod_dab.c`

Decodificador WIDE a **2 048 000 S/s exactos** (muestras Q11 crudas). Obtiene del FIC
el conjunto (EId, etiqueta) y los servicios (SId, etiqueta, subcanal, tasa, protección,
tipo). **No decodifica el MSC (audio).** Referencia: ETSI EN 300 401.

## Parámetros del modo I

T = 1/2 048 000 s. Trama = 196 608 T (96 ms) = símbolo nulo (2656 T) + 76 símbolos de
2552 T (guarda 504 + útil 2048). Símbolo 0 = PRS; 1–3 = FIC; 4–75 = MSC. 1536
portadoras separadas 1 kHz (k = −768..768, k ≠ 0; bin k o 2048+k). D-QPSK entre
símbolos consecutivos. Desentrelazado de frecuencia Π(0)=0, Π(i)=(13·Π(i−1)+511) mod
2048, conservando 256 ≤ Π ≤ 1792 y Π ≠ 1024 (la portadora es Π−1024).

FIC: 12 FIB por trama (30 bytes de FIG + CRC-16 CCITT con init 0xFFFF y salida
complementada). 3 FIB = 768 bits → dispersión (PRBS x⁹+x⁵+1, init 1…1) → convolucional
K=7 tasa 1/4 (0133, 0171, 0145, 0133 octal) con 6 bits de cola → 3096 bits madre →
punzonado: 21 bloques de 128 con PI_16, 3 con PI_15 y 24 de cola con PI_X → **2304
bits**; 4 grupos = 9216 bits = 3 símbolos × 3072 bits.

## Cadena DSP (por trama)

1. **Símbolo nulo**: suma móvil de |x|² (2304 muestras) mínima < 40 % de la media
   (ruido solo ≈ 100 % → sin falsas alarmas).
2. **Sincronía fina** por prefijo cíclico: |Σ x[t+i]·conj(x[t+i+2048])| (i<504) sumado
   en 4 símbolos, búsqueda ±700 (±48 en seguimiento; ±20 muestras/trama a 100 ppm).
3. **CFO fraccional** ε = −arg(ΣC)/2π con los 76 símbolos (error de pocos Hz).
4. **CFO entero** con el PRS: correlación diferencial entre portadoras vecinas con el
   PRS conocido (inmune a la pendiente de fase de la ventana) para m=−300..300; los 8
   mejores se confirman con correlación coherente (el PRS es periódico cada 16
   portadoras, m=±16 correla ~50 %). Aceptado si el mejor ≥ 2,5× el segundo (en
   potencia; en la práctica > 100×). La pendiente medida coincide con la teórica
   (−2π·40/2048) → comprobación de la temporización.
5. FFT de 2048 con la ventana 40 muestras antes de la parte útil; D-QPSK
   d = R_l·conj(R_{l−1}); se quita el giro conocido 2π·m·2552/2048 por símbolo (sin
   esto fallaba para |m| grande: la fase común solo se resuelve a ±45°) y el error de
   fase común residual (arg Σd⁴ = π); decisiones blandas −Re d / −Im d con peso
   natural por amplitud de portadora; desentrelazado.
6. FIC: despunzonado con borrados, Viterbi K=7 blando (`fec.h`), desaleatorizado,
   CRC-16 de cada FIB.
7. FIG 0/0, 0/1 (subcanales: UEP corto con la tabla 8; EEP-A/B largo), 0/2
   (servicios/componentes: audio MP2 ASCTy 0, DAB+ ASCTy 63, datos), 0/17 (PTY), 1/0,
   1/1 y 1/5 (etiquetas; juego 0 EBU Latin, 15 UTF-8, otros aproximados).

Tasa: UEP de la tabla; EEP-A nivel L: 8·tamaño/(12,8,6,4); EEP-B: 32·tamaño/(27,21,18,15).

## Eventos y estado

* `{"kind":"ensemble","eid":"C1D2","label":"…","text":"Ensemble C1D2 \"…\""}`
* `{"kind":"service","sid":"C201","label":"Radio Uno","subch":1,"kbps":128,
  "prot":"EEP 3-A","type":"DAB+","pty":10,"text":"…"}` cuando el servicio está completo
  (etiqueta + componente + datos del subcanal) y cada vez que cambia.
* `state_json`: `{"ensemble":{"id","label"},"services":[{sid,label,subch,kbps,prot,
  type,pty}],"sync":bool,"frames","fib_ok","fib_bad","cfo_hz"}`. Cambio de EId →
  se borra todo. Sin FIB válido 30 s → vacío; servicio sin verse 120 s → baja.

## Modulador (`modulate`, 2,048 MS/s, `air_ok=false`)

Cadena inversa completa: FIG → FIB+CRC → FIC (dispersión, convolucional, punzonado) →
QPSK diferencial sobre el PRS → IFFT 2048 + guarda → trama de 196 608 muestras (nulo +
PRS + 3 FIC + 72 símbolos MSC pseudoaleatorios). Parámetros: `eid`, `label`, `frames`,
`services:[{sid,label,subch,kbps,prot:"3-A"|"4-B"|"uep:24",dabplus,pty}]`.

## Pruebas (`tests/test_dab.c`)

Tablas (permutación de las 1536 portadoras, PRS de módulo 1, CRC-16 GENIBUS check
0xD64E, tabla UEP, ida y vuelta del FIC con ruido blando); síntesis: limpio, 10 dB,
CFO +37,4 / −100,3 / +200,6 kHz (fraccional y entero), ±100 ppm → conjunto y 3
servicios (EEP-A, UEP, EEP-B) correctos; solo ruido (3 s) → 0 eventos; coste ≈ 0,04–0,08
s/s. Con AWGN el FIC se decodifica hasta ~4 dB de SNR (2 dB parcial).

El CFO de ±¼ del ancho de canal (±384 kHz) no cabe a 2,048 MS/s (la señal llegaría a
±1152 kHz): se prueba hasta ±200 kHz (y ±250 kHz a mano).

## Riesgo principal y limitaciones

* **No hay vectores reales ni capturas de DAB** en este entorno y no se pudo leer la
  norma en papel. Las tablas críticas (punzonado PI_15/PI_16/PI_X, desentrelazado, PRS
  h/k',i,n, UEP) se contrastaron con la implementación abierta **welle.io**
  (`fic-handler.cpp`, `protTables.cpp`, `freq-interleaver.cpp`, `phasetable.cpp`,
  `dab-constants.cpp`, `fib-processor.cpp`), no con EN 300 401 directamente.
  Modulador y decodificador comparten esas tablas, así que **los tests propios no
  pueden detectar un error de tabla**: si alguna estuviera mal, la señal sintética
  decodificaría igual pero una emisora real no. Un fallo en PI_15 (3 de 24 bloques)
  degradaría solo el final del FIC; uno en el PRS o en el entrelazado lo impediría
  por completo. Primera prueba real recomendada: bloque 12C (227,360 MHz), mirar
  `cfo_hz`, `fib_ok`/`fib_bad` en `state_json`.
* Solo modo I; sin MSC/audio, sin FIG 0/5, 0/8, 0/13, 0/14, 0/18…; los flags C/N
  (reconfiguración) y OE se ignoran/descartan; la tabla de caracteres es EBU Latin
  completa para el juego 0 (en ella 0x24 es "ł" y "$" está en 0xAB).
* Multitrayecto de SFN: la ventana de FFT se adelanta 40 muestras dentro de la guarda;
  no hay ecualización más allá de la diferencial.

## Validación con señal real (V2, 2026-11-15)

Grabación: sigidwiki.com `DAB.zip` (IQ 8 bit a 2,0 MS/s, 2,7 s; ensemble eslovaco), remuestreada a 2,048 MS/s con
`build/v2/tools/wav2sigmf.py`. `--replay … --decode dab@227360000`:
`Ensemble 5001 "Towercom"` y `Service 5201 "Radio Slovensko" … 96 kbps, EEP 3-A, DAB+` → **sincronía OFDM modo I, DQPSK,
desentrelazado de frecuencia y tiempo, punzonado, Viterbi, dispersión y CRC-16 del FIB confirmados** (36/36 FIB válidos en
3 tramas). Las tablas de punzonado/entrelazado del FIC eran el principal riesgo y **no necesitaron cambios**.
`tests/data/dab_real_fic_soft.bin` guarda los bits blandos del FIC de 3 tramas (27 kB; un fragmento IQ de una trama ocupa
≥ 390 kB y, recortado, no sincroniza). Sigue **sin validar**: tablas UEP/EEP de varios subcanales (solo 1 servicio visto) y
el PRS con otro modo.
