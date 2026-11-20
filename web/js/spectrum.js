// spectrum.js — espectro (línea) y cascada con zoom, desplazamiento y sintonía.
//
// Tres capas dentro de #spec-wrap:
//   · plotC  : curva del espectro (fondo, relleno y traza).   ┐ las dos las pinta specrender.js,
//   · wfC    : cascada (WebGL, anillo de 1024 filas).         ┘ normalmente en un Worker con OffscreenCanvas.
//   · ovC    : canvas 2D transparente encima de todo: rejilla en dB, eje de frecuencia, filtro,
//              marcadores y lecturas; recibe el ratón/táctil. Solo se repinta cuando cambia algo de
//              la interfaz (no con cada trama de espectro).
// Este fichero conserva el HILO PRINCIPAL ligero: geometría y vista (zoom, desplazamiento, inercia,
// pellizco) y los textos; al worker solo le manda la vista (inicio/anchura, rango de dB) y las tramas
// binarias por transferencia. Sin Worker u OffscreenCanvas (o con ?wf=main) el mismo renderizador
// corre en este hilo; con ?wf=2d se fuerza la cascada 2D.
//
// El servidor manda v = (dB − dbmin)/dbstep en un byte. Para la cascada se
// recodifica a q = (dB − QMIN)/QSTEP para que cambiar el rango de colores no
// obligue a reescribir el historial. Las frecuencias de los bins: el bin i
// corresponde a center − rate/2 + (i+0,5)·rate/n.
//
// Modo barrido (mensaje binario tipo 4): misma cabecera, pero cubre [inicio, fin]
// (centro y «tasa» = fin − inicio) y n puede ser de decenas de miles de bins. El
// espectro se pinta igual; la cascada recibe una fila por pasada completa, con la
// anchura reducida (máximo por columna) a WF_MAX_SWEEP. Sin filtro de canal,
// señales ni decodificadores: no hay receptor mientras se barre.
import { S, bus, $, clamp, fmtHz, fmtAxis, throttle, dispOff, dispUnit } from './util.js';
import { SpecRenderer, QMIN, QSTEP, WF_MAX_SWEEP, AXIS_H } from './specrender.js';
import { setChannel, filterEdges, getStep, BW_MAX, BW_MIN } from './receiver.js';

// Codificación interna de la cascada (1 byte): cubre de -160 a +18 dBFS, más que cualquier cabecera (tipo 1: -145..+8; barrido: -140..-12,5).
// Cada trama se convierte con SU dbmin/dbstep (lut), no con constantes del servidor.

let wrap, plotC, wfC, ovC, ox;
let W = 0, H = 0, plotH = 0, wfH = 0, dpr = 1;
const view = { start: 0, span: 1 };
let n = 0, avg = new Float32Array(0), lastDb = new Float32Array(0), dataDirty = false, ovLo = 0, ovHi = 0;
let rangeLo = -110, rangeHi = -30, auto = true, holdOn = false, dirty = true, lastRangeUi = 0;
let hover = null, sweepPick = null;

/* ---- renderizador: worker con OffscreenCanvas (normal) o en este hilo (alternativa) ----
 * R expone lo mismo en los dos casos: spectrum(buf, sweep), size(...) y view(...). En el worker los
 * datos binarios viajan por transferencia (el ArrayBuffer del WebSocket no se copia). */
let R = null, worker = null, glKind = '', inflight = 0, dropped = 0;
const sig = { view: '', size: '' };

function newCanvases() {
  const mk = (cls) => { const c = document.createElement('canvas'); if (cls) c.className = cls; return c; };
  const np = mk(plotC && plotC.className), nw = mk(wfC && wfC.className);
  if (plotC) { np.style.cssText = plotC.style.cssText; plotC.replaceWith(np); }
  if (wfC) { nw.style.cssText = wfC.style.cssText; wfC.replaceWith(nw); }
  plotC = np; wfC = nw;
}

function startLocal(force2d) {
  if (worker) { worker.terminate(); worker = null; }
  inflight = 0;
  if (R) newCanvases();
  const r = new SpecRenderer(plotC, wfC, { force2d }, (ev) => onRenderEvent(ev));
  if (r.failed) return;                       // onRenderEvent('fail') ya reintenta en 2D
  R = { spectrum: (b, sw) => r.spectrum(b, sw), size: (...a) => r.setSize(...a), view: (...a) => r.setView(...a), stats: () => r.stats };
}

function startRenderer(force2d) {
  const can = typeof Worker !== 'undefined' && typeof OffscreenCanvas !== 'undefined' && plotC.transferControlToOffscreen && !/[?&]wf=main\b/.test(location.search);
  if (can) {
    try {
      worker = new Worker(new URL('./specworker.js', import.meta.url), { type: 'module' });
      const po = plotC.transferControlToOffscreen(), wo = wfC.transferControlToOffscreen();
      worker.onmessage = (e) => { if (e.data.t === 'ack') inflight = Math.max(0, inflight - 1); else onRenderEvent(e.data); };
      worker.onerror = () => onRenderEvent({ t: 'fail' });
      worker.postMessage({ t: 'init', plot: po, wf: wo, force2d: force2d || /[?&]wf=2d\b/.test(location.search) }, [po, wo]);
      // Contrapresión: si el worker va retrasado (máquina saturada, pestaña en segundo plano) se descartan tramas en
      // vez de acumular mensajes sin límite; cada trama procesada se confirma con un 'ack'.
      R = { spectrum: (b, sw) => { if (inflight >= 12) { dropped++; return; } inflight++; worker.postMessage({ t: 'spec', buf: b, sweep: sw }, [b]); }, size: (W, pH, wH, d) => worker.postMessage({ t: 'size', W, plotH: pH, wfH: wH, dpr: d }),
        view: (start, span, lo, hi, hold) => worker.postMessage({ t: 'view', start, span, lo, hi, hold }), stats: () => ({}) };
      layout(); return;
    } catch (e) { console.warn('worker del espectro no disponible:', e); if (worker) { worker.terminate(); worker = null; } newCanvases(); }
  }
  startLocal(force2d || /[?&]wf=2d\b/.test(location.search));
  layout();
}

function onRenderEvent(ev) {
  if (ev.t === 'info') { glKind = ev.gl ? 'WebGL' : '2D'; updateInfo(); }
  else if (ev.t === 'fail') {                   // WebGL descartado con el lienzo ya ligado a GL: lienzos nuevos y 2D en este hilo
    if (worker) { worker.terminate(); worker = null; }
    newCanvases(); R = null; startLocal(true); sig.view = sig.size = ''; layout();
  }
}

/* ---- geometría y mapeo ---- */
const fLo = () => S.spec.center - S.spec.rate / 2;
export const xToFreq = (x) => fLo() + (view.start + (x / W) * view.span) * S.spec.rate;
export const freqToX = (f) => (((f - fLo()) / S.spec.rate - view.start) / view.span) * W;
const sweeping = () => !!S.spec.sweep;
const minSpan = () => (sweeping() ? clamp(32 / (n || 1), 1 / 4096, 1) : clamp(5000 / (S.spec.rate || 1e6), 1 / 4096, 1));

function layout() {
  const r = wrap.getBoundingClientRect();
  dpr = Math.min(window.devicePixelRatio || 1, 2);
  W = Math.max(50, Math.floor(r.width)); H = Math.max(120, Math.floor(r.height));
  plotH = Math.round(clamp(H * 0.36, 110, 240)); wfH = H - plotH;
  plotC.style.height = plotH + 'px'; plotC.style.top = '0';
  wfC.style.top = plotH + 'px'; wfC.style.height = wfH + 'px';
  ovC.width = Math.round(W * dpr); ovC.height = Math.round(H * dpr);
  const sz = [W, plotH, wfH, dpr].join();
  if (R && sz !== sig.size) { sig.size = sz; R.size(W, plotH, wfH, dpr); }
  dirty = true;
}

export function zoomAt(frac, factor) {
  const ns = clamp(view.span / factor, minSpan(), 1);
  view.start = clamp(frac - (frac - view.start) * (ns / view.span), 0, 1 - ns); view.span = ns; dirty = true;
}
export const zoomBy = (f) => zoomAt(view.start + view.span / 2, f);
export const getView = () => ({ start: view.start, span: view.span });
export function zoomReset() { view.start = 0; view.span = 1; dirty = true; }
export function zoomToChan() {
  const [lo, hi] = filterEdges(), rate = S.spec.rate || 1;
  const span = clamp(Math.max((hi - lo) * 8, 20000) / rate, minSpan(), 1);
  const c = (S.chan.freq - fLo()) / rate;
  view.span = span; view.start = clamp(c - span / 2, 0, 1 - span); dirty = true;
}
/** Centra la vista en la frecuencia f; con la vista completa (span > 0,5) acerca además a un cuarto de la banda. */
export function centerViewOn(f) {
  const rate = S.spec.rate || 1;
  if (view.span > 0.5) view.span = clamp(0.25, minSpan(), 1);
  view.start = clamp((f - fLo()) / rate - view.span / 2, 0, 1 - view.span); dirty = true;
}
function centerOnChan() {
  const c = (S.chan.freq - fLo()) / (S.spec.rate || 1);
  view.start = clamp(c - view.span / 2, 0, 1 - view.span); dirty = true;
}

/* ---- datos entrantes ---- */
function onSpectrum(buf, isSweep = false) {
  const buf_t = performance.now();
  if (!R || buf.byteLength < 32) return;
  const dv = new DataView(buf);
  const nn = dv.getUint32(4, true), center = dv.getFloat64(8, true), rate = dv.getFloat64(16, true);
  const dbmin = dv.getFloat32(24, true), dbstep = dv.getFloat32(28, true);
  if (!nn || buf.byteLength < 32 + nn || !(rate > 0)) return;
  const sp = S.spec;
  const modeChanged = !!sp.sweep !== isSweep;
  const rateChanged = modeChanged || Math.abs(rate - sp.rate) > 1 || Math.abs(center - sp.center) > 1;
  if (nn !== n) { n = nn; avg = new Float32Array(n).fill(NaN); lastDb = new Float32Array(n); }
  if ((rate !== sp.rate && sp.rate) || modeChanged) { zoomReset(); avg.fill(NaN); }
  Object.assign(sp, { n, center, rate, dbmin, dbstep, engine: ['CPU', 'GPU', 'FPGA'][dv.getUint8(1)] || 'CPU', sweep: isSweep });
  // Aquí solo lo que necesita la interfaz (rango automático, lectura bajo el cursor, panel de señal); la cascada, la
  // media y la retención de picos las calcula el renderizador sobre los mismos bytes.
  const v = new Uint8Array(buf, 32, n);
  for (let i = 0; i < n; i++) {
    const db = dbmin + v[i] * dbstep; lastDb[i] = db;
    avg[i] = isSweep || Number.isNaN(avg[i]) ? db : avg[i] + (db - avg[i]) * 0.55;
  }
  if (auto) autoRange(buf_t);
  dataDirty = true;
  if (rateChanged) { updateInfo(); dirty = true; }
  R.spectrum(buf, isSweep);                          // el último uso: en el worker el buffer se TRANSFIERE (queda vacío aquí)
}

function refreshRangeUi() { const o = dispOff(); $('#rng-min').value = Math.round(rangeLo + o); $('#rng-max').value = Math.round(rangeHi + o); }

let lastAuto = 0;
/** Rango automático: se recalcula como mucho cada ~45 ms (ordenar 512 muestras por trama es trabajo de más a 60 tramas/s);
 *  el suavizado se ajusta al tiempo transcurrido para que la dinámica no dependa de la tasa de tramas. */
function autoRange(now) {
  const dt = lastAuto ? Math.min(500, now - lastAuto) : 50;
  if (dt < 45) return;
  lastAuto = now;
  const k = 1 - Math.pow(1 - 0.08, dt / 50);
  const step = Math.max(1, Math.floor(n / 512)), s = [];
  for (let i = 0; i < n; i += step) s.push(lastDb[i]);
  s.sort((a, b) => a - b);
  // Suelo = mediana (robusta ante señales fuertes) menos 6 dB: el ruido cae en el azul oscuro de la paleta.
  const med = s[Math.floor(s.length * 0.5)], lo = med - 6, hi = Math.max(s[s.length - 1] + 6, med + 40);
  rangeLo += (lo - rangeLo) * k; rangeHi += (hi - rangeHi) * k;
  if (now - lastRangeUi > 600) { lastRangeUi = now; refreshRangeUi(); }
}

function updateInfo() {
  const s = S.spec;
  const bins = s.n ? `${fmtHz(s.rate / s.n, 1)}/bin` : '';
  $('#spec-info').textContent = !s.n ? '' : s.sweep
    ? `Barrido ${fmtHz(s.center - s.rate / 2)} – ${fmtHz(s.center + s.rate / 2)} · ${s.n} bins · ${bins} · ${glKind}`
    : `${fmtHz(s.center)} · ±${fmtHz(s.rate / 2)} · FFT ${s.n} · ${bins} · ${s.engine || 'CPU'} · ${glKind}`;
}

/* ---- dibujo ---- */
function niceStep(span, target) {
  const raw = span / target, p = Math.pow(10, Math.floor(Math.log10(raw))), m = raw / p;
  return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p;
}
const dbY = (db) => (1 - (db - rangeLo) / (rangeHi - rangeLo)) * (plotH - AXIS_H);

/** Rejilla en dB, eje de frecuencia y rótulos: capa estática sobre la curva (la curva la pinta el renderizador). */
function drawGrid(c) {
  const rate = S.spec.rate;
  c.font = '10px ui-monospace, monospace'; c.textBaseline = 'middle';
  // rejilla horizontal en dB
  const dstep = niceStep(rangeHi - rangeLo, 5), off = dispOff();
  c.strokeStyle = 'rgba(255,255,255,.07)'; c.fillStyle = 'rgba(255,255,255,.4)'; c.lineWidth = 1; c.textAlign = 'left';
  // la rejilla se calcula en la unidad que se muestra (dBm = dBFS + off) para que caiga en cifras redondas
  for (let d = Math.ceil((rangeLo + off) / dstep) * dstep; d < rangeHi + off; d += dstep) {
    const y = Math.round(dbY(d - off)) + .5; c.beginPath(); c.moveTo(0, y); c.lineTo(W, y); c.stroke(); if (y > 30 && y < plotH - AXIS_H - 4) c.fillText(d.toFixed(0), 3, y - 6);   // sin pisar la franja de señales ni el eje
  }
  c.textAlign = 'right'; c.fillText(dispUnit(), W - 4, 30); c.textAlign = 'left';
  if (sweeping()) { c.fillStyle = 'rgba(255,211,125,.9)'; c.fillText('BARRIDO' + (S.sweepPct != null ? ` ${S.sweepPct} %` : ''), 3, plotH - AXIS_H - 8); }
  if (!rate || !n) { c.fillStyle = 'rgba(255,255,255,.5)'; c.textAlign = 'center'; c.fillText(S.connected ? 'Esperando espectro…' : 'Sin conexión con el servidor', W / 2, plotH / 2); return; }
  // eje de frecuencia
  const spanHz = view.span * rate, fstep = niceStep(spanHz, Math.max(2, W / 95));
  const f0 = xToFreq(0), f1 = xToFreq(W);
  c.textAlign = 'center'; c.strokeStyle = 'rgba(255,255,255,.07)';
  for (let f = Math.ceil(f0 / fstep) * fstep; f <= f1; f += fstep) {
    const x = Math.round(freqToX(f)) + .5;
    c.beginPath(); c.moveTo(x, 0); c.lineTo(x, plotH - AXIS_H); c.stroke();
    if (x > 16 && x < W - 16) { c.fillStyle = 'rgba(255,255,255,.55)'; c.fillText(fmtAxis(f, fstep), x, plotH - AXIS_H / 2); }
  }
  c.textAlign = 'right'; c.fillStyle = 'rgba(255,255,255,.4)'; c.fillText(Math.abs(f1) >= 1e6 ? 'MHz' : 'kHz', W - 4, plotH - AXIS_H - 8);   // unidad del eje
}

// Marcadores: el servidor ya excluye bordes, fusiona y aplica histéresis (src/dsp/detector.c).
const sigList = () => {
  if (sweeping() || !S.spec.rate) return [];
  return S.signals.list.map(([off, bw, peak, snr]) => ({ freq: S.signals.center + off, bw, peak, snr }));
};

function drawSweepOverlay(c) {
  // sin canal ni decodificadores: solo el punto elegido con un clic y la lectura bajo el cursor
  if (sweepPick != null) {
    const x = freqToX(sweepPick);
    c.strokeStyle = 'rgba(255,120,100,.95)'; c.lineWidth = 1.5; c.beginPath(); c.moveTo(x, 0); c.lineTo(x, H); c.stroke();
  }
  drawHover(c);
}

function drawOverlay() {
  const c = ox; c.setTransform(dpr, 0, 0, dpr, 0, 0); c.clearRect(0, 0, W, H);
  ovLo = rangeLo; ovHi = rangeHi;
  drawGrid(c);
  if (!S.spec.rate) return;
  if (sweeping()) { drawSweepOverlay(c); return; }
  // filtro del canal
  const [lo, hi] = filterEdges(), xl = freqToX(lo), xh = freqToX(hi), xc = freqToX(S.chan.freq);
  c.fillStyle = 'rgba(208,188,255,.16)'; c.fillRect(xl, 0, Math.max(2, xh - xl), H);
  c.strokeStyle = 'rgba(208,188,255,.9)'; c.lineWidth = 1.5;
  for (const x of [xl, xh]) { c.beginPath(); c.moveTo(x, 0); c.lineTo(x, H); c.stroke(); c.fillStyle = '#d0bcff'; c.fillRect(x - 3, plotH / 2 - 10, 6, 20); }
  c.strokeStyle = 'rgba(255,120,100,.95)'; c.beginPath(); c.moveTo(xc, 0); c.lineTo(xc, H); c.stroke();
  // señales detectadas (franja superior)
  c.font = '10px ui-monospace, monospace'; c.textAlign = 'center'; c.textBaseline = 'top';
  for (const s of sigList()) {
    const x = freqToX(s.freq), w = Math.max(6, (s.bw / (S.spec.rate * view.span)) * W);
    if (x + w < 0 || x - w > W) continue;
    c.fillStyle = 'rgba(255,211,125,.85)'; c.fillRect(x - w / 2, 1, w, 6);
    c.beginPath(); c.moveTo(x - 4, 8); c.lineTo(x + 4, 8); c.lineTo(x, 14); c.closePath(); c.fill();
    if (w > 28) { c.fillStyle = '#ffd37d'; c.fillText(s.snr.toFixed(0) + ' dB', Math.min(Math.max(x + w / 2 + 14, 16), W - 16), 2); }   // etiqueta al lado: no tapa el pico
  }
  // Instancias de decodificador. Narrow: línea discontinua en su frecuencia. Wide: la instancia ocupa
  // toda su banda de captura (rate), así que se pinta como banda sombreada recortada a la captura, y solo
  // si su frecuencia (el centro con el que se creó) cae dentro de la captura actual; si no, no se pinta.
  const cLo = fLo(), cHi = cLo + S.spec.rate;
  c.textBaseline = 'top';
  for (const d of S.decList) {
    const cat = S.decoders.find((x) => x.id === d.proto), wide = cat && cat.kind === 'wide';
    if (d.freq < cLo || d.freq > cHi) continue;
    const col = d.state === 'error' ? '#ffb4ab' : d.state === 'waiting' ? '#ffd37d' : '#8ed6a5';
    if (wide) {
      const half = (d.rate || cat.wide_rate || S.spec.rate) / 2;
      const x0 = Math.max(0, freqToX(Math.max(cLo, d.freq - half))), x1 = Math.min(W, freqToX(Math.min(cHi, d.freq + half)));
      if (x1 < 0 || x0 > W) continue;
      c.fillStyle = col; c.globalAlpha = 0.09; c.fillRect(x0, plotH, x1 - x0, H - plotH);
      c.globalAlpha = 0.8; c.fillRect(x0, plotH, x1 - x0, 2); c.globalAlpha = 1;
      c.textAlign = 'left'; c.fillText(d.proto, Math.max(x0, 0) + 4, plotH + 5);
    } else {
      const x = freqToX(d.freq); if (x < -5 || x > W + 5) continue;
      c.setLineDash([4, 4]); c.strokeStyle = col; c.beginPath(); c.moveTo(x, plotH); c.lineTo(x, H); c.stroke(); c.setLineDash([]);
      c.fillStyle = col; c.textAlign = 'left'; c.fillText(d.proto, x + 3, plotH + 3);
    }
  }
  if (drag && drag.type === 'edge') {
    const t = `Ancho ${fmtHz(S.chan.bw, 2)}`; c.font = '11px ui-monospace, monospace'; const tw = c.measureText(t).width + 10, tx = clamp((xl + xh) / 2 - tw / 2, 0, W - tw);
    c.fillStyle = 'rgba(20,18,24,.9)'; c.fillRect(tx, plotH + 6, tw, 16); c.fillStyle = '#d0bcff'; c.textAlign = 'left'; c.textBaseline = 'middle'; c.fillText(t, tx + 5, plotH + 14);
  }
  drawHover(c);
}

/* lectura de frecuencia y nivel bajo el cursor */
function drawHover(c) {
  if (!hover || !S.spec.rate || !n) return;
  const f = xToFreq(hover.x), i = clamp(Math.floor(((f - fLo()) / S.spec.rate) * n), 0, n - 1);
  const t = `${fmtHz(f, 4)}  ${avg[i] != null && isFinite(avg[i]) ? (avg[i] + dispOff()).toFixed(1) + ' ' + dispUnit() : ''}`;
  c.font = '11px ui-monospace, monospace'; const tw = c.measureText(t).width + 10, tx = clamp(hover.x + 10, 0, W - tw);
  c.fillStyle = 'rgba(20,18,24,.85)'; c.fillRect(tx, plotH - AXIS_H - 20, tw, 16);
  c.fillStyle = '#e6e0e9'; c.textAlign = 'left'; c.textBaseline = 'middle'; c.fillText(t, tx + 5, plotH - AXIS_H - 12);
  c.strokeStyle = 'rgba(255,255,255,.25)'; c.lineWidth = 1; c.beginPath(); c.moveTo(hover.x + .5, 0); c.lineTo(hover.x + .5, H); c.stroke();
}

/* Un ciclo por imagen. La capa de rótulos (rejilla, filtro, marcadores) solo se redibuja si cambió algo
 * de la interfaz o si el rango automático se movió ≥ 0,5 dB: con espectro nuevo a 20-60 tramas/s no hace
 * falta repintar un lienzo del tamaño de la ventana. La vista se manda al renderizador solo si cambió. */
function frame() {
  if (R) {
    if (!dirty && dataDirty && (Math.abs(rangeLo - ovLo) > 0.5 || Math.abs(rangeHi - ovHi) > 0.5)) dirty = true;
    const k = [view.start, view.span, rangeLo.toFixed(2), rangeHi.toFixed(2), holdOn].join();
    if (k !== sig.view) { sig.view = k; R.view(view.start, view.span, rangeLo, rangeHi, holdOn); }
    if (dirty) { drawOverlay(); }
    dirty = dataDirty = false;
  }
  requestAnimationFrame(frame);
}

/* ---- interacción ---- */
const ptrs = new Map();
let drag = null, lpTimer = 0;
/** Bins del último espectro (dBFS) para el panel «¿Qué es esta señal?»; solo lectura. */
export const spectrumBins = () => ({ db: lastDb, n, center: S.spec.center, rate: S.spec.rate, sweep: sweeping() });
/** Pulsación larga o clic derecho: pide el panel en una frecuencia cualquiera (tol = 10 px en Hz). */
function askHere(cx, cy, x) {
  if (sweeping() || !S.spec.rate) return;
  bus.emit('sig-menu', { sig: null, freq: xToFreq(x), tol: (10 / W) * view.span * S.spec.rate, x: cx, y: cy });
}

function hitEdge(x, tol = 7) {
  if (sweeping()) return null;
  const [lo, hi] = filterEdges(), xl = freqToX(lo), xh = freqToX(hi);
  // con filtro muy estrecho los dos bordes se solapan: gana el más cercano
  const dl = Math.abs(x - xl), dh = Math.abs(x - xh);
  if (dl <= tol && dl <= dh) return 'lo';
  if (dh <= tol) return 'hi';
  return null;
}
/* Inercia del desplazamiento: al soltar con velocidad, la vista sigue y se frena exponencialmente (τ ≈ 0,25 s) hasta que le queden menos de 0,4 px por recorrer. */
const reduceMotion = () => !!(window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
let inertia = null;
function stopInertia() { inertia = null; }
function startInertia(v) {            // v: fracción de banda de la vista por ms
  if (reduceMotion() || Math.abs(v) < 2e-5) return;
  inertia = { v, t: performance.now() };
  const step = (now) => {
    if (!inertia) return;
    const dt = Math.min(50, now - inertia.t); inertia.t = now;
    const ns = clamp(view.start + inertia.v * dt, 0, 1 - view.span);
    if (ns === view.start) { inertia = null; return; }
    view.start = ns; inertia.v *= Math.exp(-dt / 250); dirty = true;
    if (Math.abs(inertia.v) * 250 < 0.4 * view.span / W) { inertia = null; return; }
    requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}
function hitSignal(x, y) {
  if (y > 22) return null;
  let best = null, bd = 1e9;
  for (const s of sigList()) {
    const sx = freqToX(s.freq), w = Math.max(8, (s.bw / (S.spec.rate * view.span)) * W) / 2 + 4, d = Math.abs(sx - x);
    if (d <= w && d < bd) { best = s; bd = d; }
  }
  return best;
}
const snap = (f) => { const st = Math.max(10, Math.min(getStep(), 100000)); return Math.round(f / st) * st; };
const localPt = (e) => { const r = ovC.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top }; };

function setEdge(which, x) {
  const f = xToFreq(x), c = S.chan; let bw;
  if (c.mode === 'usb') bw = f - c.freq; else if (c.mode === 'lsb') bw = c.freq - f;
  else bw = 2 * Math.abs(f - c.freq);
  if ((c.mode === 'usb' && which === 'lo') || (c.mode === 'lsb' && which === 'hi')) return; // ese borde es el de la portadora
  setChannel({ bw: clamp(bw, BW_MIN, BW_MAX[c.mode]) });
}

function onDown(e) {
  ovC.setPointerCapture(e.pointerId);
  const p = localPt(e); ptrs.set(e.pointerId, p);
  if (ptrs.size === 2) { drag = { type: 'pinch', d0: dist(), span0: view.span, start0: view.start }; return; }
  stopInertia();
  // botón central o Mayús: desplazar la vista siempre (sin sintonizar al soltar)
  const force = e.button === 1 || e.shiftKey;
  if (e.button === 1) e.preventDefault();
  const edge = force ? null : hitEdge(p.x, e.pointerType === 'touch' ? 16 : 7), sig = edge || force ? null : hitSignal(p.x, p.y);
  clearTimeout(lpTimer);
  if (e.pointerType === 'touch' && !edge && ptrs.size === 1) { const cx = e.clientX, cy = e.clientY, px0 = p.x; lpTimer = setTimeout(() => { if (drag && !drag.moved) { drag = { type: 'none' }; askHere(cx, cy, px0); } }, 550); }
  drag = { type: edge ? 'edge' : sig ? 'sig' : 'pan', force, edge, sig, x0: p.x, y0: p.y, start0: view.start, moved: false, cx: e.clientX, cy: e.clientY, hist: [[performance.now(), p.x]] };
}
const dist = () => { const [a, b] = [...ptrs.values()]; return Math.hypot(a.x - b.x, a.y - b.y) || 1; };
function onMove(e) {
  const p = localPt(e);
  if (!ptrs.has(e.pointerId)) {
    hover = e.pointerType === 'mouse' ? p : null;
    ovC.style.cursor = e.shiftKey ? 'grab' : hitEdge(p.x) ? 'ew-resize' : hitSignal(p.x, p.y) ? 'pointer' : 'crosshair';
    dirty = true; return;
  }
  ptrs.set(e.pointerId, p);
  if (!drag) return;
  if (drag.type === 'pinch' && ptrs.size === 2) {
    const [a, b] = [...ptrs.values()], mid = ((a.x + b.x) / 2 / W) * drag.span0 + drag.start0;
    const ns = clamp(drag.span0 * (drag.d0 / dist()), minSpan(), 1);
    view.start = clamp(mid - (((a.x + b.x) / 2) / W) * ns, 0, 1 - ns); view.span = ns; dirty = true; return;
  }
  if (Math.hypot(p.x - drag.x0, p.y - drag.y0) > 4) { drag.moved = true; clearTimeout(lpTimer); }
  if (drag.type === 'edge') setEdge(drag.edge, p.x);
  else if (drag.moved && (drag.type === 'pan' || drag.type === 'sig')) {
    drag.type = 'pan';
    view.start = clamp(drag.start0 - ((p.x - drag.x0) / W) * view.span, 0, 1 - view.span); dirty = true;
    const hs = drag.hist, now = performance.now(); hs.push([now, p.x]); while (hs.length > 2 && now - hs[0][0] > 100) hs.shift();
  }
  hover = e.pointerType === 'mouse' ? p : null; dirty = true;
}
function onUp(e) {
  clearTimeout(lpTimer);
  ptrs.delete(e.pointerId);
  if (drag && drag.type === 'pinch') { if (ptrs.size < 2) drag = ptrs.size ? { type: 'none' } : null; return; }
  const d = drag; drag = null;
  if (d && d.moved && d.type === 'pan' && d.hist && d.hist.length > 1) {
    const [t0, x0] = d.hist[0], [t1, x1] = d.hist[d.hist.length - 1];
    if (performance.now() - t1 < 80 && t1 > t0) startInertia(-((x1 - x0) / W) * view.span / (t1 - t0));   // soltó en movimiento
  }
  if (!d || d.moved || d.force) return;
  if (d.type === 'sig') bus.emit('sig-menu', { sig: d.sig, x: d.cx, y: d.cy });
  else if (d.type === 'pan') {
    if (sweeping()) { sweepPick = Math.round(xToFreq(d.x0)); bus.emit('sweep-pick', sweepPick); dirty = true; }
    else setChannel({ freq: snap(xToFreq(d.x0)) }, { user: true });
  }
}
/* Rueda: sin modificador cambia la frecuencia un paso (el del modo); Ctrl (o el pellizco del panel táctil, que llega como Ctrl+rueda)
 * hace zoom alrededor del cursor. En modo barrido no hay canal y la rueda hace zoom directamente. Los paneles táctiles envían muchos
 * deltas pequeños: se acumulan hasta 40 unidades para dar un paso; una muesca de ratón (≥ 50) da siempre uno. */
let wheelAcc = 0;
function onWheel(e) {
  e.preventDefault(); stopInertia();
  const p = localPt(e), frac = view.start + (p.x / W) * view.span;
  const dy = e.deltaY || e.deltaX;
  if (e.ctrlKey || sweeping()) {
    zoomAt(frac, Math.abs(dy) >= 50 ? (dy < 0 ? 1.25 : 0.8) : Math.exp(-dy * 0.02));
    return;
  }
  let n;
  if (Math.abs(dy) >= 50) { n = Math.sign(-dy); wheelAcc = 0; }
  else { wheelAcc += dy; n = 0; while (wheelAcc <= -40) { n++; wheelAcc += 40; } while (wheelAcc >= 40) { n--; wheelAcc -= 40; } }
  if (n) setChannel({ freq: S.chan.freq + n * getStep() * (e.shiftKey ? 10 : 1) }, { user: true });
}

export function initSpectrum() {
  wrap = $('#spec-wrap');
  plotC = document.createElement('canvas'); wfC = document.createElement('canvas'); ovC = document.createElement('canvas');
  ovC.className = 'ov'; ovC.tabIndex = 0; ovC.setAttribute('role', 'img');
  ovC.setAttribute('aria-label', 'Espectro y cascada. Clic para sintonizar, rueda para cambiar de frecuencia, Ctrl+rueda para el zoom, flechas para ajustar.');
  wrap.append(plotC, wfC, ovC);
  ox = ovC.getContext('2d');
  startRenderer(false);
  new ResizeObserver(() => layout()).observe(wrap);
  ovC.addEventListener('pointerdown', onDown);
  ovC.addEventListener('pointermove', onMove);
  ovC.addEventListener('pointerup', onUp);
  ovC.addEventListener('pointercancel', onUp);
  ovC.addEventListener('pointerleave', () => { hover = null; dirty = true; });
  ovC.addEventListener('wheel', onWheel, { passive: false });
  ovC.addEventListener('dblclick', (e) => { if (!sweeping()) centerViewOn(xToFreq(localPt(e).x)); });
  ovC.addEventListener('contextmenu', (e) => { e.preventDefault(); const p = localPt(e); askHere(e.clientX, e.clientY, p.x); });
  ovC.addEventListener('auxclick', (e) => e.preventDefault());   // el botón central desplaza; no debe abrir nada

  $('#z-in').addEventListener('click', () => zoomBy(1.6));
  $('#z-out').addEventListener('click', () => zoomBy(1 / 1.6));
  $('#z-reset').addEventListener('click', zoomReset);
  $('#z-chan').addEventListener('click', zoomToChan);
  const ra = $('#rng-auto'), rmin = $('#rng-min'), rmax = $('#rng-max');
  ra.addEventListener('change', () => { auto = ra.checked; rmin.disabled = rmax.disabled = auto; dirty = true; });
  const man = () => { if (!auto) { rangeLo = +rmin.value - dispOff(); rangeHi = Math.max(+rmax.value - dispOff(), rangeLo + 5); dirty = true; } };
  rmin.addEventListener('input', man); rmax.addEventListener('input', man);
  $('#peak-hold').addEventListener('change', (e) => { holdOn = e.target.checked; dirty = true; });

  bus.on('bin:spectrum', (b) => { S.sweepPct = null; onSpectrum(b, false); });
  bus.on('bin:sweep', (b) => {
    // progreso para la cabecera; los bytes [2] y [3] los lee onSpectrum / sweep.js
    if (b.byteLength >= 4) S.sweepPct = new Uint8Array(b, 3, 1)[0];
    dirty = true;
    onSpectrum(b, true);
  });
  const dbm = $('#dbm-on');
  dbm.addEventListener('change', () => { S.dbmOn = dbm.checked; refreshRangeUi(); bus.emit('dbm'); dirty = true; });
  bus.on('msg:status', (st) => {
    const has = st.dbm_offset != null;
    dbm.closest('label').hidden = !has;
    if (!has && S.dbmOn) { S.dbmOn = false; dbm.checked = false; refreshRangeUi(); }
    if (has && auto) refreshRangeUi();
    if (!st.sweep && S.spec.sweep) dirty = true;
  });
  bus.on('msg:signals', () => { dirty = true; });
  bus.on('msg:dec_list', () => { dirty = true; });
  bus.on('ws', () => { dirty = true; });
  bus.on('chan', ({ patch, user }) => {
    dirty = true;
    // si la frecuencia sale de la vista, desplazarla para seguirla
    if (patch.freq != null && S.spec.rate) {
      const f = (S.chan.freq - fLo()) / S.spec.rate;
      if (f < view.start || f > view.start + view.span) centerOnChan();
    }
  });
  bus.on('msg:status', () => { updateInfo(); dirty = true; });
  // El teclado sobre el lienzo (flechas) lo gestiona main.js: el lienzo es «libre» para los atajos.
  requestAnimationFrame(frame);
}
export const spectrumReady = () => !!R;
/** Diagnóstico para las pruebas: ¿worker?, ¿WebGL? y tiempo medio de dibujo (solo en este hilo). */
export const specRenderInfo = () => ({ worker: !!worker, gl: glKind === 'WebGL', stats: R ? R.stats() : null, inflight, dropped });
