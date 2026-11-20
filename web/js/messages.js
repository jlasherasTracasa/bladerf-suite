// messages.js — lista de mensajes de texto (buscapersonas, ACARS, VDL2, Meshtastic…).
//
// Sin conocer los campos de cada protocolo: un evento «dec» es un mensaje si trae un
// campo de cuerpo con nombre típico (message, msg, body, content, payload…) de ≥ 8
// caracteres o, si no, si su campo «text» (el resumen de una línea que añade el host)
// es largo (≥ 24) y el evento no es una posición (lat/lon: eso va al mapa). El texto
// se destaca grande; el resto de campos escalares se listan pequeños debajo.
// Filtros, fijados, detalle, exportación y virtualización: los da evlist.js.
import { S, $, h, fmtHz, fmtTime } from './util.js';
import { fmtVal } from './tables.js';
import { createEventList } from './evlist.js';

const BODY = ['message', 'msg', 'body', 'content', 'payload', 'txt', 'rt'];
const MIN_BODY = 8, MIN_TEXT = 24, MAX = 2000;

const hasPos = (m) => ['lat', 'latitude'].some((k) => typeof m[k] === 'number') && ['lon', 'lng', 'longitude'].some((k) => typeof m[k] === 'number');
export function bodyOf(ev) {
  const m = ev.msg || {};
  for (const k of BODY) if (typeof m[k] === 'string' && m[k].trim().length >= MIN_BODY) return { text: m[k].trim(), key: k };
  if (typeof m.text === 'string' && m.text.length >= MIN_TEXT && !hasPos(m)) return { text: m.text, key: 'text' };
  return null;
}

function render(ev) {
  const b = ev._b, d = S.decoders.find((x) => x.id === ev.proto);
  const kv = Object.entries(ev.msg).filter(([k, v]) => k !== b.key && k !== 'text' && v != null && typeof v !== 'object').slice(0, 8);
  return h('article', { class: 'msg' + (ev.replay ? ' replay' : '') },
    h('div', { class: 'h' }, h('span', { text: fmtTime(ev.t) }), h('b', { text: d ? d.name : ev.proto }), h('span', { text: fmtHz(ev.freq, 4) }), h('span', { text: '#' + ev.inst })),
    h('p', { class: 'body', text: b.text }),
    kv.length ? h('div', { class: 'kvs' }, kv.map(([k, v]) => h('span', {}, k + ': ', h('b', { text: fmtVal(v) })))) : null);
}
const list = createEventList({
  prefix: 'ms', max: MAX, render, textOf: (ev) => ev._b.text,
  accept: (ev) => { const b = bodyOf(ev); if (!b) return false; ev._b = b; return true; },
  empty: 'Sin mensajes todavía. Aquí aparecen los textos de buscapersonas, ACARS, VDL2, Meshtastic…',
  visible: () => { const p = document.getElementById('p-mensajes'); return !p || p.style.display !== 'none'; },
});
export function initMessages() { list.init(); void $; }
export const msList = list;
