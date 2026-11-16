// messages.js — lista de mensajes de texto (buscapersonas, ACARS, VDL2, Meshtastic…).
//
// Sin conocer los campos de cada protocolo: un evento «dec» es un mensaje si trae un
// campo de cuerpo con nombre típico (message, msg, body, content, payload…) de ≥ 8
// caracteres o, si no, si su campo «text» (el resumen de una línea que añade el host)
// es largo (≥ 24) y el evento no es una posición (lat/lon: eso va al mapa). El texto
// se destaca grande; el resto de campos escalares se listan pequeños debajo.
import { S, bus, $, h, fmtHz, fmtTime } from './util.js';
import { fmtVal } from './tables.js';

const BODY = ['message', 'msg', 'body', 'content', 'payload', 'txt', 'rt'];
const MIN_BODY = 8, MIN_TEXT = 24, MAX = 400, SHOW = 150;
const msgs = [];
let paused = false;

const hasPos = (m) => ['lat', 'latitude'].some((k) => typeof m[k] === 'number') && ['lon', 'lng', 'longitude'].some((k) => typeof m[k] === 'number');
export function bodyOf(ev) {
  const m = ev.msg || {};
  for (const k of BODY) if (typeof m[k] === 'string' && m[k].trim().length >= MIN_BODY) return { text: m[k].trim(), key: k };
  if (typeof m.text === 'string' && m.text.length >= MIN_TEXT && !hasPos(m)) return { text: m.text, key: 'text' };
  return null;
}

function render(ev, b) {
  const d = S.decoders.find((x) => x.id === ev.proto);
  const kv = Object.entries(ev.msg).filter(([k, v]) => k !== b.key && k !== 'text' && v != null && typeof v !== 'object').slice(0, 8);
  return h('article', { class: 'msg' + (ev.replay ? ' replay' : '') },
    h('div', { class: 'h' }, h('span', { text: fmtTime(ev.t) }), h('b', { text: d ? d.name : ev.proto }), h('span', { text: fmtHz(ev.freq, 4) })),
    h('p', { class: 'body', text: b.text }),
    kv.length ? h('div', { class: 'kvs' }, kv.map(([k, v]) => h('span', {}, k + ': ', h('b', { text: fmtVal(v) })))) : null);
}
const passes = (ev) => {
  const f = $('#ms-filter').value, q = $('#ms-search').value.trim().toLowerCase();
  return (!f || ev.proto === f) && (!q || (ev._b.text + JSON.stringify(ev.msg)).toLowerCase().includes(q));
};
function renderAll() {
  const list = msgs.filter(passes).slice(-SHOW).reverse();
  $('#ms-list').replaceChildren(...(list.length ? list.map((e) => render(e, e._b)) : [h('p', { class: 'muted', text: 'Sin mensajes todavía. Aquí aparecen los textos de buscapersonas, ACARS, VDL2, Meshtastic…' })]));
  $('#ms-count').textContent = msgs.length;
}
function onDec(ev) {
  const b = bodyOf(ev); if (!b) return;
  ev._b = b; msgs.push(ev); if (msgs.length > MAX) msgs.shift();
  $('#ms-count').textContent = msgs.length;
  if (paused || !passes(ev)) return;
  const box = $('#ms-list');
  if (!box.querySelector('article')) box.replaceChildren();
  box.prepend(render(ev, b));
  while (box.childElementCount > SHOW) box.lastElementChild.remove();
}
function fillFilter() {
  const f = $('#ms-filter'), cur = f.value;
  f.replaceChildren(h('option', { value: '', text: 'Todos' }), ...S.decoders.map((d) => h('option', { value: d.id, text: d.name })));
  f.value = cur;
}
export function initMessages() {
  $('#ms-filter').addEventListener('change', renderAll);
  $('#ms-search').addEventListener('input', renderAll);
  $('#ms-pause').addEventListener('change', (e) => { paused = e.target.checked; if (!paused) renderAll(); });
  $('#ms-clear').addEventListener('click', () => { msgs.length = 0; renderAll(); });
  bus.on('msg:decoders', fillFilter);
  bus.on('msg:dec', onDec);
  renderAll();
}
