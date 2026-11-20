# Piloto de calibración por inyección (P3a)

Objetivo: comprobar que se puede medir P(el decodificador recupera un mensaje
inyectado) en función de la SNR, con intervalos de confianza, sin tocar `src/`.
Protocolos: ais, pocsag (2400 bps), aprs, lora (SF7, BW125, LoRaWAN) y adsb
(una trama de identificación). Ninguno se cambió.

## Reproducir

`paper/exp/piloto/medir.sh` (compila en `build/p3a`, escribe el IQ en
`build/p3a/piloto/` y lo borra al terminar; ~16 min con 10 núcleos). Semilla
fija (`SEED`, por defecto 20261120). `COARSE=1` hace el barrido grueso que fijó
las rejillas de `rejillas.sh`. Salidas: `resultados.json`, `curvas.csv`,
`curvas.png`, `falsos_positivos.json`.

## Método

1. `bladerf-lab --gen <id> --gen-params …` genera una variante limpia por copia,
   con un identificador único dentro del mensaje (MMSI, RIC, texto de APRS,
   FCnt de LoRaWAN, ICAO). Así cada copia se empareja de forma exacta con su
   evento (no por recuento).
2. `inyectar.py` recorta la ráfaga activa (ADS-B: solo la 1.ª trama), la
   multiplica por una amplitud que fija la SNR, le aplica fase aleatoria y
   desvío de frecuencia uniforme en ±1 kHz (0 en ADS-B), y la suma al fondo en
   instantes sorteados (huecos de 20 ms a 200 ms según protocolo) sobre 10
   ficheros («shards») por caso. Los niveles de SNR van mezclados en cada
   fichero y el ruido es constante. N = 100 copias por punto de SNR (1 dB).
3. `bladerf-lab --replay f.sigmf-meta --decode <id>@<Hz> --print --duration <largo
   del fichero>`; 10 réplicas en paralelo, puertos 8940-8949. Duración = largo
   del fichero (con 0,6 s de cola vacía), así que no hay vueltas.
4. `ajustar.py`: una copia cuenta si aparece un evento con su identificador.
   Wilson 95 % por punto. Ajuste por máxima verosimilitud de
   `p = g + (c−g)·sigmoide((SNR−s50)/w)` (suelo g, techo c; sin suelo ni techo no
   ajusta la meseta de ~0,94 de POCSAG ni el suelo de 0,2 del fondo real). Las
   columnas «50 %» y «90 %» son las SNR donde la curva ajustada vale 0,5 y 0,9
   en términos absolutos; IC95 % por la región 2ΔLL ≤ 3,84.

### Definición de SNR

SNR = P_señal / P_ruido, en dB, **dentro del ancho ocupado B** del protocolo
(el que imprime `--gen`: ais 18,6 kHz, pocsag 21,7 kHz, aprs 13,9 kHz, lora
125 kHz (el BW de la modulación; `--gen` imprime 419 kHz con lóbulos, no se usó), adsb 2 MHz = toda la banda).
P_señal = potencia media de la ráfaga activa × fracción de esa potencia dentro
de B (Welch, variante 0; resultó 0,98–1,0). P_ruido = potencia del fondo
integrada en B (Welch, Hann, 2048 muestras, 50 %) alrededor de la frecuencia del
canal. En ruido blanco vale σ²·B/fs, con σ² = 60² (LSB Q11) por muestra compleja.
En el fondo real se escala la grabación para que su P_ruido en B sea igual a la
del caso blanco: la diferencia entre curvas es estructura del fondo, no nivel.

### Fondos

* Blanco: gaussiano complejo.
* Real (de `tests/data/real/`, otra banda o protocolo): para ais/pocsag/aprs,
  `ism_f007th` (433,92 MHz, 250 kS/s, cu8, 0,2 s, ruido de receptor RTL y ráfagas
  ISM); para lora/adsb, `lte` (LTE 1,4 MHz, 50 ms) remuestreado por FFT a 1 y 2
  MS/s. Se repite en bucle con giros aleatorios. No hay ninguna grabación real
  de ruido sin señal.

Tasas de muestreo: 250 kS/s (ais, pocsag, aprs), 1 MS/s (lora), 2 MS/s (adsb).

## Resultados (N = 100 por punto)

SNR en dB, IC95 % entre corchetes; NA = la curva no llega.

| Protocolo | Fondo | SNR al 50 % | SNR al 90 % | Suelo | Techo |
|---|---|---|---|---|---|
| ais | blanco | 10,0 [10,0; 10,2] | 11,6 [11,6; 11,8] | 0,00 | 1,00 |
| ais | real | 15,7 [15,5; 16,0] | 18,4 [18,1; 18,8] | 0,17 | 1,00 |
| pocsag | blanco | 0,5 [0,3; 0,5] | 1,8 [1,4; 2,2] | 0,00 | 0,94 |
| pocsag | real | −4,3 [−4,6; −4,1] | −0,2 [−1,2; 0,6] | 0,05 | 0,92 |
| aprs | blanco | 9,0 [9,0; 9,2] | 9,9 [9,9; 10,2] | 0,00 | 1,00 |
| aprs | real | 16,4 [16,4; 16,6] | 17,1 [17,0; 17,5] | 0,00 | 1,00 |
| lora | blanco | −5,0 [−5,0; −4,8] | −3,2 [−3,3; −3,0] | 0,00 | 1,00 |
| lora | real | −4,0 [−4,2; −4,0] | −1,9 [−2,1; −1,5] | 0,00 | 1,00 |
| adsb | blanco | 6,6 [6,6; 6,6] | 8,0 [8,0; 8,2] | 0,00 | 1,00 |
| adsb | real | 7,7 [7,4; 8,0] | 11,7 [11,2; 12,2] | 0,22 | 0,99 |

Falsos positivos en 60 s de fondo sin copias (5 protocolos × 2 fondos): 0 eventos.
Con copias presentes hubo eventos de contenido ajeno (superan CRC/BCH sin ser
una copia) solo en POCSAG: 114 (blanco) y 245 (real); son tramas parciales con
SNR bajo que el BCH «corrige» (corrected 2–5). Ninguno en ais, aprs, lora, adsb.

Error de posición: el evento lleva `t` de reloj de pared, no muestra. Restando
por fichero la mediana (origen desconocido), la dispersión respecto al final de
la copia es p5–p95 de ±30 ms en 250 kS/s (std 19 ms) y ±9 a ±10 ms en 1–2 MS/s
(std 6 ms). Es la granularidad del bloque de proceso, no una medida de posición.

## Lectura

* El fondo real mueve la curva de forma distinta por protocolo: +5,7 dB (ais),
  +7,4 dB (aprs), −4,8 dB (pocsag, mejora), +1 dB (lora), +1,1 dB al 50 % y
  +3,7 dB al 90 % (adsb). La SNR media en banda no resume la dificultad del fondo.
* El fondo real no es estacionario: la potencia local varía (ráfagas ISM, símbolos
  OFDM), de modo que una parte de las copias cae en huecos y se recupera con SNR
  nominal muy baja. Aparece un suelo de 0,17–0,22 (ais, adsb) y la curva deja de
  ser una sigmoide pura. Una SNR media por copia no basta: hay que medir la SNR
  local en la ventana de la copia.
* POCSAG no llega al 100 % ni a 25 dB con ruido blanco (techo ~0,94). Es
  determinista (se repite igual con el mismo fichero), con independencia del CFO
  y del hueco previo: es un fallo del decodificador o de su alineación, y es
  justo lo que esta calibración debe destapar. No se investigó la causa.

## Limitaciones

* Sin posición en muestras ni SNR/ancho/frecuencia en el evento: es lo que P2
  debe exportar. El emparejado funciona solo porque cada copia lleva un
  identificador único en el mensaje; con señales reales no existe.
* Para ADS-B se inyecta una trama, no una ráfaga de cuatro; la probabilidad es
  por trama. ADS-B se recorta a 1 trama y se ve solo 1 evento «new» por ICAO.
* Fondos reales cortos (0,2 s y 50 ms) repetidos: el ruido real se repite con
  esa periodicidad. Con ADS-B y LoRa el fondo real es una portadora LTE, no ruido;
  con ais/pocsag/aprs solo se cubre una captura de 433 MHz.
* Tasas reducidas (250 kS/s) respecto al hardware real; CFO limitado a ±1 kHz,
  sin desvío de reloj ni multitrayecto. Una sola semilla; las barras de error
  recogen solo el muestreo binomial de las copias.
* Los 10 procesos corrían a la vez en una máquina compartida; no se observaron
  pérdidas de bloques (los fallos de POCSAG se repiten idénticos).
* Hay clipping en 0 muestras en la medida final.

## Conclusión de viabilidad

Viable. Con 100 copias por punto se obtienen curvas con IC estrechos
(± 0,2–0,5 dB en el 50 %) para cinco protocolos en ~16 min con el binario tal
cual, y se detectan cosas útiles: dependencia del fondo, techo de POCSAG,
falsos positivos de BCH. Para el artículo falta lo que el piloto no puede hacer:
(1) medir la posición y el ancho que ve el decodificador, (2) SNR local por copia
en vez de SNR media, y (3) validar con fondos reales largos y de la misma banda,
no recortes de pruebas. Sin eso, el sesgo medido en fondo real queda confundido
con el nivel local del fondo.
