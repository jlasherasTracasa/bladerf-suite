// audio.js — audio del navegador: AudioContext + AudioWorklet (audio-worklet.js).
// Recibe tramas binarias tipo 2 (PCM int16 a 48 kHz, mono o estéreo intercalado),
// las pasa a Float32 L/R (el mono se duplica) y las envía al worklet, que se
// encarga del búfer anti-jitter y de convertir 48 kHz a la tasa del contexto.
import { S, bus, $, h, clamp } from './util.js';

let ctx = null, node = null, gain = null, on = false;
let stats = { fillMs: 0, under: 0, over: 0 };
let vol = 0.8, targetMs = 120;
try { vol = parseFloat(localStorage.getItem('vol') ?? '0.8') || 0.8; targetMs = +localStorage.getItem('abuf') || 120; } catch { /* sin almacenamiento */ }

export const audioOn = () => on;

function applyVol() { if (gain) gain.gain.value = vol * vol; }   // curva cuadrática: más natural

export async function startAudio() {
  if (on) return true;
  try {
    ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('js/audio-worklet.js');
    node = new AudioWorkletNode(ctx, 'sdr-player', { numberOfInputs: 0, outputChannelCount: [2] });
    gain = ctx.createGain();
    node.connect(gain).connect(ctx.destination);
    node.port.onmessage = (e) => { stats = e.data; renderInfo(); };
    node.port.postMessage({ t: 'cfg', inRate: 48000, targetMs });
    applyVol();
    await ctx.resume();
    on = true;
  } catch (e) {
    console.error(e);
    bus.emit('audio:error', e);
    try { ctx && ctx.close(); } catch { /* nada */ }
    ctx = node = gain = null;
    return false;
  }
  bus.emit('audio', true);
  renderInfo();
  return true;
}

export function stopAudio() {
  if (!on) return;
  on = false;
  try { node.disconnect(); ctx.close(); } catch { /* nada */ }
  ctx = node = gain = null;
  bus.emit('audio', false);
  renderInfo();
}

function onFrame(buf) {
  if (!on || !node) return;
  const dv = new DataView(buf);
  if (dv.getUint8(0) !== 2 || buf.byteLength <= 8) return;
  const ch = dv.getUint8(2) === 2 ? 2 : 1;
  const frames = ((buf.byteLength - 8) >> 1) / ch | 0;
  if (!frames) return;
  const pcm = new Int16Array(buf.slice(8, 8 + frames * ch * 2));   // slice: alineación a 2 bytes garantizada
  const l = new Float32Array(frames), r = ch === 2 ? new Float32Array(frames) : l;
  if (ch === 2) for (let i = 0; i < frames; i++) { l[i] = pcm[2 * i] / 32768; r[i] = pcm[2 * i + 1] / 32768; }
  else for (let i = 0; i < frames; i++) l[i] = pcm[i] / 32768;
  const tr = ch === 2 ? [l.buffer, r.buffer] : [l.buffer];
  node.port.postMessage({ t: 'pcm', l, r: ch === 2 ? r : l.slice() }, ch === 2 ? tr : [l.buffer]);
}

function renderInfo() {
  const kv = $('#au-kv'), info = $('#au-info');
  if (!kv) return;
  info.textContent = on ? 'Audio activo.' : 'Pulsa «Escuchar» para activar el audio.';
  const rows = on ? [
    ['Tasa del dispositivo', `${ctx.sampleRate} Hz (el servidor envía 48000 Hz)`],
    ['Búfer', `${stats.fillMs.toFixed(0)} ms (objetivo ${targetMs} ms)`],
    ['Vaciados / desbordes', `${stats.under} / ${stats.over}`],
    ['Latencia de salida', `${(((ctx.baseLatency || 0) + (ctx.outputLatency || 0)) * 1000).toFixed(0)} ms`],
  ] : [];
  kv.replaceChildren(...rows.flatMap(([k, v]) => [h('span', { class: 'k', text: k }), h('span', { class: 'v mono', text: v })]));
}

export function initAudio() {
  const v = $('#vol');
  v.value = vol;
  v.addEventListener('input', () => { vol = clamp(+v.value, 0, 1); applyVol(); try { localStorage.setItem('vol', vol); } catch { /* nada */ } });
  // selector del búfer anti-jitter (dentro de la tarjeta de audio)
  const sel = h('select', { id: 'au-buf', 'aria-label': 'Búfer anti-jitter' },
    ...[60, 120, 250, 500].map((ms) => h('option', { value: ms, text: ms + ' ms', selected: ms === targetMs })));
  sel.addEventListener('change', () => {
    targetMs = +sel.value; try { localStorage.setItem('abuf', targetMs); } catch { /* nada */ }
    if (node) node.port.postMessage({ t: 'cfg', targetMs });
  });
  $('#au-kv').before(h('label', {}, 'Búfer anti-jitter', sel));
  bus.on('bin:audio', onFrame);
  renderInfo();
}
