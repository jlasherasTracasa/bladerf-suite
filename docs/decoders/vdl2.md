# VDL Modo 2 (D8PSK 10 500 Bd, AVLC, ACARS sobre AVLC)

Ficheros: `src/decoders/vdl2.c` (demodulador y tablas), `vdl2_frame.{c,h}` (cabecera, aleatorizador,
RS entrelazado, HDLC), `vdl2_avlc.{c,h}` (direcciones y control AVLC), `acars_blk.{c,h}` (ACARS sobre
AVLC), `src/tx/mod_vdl2.c`, `tests/test_vdl2.c`.

## Teoría y estado de verificación

VDL Modo 2 (ICAO Anexo 10 vol. III, Doc 9776; ETSI EN 301 841-1; ARINC 631) usa canales de 25 kHz
(136,725/136,775/136,875/136,975 MHz en Europa), modulación **D8PSK a 10 500 símbolos/s** (31,5 kbit/s),
filtro de coseno alzado α = 0,6 (RRC en cada extremo) y acceso CSMA. Una ráfaga:

| campo | contenido | origen de la constante |
|---|---|---|
| rampa | 3-5 símbolos nulos | Kim et al. (KICS 2009), arXiv:2005.11456 |
| palabra única | 16 símbolos: `000 010 011 110 000 001 101 110 001 100 011 111 101 111 100 010` | arXiv:2005.11456 |
| cabecera | 3 bits reservados (0) + longitud 17 bits LSB primero (bits del flujo AVLC, sin paridad RS ni relleno) + 5 bits de paridad | misma fuente; el polinomio **no** está publicado en lo que pude consultar |
| datos | AVLC → RS(255,249) → entrelazado → aleatorizador | Kim et al. (reparto y entrelazado); aleatorizador de memoria |

Codificación diferencial: el triplete (Gray) se convierte a binario y es el incremento de fase en
pasos de π/4 (000→0, 001→1, 011→2, 010→3, 110→4, 111→5, 101→6, 100→7; Tabla 5 de Kim et al.).
Los bits forman un flujo continuo desde la UW (48 bits UW + 25 cabecera = 73 bits: los datos
empiezan a mitad de un símbolo).

RS: GF(2⁸) con 0x187, fcr 120, 6 raíces (preset `rs_init_vdl2` de `dsp/fec`, procedente de dumpvdl2).
Bloques de 249 bytes de datos; el último, más corto, lleva 0/2/4/6 bytes de paridad según tenga
≤2 / 3-30 / 31-67 / ≥68 bytes (Kim et al., Tabla 3). Se codifica como bloque de 255 con ceros
virtuales entre datos y paridad; el receptor reconstruye los ceros y marca como **borrados** la paridad
no enviada. Entrelazado: se escriben los bloques por filas y se leen por columnas (byte p de cada
bloque, p = 0…254), saltando los bytes que no existen.

HDLC/AVLC: el flujo de bits (octetos MSB primero tras el RS) contiene flags 0x7E, relleno de ceros tras
cinco unos, tramas con bytes LSB primero y FCS CRC-16/X-25 (byte bajo primero). Trama AVLC:
destino(4) origen(4) control(1) información FCS(2). Dirección: 4 octetos × 7 bits (bit 0 = extensión),
28 bits = ICAO(24) | indicador(1) | tipo(3). Tipos usados: 1 aeronave, 4 estación de tierra,
6 tierra (cualquiera), 7 difusión. Control como HDLC módulo 8 (I, RR/RNR/REJ/SREJ, UI/XID/TEST/SABM/DISC/UA/DM/FRMR).
ACARS sobre AVLC: información que empieza por SOH (0x01) + bloque ACARS (se enmascara el bit 7).

**Constantes NO contrastadas con la norma** (actualización V1 abajo: la UW y el D8PSK sí se contrastaron con una captura real) (el test solo comprueba que
modulador y decodificador son coherentes entre sí): polinomio y orden de la paridad (25,20)
(x⁵+x²+1, primer bit = grado más alto; el decodificador sigue adelante aunque el síndrome falle y
confía en RS/FCS), aleatorizador (x¹⁵+x¹⁴+1, estado inicial 0x6959, salida = bit realimentado, reinicio
al primer bit de datos), qué bytes de paridad se omiten en el último bloque (se asume que se envían
los primeros), tipos de dirección y significado del bit 3, SOH inicial en AOA y presencia de BCS.
Si alguna fuera distinta, el efecto sería **no decodificar tráfico real** (sin falsos positivos: todo
se valida con RS y FCS). Con una captura real se contrastaría en minutos.

## Cadena DSP (105 kHz = 10 muestras/símbolo)

Ver la cabecera de `vdl2.c`. Resumen: detector de energía por bloques de 256 muestras con suelo de
ruido adaptativo (coste nulo en reposo) → ráfaga completa en memoria → CFO grueso por centroide
espectral (necesario: ±6,25 kHz ≈ 0,6 veces la tasa de símbolo) → RRC adaptado → correlación diferencial
con la UW (módulo independiente de CFO y fase; fija el tiempo) → CFO fino con la fase de esa
correlación (±5,25 kHz sin ambigüedad) → lazo de Gardner con Farrow cúbico (absorbe ±100 ppm) →
decisión diferencial con seguimiento de fase → cabecera → RS → HDLC → AVLC.

## Eventos

```json
{"proto":"vdl2","src":"4840D6","src_type":"aircraft","dst":"2B4F10","dst_type":"ground","cr":0,
 "ftype":"I","ctrl":0,"poll":0,"ns":0,"nr":0,"len":58,"info":"01 32…",
 "acars":{"mode":"2","reg":"EC-MXY","label":"H1","block":"3","msgno":"M12A","flight":"IB3456","body":"…"},
 "cfo_hz":2501,"rs_fixed":0,"level_db":-14.0,
 "text":"4840D6(aircraft) > 2B4F10(ground) I ns=0 nr=0 | ACARS EC-MXY IB3456 H1: …"}
```
Solo se emiten tramas con FCS correcto. `info` = primeros 64 bytes en hexadecimal. `acars` aparece si
la información empieza por SOH y se interpreta como bloque ACARS.

## state_json

`{"bursts":N,"frames":N,"bad":N,"ac":[{"icao","reg","flight","msgs","age","last"}],"gs":[{"icao","msgs","age","last"}]}`
— aeronaves y estaciones de tierra vistas (TTL 900 s, máx. 256 cada una).

## Pruebas (`make test T=vdl2`)

Autoconsistencia de cabecera (corrección de 1 bit en todas las posiciones), aleatorizador, RS entrelazado
(bloque corto, 249, 2-3 bloques, ráfagas de 6 bytes repartidas por el entrelazado), HDLC con relleno y
FCS, direcciones; modulador → AWGN 10 dB, CFO ±6,25 kHz, ±100 ppm → eventos; tipos UI/RR/XID/I; trama
de 350 bytes (2 bloques RS); ruido solo y tono continuo → 0 eventos; coste con tráfico denso.
Medido: 40/40 ráfagas a 10 dB con CFO ±6,25 kHz y ±100 ppm (39/40 a 8 dB); coste ≈ 0,01 s de CPU por s de señal.

## Referencias

* ICAO Annex 10 Vol. III Part I §6 y Doc 9776; ETSI EN 301 841-1; ARINC 631/620 (no consultadas, de pago).
* Kim et al., "VDL Mode-2 D8PSK 모뎀 설계 및 개발", J. KICS 34(11), 2009 (UW, tablas RS, entrelazado, mapa diferencial).
* arXiv:2005.11456, "Advanced Physical-Layer Technologies in VHF Data Link Communications" (formato de ráfaga).
* Wavecom, "VDL-M2 Aeronautical Data Link" (visión general).

## Limitaciones

Sin vectores reales (ver arriba); una ráfaga por vez (solapes de dos estaciones en el mismo canal no se separan);
ráfaga máx. ~20 s de memoria; no se interpretan XID (parámetros), ISO 8208/X.25 ni CLNP/ATN: se da la
información en hexadecimal; no hay reensamblado de mensajes ACARS multibloque.

## Validación con señales reales (V1, 2026-11-15) — resultado parcial

Fuente: "VDL-M2 IQ" de la Signal Identification Wiki (IQ estéreo a 48 kHz, 46,9 s, canal en banda
base, ~50 ráfagas de ~24 ms a 21 dB sobre el suelo). **Hallazgos con evidencia:**

* El WAV trae **I/Q intercambiados** (espectro invertido): tanto `dumpvdl2` 2.6.0 como este
  decodificador solo funcionan con `Q + jI`. Con eso `dumpvdl2` decodifica 11 tramas AVLC reales
  (aeronaves 47806D/4BAA41, ACARS LN-RPA SK0690 y TC-JRA TK1952, "Media Advisory"); sin
  intercambiar no decodifica ninguna. (Todos los demás pasos de `dumpvdl2` exigen S16 a
  105 kS/s por canal: hay que escalar ×3-×14 y sobremuestrear.)
* **Constantes confirmadas:** velocidad de símbolo 10 500 Bd (línea cíclica a 10 494 Hz), RRC con
  α = 0,6, D8PSK, y la **palabra única**: la correlación diferencial con la UW de este
  decodificador da 0,98-0,99 en las ráfagas reales (los 8 incrementos de fase y el mapeo Gray
  quedan verificados). Los 3 bits reservados de la cabecera son 000 en 29 de 61 ráfagas (frente
  a 7,6 esperadas al azar), así que el reparto de símbolos tras la UW es correcto.
* **No resuelto:** los 22 bits siguientes (longitud de 17 bits + 5 de paridad) **no cumplen el
  código (25,20) con x⁵+x²+1 ni con ninguna permutación de los 3 bits del símbolo, ni con un
  aleatorizador aditivo x¹⁵+x¹⁴+1 / x¹⁵+x+1 de cualquier fase** (búsqueda exhaustiva sobre 61
  ráfagas), de modo que la cabecera, el polinomio del código o el aleatorizador son otros. El
  decodificador encuentra 61 ráfagas y 0 tramas sobre esta grabación. Pendiente: contrastar con
  la norma (ETSI EN 301 841-1 §5.4) o con una traza de la referencia.
* La salida de `--gen vdl2` tampoco la decodifica `dumpvdl2` (0 tramas, con y sin ruido, escala
  ×1-×14): coherente con la cabecera/aleatorizador erróneos.

### Segunda pasada (V1, 2026-11-16)

* **ETSI EN 301 841-1 V1.1.1 §5.4** (pdf público de etsi.org) confirma la estructura del
  entrenamiento: rampa de 5 símbolos, palabra única de 16 símbolos, **1 símbolo reservado**, longitud
  de 17 bits (LSB primero; cuenta los bits que siguen a la cabecera sin el FEC) y 5 bits de FEC de
  cabecera: la cabecera de 25 bits es la implementada. Esa edición **no** da el polinomio del FEC de
  cabecera ni el aleatorizador (están en el Anexo 10 vol. III de la OACI, no libre).
* **Aleatorizador, corregido con la grabación:** con 8 ráfagas cuyo primer octeto de datos es la
  bandera 0x7E y la dirección empieza por 0x14 (trama a la estación 10981A, cuya dirección es
  conocida por `dumpvdl2`), la secuencia de clave medida cumple g[n] = g[n−1] ⊕ g[n−15] (polinomio
  x¹⁵+x¹⁴+1, 23 de 25 comprobaciones; las dos discrepancias vienen de bits de dato no conocidos), con
  los 15 primeros bits **010011100011111**; la cabecera NO se aleatoriza y los datos empiezan en el
  bit 25. El código anterior (estado inicial 0x6959, realimentación x¹⁵+x+1 en la práctica) no
  coincidía. Corregido en `vdl2_frame.c` (el modulador usa la misma función).
* **Longitud:** el decodificador ya no depende solo de la cabecera de 25 bits: prueba su longitud si
  es verosímil y, si no, todas las longitudes D tales que datos + paridad RS caben en los bits
  recibidos (±3 octetos por la rampa de bajada).
* **Por qué sigue sin sacar tramas de esa grabación:** los incrementos de fase de la UW tienen un
  error **sistemático** (media de ±0,5 de incremento por posición, desviación 0,2, el mismo patrón
  en las 50 ráfagas) que no depende del filtro (RRC con β = 0,35-0,8 o caja) ni del instante de
  muestreo: es interferencia entre símbolos del propio WAV (diezmado a 48 kHz con un filtro de
  paso bajo que recorta el espectro de 16,8 kHz). Eso da una tasa de error de bit de ~10 %, más
  de lo que corrigen los 2 octetos de paridad RS de las tramas cortas; `dumpvdl2` solo logra 11 de
  ~50 ráfagas. Con ese BER tampoco se puede decir si el código de la cabecera es el correcto (solo
  2 de 29 cabeceras pasan la paridad, justo lo que se espera con ese BER). Hace falta un
  ecualizador entrenado con la UW, o una grabación de mejor calidad.
