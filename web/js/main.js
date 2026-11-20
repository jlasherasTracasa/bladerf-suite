// main.js — arranque: pestañas, barra superior, atajos de teclado, avisos.
import { S, bus, $, $$, h, toast, fmtHz } from './util.js';
import { connect } from './ws.js';
import { initAudio } from './audio.js';
import { initReceiver, setChannel, tuneBy, getStep, MODES, toggleListen } from './receiver.js';
import { initSpectrum, zoomBy, zoomReset, xToFreq, freqToX, getView } from './spectrum.js';
import { initSweep } from './sweep.js';
import { initHardware, toggleRecording } from './hardware.js';
import { initProtocols } from './protocols.js';
import { initDecoders } from './decoders.js';
import { initMap } from './map.js';
import { initMessages } from './messages.js';
import { initRds } from './rds.js';
import { initTx } from './tx.js';
import { initTv } from './tv.js';
import { initPalette, openPalette } from './palette.js';

/* ---- pestañas ---- */
let curTab = 'receptor';
const visiblePanels = () => $$('.panel').filter((p) => !p.hidden);

function buildTabs() {
  const bar = $('#tabs');
  bar.replaceChildren(...visiblePanels().map((p) => {
    const id = p.id.slice(2);
    return h('button', {
      role: 'tab', id: 'tab-' + id, 'aria-controls': p.id, 'aria-selected': String(id === curTab), tabindex: id === curTab ? '0' : '-1',
      title: `${p.dataset.title} (${p.dataset.key})`, text: p.dataset.short || p.dataset.title, 'aria-label': p.dataset.title, onclick: () => showTab(id, true),
      onkeydown: (e) => {
        const tabs = $$('[role=tab]', bar), i = tabs.indexOf(e.currentTarget);
        const n = e.key === 'ArrowRight' ? i + 1 : e.key === 'ArrowLeft' ? i - 1 : -1;
        if (n >= 0) { const t = tabs[(n + tabs.length) % tabs.length]; t.focus(); showTab(t.id.slice(4), false); e.preventDefault(); }
      },
    });
  }));
}
function showTab(id, user) {
  let target = $('#p-' + id);
  if (!target || target.hidden) { id = 'receptor'; target = $('#p-receptor'); }
  curTab = id;
  for (const p of $$('.panel')) {
    p.style.display = p === target ? '' : 'none';
    p.setAttribute('aria-labelledby', 'tab-' + p.id.slice(2));
  }
  for (const t of $$('[role=tab]')) { const on = t.id === 'tab-' + id; t.setAttribute('aria-selected', on); t.tabIndex = on ? 0 : -1; }
  const act = $('#tab-' + id); if (act && act.scrollIntoView) act.scrollIntoView({ block: 'nearest', inline: 'center' });
  // En pantallas estrechas el panel queda bajo el espectro: llevarlo a la vista (la cabecera fija se compensa con scroll-margin-top).
  if (user && innerWidth < 1000) $('#tabs').scrollIntoView({ block: 'start', behavior: 'smooth' });
  bus.emit('tab', id);
}

/* ---- barra superior ---- */
let lastOver = null, overTimer = 0;
function renderTop() {
  const conn = $('#st-conn');
  conn.replaceChildren(h('i', { class: 'dot' + (S.connected ? '' : ' off') }), h('b', { text: S.connected ? 'Conectado' : 'Sin conexión' }));
  const st = S.status;
  if (!st) return;
  const d = st.device, s = st.stats || {};
  $('#st-dev').textContent = `${d.board || d.kind}${d.serial ? ' · ' + d.serial.slice(-6) : ''}`;
  $('#st-rate').textContent = `${((s.rate || 0) / 1e6).toFixed(2)} Msps`;
  const ld = $('#st-load');
  ld.textContent = `DSP ${((s.load || 0) * 100).toFixed(0)}%`;
  ld.className = 'chip' + (s.load > 0.9 ? ' bad' : s.load > 0.7 ? ' warn' : '');
  $('#st-temp').textContent = s.temp > 0 ? `${s.temp.toFixed(0)} °C` : '— °C';
  $('#st-clients').textContent = `${s.clients ?? '—'} cli.`;
  const ov = $('#st-over');
  ov.textContent = `OVR ${s.overruns ?? 0}`;
  if (lastOver != null && s.overruns > lastOver) {
    ov.className = 'chip bad';
    clearTimeout(overTimer);
    overTimer = setTimeout(() => { ov.className = 'chip'; }, 4000);
  }
  lastOver = s.overruns;
}

/* ---- atajos de teclado ---- */
function shortcuts(e) {
  // Ctrl/⌘+K abre la paleta de órdenes incluso desde un campo de texto
  if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === 'k' || e.key === 'K')) { e.preventDefault(); openPalette(); return; }
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  const t = e.target;
  // nunca con el foco en un campo de texto, selector, diálogo o zona editable
  if (t.closest && t.closest('input:not([type=checkbox]):not([type=range]), textarea, select, dialog, [contenteditable]')) return;
  if (t.matches && t.matches('button, [role=tab], summary, a') && (e.key === ' ' || e.key === 'Enter')) return;
  const free = t.tagName === 'BODY' || (t.classList && t.classList.contains('ov'));
  const step = getStep() * (e.shiftKey ? 10 : 1);
  switch (e.key) {
    case 'ArrowLeft': if (!free) return; tuneBy(-step); break;
    case 'ArrowRight': if (!free) return; tuneBy(step); break;
    case 'ArrowUp': case 'ArrowDown': {
      if (!free) return;
      const v = $('#vol'); v.value = Math.min(1, Math.max(0, +v.value + (e.key === 'ArrowUp' ? 0.05 : -0.05))).toFixed(2);
      v.dispatchEvent(new Event('input', { bubbles: true })); break;
    }
    case ' ': toggleListen(); break;
    case 'm': case 'M': { const i = MODES.findIndex((m) => m[0] === S.chan.mode); setChannel({ mode: MODES[(i + (e.shiftKey ? MODES.length - 1 : 1)) % MODES.length][0] }, { user: true }); break; }
    case 's': case 'S': setChannel({ squelch: S.chan.squelch != null ? null : (+$('#rx-sql').value || -90) }); break;
    case '+': case '=': zoomBy(1.6); break;
    case '-': case '_': zoomBy(1 / 1.6); break;
    case '0': case 'f': case 'F': case 'Home': zoomReset(); break;
    case 'g': case 'G': $('#freq-input').focus(); break;
    case 'r': case 'R': toggleRecording(); break;
    case '?': $('#help-dlg').showModal(); break;
    default: {
      if (/^[1-7]$/.test(e.key)) {
        const p = visiblePanels()[+e.key - 1];
        if (p) showTab(p.id.slice(2), true);   // el foco no cambia: las flechas siguen sintonizando
        break;
      }
      return;
    }
  }
  e.preventDefault();
}

function boot() {
  initAudio(); initReceiver(); initSpectrum(); initSweep(); initHardware(); initProtocols(); initDecoders(); initMap(); initMessages(); initRds(); initTx(); initTv(); initPalette();
  buildTabs(); showTab(decodeURIComponent(location.hash.slice(1)) || 'receptor');
  bus.on('tabs-changed', () => { buildTabs(); showTab(curTab); });
  window.addEventListener('hashchange', () => showTab(decodeURIComponent(location.hash.slice(1)) || 'receptor'));
  $('#btn-help').addEventListener('click', () => $('#help-dlg').showModal());
  document.addEventListener('keydown', shortcuts);
  let everDown = false;
  bus.on('ws', (up) => { renderTop(); if (!up) everDown = true; if (!up || everDown) toast(up ? 'Conectado de nuevo al servidor' : 'Conexión perdida; reintentando…', up ? 'ok' : 'err'); });   // sin aviso en la primera conexión
  bus.on('msg:status', renderTop);
  const setTitle = () => { document.title = `${fmtHz(S.chan.freq, 3)} · BladeRF Lab`; };
  bus.on('chan', setTitle); setTitle();
  bus.on('msg:error', (m) => toast(m.msg || 'Error del servidor', 'err'));
  renderTop();
  connect();
}
// Gancho de solo lectura para las pruebas E2E (tests/web_e2e.mjs) y la consola del navegador.
window.__app = { S, bus, xToFreq, freqToX, getView };
boot();
