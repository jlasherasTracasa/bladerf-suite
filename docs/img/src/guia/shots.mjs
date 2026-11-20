// Capturas anotadas de la web para docs/uso/GUIA.md (DOC2b). Puertos propios 8931–8935.
// Uso (desde la raíz): node docs/img/src/guia/shots.mjs   → docs/img/guia/captura-<nombre>.png
process.env.BIN = path_resolve(process.env.BIN || 'build/doc2b/bladerf-lab');
function path_resolve(p) { return new URL(p, 'file://' + process.cwd() + '/').pathname; }
import path from 'node:path';
import fs from 'node:fs';
import { execSync } from 'node:child_process';
const T = await import('../../../../tests/web_e2e.mjs');
const { Browser, launch, killAll, waitHttp, startServer, startMock, makeData, sleep } = T;
const ROOT = process.cwd();
makeData();
const tmp = path.join(ROOT, 'build/doc2b/shots'); fs.mkdirSync(tmp, { recursive: true });
execSync(`${process.env.BIN} --gen rds --gen-freq 100400000 --gen-center 100000000 --gen-rate 2400000 --out ${path.join(tmp, 'tvfm')}`, { stdio: 'ignore' });
const br = new Browser(8931); await br.start();
const fm = startServer(8932, 'fm', ['--enable-tx', '--callsign', 'EA2XXX', '--decode', 'rds@100400000']);
const mock = startMock(8933, false);
const marine = startServer(8934, 'marine', ['--decode', 'ais@161975000', '--decode', 'aprs@162025000']);
const tvts = process.env.TV_TS || path.join(ROOT, 'build/tv2/data/test.ts');
launch(process.env.BIN, ['--port', '8935', '--tv-ts', tvts, '--replay', path.join(tmp, 'tvfm.sigmf-meta'), '--config', 'none', '--web', path.join(ROOT, 'web')]);
for (const p of [8932, 8933, 8934, 8935]) await waitHttp(`http://127.0.0.1:${p}/`, 15000);
const outDir = path.join(ROOT, 'docs/img/guia'); fs.mkdirSync(outDir, { recursive: true });

// Resolución de un selector: CSS, 'h2:Texto' (la tarjeta cuyo h2 lo contiene) o lista (unión de rectángulos)
const RECT = `(spec)=>{const one=(s)=>{let e;if(s.startsWith('h2:')){const t=s.slice(3);const h=[...document.querySelectorAll('h2')].find(x=>x.offsetParent&&x.textContent.includes(t));e=h&&(h.closest('.card')||h)}else e=document.querySelector(s);if(!e||!e.offsetParent&&e.tagName!=='BODY'&&getComputedStyle(e).position!=='fixed')return null;const r=e.getBoundingClientRect();return r.width?[r.left,r.top,r.right,r.bottom]:null};
const rs=(Array.isArray(spec)?spec:[spec]).map(one).filter(Boolean);if(!rs.length)return null;return [Math.min(...rs.map(r=>r[0])),Math.min(...rs.map(r=>r[1])),Math.max(...rs.map(r=>r[2])),Math.max(...rs.map(r=>r[3]))]}`;

const shots = [
  { name: 'receptor', port: 8932, tab: 'receptor', wait: 9000, title: 'Pestaña Receptor (FM generada con --gen rds)', ann: [
    ['#freq-input', 'Frecuencia de escucha: escribe MHz o pulsa F. Clic en el espectro también sintoniza.'],
    ['#mode-seg', 'Modo de demodulación: WFM, NFM, AM, USB, LSB, CW.'],
    ['#btn-listen', 'Escuchar: activa el audio de este navegador (cada cliente tiene su canal).'],
    ['.smeter', 'S-meter y banderas: SQL squelch, ST estéreo, 19k piloto, FUERA fuera de banda.'],
    ['#spec-wrap', 'Espectro y cascada. El rectángulo violeta es el filtro del canal (arrastra sus bordes); las marcas amarillas son señales detectadas: clic para sintonizar o «Decodificar como…».'],
    ['.zoombar', 'Zoom, rango de dB, máximo retenido y botón Barrido.'],
    [['#st-dev', '#st-over'], 'Estado: fuente, tasa, carga del hilo DSP, temperatura, clientes y overruns.'],
    ['#tabs', 'Pestañas (teclas 1 a 8).'],
    ['h2:Canal de audio', 'Anchura del filtro, paso de sintonía, squelch y deénfasis.'],
    ['#rds-card', 'RDS: PS, texto de radio, tipo de programa, hora y frecuencias alternativas.'],
  ] },
  { name: 'fuera-de-banda', port: 8932, tab: 'receptor', wait: 6000, title: 'Sintonizar fuera de la banda capturada', act: async (pg) => { await pg.fill('#freq-input', '92.2'); await sleep(2500); }, ann: [
    ['#freq-input', 'Frecuencia pedida (92,2 MHz) fuera de 100 MHz ± 1,2 MHz.'],
    ['#fl-oob', 'FUERA: el canal queda fuera de la banda; el audio calla.'],
    ['.zoombar', 'Para verla, mueve el centro (pestaña Hardware) o sube la tasa.'],
  ] },
  { name: 'hardware', h: 1100, port: 8932, tab: 'hardware', wait: 5000, title: 'Pestaña Hardware', ann: [
    ['h2:Sintonía y muestreo', 'Centro, tasa, ancho RF y modo de ganancia (también AGC rápido, lento e híbrido).'],
    ['#hw-bias-l', 'Bias-tee: solo con antenas o LNA que lo admitan.'],
    ['h2:Espectro y detector', 'Tamaño de FFT, imágenes por segundo y umbral del detector.'],
    ['h2:Ubicación del receptor', 'Latitud y longitud: las usan el mapa y ADS-B.'],
    ['h2:Grabación IQ', 'Grabar (tecla R) guarda un SigMF en recordings/.'],
    ['h2:Equipo', 'Placa, versiones de firmware y FPGA, límites.'],
  ] },
  { name: 'protocolos', port: 8932, tab: 'protocolos', wait: 5000, title: 'Pestaña Protocolos', ann: [
    ['h2:Perfiles', 'Perfiles: un clic fija centro, tasa, audio y decodificadores.'],
    ['#prof-groups', 'Agrupados por familia; cada botón muestra centro y tasa.'],
    ['h2:Catálogo de decodificadores', 'Catálogo: añadir una instancia en las frecuencias típicas o en la actual.'],
  ] },
  { name: 'decodificadores', h: 1100, port: 8934, tab: 'decoders', wait: 9000, title: 'Pestaña Decodificadores (AIS y APRS generados)', ann: [
    ['h2:Añadir instancia', 'Protocolo, frecuencia y opciones JSON.'],
    ['h2:Instancias', 'Instancias en marcha: estado, carga y descartes; Oír, Mover, Quitar.'],
    ['h2:Estado de las tablas', 'Tablas que mantienen los decodificadores (barcos, estaciones…).'],
    ['h2:Eventos', 'Flujo de eventos con filtro, búsqueda y pausa.'],
  ] },
  { name: 'decodificar-como', port: 8932, tab: 'receptor', wait: 7000, title: 'Menú «Decodificar como…» sobre una señal detectada',
    act: async (pg) => { await pg.ev(`(()=>{const S=__app.S;const l=S.signals.list.map(([o,b,p,s])=>({freq:S.signals.center+o,bw:b,peak:p,snr:s})).sort((a,b)=>b.snr-a.snr)[0];__app.bus.emit('sig-menu',{sig:l,x:700,y:170});return 1})()`); await sleep(500); }, ann: [
    ['#sig-menu', 'Menú de la señal: «Escuchar aquí» o «Decodificar como…». ★ marca los protocolos cuyo ancho se parece al de la señal.'],
    ['#spec-wrap', 'Las marcas amarillas son las señales que detecta el servidor.'],
  ] },
  { name: 'mapa', h: 1100, port: 8934, tab: 'mapa', wait: 9000, title: 'Pestaña Mapa (barcos AIS y estación APRS generados)', ann: [
    ['#mp-layers', 'Capas: una por protocolo; actívalas o desactívalas.'],
    ['#mp-map', 'Mapa: cada objeto con lat y lon; clic para ver sus campos.'],
    ['h2:Objeto seleccionado', 'Todos los campos del objeto elegido.'],
    ['h2:Tablas por capa', 'Tablas ordenables por capa.'],
  ] },
  { name: 'mapa-demo', port: 8933, tab: 'mapa', wait: 7000, title: 'Mapa con datos de DEMOSTRACIÓN (tests/web_mock.js, no es radio real)', ann: [
    ['#mp-layers', 'Capas de demostración: AIS, APRS, Meshtastic…'],
    ['#mp-map', 'Posiciones inventadas por el servidor de pruebas.'],
  ] },
  { name: 'mensajes', h: 1100, port: 8934, tab: 'mensajes', wait: 9000, title: 'Pestaña Mensajes', ann: [
    ['#ms-filter', 'Filtrar por protocolo.'],
    ['#ms-search', 'Buscar texto.'],
    ['#ms-pause', 'Pausar el flujo para leer.'],
    ['#ms-list', 'Mensajes decodificados con su hora, protocolo y frecuencia.'],
  ] },
  { name: 'barrido', port: 8932, tab: 'receptor', wait: 5000, title: 'Barra de barrido (la fuente es un fichero: no se puede barrer)', act: async (pg) => { await pg.clickSel('#sw-toggle'); await sleep(800); }, ann: [
    ['#sw-toggle', 'Botón Barrido.'],
    ['#sweepbar', 'Inicio, fin, FFT por salto, Iniciar y Parar. Con la placa recorre 47 MHz a 6 GHz.'],
  ] },
  { name: 'emision', h: 1100, port: 8932, tab: 'tx', wait: 5000, title: 'Pestaña Emisión (--enable-tx; no se emite nada)', ann: [
    ['#tx-pol', 'Política vigente: aire, indicativo, ganancia máxima y bandas.'],
    [['#tx-proto', '#tx-mode'], 'Protocolo y modo: fichero SigMF (seguro), bucle interno o aire.'],
    ['#tx-params', 'Parámetros del mensaje en JSON.'],
    ['#tx-send', 'Emitir: en modo fichero escribe un SigMF en recordings/.'],
    ['h2:Reproducir una grabación', 'Reproducir una grabación en modo fichero o bucle.'],
    ['h2:Calibración del TX', 'Calibración IQ/LO por bucle interno.'],
  ] },
  { name: 'tv', port: 8935, tab: 'tv', wait: 7000, h: 1150, act: async (pg) => { await pg.clickSel('#tv-svcs button'); await sleep(500); await pg.clickSel('#tv-play'); await sleep(4000); }, title: 'Pestaña TV (TS de prueba en bucle, sin radio)', ann: [
    ['#tv-stage', 'Imagen: H.264 decodificado por el navegador (WebCodecs).'],
    ['#tv-play', 'Ver / Detener el servicio elegido.'],
    ['h2:Múltiplex', 'Canal 21–48, Sintonizar y Explorar (con un fichero .ts no aplican).'],
    ['h2:Servicios', 'Servicios del múltiplex con códec y resolución.'],
    ['h2:Recepción', 'Enganche, MER/SNR, BER antes de Viterbi y de Reed-Solomon, tasa del TS.'],
  ] },
];

const esc = (s) => s.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
for (const sh of shots) {
  const VH = sh.h || 900;
  const pg = await br.newPage(1440, VH);
  await pg.goto(`http://127.0.0.1:${sh.port}/`, 400);
  await pg.waitFor('window.__app && __app.S.connected && __app.S.spec.n > 0', 15000);
  await pg.clickSel('#tab-' + sh.tab);
  await sleep(sh.wait);
  if (sh.act) await sh.act(pg);
  const boxes = [];
  for (const [spec, label] of sh.ann) {
    const r = await pg.ev(`(${RECT})(${JSON.stringify(spec)})`);
    if (!r) { console.log('  sin elemento:', sh.name, JSON.stringify(spec)); continue; }
    boxes.push({ r, label });
  }
  const raw = path.join(tmp, sh.name + '.png');
  await pg.shot(raw);
  pg.close();
  boxes.splice(0, boxes.length, ...boxes.filter((b) => b.r[1] < VH - 20));
  const body = boxes.map((b, i) => {
    const [x0, y0, x1, y1] = [b.r[0], b.r[1], b.r[2], Math.min(b.r[3], VH - 2)];
    return `<div class="box" style="left:${x0 - 3}px;top:${y0 - 3}px;width:${x1 - x0 + 6}px;height:${y1 - y0 + 6}px"></div><div class="bd" style="left:${Math.max(2, x0 - 13)}px;top:${Math.max(2, y0 - 13)}px">${i + 1}</div>`;
  }).join('');
  const legend = boxes.map((b, i) => `<li><span class="bd s">${i + 1}</span><span>${esc(b.label)}</span></li>`).join('');
  const html = `<!doctype html><meta charset="utf-8"><style>
:root{--bg:#141218;--card:#211F26;--on:#E6E0E9;--var:#CAC4D0;--pri:#D0BCFF;--onpri:#381E72}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--on);font-family:Inter,system-ui,sans-serif;width:1440px}
.shot{position:relative;width:1440px;height:${VH}px}.shot img{display:block}
.box{position:absolute;border:2px solid var(--pri);border-radius:10px;background:rgba(208,188,255,.07);box-shadow:0 0 0 1px rgba(20,18,24,.6)}
.bd{position:absolute;min-width:26px;height:26px;border-radius:13px;background:var(--pri);color:var(--onpri);font:700 14px/26px Inter,sans-serif;text-align:center;padding:0 6px;box-shadow:0 1px 4px rgba(0,0,0,.6)}
.bd.s{position:static;flex:none}
.leg{margin:0;padding:20px 28px 22px;background:var(--card);border-top:1px solid #3A3741}
.leg h3{margin:0 0 12px;font:600 17px Inter,sans-serif}
ol{list-style:none;margin:0;padding:0;columns:2;column-gap:36px}
li{display:flex;gap:10px;align-items:flex-start;margin:0 0 9px;break-inside:avoid;font-size:14px;line-height:1.35;color:var(--var)}
li span:last-child{padding-top:3px}
</style><div class="shot"><img src="${sh.name}.png" width="1440" height="${VH}">${body}</div><div class="leg"><h3>${esc(sh.title)}</h3><ol>${legend}</ol></div>`;
  const hp = path.join(tmp, sh.name + '.html'); fs.writeFileSync(hp, html);
  const cp = await br.newPage(1440, 1400);
  await cp.goto('file://' + hp, 600);
  const H = await cp.ev('document.documentElement.scrollHeight');
  await cp.resize(1440, H);
  await sleep(300);
  const out = path.join(outDir, `captura-${sh.name}.png`);
  { const r = await cp.send('Page.captureScreenshot', { format: 'png', clip: { x: 0, y: 0, width: 1440, height: H, scale: 1 } }); fs.writeFileSync(out, Buffer.from(r.data, 'base64')); }
  cp.close();
  if (fs.statSync(out).size > 380 * 1024) execSync(`convert '${out}' +dither -colors 160 -define png:compression-level=9 'PNG8:${out}'`);
  console.log(`captura-${sh.name}.png`, boxes.length, 'anotaciones', Math.round(fs.statSync(out).size / 1024), 'KB');
}
br.stop(); killAll(); process.exit(0);
