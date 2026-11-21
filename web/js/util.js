// util.js — utilidades comunes, estado compartido y bus de eventos.
// Todos los módulos de la interfaz leen el estado del servidor desde S y se
// avisan entre sí por el bus (EventTarget), sin acoplarse directamente.

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

/** Crea un elemento: h('div', {class:'x', onclick:fn}, 'texto', otroNodo). */
export function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on') && typeof v === 'function') e.addEventListener(k.slice(2), v);
    else if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) if (c != null && c !== false) e.append(c.nodeType ? c : document.createTextNode(String(c)));
  return e;
}

export const clamp = (x, a, b) => Math.min(b, Math.max(a, x));

class Bus extends EventTarget {
  emit(name, detail) { this.dispatchEvent(new CustomEvent(name, { detail })); }
  on(name, fn) { this.addEventListener(name, (e) => fn(e.detail)); }
}
export const bus = new Bus();

/** Estado compartido (lo rellena ws.js con lo que manda el servidor). */
export const S = {
  connected: false,
  status: null,        // mensaje "status"
  signals: { center: 0, noise: 0, list: [] },
  recording: null,
  decoders: [],        // catálogo
  profiles: [],
  decList: [],         // instancias
  decState: {},        // inst -> {proto,t,state}
  txPolicy: null,
  spec: { n: 0, center: 0, rate: 0, dbmin: -145, dbstep: 0.6, gpu: false },
  // canal de audio de ESTE navegador (lo gestiona receiver.js)
  chan: { freq: 100300000, mode: 'wfm', bw: 180000, squelch: null, deemph: 50, stereo: true, audio: false },
  level: { db: -120, flags: 0 },
  dbmOn: false,        // mostrar niveles en dBm (solo si status.dbm_offset != null)
};

/** Desplazamiento dBFS → dBm que se aplica al mostrar (0 si no hay calibración o está apagado). */
export const dispOff = () => (S.dbmOn && S.status && S.status.dbm_offset != null ? S.status.dbm_offset : 0);
export const dispUnit = () => (S.dbmOn && S.status && S.status.dbm_offset != null ? 'dBm' : 'dBFS');

/** Formatea Hz: 100300000 → "100.300 MHz". */
export function fmtHz(hz, dec) {
  if (hz == null || !isFinite(hz)) return '—';
  const a = Math.abs(hz);
  // GHz: al menos 6 decimales (1 kHz de resolución) y se recortan ceros sobrantes dejando 3 ("1.090 GHz").
  if (a >= 1e9) return (hz / 1e9).toFixed(Math.max(dec ?? 6, 6)).replace(/(\.\d{3}\d*?)0+$/, '$1') + ' GHz';
  if (a >= 1e6) return (hz / 1e6).toFixed(dec ?? 3) + ' MHz';
  if (a >= 1e3) return (hz / 1e3).toFixed(dec ?? 1) + ' kHz';
  return Math.round(hz) + ' Hz';
}
/** Frecuencia compacta para etiquetas del eje. */
export function fmtAxis(hz, step) {
  // Por encima de 1 GHz se rotula en MHz (1089.2) para no arrastrar cuatro decimales de GHz.
  if (Math.abs(hz) >= 1e9) return (hz / 1e6).toFixed(step >= 1e6 ? 0 : step >= 1e5 ? 1 : step >= 1e4 ? 2 : 3);
  if (Math.abs(hz) >= 1e6) return (hz / 1e6).toFixed(step >= 1e6 ? 0 : step >= 1e5 ? 1 : step >= 1e4 ? 2 : step >= 1e3 ? 3 : 4);
  return (hz / 1e3).toFixed(step >= 1e3 ? 0 : 1) + 'k';
}
/** Interpreta "100.3", "100.3M", "433,92 MHz", "162k", "1090000000", "1.09G". Devuelve null si no es válido.
 *  Un número sin unidad: <10000 se toma como MHz; si no, como Hz. */
export function parseHz(txt) {
  if (txt == null) return null;
  const m = String(txt).trim().replace(/[\s\u00a0_]+(?=\d)/g, '').replace(',', '.').match(/^(-?\d*\.?\d+(?:e[+-]?\d+)?)\s*([kmg]?)(?:hz)?$/i);
  if (!m) return null;
  let v = parseFloat(m[1]);
  const u = m[2].toLowerCase();
  if (u === 'k') v *= 1e3; else if (u === 'm') v *= 1e6; else if (u === 'g') v *= 1e9;
  else if (Math.abs(v) < 10000 && !/e/i.test(m[1])) v *= 1e6;
  return Math.round(v);
}
export const fmtTime = (t) => new Date(t * 1000).toLocaleTimeString('es-ES', { hour12: false });
export function fmtBytes(n) {
  if (n >= 1e9) return (n / 1e9).toFixed(2) + ' GB';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + ' MB';
  if (n >= 1e3) return (n / 1e3).toFixed(0) + ' kB';
  return n + ' B';
}

export function toast(msg, kind = '') {
  const box = $('#toasts');
  if (!box) return;
  if ([...box.children].some((x) => x.textContent === msg)) return;   // no repetir un aviso idéntico al que ya está visible
  const t = h('div', { class: 'toast ' + kind, text: msg });   // sin role propio: el contenedor #toasts ya es aria-live=polite (un role=status anidado se leería dos veces)
  box.append(t);
  setTimeout(() => t.remove(), kind === 'err' ? 7000 : 3500);
}

/** Rellena un <select> con [valor, texto]. */
export function fillSelect(sel, items, current) {
  sel.replaceChildren(...items.map(([v, t]) => h('option', { value: v, text: t })));
  if (current != null) sel.value = String(current);
}

/** Aplica una función como mucho cada ms milisegundos (último valor gana). */
export function throttle(fn, ms) {
  let t = 0, pend = null, timer = 0;
  return (...a) => {
    const now = performance.now();
    if (now - t >= ms) { t = now; fn(...a); }
    else { pend = a; if (!timer) timer = setTimeout(() => { timer = 0; t = performance.now(); fn(...pend); }, ms - (now - t)); }
  };
}

/** Evita pisar un campo mientras el usuario lo edita. */
export const editing = (el) => document.activeElement === el;
