# DSC VHF (ITU-R M.493, canal 70)

Fuentes: ITU-R M.493 (formato) y M.541. **El PDF de M.493 no se pudo leer en el
entorno de desarrollo**; se contrastó con el código del decodificador libre
CuteSDR (`dsp/fskdemod.cpp`) y con el resumen de GopherTrunk.

## Qué está verificado y qué no

| Constante | Estado |
|---|---|
| Especificadores 112 socorro, 114 grupo, 116 todos, 120 individual, 102 área, 123 automático | confirmado (CuteSDR/GopherTrunk) |
| Símbolo de fase DX = 125, RX = 111…104, DX/RX con 4 símbolos de separación | confirmado (CuteSDR) |
| EOS 117/122/127 y ECC = XOR desde el especificador de formato | confirmado (CuteSDR) |
| Símbolo = 7 bits de información LSB primero + 3 bits = nº de CEROS | confirmado en parte (la cuenta de ceros, patente; LSB primero, CuteSDR) |
| Orden de los 3 bits de comprobación | **sin verificar**: el receptor acepta LSB o MSB primero |
| Polaridad (¿1300 Hz = 1?) | **sin verificar**: el receptor prueba ambas |
| Categorías 100/108/110/112, naturaleza del socorro 100-112, telemandos, posición (cuadrante+grados+minutos), hora hhmm, MMSI en 5 símbolos de 2 dígitos (10º dígito 0), estructura de la llamada de socorro | de memoria de la norma, **sin verificar** |
| Patrón de puntos de 200 bits, desviación FM de la subportadora | sin verificar (el transmisor usa 200 bits y ±2,5 kHz) |

## Cadena DSP

`fmdisc` → paso alto (CFO) → FIR 2,7 kHz → dos demoduladores AFSK por
correladores (1300/2100 Hz, 1200 Bd, ganancias 0,7/1,4) → bits. Cada bit entra en 10
alineaciones de símbolo × 2 polaridades (20 seguidores): el que ve la secuencia de
fase (símbolo 104 con ≥ 3 de 111…105 en huecos impares y ≥ 3 símbolos 125 en
pares) arranca la llamada. Luego se toma cada símbolo del hueco DX (2i) y, si su
comprobación falla, de su copia RX (2i+5). El primer EOS (≥ 9º símbolo) fija el
final; el ECC se compara (si DX y RX difieren se prueban las combinaciones hasta que
el ECC cuadre). Sin ECC correcto no se emite.

## Mensajes

- Socorro (112): ID propio (5 símbolos), naturaleza, coordenadas (5), hora (2),
  comunicación subsiguiente, EOS.
- Todos los barcos (116): categoría, ID, telemandos 1 y 2, extensión (canal/frecuencia), EOS.
- Individual (120), grupo (114), área (102): igual con dirección de 5 símbolos.
- Retransmisión o acuse de socorro (telemando 109/110): MMSI del socorro,
  naturaleza, posición y hora.

## Eventos y estado

`{"fs","format","category","category_name","mmsi","to","distress_mmsi","nature",
"nature_name","lat","lon","utc","tc1","tc2","tc1_name","subsequent","eos","ext":[],
"symbols":[],"text"}`. `state_json`: últimas 64 llamadas (caducidad 1 h):
`{"calls_total","count","calls":[{"fs","format","mmsi","to","category","nature","lat","lon","age"}]}`.
Una llamada vista por varios seguidores se emite una vez (huella, 8 s).

## Modulador

`mod_dsc.c`: `air_ok = false`. Llamadas de los cinco tipos (por defecto las cinco,
3,9 s); `params`: `type, mmsi, to, area, category, nature, lat, lon, time, tc1, tc2`.

## Pruebas

Símbolos (128 valores ida y vuelta, todo error en la información detectado), ECC,
mensaje hecho a mano, cinco llamadas con AWGN 10 dB / CFO ±4 kHz / ±100 ppm:
45/45, polaridad invertida (IQ conjugado) 5/5, ruido solo 0, coste ~0,005 s/s.
Sin vectores reales (no se pudieron obtener): las pruebas solo garantizan
coherencia emisor-receptor y los hechos confirmados arriba.

## Limitaciones

No se decodifican llamadas con especificadores no listados ni mensajes con
extensiones largas (frecuencia en 3 símbolos se deja en `ext`); sin corrección de
dos errores; el canal 70 de socorro se decodifica pero nunca se emite.

## Validación con señales reales (V1, 2026-11-15)

**Sin validar.** No se encontró ninguna grabación pública de DSC VHF (canal 70, 1200 Bd,
1300/2100 Hz): ni la Signal Identification Wiki ni los repositorios de pruebas consultados la
tienen, y ninguno de los decodificadores de referencia instalados (multimon-ng, acarsdec,
dumpvdl2, redsea, dump1090-fa) decodifica DSC. Los bits de comprobación y las categorías siguen
sin contrastar con tráfico real.
