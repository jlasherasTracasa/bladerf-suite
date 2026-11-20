// decoders.js — instancias de decodificadores, registro de eventos y vista
// genérica de "dec_state"; menú «decodificar como…» sobre señales detectadas.
import { S, bus, $, h, fmtHz, parseHz, fmtTime, fillSelect, toast, editing } from './util.js';
import { send } from './ws.js';
import { setChannel } from './receiver.js';
import { stateView } from './tables.js';
import { createEventList } from './evlist.js';

const msps = (r) => `${+(r / 1e6).toFixed(3)} Msps`;   // tasas de muestreo en Msps (no «MHz»)

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
  onProtoChange();
}
function onProtoChange() {
  const d = S.decoders.find((x) => x.id === $('#da-proto').value);
  if (!d) return;
  const wide = d.kind === 'wide';
  $('#da-freq').disabled = wide;
  $('#da-freq').value = wide ? '' : fmtHz((d.freqs && d.freqs[0]) || S.chan.freq, 6);
  const rate = d.kind === 'wide' ? (d.wide_rate ? `exige ${msps(d.wide_rate)} de tasa` : `tasa mínima ${msps(d.wide_min_rate)}`) : `canal de ${fmtHz(d.bw)} a ${msps(d.rate)}`;
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
  // Cada valor es un botón de filtro: un clic lo copia a la búsqueda (MMSI, indicativo, ICAO…).
  const kv = Object.entries(m).filter(([k]) => k !== 'text').map(([k, v]) => h('span', {}, k + ': ', h('b', { class: typeof v === 'object' ? '' : 'kvv', 'data-v': typeof v === 'object' ? null : String(v), title: typeof v === 'object' ? null : 'Filtrar por este valor', text: short(v) })));
  const kindTxt = KIND[m.kind] || m.kind;                       // ADS-B: new|callsign|squawk|emerg
  return h('div', { class: 'ev' + (ev.replay ? ' replay' : '') },
    h('div', { class: 'h' }, h('span', { text: fmtTime(ev.t) }), h('b', { text: d ? d.name : ev.proto }), m.kind ? h('span', { class: 'kind ' + m.kind, text: kindTxt }) : null, h('span', { text: fmtHz(ev.freq, 4) }), h('span', { text: '#' + ev.inst })),
    m.text != null ? h('div', { class: 'txt', text: m.text }) : null,
    kv.length ? h('div', { class: 'kvs' }, kv) : null);
}
export const evList = createEventList({ prefix: 'ev', max: 5000, render: renderEvent, empty: 'Sin eventos todavía. Añade un decodificador y espera al primer mensaje.', visible: () => decVisible() });

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

/* Rejilla de canalización según la banda: 25 kHz en aviación y marina, 100 kHz en FM comercial, 12,5 kHz en el resto de estrechas. */
function gridFor(d, f) {
  if ((f >= 118e6 && f <= 137e6) || (f >= 156e6 && f <= 163e6)) return 25e3;
  if (f >= 87.5e6 && f <= 108e6 && d.bw >= 100e3) return 100e3;
  return d.bw > 0 && d.bw <= 25e3 ? 12.5e3 : 0;
}
/** Ajusta f al canal típico del protocolo si está a menos de medio canal; si no, a la rejilla de canalización. */
export function snapFreq(d, f) {
  if (!d || d.kind === 'wide') return Math.round(f);
  const half = Math.max(d.bw || 0, 12500) / 2;
  let best = null;
  for (const t of d.freqs || []) if (t && (best == null || Math.abs(t - f) < Math.abs(best - f))) best = t;
  if (best != null && Math.abs(best - f) < half) return best;
  const g = gridFor(d, f);
  return g ? Math.round(f / g) * g : Math.round(f);
}
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
  bus.on('msg:decoders', fillProtoSelect);
  bus.on('msg:dec_list', () => { renderInstances(); });
  bus.on('msg:dec_state', renderStates);
  bus.on('msg:dec_list', renderStates);
  bus.on('tab', (t) => { if (t === 'decoders') renderStates(); });
  evList.init();
  renderInstances();
}
