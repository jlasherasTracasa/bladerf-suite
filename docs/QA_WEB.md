# QA de la interfaz web (agente UX1)

Batería: `make webtest` (`tests/web_e2e.mjs`, Chromium por CDP puerto 9661, servidores `--replay` en 8801–8806 con capturas
sintéticas de `--gen` mezcladas, y `tests/web_mock.js --rich` en 8804). `SOAK=600 make webtest` alarga la prueba de fugas a 10 min.
Capturas "antes" en `docs/img/qa/antes_*.png`.

## Primera pasada: fallos y fricciones

| # | Sev. | Fallo | Reproducir | Estado |
|---|---|---|---|---|
| 1 | Alta | Al conectar el canal queda en 100,3 MHz fijo aunque la emisora esté en 100,4 (o 99,2 en la antena real) | Abrir la web con una captura FM: marcador sobre el valle entre emisoras (`antes_receptor_1440.png`) | Corregido: sintoniza la señal más fuerte detectada (rejilla 100 kHz en WFM) salvo que el perfil fije frecuencia o la persona ya haya tocado |
| 2 | Alta | Escala del espectro: constantes QMIN/QSTEP fijas (-140/0,5) saturan con la escala nueva (-145/0,6, hasta +8 dBFS) | Tono de fondo de escala en la cascada | Corregido: codificación interna -160/0,7 y LUT por trama con su dbmin/dbstep |
| 3 | Alta | Emisión en modo fichero: centro/tasa por defecto son los de la captura y la frecuencia del protocolo no cabe ("la señal no cabe en 2400000 sps") | Emisión → POCSAG → Emitir | Corregido: se centra la captura sintética en la frecuencia |
| 4 | Media | "100 400 000" (con espacios) no se interpreta | Escribir en el campo de frecuencia | Corregido |
| 5 | Media | Frecuencia no válida: se descarta sin avisar | Escribir "abc" | Corregido: aviso con ejemplos |
| 6 | Media | Pestañas en dos filas desiguales; pestañas ocultas en móvil sin pista | Escritorio 1440 px | Corregido: cuadrícula de 4 columnas / fila desplazable con la activa visible |
| 7 | Media | El espectro no aprovecha la altura de la ventana (hueco vacío bajo la tarjeta) | 1440×900 | Corregido |
| 8 | Media | Contraste AA: indicadores ST/19k/FUERA 2,9:1, botón Barrido 2,7:1 | Prueba de accesibilidad | Corregido (sin `opacity`) |
| 9 | Media | Objetivos táctiles de 28 px (zoom, rango) en móvil | 360 y 768 px | Corregido (40 px en táctil) |
| 10 | Media | Paneles ocultos se redibujan cada segundo (tablas de cientos de filas, hasta 18 000 nodos DOM) | Mock rico, 60 s | Corregido: se repintan al abrir la pestaña; tope de eventos 150 |
| 11 | Baja | Etiquetas de dB pisan la franja de señales detectadas | Receptor | Corregido |
| 12 | Baja | Título del documento sin frecuencia hasta el primer cambio | Recargar | Corregido |
| 13 | Baja | Eje de frecuencias del barrido sin unidad | Barrido 80–400 MHz | Corregido (rótulo MHz/kHz) |
| 14 | Baja | "Ancho RF" muestra "—" con fuente de fichero | Hardware con --replay | Abierto (cosmético: el campo está desactivado) |
| 15 | Alta | Fuente de fichero: escribir 92,2 MHz movía el canal fuera de banda (medidor −150, FUERA) | Escribir 92,2 con `--replay` | Corregido: el canal no se mueve, el campo vuelve y el aviso da la banda real (detectado por el orquestador) |
| 16 | Alta | Perfil ADS-B con fichero a 162 MHz sacaba el canal de audio de la banda | Protocolos → ADS-B con `--replay` | Corregido: aviso claro; solo se aplican los decodificadores; el canal no se toca fuera de banda |
| 17 | Media | Marcadores de señal falsos en los bordes (caída del filtro) y flechas amontonadas; la etiqueta "NN dB" tapaba el pico | Receptor | Corregido: se descarta el 5 % de cada borde (10 % para elegir canal), agrupación a 12 px, etiqueta al lado del pico |
| 18 | Media | Pestañas en dos filas (Mapa/Mensajes/TV abajo) | 1440 px | Corregido: panel lateral de 500 px, etiqueta corta "Decod.", una sola fila |
| 19 | Media | Botón Barrido desactivado sin explicación | `--replay` | Corregido: se abre y explica por qué no está disponible |
| 20 | Media | TV: canal por defecto C34, no se recuerda el último; con fichero los controles no se explican | Pestaña TV | Corregido: C32 por defecto, último usado en localStorage, nota con `--tv-ts` |
| 21 | Media | Móvil: aviso tapaba los controles inferiores, etiqueta dBFS solapada, banderas del medidor en dos líneas | 360 px | Corregido (avisos arriba, medidor en una línea, unidad desplazada) |
| 22 | Media | Mapa: objetos en la misma posición pisan sus etiquetas | Mock rico / AIS | Corregido: etiquetas apiladas |
| 23 | Baja | Avisos duplicados ("Conexión perdida" ×2) y aviso "Conectado" en la primera conexión | Matar el servidor | Corregido |
| 24 | Media | Atajos 1–7 llevaban el foco a la pestaña y entonces ←/→ cambiaban de pestaña en lugar de sintonizar | Pulsar 3 y ← | Corregido: el atajo no mueve el foco |
| 25 | Media | Móvil: al pulsar una pestaña el contenido queda bajo el espectro sin que se note | 360 px | Corregido: se lleva el panel a la vista bajo la cabecera |
| 26 | Media | Móvil: cabecera de 2 filas que tapa el campo de frecuencia | 360 px | Corregido: una fila, `scroll-padding-top` |
| 27 | Baja | "Aviso espurio «No disponible con una fuente de fichero»" al sintonizar dentro de banda | Recorrer pestañas y escribir 100,4 | No reproducible: ese texto era el de la barra de barrido (oculta); ahora hay prueba que exige 0 avisos |

Verificado en el bladeRF real (solo RX): 92,2 MHz fuera de banda retunea (centro 91,6 MHz) y el canal se mantiene en 92,2;
100,4 MHz (Cadena SER) con audio estéreo y piloto 19k a -13,5 dBFS; barrido 80–400 MHz y parada correctos.

## Estado final

`make BUILD=build/ux1 CUDA=0` sin avisos; `make webtest`: **266/266 comprobaciones correctas**; `SOAK=600 make webtest` (10 minutos con
el mock rico, 80 aviones y 60 barcos): 266/266, memoria JS estable (< 25 MB de crecimiento), sin fuga de nodos ni de oyentes, ≥ 12 FPS
con GL por software. `node --check` de todo `web/js`, `tests/*.mjs` y `tests/web_mock.js` correcto.

Cobertura de la batería (`tests/web_e2e.mjs`, 20 suites): arranque; estado inicial; pestañas y teclado; todos los controles de cada
pestaña (nombre accesible y accionados sin excepciones); sintonía (campo con varios formatos, rueda, flechas, clic y arrastre en
espectro y cascada, bordes del filtro, modos); zoom y desplazamiento; cascada con píxeles no negros en tiempo real; los 17 perfiles
(mock) y retuneo fuera de banda (92,2 MHz); añadir, quitar y retunear decodificadores con AIS real de la captura sintética;
mapa y tablas ordenables; mensajes; grabación (iniciar, parar con R, listar, descargar, borrar); barrido; emisión en modo `file`;
TV con `--tv-ts`; reconexión del WebSocket; atajos; audio; fuente de fichero; 360/768/1440/1920 px; accesibilidad (etiquetas,
foco visible con Tab, contraste AA, saltar al contenido); latencia de sintonía de la UI; fugas y FPS; cero excepciones, errores de
consola y peticiones fallidas (CSP).

Verificado con el bladeRF real (solo RX): sintonía 92,2 MHz con retuneo del hardware, 100,4 MHz (Cadena SER) con audio estéreo,
piloto 19k y −13,5 dBFS, y barrido 80–400 MHz.

Capturas antes y después en `docs/img/qa/`:

| Antes | Después |
|---|---|
| `antes_receptor_1440.png` (canal en 100,3 entre emisoras, pestañas en dos filas, hueco bajo la cascada) | `despues_receptor_1440.png` (canal sobre 100,4, una fila de pestañas, espectro a toda altura) |
| `antes_protocolos_1440.png` | `despues_protocolos_1440.png` |
| `antes_real_ser_1004.png` | `despues_barrido_1440.png`, `despues_mapa_1440.png`, `despues_decodificadores_1440.png` |
| — | `despues_movil_360.png`, `despues_movil_pestana_360.png`, `despues_tablet_768.png`, `despues_escritorio_1920.png` |

Notas para otros agentes: el espectro tipo 1 se lee siempre con el `dbmin`/`dbstep` de la cabecera (-145/0,6 tras la corrección de C2);
la cascada recodifica a un byte interno de -160 a +18 dBFS y el barrido (tipo 4, -140/0,5) también usa su cabecera. Pendiente fuera de la
web: el detector del servidor da falsos positivos en los bordes de la banda (la web los oculta, pero conviene excluirlos en `detector.c`).
