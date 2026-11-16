# BladeRF Lab

Receptor SDR completo para el **bladeRF 2.0 micro**, escrito desde cero en C11
(con aceleración CUDA opcional) y manejado desde el navegador.

Un único binario captura la señal, calcula el espectro, demodula audio para
varios oyentes a la vez, detecta señales, graba IQ en SigMF y decodifica
protocolos de radio (aviación, marítimo, buscapersonas, ISM, LoRa, telefonía,
WiFi, radiodifusión…). Sin GNU Radio, sin OpenWebRX y sin bibliotecas DSP de
terceros: todo el tratamiento de señal está implementado y comentado en este
repositorio.

> **Estado (2026-11-15):** los 20 decodificadores están implementados y
> pasan sus pruebas con señales sintéticas (`make check`: 33/33). Falta
> validarlos con señales reales y probar con el equipo las funciones de
> hardware nuevas. El plan y las reglas de trabajo están en [AGENTS.md](AGENTS.md).

---

## Funciones

"Verificado" = compilado y probado por el orquestador (tests, sanitizers y
prueba de extremo a extremo con ficheros). "Sin probar en hardware" = funciona
con ficheros pero aún no se ha ejecutado con el bladeRF.

| Función | Estado |
|---|---|
| Captura del bladeRF o de ficheros `cs16`/`cs8`/`cu8`/SigMF | Verificado con ficheros |
| Espectro y cascada con FFT propia en CPU o en GPU (CUDA) | Verificado |
| Audio por cliente: WFM estéreo, NFM, AM, USB, LSB, CW | Verificado con ficheros |
| Detector automático de señales (OS-CFAR) | Verificado |
| Grabación IQ en SigMF y descarga desde la web | Verificado |
| Servidor HTTP + WebSocket propio, interfaz web completa con mapa y tablas | Verificado |
| Marco de decodificadores: un hilo por decodificador, varios a la vez | Verificado |
| DDC de todos los decodificadores en GPU (16 decodificadores a 20 Msps: de 5,2 núcleos a 0,3) | Verificado |
| Canalizador polifásico en GPU | Verificado (aún no lo usa ninguna función) |
| 22 perfiles predefinidos, configuración persistente | Verificado |
| Emisión: fichero, bucle interno y aire con lista blanca de bandas | Verificado en modo fichero; sin probar en hardware |
| Modo de 122,88 Msps, modos de AGC, barrido rápido de banda ancha | Sin probar en hardware |
| Imagen FPGA propia con motor de espectro | En desarrollo |
| Validación de los decodificadores con grabaciones reales | En curso |

## Protocolos

| Grupo | Protocolos |
|---|---|
| Radiodifusión | FM + **RDS**, **DAB/DAB+** (lista de servicios) |
| Aviación | **ADS-B/Mode S** 1090 MHz, **ACARS**, **VDL Modo 2**, voz AM en banda aérea |
| Marítimo | **AIS** (dos canales a la vez), **DSC** canal 70, voz en canal 16 |
| Radioaficionado | **APRS/AX.25**, **DMR** (solo metadatos), NFM/USB/LSB/CW |
| Buscapersonas | **POCSAG**, **FLEX** |
| ISM y telemetría | Sensores **OOK/FSK de 433/868 MHz**, **LoRa/LoRaWAN**, **Meshtastic**, radiosondas **RS41** |
| Telefonía y redes | Identificación de celdas **LTE** y **5G NR**, **GSM** (BSIC), beacons **WiFi** |
| Televisión | **DVB-T** (parámetros TPS) |

Solo se decodifica información de difusión o de sistema: nada de voz digital,
identidades de abonados ni tráfico cifrado.

Todos superan la prueba de extremo a extremo (generar la señal con su
modulador, añadir ruido y desvíos, reproducirla y decodificarla). Con datos
reales publicados ya se han contrastado ADS-B (vectores de *The 1090 MHz
Riddle*), AIS (sentencias AIVDM de gpsd), APRS (ejemplos de APRS101), RDS
(vectores de redsea), DMR (tablas de ETSI TS 102 361-1) y AES de Meshtastic
(FIPS-197). El resto está pendiente de validación con grabaciones reales; ver
`docs/VALIDACION.md` y cada `docs/decoders/<protocolo>.md`.

---

## Requisitos

- bladeRF 2.0 micro (probado con FPGA 0.16.0, firmware FX3 2.6.0) — o un
  fichero IQ para trabajar sin hardware.
- Linux, `gcc` (C11), `make`.
- `libbladeRF` ≥ 2.5 con sus cabeceras (`libbladerf-dev` o compilada desde el
  código de Nuand).
- Opcional: CUDA Toolkit (se detecta en el `PATH` o en `/usr/local/cuda`) y una
  GPU NVIDIA para el espectro y los canalizadores en GPU.
- Un navegador moderno.

## Compilar

```sh
make            # build/bladerf-lab (usa CUDA si la encuentra)
make CUDA=0     # sin GPU
make test       # pruebas unitarias y de extremo a extremo con señales sintéticas
make check      # compilación estricta (-Werror) + pruebas
```

## Uso

```sh
./build/bladerf-lab                     # abre el primer bladeRF y sirve http://127.0.0.1:8090
./build/bladerf-lab --host 0.0.0.0      # accesible desde la red local
./build/bladerf-lab --profile adsb      # arranca con un perfil (adsb, fm, maritimo, lora…)
./build/bladerf-lab --replay grabacion.sigmf-meta   # sin hardware, desde un fichero
./build/bladerf-lab --profile maritimo --print      # eventos decodificados en JSON por la consola
```

Si `bladerf-lab` no puede abrir el equipo, comprueba que no lo esté usando otro
programa (por ejemplo el servicio OpenWebRX: `sudo systemctl stop openwebrx`).

### Generar señales de prueba

Cada protocolo tiene su modulador, así que se puede probar un decodificador de
extremo a extremo sin antena:

```sh
./build/bladerf-lab --gen ais --gen-freq 161975000 --gen-center 162000000 \
                    --gen-rate 1000000 --out /tmp/ais
./build/bladerf-lab --replay /tmp/ais.sigmf-meta --decode ais@161975000 --print
```

## Emisión

La emisión está desactivada por defecto y sigue una política estricta
(detalles en [AGENTS.md §6](AGENTS.md)):

- **file**: genera la señal en un fichero SigMF. Siempre disponible.
- **loopback**: bucle interno del transceptor; no sale nada por la antena
  (`--enable-tx`).
- **air**: solo con `--enable-tx-air`, solo en bandas ISM/SRD (433 y 868 MHz)
  o de radioaficionado con `--callsign`, con potencia y ciclo de trabajo
  limitados. Los protocolos de aviación, marítimos, de radiosondas, telefonía o
  radiodifusión **nunca** se pueden radiar.

## Estructura

```
src/
  main.c            programa principal y órdenes del WebSocket
  engine.*          hilo DSP: captura → espectro, audio, detector, grabación, decodificadores
  sdr/              fuente de muestras (bladeRF o fichero)
  dsp/              FFT, filtros, DDC, canal de audio, detector, FEC, sincronía
  decoders/         un fichero por protocolo + host de decodificadores
  tx/               moduladores y política de emisión
  gpu/              espectro y canalizadores en CUDA (con sustitutos sin GPU)
  net/              servidor HTTP/WebSocket, SHA-1
  util/             JSON, cadenas, registro, grabador SigMF
web/                interfaz (HTML/CSS/JS sin dependencias)
tests/              pruebas
docs/               documentación técnica
legacy/             montaje anterior con OpenWebRX+ (solo referencia)
```

## Documentación

- [AGENTS.md](AGENTS.md): arquitectura, contratos entre módulos, protocolo
  WebSocket, catálogo de protocolos y plan de trabajo.
- `docs/`: arquitectura, protocolo, DSP, ADS-B, emisión, firmware, CUDA, un
  documento por decodificador y la investigación de papers aplicados.

## Aviso legal

Recibir señales de radio es legal en general, pero **interceptar, guardar o
divulgar comunicaciones que no están destinadas al público** (por ejemplo,
mensajes de buscapersonas) puede no serlo; infórmate de la normativa de tu
país. Para transmitir hay que respetar el cuadro de atribución de frecuencias,
los límites de potencia y de ciclo de trabajo de las bandas ISM/SRD y, en
bandas de radioaficionado, tener licencia. Usar este software es
responsabilidad de quien lo utiliza.
