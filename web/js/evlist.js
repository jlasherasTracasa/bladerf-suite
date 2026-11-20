// evlist.js — lista de eventos reutilizable (pestañas Decodificadores y Mensajes).
//
// Una «lista» gestiona: el almacén de eventos «dec» (tope configurable), los filtros
// (protocolo, instancia, texto, solo fijados), la pausa, los eventos fijados, el panel
// de detalle con el JSON formateado y la exportación (copiar, JSONL, CSV) de lo que
// pasa los filtros.
//
// Virtualización. Hasta VIRT_MIN filas se pintan las SHOW más recientes como filas
// completas (varias líneas). Por encima, el DOM no puede con miles de filas de altura
// variable: se pasa a filas compactas de altura fija (ROW_H) y solo se crean las que
// caben en la ventana visible más un margen (índice = scrollTop / ROW_H). El contenedor
// lleva dos separadores (arriba y abajo) con la altura de las filas no creadas, de modo
// que la barra de desplazamiento representa la lista entera.
//
// El HTML trae, con prefijo P: #P-filter, #P-inst, #P-search, #P-pause, #P-clear,
// #P-count, #P-pinned, #P-copy, #P-jsonl, #P-csv, #P-pins, #P-list y #P-detail.
import { S, bus, $, h, fmtHz, fmtTime, toast } from './util.js';
import { fmtVal } from './tables.js';

export const VIRT_MIN = 2000, ROW_H = 26, SHOW = 200;
const EXTRA = ['sample', 'sample_end', 'snr_db', 'rssi_dbfs'];   // campos de etiquetado que el detalle destaca

/** Evento sin los campos privados (_id, _s, _b…) tal como se exporta. */
const clean = (ev) => ({ inst: ev.inst, proto: ev.proto, freq: ev.freq, t: ev.t, msg: ev.msg });

/** CSV (RFC 4180) con las columnas fijas y la unión de los escalares de msg por frecuencia de aparición. */
export function toCsv(list) {
  const cnt = new Map();                                   // todas las claves, por frecuencia de aparición (sin el tope de las tablas)
  for (const e of list) for (const k of Object.keys(e.msg || {})) cnt.set(k, (cnt.get(k) || 0) + 1);
  const order = [...cnt.keys()], cols = order.sort((a, b) => cnt.get(b) - cnt.get(a) || order.indexOf(a) - order.indexOf(b));
  const q = (v) => { const s = v == null ? '' : typeof v === 'object' ? JSON.stringify(v) : String(v); return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s; };
  const rows = [['t', 'proto', 'inst', 'freq_hz', ...cols].map(q).join(',')];
  for (const e of list) rows.push([new Date(e.t * 1000).toISOString(), e.proto, e.inst, e.freq, ...cols.map((k) => (e.msg || {})[k])].map(q).join(','));
  return rows.join('\r\n') + '\r\n';
}
export const toJsonl = (list) => list.map((e) => JSON.stringify(clean(e))).join('\n') + (list.length ? '\n' : '');

function download(name, text, mime) {
  const url = URL.createObjectURL(new Blob([text], { type: mime }));
  const a = h('a', { href: url, download: name });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
}
async function copyText(text) {
  try { await navigator.clipboard.writeText(text); return true; } catch { /* sin permiso: alternativa clásica */ }
  const ta = h('textarea', { style: 'position:fixed;left:-999px' }); ta.value = text; document.body.append(ta); ta.select();
  let ok = false; try { ok = document.execCommand('copy'); } catch { ok = false; }
  ta.remove(); return ok;
}
const stamp = () => new Date().toISOString().replace(/[-:]/g, '').replace(/\..*/, '').replace('T', '-');

/**
 * cfg: prefix ('ev'|'ms'), max (tope del almacén), render(ev) → fila completa (ya con
 * el botón de fijar vía pinBtn), accept(ev) → bool (opcional; puede anotar ev._b),
 * textOf(ev) → texto extra para la búsqueda (opcional), empty (texto de lista vacía),
 * visible() → bool (si la lista está oculta no se repinta al llegar un evento).
 */
export function createEventList(cfg) {
  const P = (s) => $('#' + cfg.prefix + '-' + s);
  const store = [];                 // eventos aceptados, más antiguo primero
  const pinned = new Set();         // ids fijados
  let paused = false, seq = 0, selected = null, virtual = false, onlyPinned = false, vScroll = 0, raf = 0;
  let instFocus = null;             // inst a seleccionar cuando aparezca (alta desde «¿Qué es esta señal?»)

  const lower = (ev) => ev._s || (ev._s = (ev.proto + ' ' + (cfg.textOf ? cfg.textOf(ev) : '') + ' ' + JSON.stringify(ev.msg)).toLowerCase());
  const passes = (ev) => {
    const f = P('filter').value, i = P('inst').value, q = P('search').value.trim().toLowerCase();
    return (!f || ev.proto === f) && (!i || String(ev.inst) === i) && (!onlyPinned || pinned.has(ev._id)) && (!q || lower(ev).includes(q));
  };
  const filtered = () => store.filter(passes);

  /* ---- filas ---- */
  const pinBtn = (ev) => h('button', {
    class: 'pin' + (pinned.has(ev._id) ? ' on' : ''), type: 'button', title: pinned.has(ev._id) ? 'Quitar de fijados' : 'Fijar evento',
    'aria-label': pinned.has(ev._id) ? 'Quitar de fijados' : 'Fijar evento', 'aria-pressed': String(pinned.has(ev._id)), text: pinned.has(ev._id) ? '★' : '☆',
    onclick: (e) => { e.stopPropagation(); togglePin(ev); },
  });
  function full(ev) {
    const row = cfg.render(ev);
    row.classList.add('evrow'); row.dataset.id = ev._id;
    if (pinned.has(ev._id)) row.classList.add('pinned');
    if (selected && selected._id === ev._id) row.classList.add('sel');
    row.tabIndex = 0;
    const hd = row.querySelector('.h'); if (hd) hd.append(pinBtn(ev));
    row.addEventListener('click', (e) => { if (!e.target.closest('button, a')) select(ev); });
    row.addEventListener('keydown', (e) => { if (e.key === 'Enter' && e.target === row) select(ev); });
    return row;
  }
  function compact(ev) {
    const m = ev.msg || {}, d = S.decoders.find((x) => x.id === ev.proto);
    const row = h('div', { class: 'ev compact evrow' + (pinned.has(ev._id) ? ' pinned' : '') + (selected && selected._id === ev._id ? ' sel' : ''), 'data-id': ev._id, tabindex: 0, style: `height:${ROW_H}px` },
      h('span', { class: 't', text: fmtTime(ev.t) }), h('b', { text: d ? d.name : ev.proto }), h('span', { class: 'f', text: '#' + ev.inst }),
      h('span', { class: 'x', text: m.text != null ? m.text : JSON.stringify(m) }), pinBtn(ev));
    row.addEventListener('click', (e) => { if (!e.target.closest('button')) select(ev); });
    row.addEventListener('keydown', (e) => { if (e.key === 'Enter' && e.target === row) select(ev); });
    return row;
  }

  /* ---- pintado ---- */
  function paintVirtual(list) {
    const box = P('list'), top = box.scrollTop, vh = box.clientHeight || 360;
    const rows = list.slice().reverse();                                   // más reciente arriba
    const i0 = Math.max(0, Math.floor(top / ROW_H) - 8), i1 = Math.min(rows.length, Math.ceil((top + vh) / ROW_H) + 8);
    box.replaceChildren(h('div', { class: 'vsp', style: `height:${i0 * ROW_H}px` }), ...rows.slice(i0, i1).map(compact), h('div', { class: 'vsp', style: `height:${(rows.length - i1) * ROW_H}px` }));
    box.scrollTop = top;
  }
  function paintPins() {
    const box = P('pins'); if (!box) return;
    const list = store.filter((e) => pinned.has(e._id)).reverse();
    box.hidden = !list.length;
    box.replaceChildren(...(list.length ? [h('div', { class: 'pinh muted', text: `Fijados (${list.length})` }), ...list.map(full)] : []));
  }
  function paint() {
    raf = 0;
    const box = P('list'), list = filtered();
    P('count').textContent = list.length === store.length ? `${store.length}` : `${list.length} de ${store.length}`;
    virtual = list.length > VIRT_MIN;
    box.classList.toggle('virtual', virtual);
    if (virtual) paintVirtual(list);
    else {
      const rows = list.slice(-SHOW).reverse().map(full);
      box.replaceChildren(...(rows.length ? rows : [h('p', { class: 'muted', text: store.length ? 'Ningún evento cumple los filtros.' : cfg.empty })]));
    }
    paintPins();
  }
  const schedule = () => { if (!raf) raf = requestAnimationFrame(paint); };
  const repaint = () => { if (raf) cancelAnimationFrame(raf); paint(); };

  /* ---- detalle ---- */
  function select(ev) {
    selected = ev;
    const box = P('detail'); box.hidden = false;
    const m = ev.msg || {};
    const chips = EXTRA.filter((k) => m[k] != null).map((k) => h('span', { class: 'chip' }, k + ' ', h('b', { class: 'mono', text: fmtVal(m[k]) })));
    const d = S.decoders.find((x) => x.id === ev.proto);
    box.replaceChildren(
      h('div', { class: 'row wrap' },
        h('b', { text: `${d ? d.name : ev.proto} · #${ev.inst} · ${fmtHz(ev.freq, 4)} · ${new Date(ev.t * 1000).toLocaleString('es-ES', { hour12: false })}` }),
        h('span', { class: 'sp' }),
        h('button', { class: 'btn small', type: 'button', text: 'Copiar JSON', onclick: async () => toast(await copyText(JSON.stringify(clean(ev), null, 2)) ? 'JSON copiado' : 'No se pudo copiar', 'ok') }),
        h('button', { class: 'btn small', type: 'button', text: 'Cerrar', 'aria-label': 'Cerrar el detalle', onclick: () => { selected = null; box.hidden = true; repaint(); } })),
      chips.length ? h('div', { class: 'row wrap evx' }, chips) : null,
      h('pre', { class: 'json', tabindex: 0, 'aria-label': 'JSON del evento', text: JSON.stringify(clean(ev), null, 2) }));
    repaint();
  }
  function togglePin(ev) { if (pinned.has(ev._id)) pinned.delete(ev._id); else pinned.add(ev._id); repaint(); }

  /* ---- llegada de eventos ---- */
  let cRaf = 0;
  const updateCount = () => { cRaf = 0; const n = filtered().length; P('count').textContent = n === store.length ? `${n}` : `${n} de ${store.length}`; };
  function onDec(ev) {
    if (cfg.accept && !cfg.accept(ev)) return;
    ev._id = ++seq; store.push(ev);
    if (store.length > cfg.max) { const i = store.findIndex((e) => !pinned.has(e._id)); if (i >= 0) store.splice(i, 1); }
    if (!instOpts.has(ev.inst)) fillInst();
    if (!cRaf) cRaf = requestAnimationFrame(updateCount);
    if (paused || !cfg.visible() || !passes(ev)) return;
    if (virtual) { const box = P('list'); if (box.scrollTop > 0) box.scrollTop += ROW_H; schedule(); return; }
    const box = P('list');
    if (!box.querySelector('.evrow')) box.replaceChildren();
    box.prepend(full(ev));
    while (box.querySelectorAll(':scope > .evrow').length > SHOW) box.lastElementChild.remove();
    if (store.length > VIRT_MIN && filtered().length > VIRT_MIN) schedule();
  }

  /* ---- selectores ---- */
  const instOpts = new Set();
  function fillFilter() {
    const f = P('filter'), cur = f.value;
    f.replaceChildren(h('option', { value: '', text: 'Todos' }), ...S.decoders.map((d) => h('option', { value: d.id, text: d.name })));
    f.value = cur;
  }
  function fillInst() {
    const sel = P('inst'), cur = sel.value, ids = new Set([...S.decList.map((i) => i.inst), ...store.map((e) => e.inst)]);
    instOpts.clear(); ids.forEach((i) => instOpts.add(i));
    sel.replaceChildren(h('option', { value: '', text: 'Todas' }), ...[...ids].sort((a, b) => a - b).map((i) => {
      const l = S.decList.find((x) => x.inst === i), d = S.decoders.find((x) => x.id === (l ? l.proto : null));
      return h('option', { value: String(i), text: l ? `#${i} ${d ? d.name : l.proto} ${fmtHz(l.freq, 3)}` : `#${i} (quitada)` });
    }));
    sel.value = ids.has(+cur) ? cur : '';
  }

  /* ---- exportación ---- */
  const text = (fmt) => (fmt === 'csv' ? toCsv(filtered()) : toJsonl(filtered()));
  async function doCopy() { const n = filtered().length; toast(n ? (await copyText(toJsonl(filtered())) ? `${n} ${n === 1 ? 'evento copiado' : 'eventos copiados'} (JSONL)` : 'No se pudo copiar') : 'No hay eventos que copiar', n ? 'ok' : ''); }
  function doExport(fmt) {
    const l = filtered(); if (!l.length) { toast('No hay eventos que exportar'); return; }
    const tag = P('filter').value || 'todos';
    download(`eventos_${tag}_${stamp()}.${fmt}`, text(fmt), fmt === 'csv' ? 'text/csv' : 'application/x-ndjson');
    toast(`${l.length} ${l.length === 1 ? 'evento exportado' : 'eventos exportados'} (${fmt.toUpperCase()})`, 'ok');
  }

  function init() {
    for (const k of ['filter', 'inst']) P(k).addEventListener('change', repaint);
    P('search').addEventListener('input', repaint);
    P('pause').addEventListener('change', (e) => { paused = e.target.checked; if (!paused) repaint(); });
    P('clear').addEventListener('click', () => { for (let i = store.length - 1; i >= 0; i--) if (!pinned.has(store[i]._id)) store.splice(i, 1); selected = null; P('detail').hidden = true; fillInst(); repaint(); });
    P('pinned').addEventListener('change', (e) => { onlyPinned = e.target.checked; repaint(); });
    P('copy').addEventListener('click', doCopy);
    P('jsonl').addEventListener('click', () => doExport('jsonl'));
    P('csv').addEventListener('click', () => doExport('csv'));
    P('list').addEventListener('scroll', () => { if (virtual && !raf) { vScroll = P('list').scrollTop; raf = requestAnimationFrame(() => { paint(); }); } }, { passive: true });
    P('list').addEventListener('click', (e) => {      // clic en un valor = filtrar por él
      const v = e.target.closest('.kvv'); if (!v) return;
      P('search').value = v.dataset.v; repaint();
    });
    bus.on('msg:decoders', () => { fillFilter(); repaint(); });
    bus.on('msg:dec_list', () => {
      fillInst();
      if (instFocus) {
        const n = S.decList.filter((i) => i.proto === instFocus.proto && !instFocus.before.has(i.inst)).sort((a, b) => b.inst - a.inst)[0];
        if (n) { P('inst').value = String(n.inst); P('filter').value = ''; instFocus = null; repaint(); }
      }
    });
    bus.on('msg:dec', onDec);
    bus.on('tab', () => { if (cfg.visible()) { fillInst(); paint(); } });
    bus.on(cfg.prefix + '-focus', (f) => { instFocus = f; P('search').value = ''; P('filter').value = ''; P('inst').value = ''; onlyPinned = false; P('pinned').checked = false; });
    fillFilter(); fillInst(); paint();
    void vScroll;
  }
  return { init, text, count: () => filtered().length, total: () => store.length, pin: togglePin, store };
}
