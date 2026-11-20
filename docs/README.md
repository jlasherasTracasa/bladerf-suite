# Documentación de BladeRF Lab

Índice de `docs/`, ordenado por temas. Las rutas son relativas a esta carpeta.
Para el curso completo en presentación, ver [el curso en PDF](presentacion/BladeRF_Lab.pdf).

## Uso

Para quien quiere arrancar el servidor, recibir señales y probar los decodificadores.

| Documento | Contenido |
|---|---|
| [GUIA.md](uso/GUIA.md) | Guía paso a paso con capturas, desde el primer arranque hasta la TDT, con solución de problemas. |
| [PROTOCOLOS.md](uso/PROTOCOLOS.md) | Una ficha por protocolo del catálogo: qué es, en qué frecuencia se busca, la cadena de proceso y hasta dónde está validado. |

## Arquitectura

Cómo está hecho el software: hilos, DSP, protocolo WebSocket, GPU y emisión.

| Documento | Contenido |
|---|---|
| [ARQUITECTURA.md](arquitectura/ARQUITECTURA.md) | Hilos, host de decodificadores, flujo de datos y módulos. |
| [WEBSOCKET.md](arquitectura/WEBSOCKET.md) | Protocolo `GET /ws`: mensajes binarios y JSON en ambos sentidos. |
| [DSP.md](arquitectura/DSP.md) | Filtros, remuestreo, DDC y canal de audio. |
| [CUDA.md](arquitectura/CUDA.md) | DDC por lotes, canalizador PFB y espectro asíncrono en GPU, con medidas. |
| [RENDIMIENTO.md](arquitectura/RENDIMIENTO.md) | Qué se cambió para sostener 61,44 y 122,88 Msps, y cómo se midió. |
| [TX.md](arquitectura/TX.md) | Emisión: moduladores, política de los modos `file`, `loopback` y `air`, y calibración. |

## Hardware

Lo que hace la placa bladeRF 2.0 micro y la FPGA.

| Documento | Contenido |
|---|---|
| [HARDWARE.md](hardware/HARDWARE.md) | Qué hace el servidor con las funciones de la FPGA 0.16 y de libbladeRF 2.6.1. |
| [FIRMWARE.md](hardware/FIRMWARE.md) | Cadena de herramientas de la FPGA, análisis y plan de bloques DSP nuevos. |

## Protocolos

Una ficha técnica por decodificador: estructura de la señal, cadena DSP, validación y referencias.

| Documento | Contenido |
|---|---|
| [ADS-B / Mode S](protocolos/adsb.md) | Aviones en 1090 MHz, decodificación de tramas con CRC y vista en el mapa. |
| [ACARS](protocolos/acars.md) | Mensajes de aviación en VHF (ARINC 618). |
| [AIS](protocolos/ais.md) | Posiciones y datos de barcos en VHF marítima. |
| [APRS / AX.25](protocolos/aprs.md) | Paquetes AFSK 1200 de radioaficionados sobre FM. |
| [DAB / DAB+](protocolos/dab.md) | Radiodifusión digital, modo I: conjunto, servicios y subcanales. |
| [DMR](protocolos/dmr.md) | Radio digital profesional: solo metadatos, sin voz. |
| [DSC](protocolos/dsc.md) | Llamada selectiva digital en el canal 70 de VHF marina. |
| [DVB-T](protocolos/dvbt.md) | Receptor de TDT: sincronía OFDM, TPS y tasa del multiplex. |
| [FLEX](protocolos/flex.md) | Buscapersonas FLEX de 1600 a 6400 bps. |
| [GSM](protocolos/gsm.md) | Canales de control GSM: FCCH, SCH y BSIC. |
| [ISM](protocolos/ism.md) | Analizador genérico OOK/FSK en 433 y 868 MHz, con sensores y mandos comunes. |
| [LoRa / LoRaWAN](protocolos/lora.md) | Modulación CSS, cabecera explícita y MHDR de LoRaWAN. |
| [LTE](protocolos/lte.md) | Búsqueda de celda: PSS y SSS para el PCI, CFO y potencia. |
| [Meshtastic](protocolos/meshtastic.md) | Mensajes LoRa de Meshtastic con la clave pública por defecto. |
| [5G NR](protocolos/nr.md) | Bloque SSB en la rejilla GSCN: PSS, SSS y PCI. |
| [POCSAG](protocolos/pocsag.md) | Buscapersonas POCSAG a 512, 1200 y 2400 bps, con BCH y corrección. |
| [RDS](protocolos/rds.md) | Datos de la emisora en FM: PI, PS, PTY, RT y grupos. |
| [RS41](protocolos/rs41.md) | Radiosondas Vaisala RS41: posición, altura y PTU. |
| [VDL Modo 2](protocolos/vdl2.md) | Enlace aeronáutico D8PSK con AVLC y ACARS sobre AVLC. |
| [WiFi](protocolos/wifi.md) | Balizas 802.11b/g/a: SSID, BSSID y canal. |
| [TDT](protocolos/tdt.md) | Demux TS, PSI/SI, audio MP2 y reproductor de vídeo en el navegador. |

## Calidad

Qué está contrastado con señales reales y qué se ha probado de la interfaz web.

| Documento | Contenido |
|---|---|
| [VALIDACION.md](calidad/VALIDACION.md) | Matriz de validación: qué protocolo se ha contrastado con aire, datos reales o solo con señales sintéticas. |
| [QA_WEB.md](calidad/QA_WEB.md) | Batería `make webtest` de la interfaz: servidores de prueba, navegador y fugas de memoria. |

## Investigación

| Documento | Contenido |
|---|---|
| [INVESTIGACION.md](investigacion/INVESTIGACION.md) | Papers y documentos técnicos cuyas técnicas se pueden implementar desde cero en C y CUDA. |

## Imágenes y curso

| Carpeta o documento | Contenido |
|---|---|
| [img/](img/) | Figuras y capturas de los documentos. Los generadores están en `img/src/`. |
| [presentacion/](presentacion/) | Curso en PPTX y PDF ([BladeRF_Lab.pdf](presentacion/BladeRF_Lab.pdf)) y su generador en `presentacion/src/`. |
