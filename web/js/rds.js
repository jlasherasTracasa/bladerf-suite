// rds.js — tarjeta RDS del receptor. Se muestra cuando hay una instancia «rds».
// Campos fijos del estado (AGENTS.md §8): pi, ps, rt, pty, pty_name, tp, ta, ms, af, ct.
// Se completan con los del último evento «dec» de la instancia por si el estado aún no los trae.
import { S, bus, $, h, fmtHz } from './util.js';

const last = {};                      // inst -> campos del último evento
const FIELDS = ['pi', 'ps', 'rt', 'pty', 'pty_name', 'tp', 'ta', 'ms', 'af', 'ct'];

function pick(inst) {
  const st = (S.decState[inst] && S.decState[inst].state) || {}, ev = last[inst] || {}, out = {};
  for (const k of FIELDS) out[k] = st[k] != null && st[k] !== '' ? st[k] : ev[k];
  return out;
}
// AF puede llegar en MHz (98.1), en kHz (98100) o en Hz; se normaliza a MHz para mostrar.
const afMhz = (v) => (typeof v === 'number' ? (v > 1e6 ? v / 1e6 : v > 1e4 ? v / 1e3 : v) : parseFloat(v));
/** Lista de AF en MHz (valores no numéricos descartados). Reutilizada por la tabla de estado de Decod. */
export const afListMhz = (list) => (Array.isArray(list) ? list.map(afMhz).filter((x) => isFinite(x)) : []);
const flag = (on, t, title) => h('span', { class: 'flag' + (on ? ' on' : ''), title, text: t });

function render() {
  const card = $('#rds-card'), insts = S.decList.filter((i) => i.proto === 'rds');
  card.hidden = !insts.length;
  if (!insts.length) return;
  // la instancia más cercana a la frecuencia de escucha
  const i = insts.reduce((a, b) => (Math.abs(b.freq - S.chan.freq) < Math.abs(a.freq - S.chan.freq) ? b : a));
  const d = pick(i.inst), has = FIELDS.some((k) => d[k] != null);
  $('#rds-inst').textContent = `#${i.inst} · ${fmtHz(i.freq, 3)}`;
  const body = $('#rds-body');
  if (!has) { body.replaceChildren(h('p', { class: 'muted', text: i.state === 'ok' ? 'Esperando grupos RDS…' : `Instancia en estado «${i.state}»${i.err ? ': ' + i.err : ''}.` })); return; }
  const af = afListMhz(d.af);
  body.replaceChildren(
    h('div', { class: 'rds-ps', text: d.ps != null ? String(d.ps) : '········' }),
    h('div', { class: 'rds-rt', text: d.rt != null ? String(d.rt) : '' }),
    h('div', { class: 'chips' }, flag(d.tp, 'TP', 'Programa de tráfico'), flag(d.ta, 'TA', 'Anuncio de tráfico en curso'),
      d.ms != null ? h('span', { class: 'mini', title: 'Música / voz', text: typeof d.ms === 'boolean' ? (d.ms ? 'Música' : 'Voz') : String(d.ms) }) : null),
    h('div', { class: 'kv' },
      h('span', { class: 'k', text: 'PI' }), h('span', { class: 'v mono', text: d.pi != null ? String(d.pi) : '—' }),
      h('span', { class: 'k', text: 'PTY' }), h('span', { class: 'v', text: d.pty_name || (d.pty != null ? String(d.pty) : '—') }),
      h('span', { class: 'k', text: 'Hora (CT)' }), h('span', { class: 'v mono', text: d.ct != null ? String(d.ct) : '—' }),
      h('span', { class: 'k', text: 'AF' }), h('span', { class: 'v mono', text: af.length ? af.map((x) => +x.toFixed(2)).join(' · ') + ' MHz' : '—' })));
}

export function initRds() {
  bus.on('msg:dec_state', render);
  bus.on('msg:dec_list', render);
  bus.on('msg:dec', (ev) => { if (ev.proto === 'rds' && ev.msg) { last[ev.inst] = { ...(last[ev.inst] || {}), ...ev.msg }; render(); } });
  bus.on('chan', render);
  render();
}
