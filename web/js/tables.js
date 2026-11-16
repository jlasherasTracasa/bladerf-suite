// tables.js — tablas automáticas para cualquier array de objetos de «dec_state».
//
// No conoce los campos de ningún protocolo: las columnas son la unión de las claves
// de las filas, ordenadas por frecuencia de aparición (las claves presentes en más
// filas primero; a igualdad, por orden de primera aparición). Un clic en la cabecera
// ordena (otro clic invierte). El estado de orden se recuerda por tabla (id) para que
// sobreviva a los redibujados de cada segundo.
import { h } from './util.js';

const MAX_COLS = 14;
const sortState = new Map();           // id -> {key, dir}

/** Columnas de un array de objetos por frecuencia de aparición. */
export function columnsOf(rows) {
  const cnt = new Map();
  for (const r of rows.slice(0, 300)) for (const k of Object.keys(r)) if (!k.startsWith('_')) cnt.set(k, (cnt.get(k) || 0) + 1);
  const order = [...cnt.keys()];
  return order.sort((a, b) => cnt.get(b) - cnt.get(a) || order.indexOf(a) - order.indexOf(b)).slice(0, MAX_COLS);
}

/** Texto corto de cualquier valor. */
export function fmtVal(v) {
  if (v == null) return '';
  if (typeof v === 'number') return Number.isInteger(v) ? String(v) : String(+v.toFixed(5));
  if (typeof v === 'boolean') return v ? 'sí' : 'no';
  if (typeof v === 'object') { const s = JSON.stringify(v); return s.length > 60 ? s.slice(0, 57) + '…' : s; }
  return String(v);
}

const cmp = (a, b) => {
  if (a == null || a === '') return b == null || b === '' ? 0 : 1;      // vacíos siempre al final
  if (b == null || b === '') return -1;
  if (typeof a === 'number' && typeof b === 'number') return a - b;
  return String(a).localeCompare(String(b), 'es', { numeric: true });
};

/**
 * Tabla ordenable. opts: id (obligatorio, para recordar el orden), limit, onRow(fila, i),
 * isSel(fila, i) -> bool, rowKey. Devuelve un div.table-wrap.
 */
export function autoTable(rows, opts = {}) {
  const id = opts.id || 'tbl', limit = opts.limit || 100;
  const cols = columnsOf(rows);
  const st = sortState.get(id) || { key: null, dir: 1 };
  const idx = rows.map((r, i) => [r, i]);
  if (st.key) idx.sort(([a], [b]) => cmp(a[st.key], b[st.key]) * st.dir || 0);
  const redraw = (el) => { const n = autoTable(rows, opts); el.replaceWith(n); n.querySelector('th[aria-sort]:not([aria-sort=none])')?.focus?.(); };
  const num = (k) => rows.some((r) => typeof r[k] === 'number');
  const wrap = h('div', { class: 'table-wrap' }, h('table', { class: 'tbl' },
    h('thead', {}, h('tr', {}, cols.map((k) => h('th', {
      class: 'sortable' + (num(k) ? ' n' : ''), tabindex: 0, role: 'button', text: k + (st.key === k ? (st.dir > 0 ? ' ▲' : ' ▼') : ''),
      'aria-sort': st.key === k ? (st.dir > 0 ? 'ascending' : 'descending') : 'none',
      onclick: () => { sortState.set(id, st.key === k ? { key: k, dir: -st.dir } : { key: k, dir: 1 }); redraw(wrap); },
      onkeydown: (e) => { if (e.key === 'Enter') e.currentTarget.click(); },
    })))),
    h('tbody', {}, idx.slice(0, limit).map(([r, i]) => h('tr', {
      class: opts.isSel && opts.isSel(r, i) ? 'sel' : '', tabindex: opts.onRow ? 0 : null,
      onclick: opts.onRow ? () => opts.onRow(r, i) : null,
      onkeydown: opts.onRow ? (e) => { if (e.key === 'Enter') opts.onRow(r, i); } : null,
    }, cols.map((k) => h('td', { class: typeof r[k] === 'number' ? 'n' : '', text: fmtVal(r[k]) })))))));
  return wrap;
}

/** Un array «es tabla» si tiene filas y la primera es un objeto. */
export const isTable = (v) => Array.isArray(v) && v.length > 0 && typeof v[0] === 'object' && v[0] !== null && !Array.isArray(v[0]);

/** Vista completa de un estado: escalares como pares clave/valor y una tabla por cada array de objetos. */
export function stateView(state, idPrefix) {
  const scal = [], tabs = [];
  for (const [k, v] of Object.entries(state || {})) (isTable(v) ? tabs : scal).push([k, v]);
  const out = [];
  if (scal.length) out.push(h('div', { class: 'kv' }, scal.flatMap(([k, v]) => [h('span', { class: 'k', text: k }), h('span', { class: 'v mono', text: fmtVal(v) })])));
  for (const [k, rows] of tabs) out.push(h('div', { class: 'muted', text: `${k} (${rows.length})` }), autoTable(rows, { id: idPrefix + ':' + k }));
  if (!out.length) out.push(h('span', { class: 'muted', text: 'Estado vacío.' }));
  return out;
}
