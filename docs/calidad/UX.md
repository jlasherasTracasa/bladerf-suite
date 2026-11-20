# Auditoría de uso (W7): línea base y objetivos

Medida con `node tests/ux_audit.mjs` (Chromium sin cabeza por CDP, puerto 9811; servidores `--replay` en 8811–8830 con señales de `--gen`: FM con RDS a 99,6 y 100,4 MHz, AIS+APRS y ADS-B). Escribe `build/w7/ux_audit.json` y una captura del estado final por tarea en `build/w7/Tnn_*.png`. Con `--enforce` devuelve 1 si alguna tarea falla o supera sus objetivos (listo para engancharlo a `make webtest` cuando W8–W14 lleguen a la meta). Fecha de la medición: 2026-11-20, `web/` tal como estaba en ese momento.

Qué se mide por tarea:

- **Acciones** = clics + arrastres + ruedas + teclas de orden (Enter, Tab, atajos). Abrir un `<select>` y elegir cuenta como 2; el `confirm()` nativo cuenta como 1; enfocar un campo para escribir cuenta como 1.
- **Caracteres** = teclas tecleadas a mano (cada dígito cuenta; «nº a mano» = hizo falta escribir una cifra).
- **KLM** = tiempo estimado de una persona con el modelo de teclado y ratón (clic 1,1 s, tecla 0,28 s, arrastre 1,5 s, rueda 0,5 s, 1,2 s de pensar por cada cambio de pestaña o panel). Es la columna que importa: los **ms** son latencia de la máquina (CDP + servidor + espera a que aparezca el dato) y solo sirven para detectar regresiones.
- **Callejones** = avisos de error, controles desactivados sin motivo, mensajes que no explican nada.
- Éxito = criterio comprobable en el estado de la aplicación (no solo «no falló»).

Limitaciones: los servidores son de fichero (no se prueba hardware: la ganancia manual y el `retune` quedan desactivados, ver T07); las capturas salen a pocos segundos del arranque, por eso la cascada se ve a medio llenar; las tareas las ejecuta un guion que conoce la interfaz, así que es el **mejor caso**: una persona nueva tardará más (lo mide W11).

## 1. Línea base por tarea

| Id | Tarea | Éxito | Acciones (obj.) | Caracteres (obj.) | KLM s (obj.) | ms | Nº a mano | Callejones |
|---|---|---|---|---|---|---|---|---|
| T01 | Escuchar una FM y ver su RDS (perfil FM → Receptor → clic en la emisora → Escuchar) | sí | 5 (4) | 0 (0) | 9,1 (12) | 3100 | no | 1: toast rojo del perfil |
| T02 | Cambiar a NFM y a 25 kHz de ancho | sí | 4 (3) | 5 (0) | 2,8 (8) | 30 | **sí** | 0 |
| T03 | Ver aviones en el mapa (perfil ADS-B → Mapa) | sí | 3 (2) | 0 (0) | 5,7 (6) | 1900 | no | 1: toast rojo del perfil |
| T04 | Ver barcos en el mapa (perfil VHF marina → Mapa) | sí | 3 (2) | 0 (0) | 5,7 (6) | 2000 | no | 1: toast rojo del perfil |
| T05 | Añadir un decodificador sobre una señal detectada | sí | 2 (2) | 0 (0) | 3,4 (6) | 150 | no | 0 (pero ver F4) |
| T06 | Grabar IQ y borrar la grabación | sí | 5 (4) | 0 (0) | 6,7 (10) | 1800 | no | 0 (`confirm()` nativo) |
| T07 | Ganancia manual y tamaño de FFT | parcial (FFT sí; ganancia no medible con fichero) | 3 (4) | 0 (0) | 4,5 (10) | 500 | no | 1: ganancia desactivada sin motivo |
| T08 | Buscar un evento concreto (MMSI) en la lista | sí | 1 (2) | 9 (0) | 1,1 (4) | 200 | **sí** | 0 |
| T09 | Entender un decodificador en error (ADS-B con 2,4 Msps) | sí | 4 (3) | 0 (0) | 5,6 (8) | 300 | no | 0 (el texto es claro) |
| T10 | Generar una señal POCSAG en modo `file` | sí | 4 (3) | 0 (0) | 5,6 (8) | 400 | no | 1: sin enlace al fichero |
| T11 | Sintonizar con un clic en el espectro | sí (error 13,7 kHz sobre el pico) | 1 (1) | 0 (0) | 1,1 (3) | 30 | no | 0 |
| T12 | A 360 px: sintonizar, escuchar y llegar a Decod. | sí, con desplazamiento | 5 (4) | 5 (0) | 5,3 (12) | 900 | **sí** | 1: ninguna pestaña a la vista |

Resumen: las tareas son cortas porque el guion no duda; el problema no es el número de clics sino que **la interfaz miente o calla en los momentos clave** (avisos rojos cuando todo ha ido bien, instancias que se crean fuera de la rejilla del canal, texto de campos que no corresponde con lo que se envía, nada que lleve a ver el resultado). Cinco tareas (T01, T02, T03, T04, T09) superan su objetivo de acciones; T02, T08 y T12 exigen teclear cifras.

Lectura de las capturas (`build/w7/`), aparte del DOM:

- **Marcadores del detector** (T01, T09): en los bordes del espectro y sobre cada emisora FM se amontonan 6–10 flechas amarillas con rótulos solapados («39», «4…»); es el pendiente nº 10 de `AGENTS.md` y a simple vista parece una avería. Se pierde el sentido de «una flecha = una señal».
- **Perfil VHF marina** (T04): el medidor muestra «-150,0 dBFS» y el rótulo «FUERA» encendido mientras el campo de frecuencia dice 162,025 MHz (dentro de la banda). Sospecha de estado desincronizado: el servidor mueve el canal de audio a 156,8 MHz (fuera de banda) y la web, que no lo aplica, sigue enseñando 162,025. A verificar en `protocols.js` y `src/main.c` (orden `profile`).
- **Móvil 360 px** (T12): la primera pantalla es espectro, cascada y la rejilla de rango (Rango auto, mín., máx., Máx., Barrido); las pestañas empiezan en y = 724 de 740 px, así que no se ve ningún panel sin desplazar. Tras tocar «Decod.» hay 621 px de desplazamiento. Casillas de 22 px y deslizador de volumen de 16 px de alto.
- **Mapa y tablas** (T03/T04): el mapa AIS con rumbo, nombre e indicativo es claro; la tabla de abajo (`mmsi, name, callsign…`) queda cortada por la anchura del panel.
- **Decod.** (T09): el estado «error» en rojo con «requiere exactamente 2 Msps (ahora 2.4 Msps)» es el mejor mensaje de la aplicación; «esperando» explica «fuera de la banda capturada» pero no qué hacer.

## 2. Fricciones priorizadas

Puntuación = gravedad (1–5) × frecuencia (1–5, con qué frecuencia la encuentra alguien en el uso normal). Ordenadas de mayor a menor.

| # | Pt. | Grav.×Frec. | Fricción | Dónde (web/) | Propuesta concreta | Tareas | Ola |
|---|---|---|---|---|---|---|---|
| F1 | 16 | 4×4 | Aplicar el perfil «FM comercial» deja el canal en 100,3 MHz (no hay emisora) y arranca el RDS ahí («Esperando grupos RDS…»); hay que ir al espectro, buscar la emisora y pulsar Escuchar. Con RDS se pierden 3–5 s y 2 clics | `js/protocols.js` (`applyProfile`), `js/receiver.js` (sintonía inicial a la señal más fuerte, que ya existe al conectar) | Tras aplicar un perfil con decodificador `follow`, sintonizar la señal detectada más fuerte dentro de ±150 kHz de `chan_freq` (reutilizar la lógica de arranque) y activar el audio en ese mismo clic (hay gesto de usuario). Objetivo: 2 acciones | T01 | W8, W10 |
| F2 | 15 | 3×5 | Con fuente de fichero, **cada** perfil muestra un toast **rojo** («la fuente es un fichero y no se puede mover el centro…») aunque se haya aplicado bien y a veces el centro ya coincida (ADS-B 1090 y marina 162,0) | `js/protocols.js:applyProfile` (`toast(..., 'err')`) | Si centro y tasa ya valen, toast verde «Perfil aplicado»; si no, aviso informativo (no `err`) con la acción: «Abrir Hardware / usar `--replay` con centro X». Nunca rojo si no falló nada | T01, T03, T04 | W10 |
| F3 | 12 | 3×4 | Marcadores del detector amontonados en los bordes y sobre las emisoras (se leen como ruido); mezcla de tamaños y rótulos solapados | `js/spectrum.js` (dibujo de la franja de señales, `hitSignal`), origen en `src/dsp/detector.c` (pendiente nº 10) | Excluir el 5 % de cada borde en el servidor y fusionar detecciones contiguas dentro del ancho ocupado; en la web, una flecha por grupo con «×3» y rótulo solo si hay 12 px libres | T01, T09 (capturas) | W9 |
| F4 | 12 | 4×3 | «Decodificar como…»: (a) la instancia nace en la frecuencia cruda del pico (162,0273 en vez de 162,025: el AIS no engancha si se aleja del canal); (b) menú de 22 opciones de 540 px, 9 con ★ (no discrimina), AIS en 3.º lugar; (c) tras elegir solo hay un toast efímero: no se abre la pestaña Decod. ni el mapa | `js/decoders.js` (`showSignalMenu`, `score`, `addDecoder`) | (a) Ajustar a la frecuencia típica `freqs[]` del decodificador si cae a ±bw/2; (b) máx. 3 sugeridos arriba por ancho y banda (AIS en 156–163 MHz), resto en «Más…»; (c) toast con botón «Ver» que abre Decod. y resalta la instancia. Objetivo: ≤ 2 clics hasta el primer mensaje | T05 | W9 |
| F5 | 12 | 4×3 | A 360 px la primera pantalla no tiene ningún panel (pestañas en y = 724/740) y llegar a Decod. exige 621 px de desplazamiento; casillas de 22 px y deslizador de 16 px; 10 controles < 36 px | `css/app.css` (≈ líneas 178–235, reglas ≤ 640 px), `js/main.js:47` (`scrollIntoView` de la barra) | En ≤ 640 px: barra de pestañas fija abajo; espectro a 34 vh; mín./máx./Máx./Barrido detrás de un botón «⋯»; objetivos táctiles ≥ 40 px (incluido volumen). Objetivo: pestañas visibles sin desplazar y 0 controles < 36 px | T12 | W14, W13 |
| F6 | 9 | 3×3 | Estado desincronizado tras el perfil marina: medidor «FUERA −150 dBFS» con la frecuencia mostrada dentro de la banda (el servidor mueve el canal a 156,8 MHz y la web no) | `js/protocols.js:applyProfile`, `js/receiver.js`, orden `profile` en `src/main.c` | El servidor no debe mover el canal fuera de banda con fuente de fichero (o la web debe reflejar siempre `status.chan`); prueba: tras `profile`, `chan.freq` web == servidor | T04 | W10 |
| F7 | 8 | 2×4 | Ancho del filtro solo editable tecleando (5 cifras); no hay valores típicos por modo; el campo está en la pestaña Receptor lejos del selector de modo | `index.html` (`#rx-bw`), `js/receiver.js` | Chips junto al modo con anchos típicos (NFM 6,25/12,5/25; AM 6/9/12; USB 2,4/2,7/3; WFM 120/180/220 kHz) y arrastre de bordes ya existente; Objetivo: 2 clics, 0 cifras | T02 | W8 |
| F8 | 6 | 2×3 | Buscar un evento obliga a teclear el MMSI completo (9 cifras); no hay contador de coincidencias, resaltado ni «filtrar por este valor» | `js/decoders.js` (`renderAllEvents`), `index.html` (`#ev-search`) | Clic en cualquier valor de un evento (MMSI, indicativo, ICAO) = filtrar por él; contador «28 de 55»; resaltado del término | T08 | W9 |
| F9 | 6 | 3×2 | Emisión: «Centro» y «Tasa» muestran 100 MHz / 2,4 MHz pero el fichero sale a 466,075 MHz; el resultado da la ruta de texto sin enlace de descarga ni «decodificar este fichero» | `js/tx.js:95-99`, `index.html` (`#tx-center`, `#tx-result`) | Mostrar el centro/tasa **efectivos** al elegir la frecuencia; enlace de descarga y botón «Reproducir con --replay / cargar como fuente» en el resultado | T10 | W15 |
| F10 | 6 | 2×3 | Borrar grabación: `confirm()` nativo, lista con dos entradas por grabación (datos y metadatos) | `js/hardware.js:65-75` | Una fila por grabación con duración y tamaño; confirmación en línea («¿Borrar? Sí / No») sin bloquear la página | T06 | W10 |
| F11 | 6 | 2×3 | Ganancia, tasa y centro desactivados con fichero sin decir por qué; «esperando» sin acción propuesta | `index.html` (`#hw-gmode`, `#hw-gain`), `js/decoders.js:renderInstances` | Nota bajo el control: «Fuente de fichero: la ganancia no aplica»; en `waiting`, botón «Mover a la banda» o «Centrar el hardware en X» | T07, T09 | W10 |
| F12 | 4 | 2×2 | Pista del formulario: «exige 2.000 MHz de tasa» (la unidad es Msps, no MHz) | `js/decoders.js:38` | Formatear con `mhz()`/Msps como en `protocols.js` | T09 | W10 |
| F13 | 3 | 1×3 | Cascada a medio llenar durante ~20 s tras conectar (bloque negro de 390 px); pie de espectro y rejilla de rango ocupan una fila entera | `js/spectrum.js` | Rellenar el historial con el primer fotograma o encoger la cascada hasta tener datos | todas | W12 |

Lo que ya funciona bien y hay que conservar: sintonizar con un clic (T11: 1 acción, error 13,7 kHz < paso), el mensaje de error de ADS-B a 2,4 Msps, el menú de señal con «Escuchar aquí» arriba, los estados vacíos del mapa y de las tablas.

## 3. Objetivos numéricos por tarea (W8–W14)

Se comprueban con `node tests/ux_audit.mjs --enforce` (tabla `OBJ` dentro del script). «Acciones» = clics + arrastres + ruedas + teclas de orden; «caracteres» = cifras o letras tecleadas.

| Id | Acciones | Caracteres | KLM s | Callejones | Ola que lo cumple | Condición |
|---|---|---|---|---|---|---|
| T01 | ≤ 4 | 0 | ≤ 12 | 0 (ningún toast rojo) | W8 + W10 | F1, F2 |
| T02 | ≤ 3 | 0 | ≤ 8 | 0 | W8 | F7 |
| T03 | ≤ 2 | 0 | ≤ 6 | 0 | W10 | F2; el perfil ya abre el Mapa si solo trae decodificadores de posición |
| T04 | ≤ 2 | 0 | ≤ 6 | 0 | W10 | F2, F6 |
| T05 | ≤ 2 | 0 | ≤ 6 | 0 | W9 | F4; primer mensaje decodificado visible tras el segundo clic |
| T06 | ≤ 4 | 0 | ≤ 10 | 0 | W10 | F10 |
| T07 | ≤ 4 | 0 | ≤ 10 | 0 (con fichero, motivo visible) | W10 | F11 |
| T08 | ≤ 2 | 0 | ≤ 4 | 0 | W9 | F8 |
| T09 | ≤ 3 | 0 | ≤ 8 | 0 | W10 | error y «esperando» con causa y acción |
| T10 | ≤ 3 | 0 | ≤ 8 | 0 | W15 | F9 |
| T11 | ≤ 1 | 0 | ≤ 3 | 0 | W8 | ya cumplido; no empeorar |
| T12 | ≤ 4 | 0 | ≤ 12 | 0 | W14 + W13 | F5; pestañas visibles en la primera pantalla; 0 controles < 36 px |

Objetivos transversales: ninguna tarea con un toast `err` si no hubo error; 0 excepciones y 0 errores de consola durante las 12 tareas (el guion los cuenta en `exceptions` y `console_errors`); latencia de la máquina < 100 ms en T02, T05, T11 (regresión).

## 4. Cómo repetir y enganchar

```
make BUILD=build/w7 CUDA=0          # binario (o BIN=<ruta> node tests/ux_audit.mjs)
node tests/ux_audit.mjs             # medición; build/w7/ux_audit.json y capturas
node tests/ux_audit.mjs --only T05,T09 --out /tmp/ux
node tests/ux_audit.mjs --enforce   # falla si una tarea no cumple sus objetivos
```

Para `make webtest`: añadir `BIN=$(abspath $(BIN)) node tests/ux_audit.mjs --enforce` tras la batería de `web_e2e.mjs` cuando las olas de §3 hayan cerrado sus fricciones (hoy 12 de 12 tareas funcionan pero 9 superan algún objetivo, así que con `--enforce` fallaría). Usa Chromium de `~/.cache/ms-playwright/chromium-1234` (variable `CHROME`) y reutiliza `tests/web_e2e.mjs` como biblioteca (`Browser`, `Page`, `makeData`).

## 5. Fluidez del espectro y la cascada (W12): medidas

Banco: Chromium headless con GL por software (SwiftShader), mock `--rich` (80 aviones, 60 barcos), ventana 1440×900, pestaña Receptor, 20 tramas/s de espectro.

| Medida (sin carga, máquina con carga < 12) | Antes (Canvas 2D en el hilo principal) | Después (worker + OffscreenCanvas + WebGL) |
|---|---|---|
| Imágenes/s | 50–60 | 60 |
| CPU total de Chromium | 700–960 % | 390–630 % |
| Hilo principal (tarea) | 8–10 % | 6–9 % |
| Montón JS tras 8 h simuladas (`--mem 110`) | n/d | 3–4 MB estable |

El coste dominante era trazar la curva con degradado en Canvas 2D en la GPU de software (≈ 2/3 del proceso GPU). Con GPU real se espera 60 FPS sin tocar el hilo principal.

Objetivo absoluto: ≥ 30 FPS y hilo principal < 25 % de un núcleo (60 FPS con GPU). La prueba `fugas` mide primero un lienzo trivial: si el entorno no da ≥ 50 FPS (máquina saturada por otras tareas), no se puede exigir el absoluto y se exige, medido en la misma ejecución, que el modo nuevo dé ≥ 90 % de los FPS del modo original (`?wf=main&wf=2d`) sin subir el hilo principal más de 5 puntos. Con carga 25–55 las medidas absolutas caen a 4–40 FPS incluso para un lienzo vacío.
