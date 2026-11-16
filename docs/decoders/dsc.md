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

## Validación con la norma ITU-R M.493-15 (V1, 2026-11-16)

**Sin grabación real** (no existe ninguna pública de DSC VHF que se haya encontrado; ninguno de los
decodificadores de referencia instalados lo decodifica), pero la norma M.493-15 (ITU, descargable
gratis en itu.int; PDF `R-REC-M.493-15-201901-S`) se leyó y se contrastaron las constantes. Resultado:

| Constante | Antes | Según M.493 | Acción |
|---|---|---|---|
| Bits de comprobación (bits 8-10) | nº de ceros, **LSB primero** en el modulador; el decodificador aceptaba ambos órdenes | nº de ceros (B) en binario, **MSB primero** (§1.1.1, Tabla A1-1: p. ej. 6 ceros = YYB) | corregido: modulador MSB-first, decodificador solo MSB-first; test con 15 símbolos de la Tabla A1-1 |
| Tonos | 1300 Hz = 1, 2100 Hz = 0 | 2100 Hz = B = 0 (más alta), 1300 Hz = Y = 1 (§1.4) | confirmado |
| Especificador de formato | un solo símbolo | **dos idénticos seguidos** ("2 identical characters", Fig. 1) | decodificador: se elimina el duplicado antes de ECC y análisis (se tolera uno solo); modulador: lo emite duplicado |
| EOS | un símbolo + ECC | 3 veces en DX y 1 en RX; el ECC va justo tras el primer EOS (§9, Fig. 1b); en el ECC solo cuenta un FS y un EOS | el decodificador ya leía el ECC tras el primer EOS; el modulador emite los dos EOS extra |
| ECC | XOR de los símbolos FS…EOS | bits menos significativos de la suma módulo 2 (paridad vertical par) (§10.2) | confirmado |
| Categorías | 100/108/110/112 | rutina/seguridad/urgencia/socorro (Tabla A1-3) | confirmado |
| Naturaleza del socorro | 100-110, 112 | igual (§8.1.1) | confirmado |
| Especificadores | 102, 112, 114, 116, 120, 123 | igual | confirmado |
| Telemandos | 107 J3E, 109 acuse, 110 retrans., 111 TTY-FEC, 113 TTY-ARQ | 109 J3E TP, **110 acuse de socorro, 112 retransmisión de socorro**, 113 TTY-FEC, 115 TTY-ARQ, 118 prueba, 121 posición | corregidos |
| Acuse/retransmisión de socorro | dos telemandos + identidad | **un** telemando (110 ó 112) + identidad + naturaleza + posición + hora + comunicación (Tablas A1-4.2/4.3) | corregido el análisis |
| Posición | cuadrante 0 NE, 1 NO, 2 SE, 3 SO + gg mm + ggg mm | igual (§8.1.2); 9999999999 = sin posición | confirmado |
| Hora | hhmm; 8888 = no disponible | igual (§8.1.3) | confirmado |

Lo que sigue **sin validar con tráfico real**: la polaridad de tonos con una señal de aire, la
asignación de huecos DX/RX con una grabación (la norma da "cuatro caracteres entre medias", es
decir, copia RX 5 huecos después, que es lo implementado), y las llamadas de socorro, acuse y
retransmisión de extremo a extremo. Test: `tests/test_dsc.c` (símbolos de la Tabla A1-1 y un acuse
de socorro de la Tabla A1-4.2).
