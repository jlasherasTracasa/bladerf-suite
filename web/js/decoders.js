// decoders.js — instancias de decodificadores, registro de eventos y vista
// genérica de "dec_state"; menú «decodificar como…» sobre señales detectadas.
import { S, bus, $, h, fmtHz, parseHz, fmtTime, fillSelect, toast, editing } from './util.js';
import { send } from './ws.js';
import { setChannel } from './receiver.js';
import { stateView } from './tables.js';

const MAX_EV = 500, MAX_ROWS = 150;
const events = [];
let evPaused = false;

export function addDecoder(proto, freq, opts) {
  const d = S.decoders.find((x) => x.id === proto);
  const f = d && d.kind === 'wide' && S.status ? S.status.center : freq;
  const m = { cmd: 'dec_add', proto, freq: Math.round(f) };
  if (opts && Object.keys(opts).length) m.opts = opts;
  if (!send(m)) toast('Sin conexión con el servidor', 'err');
}

/* ---------- formulario de alta ---------- */
function fillProtoSelect() {
  const sel = $('#da-proto'), cur = sel.value;
  const groups = new Map();
  for (const d of S.decoders) { if (!groups.has(d.group)) groups.set(d.group, []); groups.get(d.group).push(d); }
  sel.replaceChildren(...[...groups].map(([g, ds]) => h('optgroup', { label: g }, ...ds.map((d) => h('option', { value: d.id, text: d.name })))));
  if (cur && S.decoders.some((d) => d.id === cur)) sel.value = cur;
  const f = $('#ev-filter'), fc = f.value;
  f.replaceChildren(h('option', { value: '', text: 'Todos' }), ...S.decoders.map((d) => h('option', { value: d.id, text: d.name })));
  f.value = fc;
  onProtoChange();
}
function onProtoChange() {
  const d = S.decoders.find((x) => x.id === $('#da-proto').value);
  if (!d) return;
  const wide = d.kind === 'wide';
  $('#da-freq').disabled = wide;
  $('#da-freq').value = wide ? '' : fmtHz((d.freqs && d.freqs[0]) || S.chan.freq, 6);
  const rate = d.kind === 'wide' ? (d.wide_rate ? `exige ${fmtHz(d.wide_rate)} de tasa` : `tasa mínima ${fmtHz(d.wide_min_rate)}`) : `canal de ${fmtHz(d.bw)} a ${fmtHz(d.rate)}`;
  $('#da-hint').textContent = `${d.desc} (${wide ? 'banda ancha' : 'banda estrecha'}, ${rate})`;
}

/* ---------- lista de instancias ---------- */
function renderInstances() {
  const box = $('#di-list');
  $('#di-count').textContent = S.decList.length;
  if (!S.decList.length) { box.replaceChildren(h('p', { class: 'muted', text: 'No hay instancias en marcha.' })); return; }
  box.replaceChildren(...S.decList.map((i) => {
    const d = S.decoders.find((x) => x.id === i.proto);
    return h('div', { class: 'inst' },
      h('div', {}, h('span', { class: 't', text: `#${i.inst} ${d ? d.name : i.proto}` }), ' ', h('span', { class: 'mono', text: fmtHz(i.freq, 4) })),
      h('div', { class: 'acts' },
        h('button', { class: 'btn small', text: 'Oír', title: 'Escuchar esta frecuencia', onclick: () => setChannel({ freq: i.freq }, { user: true }) }),
        h('button', { class: 'btn small', text: 'Mover', title: 'Mover la instancia a la frecuencia de escucha', onclick: () => send({ cmd: 'dec_tune', inst: i.inst, freq: Math.round(S.chan.freq) }) }),
        h('button', { class: 'btn small danger', text: 'Quitar', 'aria-label': 'Quitar instancia ' + i.inst, onclick: () => send({ cmd: 'dec_remove', inst: i.inst }) })),
      h('div', { class: 'm' },
        h('span', { class: 'st ' + i.state, text: { ok: 'activo', waiting: 'esperando', error: 'error' }[i.state] || i.state }),
        h('span', { text: `${i.events} eventos` }),
        h('span', { class: i.drops ? 'st waiting' : '', text: `${i.drops} descartes` }),
        h('span', { title: 'Carga de CPU de la instancia' }, 'carga ', h('span', { class: 'load' }, h('i', { style: `width:${Math.min(100, i.load * 100)}%` })), ` ${(i.load * 100).toFixed(0)}%`),
        h('span', { text: i.owner === 1 ? 'perfil' : 'manual' }),
        i.err ? h('span', { style: 'color:var(--err)', text: i.err }) : null));
  }));
}

/* ---------- eventos ---------- */
const KIND = { new: 'nuevo', callsign: 'indicativo', squawk: 'squawk', emerg: 'emergencia' };
const short = (v) => { const s = typeof v === 'object' ? JSON.stringify(v) : String(v); return s.length > 90 ? s.slice(0, 87) + '…' : s; };

function renderEvent(ev) {
  const m = ev.msg || {}, d = S.decoders.find((x) => x.id === ev.proto);
  const kv = Object.entries(m).filter(([k]) => k !== 'text').map(([k, v]) => h('span', {}, k + ': ', h('b', { text: short(v) })));
  const kindTxt = KIND[m.kind] || m.kind;                       // ADS-B: new|callsign|squawk|emerg
  return h('div', { class: 'ev' + (ev.replay ? ' replay' : '') },
    h('div', { class: 'h' }, h('span', { text: fmtTime(ev.t) }), h('b', { text: d ? d.name : ev.proto }), m.kind ? h('span', { class: 'kind ' + m.kind, text: kindTxt }) : null, h('span', { text: fmtHz(ev.freq, 4) }), h('span', { text: '#' + ev.inst })),
    m.text != null ? h('div', { class: 'txt', text: m.text }) : null,
    kv.length ? h('div', { class: 'kvs' }, kv) : null);
}
const passes = (ev) => {
  const f = $('#ev-filter').value, q = $('#ev-search').value.trim().toLowerCase();
  return (!f || ev.proto === f) && (!q || JSON.stringify(ev.msg).toLowerCase().includes(q));
};
function renderAllEvents() {
  const box = $('#ev-list');
  box.replaceChildren(...events.filter(passes).slice(-MAX_ROWS).reverse().map(renderEvent));
}
function onDec(ev) {
  events.push(ev); if (events.length > MAX_EV) events.shift();
  if (evPaused || !passes(ev)) return;
  if (!decVisible()) return;   // la lista se reconstruye al abrir la pestaña
  const box = $('#ev-list');
  box.prepend(renderEvent(ev));
  while (box.childElementCount > MAX_ROWS) box.lastElementChild.remove();
}

/* ---------- resumen de ADS-B (métricas de la recepción) ---------- */
function adsbSummary(st) {
  const n = (v) => (typeof v === 'number' ? v : '—');
  const chip = (k, v, tip) => h('span', { class: 'chip', title: tip }, k + ' ', h('b', { class: 'mono', text: String(v) }));
  return h('div', { class: 'row wrap' },
    chip('tramas/s', typeof st.fps === 'number' ? st.fps.toFixed(1) : '—', 'Tramas válidas por segundo (ventana de 2 s)'),
    chip('corr. 1 bit', n(st.fix1), 'Tramas corregidas de 1 bit (acumulado)'),
    chip('corr. 2 bits', n(st.fix2), 'Tramas corregidas de 2 bits (acumulado)'),
    chip('por CRC', n(st.crc), 'Recuperadas por el entramado CRC deslizante'),
    chip('rechazadas', n(st.rej), 'Candidatos DF17/18 descartados por CRC incorregible'));
}

/* ---------- vista genérica de dec_state ---------- */
const stateBoxes = new Map();
const decVisible = () => { const p = document.getElementById('p-decoders'); return !p || p.style.display !== 'none'; };
function renderStates() {
  if (!decVisible()) return;   // se repinta al abrir la pestaña
  const box = $('#ds-list');
  const items = Object.values(S.decState);
  if (!items.length) { box.className = 'muted'; box.replaceChildren('Sin datos todavía.'); stateBoxes.clear(); return; }
  if (box.className === 'muted') { box.className = ''; box.replaceChildren(); }
  for (const s of items) {
    let det = stateBoxes.get(s.inst);
    if (!det) { det = h('details', { open: true }, h('summary', {}), h('div', {})); stateBoxes.set(s.inst, det); box.append(det); }
    const d = S.decoders.find((x) => x.id === s.proto);
    det.firstChild.textContent = `#${s.inst} ${d ? d.name : s.proto} · ${fmtTime(s.t)}`;
    det.lastChild.replaceChildren(...(s.proto === 'adsb' ? [adsbSummary(s.state)] : []), ...stateView(s.state, 'dec' + s.inst));
  }
  for (const [k, det] of stateBoxes) if (!S.decState[k]) { det.remove(); stateBoxes.delete(k); }
}

/* ---------- menú «decodificar como…» ---------- */
let menuOpen = false;
function closeMenu() { const m = $('#sig-menu'); m.hidden = true; menuOpen = false; }
function showSignalMenu({ sig, x, y }) {
  const m = $('#sig-menu');
  const sorted = [...S.decoders].sort((a, b) => score(b, sig) - score(a, sig) || a.name.localeCompare(b.name));
  m.replaceChildren(
    h('div', { class: 'mh', text: `${fmtHz(sig.freq, 4)} · ${fmtHz(sig.bw)} de ancho · SNR ${sig.snr.toFixed(0)} dB` }),
    h('button', { role: 'menuitem', text: 'Escuchar aquí', onclick: () => { setChannel({ freq: Math.round(sig.freq / 100) * 100 }, { user: true }); closeMenu(); } }),
    h('div', { class: 'mh', text: 'Decodificar como…' }),
    ...sorted.map((d) => h('button', { role: 'menuitem', onclick: () => { addDecoder(d.id, Math.round(sig.freq)); closeMenu(); toast(`Añadido ${d.name} en ${fmtHz(sig.freq, 4)}`, 'ok'); } },
      d.name, score(d, sig) > 0 ? ' ★' : '', h('span', { class: 'muted', text: ` · ${d.group}${d.kind === 'wide' ? ' (banda ancha)' : ''}` }))));
  m.hidden = false; menuOpen = true;
  const r = m.getBoundingClientRect();
  m.style.left = Math.max(4, Math.min(x, innerWidth - r.width - 4)) + 'px';
  m.style.top = Math.max(4, Math.min(y + 8, innerHeight - r.height - 4)) + 'px';
  m.querySelector('button').focus();
}
// ★ = el filtro del decodificador es de un ancho parecido al de la señal detectada
function score(d, sig) { return d.kind === 'narrow' && d.bw > 0 && sig.bw >= d.bw * 0.25 && sig.bw <= d.bw * 2.5 ? 1 : 0; }

export function initDecoders() {
  $('#da-proto').addEventListener('change', onProtoChange);
  $('#da-add').addEventListener('click', () => {
    const proto = $('#da-proto').value, d = S.decoders.find((x) => x.id === proto);
    if (!d) return;
    const freq = d.kind === 'wide' ? S.status?.center : parseHz($('#da-freq').value);
    if (!freq) { toast('Frecuencia no válida', 'err'); return; }
    let opts = null; const t = $('#da-opts').value.trim();
    if (t) { try { opts = JSON.parse(t); } catch (e) { toast('Opciones JSON no válidas: ' + e.message, 'err'); return; } }
    addDecoder(proto, freq, opts);
  });
  $('#ev-filter').addEventListener('change', renderAllEvents);
  $('#ev-search').addEventListener('input', renderAllEvents);
  $('#ev-pause').addEventListener('change', (e) => { evPaused = e.target.checked; if (!evPaused) renderAllEvents(); });
  $('#ev-clear').addEventListener('click', () => { events.length = 0; renderAllEvents(); });
  bus.on('msg:decoders', fillProtoSelect);
  bus.on('msg:dec_list', () => { renderInstances(); });
  bus.on('msg:dec', onDec);
  bus.on('msg:dec_state', renderStates);
  bus.on('msg:dec_list', renderStates);
  bus.on('sig-menu', showSignalMenu);
  bus.on('tab', (t) => { if (t === 'decoders') { renderStates(); renderAllEvents(); } });
  document.addEventListener('pointerdown', (e) => { if (menuOpen && !e.target.closest('#sig-menu')) closeMenu(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && menuOpen) closeMenu(); });
  window.addEventListener('scroll', () => { if (menuOpen) closeMenu(); }, true);
  renderInstances();
}
