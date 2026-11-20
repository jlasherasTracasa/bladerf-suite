// palette.js — paleta de órdenes (Ctrl+K): búsqueda difusa sobre acciones de la interfaz.
//
// Las acciones se construyen al abrir (dependen del estado: perfiles y decodificadores que ha enviado el
// servidor, pestañas visibles, grabación en curso…). Cada una es {label, hint, run}. La búsqueda es
// difusa e insensible a mayúsculas y tildes: cada palabra de la consulta debe aparecer como subsecuencia
// del texto de la acción («perf mar» encuentra «Perfil: VHF marina»). Puntuación: +25 (+15 al inicio de palabra) si la palabra aparece literal; +10 al inicio del texto,
// +8 al inicio de palabra, +5 por letra contigua, −1 por hueco; gana la mayor.
//
// Además, si lo escrito es una frecuencia («100.3m», «1090 MHz», «162k», «ir a 433,92») se ofrece
// «Ir a …» en primer lugar, con parseHz() (el mismo analizador del campo de frecuencia).
// Teclado: ↑/↓ mueven la selección, Intro ejecuta, Esc cierra (lo hace el propio <dialog>).
import { S, $, $$, h, fmtHz, parseHz, toast } from './util.js';
import { setChannel, MODES, toggleListen } from './receiver.js';
import { zoomReset, zoomToChan, zoomBy } from './spectrum.js';
import { toggleRecording } from './hardware.js';
import { addDecoder, snapFreq } from './decoders.js';

const norm = (t) => String(t).normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();

/** Puntuación de un texto frente a una consulta (−1 si no coincide). */
export function fuzzyScore(query, text) {
  const t = norm(text), toks = norm(query).split(/\s+/).filter(Boolean);
  let total = 0;
  for (const tok of toks) {
    let ti = 0, sc = 0, prev = -2;
    const sub = t.indexOf(tok);       // una subcadena literal vale mucho más que letras sueltas («ais» → AIS, no DAB)
    if (sub >= 0) sc += 25 + (sub === 0 || /[\s:·\-/(]/.test(t[sub - 1]) ? 15 : 0);
    for (const ch of tok) {
      const k = t.indexOf(ch, ti);
      if (k < 0) return -1;
      if (k === 0) sc += 10; else if (/[\s:·\-/(]/.test(t[k - 1])) sc += 8;
      if (k === prev + 1) sc += 5; else sc -= Math.min(3, k - ti);
      prev = k; ti = k + 1;
    }
    total += sc;
  }
  return total;
}

let dlg, input, list, items = [], sel = 0;

function actions() {
  const A = [];
  const add = (label, hint, run, kw = '') => A.push({ label, hint, run, kw });
  for (const t of $$('[role=tab]')) add(`Ir a: ${t.getAttribute('aria-label') || t.textContent}`, 'pestaña', () => t.click(), 'pestana panel');
  for (const p of S.profiles || []) add(`Perfil: ${p.name}`, `${fmtHz(p.center, 3)} · ${p.group || ''}`, () => { const b = document.querySelector(`.pcard[data-id="${p.id}"]`); if (b) b.click(); });
  for (const [m, t] of MODES) add(`Modo ${t}`, S.chan.mode === m ? 'actual' : 'modo de demodulación', () => setChannel({ mode: m }, { user: true }));
  for (const d of S.decoders || []) add(`Añadir decodificador: ${d.name}`, d.group || '', () => { addDecoder(d.id, snapFreq(d, S.chan.freq)); toast(`Añadido ${d.name}`, 'ok'); }, d.id + ' ' + (d.desc || ''));
  add(S.chan.audio ? 'Silenciar audio' : 'Escuchar (activar audio)', 'Espacio', () => toggleListen(), 'sonido');
  add(S.recording && S.recording.on ? 'Parar grabación IQ' : 'Grabar IQ', 'R', () => toggleRecording());
  add('Squelch: activar / desactivar', 'S', () => setChannel({ squelch: S.chan.squelch != null ? null : (+$('#rx-sql').value || -90) }), 'silenciador');
  add('Vista completa del espectro', 'F · Inicio', () => zoomReset());
  add('Acercar al canal', '', () => zoomToChan());
  add('Acercar el espectro', '+', () => zoomBy(1.6));
  add('Alejar el espectro', '−', () => zoomBy(1 / 1.6));
  add('Editar la frecuencia', 'G', () => { $('#freq-input').focus(); });
  add('Atajos de teclado', '?', () => $('#help-dlg').showModal(), 'ayuda');
  return A;
}

function matches(q) {
  const out = [];
  const m = q.trim().replace(/^(ir( a)?|f|freq|frecuencia)\s+(?=\d)/i, '');
  if (/\d/.test(m)) {
    const v = parseHz(m);
    if (v != null && v >= 1000) out.push({ label: `Ir a ${fmtHz(v, 4)}`, hint: 'sintonizar', run: () => setChannel({ freq: v }, { user: true }), score: 1e6 });
  }
  for (const a of actions()) {
    const s = q.trim() ? fuzzyScore(q, a.label + ' ' + a.kw) : 0;
    if (s >= 0) out.push({ ...a, score: s });
  }
  if (q.trim()) out.sort((a, b) => b.score - a.score);
  return out.slice(0, 40);
}

function render() {
  list.replaceChildren(...items.map((a, i) => h('li', { role: 'option', id: 'pal-' + i, 'aria-selected': String(i === sel), class: i === sel ? 'sel' : '', onclick: () => run(i), onmousemove: () => { if (sel !== i) { sel = i; mark(); } } },
    h('span', { text: a.label }), a.hint ? h('small', { text: a.hint }) : null)));
  if (!items.length) list.append(h('li', { class: 'none', text: 'Nada coincide. Prueba «100.3m» para sintonizar o «perfil»…' }));
  input.setAttribute('aria-activedescendant', items.length ? 'pal-' + sel : '');
}
function mark() {
  [...list.children].forEach((li, i) => { li.setAttribute('aria-selected', String(i === sel)); li.classList.toggle('sel', i === sel); });
  const li = list.children[sel]; if (li && li.scrollIntoView) li.scrollIntoView({ block: 'nearest' });
  input.setAttribute('aria-activedescendant', items.length ? 'pal-' + sel : '');
}
function run(i) { const a = items[i]; if (!a) return; dlg.close(); a.run(); }

export function openPalette() {
  if (dlg.open) return;
  input.value = ''; items = matches(''); sel = 0; render();
  dlg.showModal(); input.focus();
}
export const paletteOpen = () => !!dlg && dlg.open;

export function initPalette() {
  dlg = $('#palette'); input = $('#pal-input'); list = $('#pal-list');
  input.addEventListener('input', () => { items = matches(input.value); sel = 0; render(); });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { sel = items.length ? (sel + 1) % items.length : 0; mark(); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { sel = items.length ? (sel - 1 + items.length) % items.length : 0; mark(); e.preventDefault(); }
    else if (e.key === 'Enter') { run(sel); e.preventDefault(); }
  });
  dlg.addEventListener('click', (e) => { if (e.target === dlg) dlg.close(); });
  $('#btn-palette').addEventListener('click', openPalette);
}
