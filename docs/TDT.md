# TDT: demux, audio MP2 y reproductor (TV2)

(La parte del receptor DVB-T la documenta TV1 en este mismo fichero o en `docs/decoders/dvbt.md`.)

## Cadena
`dvbt_rx` (TV1) → `src/decoders/tdt.c` (decodificador narrow a 64/7 MHz, filtro 7,61 MHz) → `tv_ts_in()` →
`ts.c` (sincronía 0x47 repetida 4 veces, continuidad con duplicados, PCR, TEI) → `psi.c` (PAT, PMT, SDT, NIT con LCN,
CRC-32/MPEG-2, texto DVB ISO 6937/8859-x/UCS-2/UTF-8 → UTF-8) → `pes.c` (reensamblado, PTS/DTS, AU H.264 por AUD) →
`tv.c` (servicio activo, reloj, JSON y binarios 5/6) → web (`web/js/tv.js`, `tv-worklet.js`).

## mp2.c
Decodificador propio de Layer II (MPEG-1 y MPEG-2 LSF, estéreo/joint/dual/mono). Las tablas de asignación y la
ventana se contrastaron con la salida de ffmpeg (solo como referencia): error rms < 0,1 LSB en 48/44,1/32/24/22,05/16 kHz
(`tests/test_mp2.c`). Latencia del demux: la AU/trama espera al siguiente PES/sincronía (~80 ms).

## Sincronía A/V
El audio es el reloj: `tv-worklet.js` devuelve el PTS de la muestra que suena (corrigiendo la latencia de salida) y el vídeo
se presenta cuando su PTS ≤ reloj. Sin audio se usa un reloj propio con 120 ms de retardo.

## Limitaciones
* Vídeo: solo H.264 por WebCodecs; MPEG-2 y HEVC se anuncian pero no se reproducen. Hace falta Chrome/Edge con H.264.
* Audio: solo MP2; AAC, AC-3 y E-AC-3 no se decodifican (el servicio se ve sin sonido). Sin descifrado (CA).
* WebCodecs exige contexto seguro (`localhost` o HTTPS).
* El vídeo se difunde a todos los clientes mientras haya un servicio elegido.
* Pruebas sin radio: `bladerf-lab --replay <cualquier .cs16> --tv-ts fichero.ts`.
