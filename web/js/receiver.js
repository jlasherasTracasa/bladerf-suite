// receiver.js — canal de audio de este navegador: frecuencia, modo, anchura,
// squelch, deénfasis, estéreo y S-meter. Es el dueño de S.chan; el resto de
// módulos lo cambian con setChannel().
import { S, bus, $, h, clamp, fmtHz, parseHz, throttle, editing, dispOff, dispUnit } from './util.js';
import { send } from './ws.js';
import { startAudio, stopAudio, audioOn } from './audio.js';

export const MODES = [['wfm', 'WFM'], ['nfm', 'NFM'], ['am', 'AM'], ['usb', 'USB'], ['lsb', 'LSB'], ['cw', 'CW']];
// Anchuras por defecto y máximas del filtro de canal (el servidor las recorta si hace falta).
const BW_DEF = { wfm: 180000, nfm: 12500, am: 9000, usb: 2700, lsb: 2700, cw: 500 };
export const BW_MAX = { wfm: 220000, nfm: 40000, am: 20000, usb: 6000, lsb: 6000, cw: 1500 };
export const BW_MIN = 100;
const STEP_DEF = { wfm: 100000, nfm: 12500, am: 5000, usb: 100, lsb: 100, cw: 100 };
const STEPS = [10, 100, 500, 1000, 5000, 6250, 8330, 9000, 10000, 12500, 25000, 50000, 100000, 200000];
let step = STEP_DEF.wfm;
export const getStep = () => step;

/** Límites [lo, hi] absolutos (Hz) del filtro según el modo (USB hacia arriba, LSB hacia abajo). */
export function filterEdges(c = S.chan) {
  if (c.mode === 'usb') return [c.freq, c.freq + c.bw];
  if (c.mode === 'lsb') return [c.freq - c.bw, c.freq];
  return [c.freq - c.bw / 2, c.freq + c.bw / 2];
}

const sendChan = throttle(() => {
  const c = S.chan;
  send({ cmd: 'channel', freq: Math.round(c.freq), mode: c.mode, bw: Math.round(c.bw), squelch: c.squelch,
    deemph: c.deemph, stereo: c.stereo, audio: c.audio });
}, 60);

/** Cambia parámetros del canal. opts.user=true marca que viene de una acción del usuario (activa "follow"). */
export function setChannel(patch, opts = {}) {
  const c = S.chan, oldMode = c.mode;
  Object.assign(c, patch);
  if (patch.mode && patch.mode !== oldMode && patch.bw == null) c.bw = BW_DEF[c.mode];
  if (patch.mode && patch.mode !== oldMode) {
    step = STEP_DEF[c.mode];
    if (c.mode === 'wfm') c.stereo = true;
  }
  c.bw = clamp(Math.round(c.bw), BW_MIN, BW_MAX[c.mode]);
  c.freq = Math.round(c.freq);
  sendChan();
  render();
  bus.emit('chan', { user: !!opts.user, patch });
}

function render() {
  const c = S.chan;
  const fi = $('#freq-input');
  if (!editing(fi)) fi.value = fmtHz(c.freq, 3);
  $('#mode-seg').querySelectorAll('button').forEach((b) => b.setAttribute('aria-checked', b.dataset.m === c.mode));
  const bw = $('#rx-bw'); if (!editing(bw)) bw.value = c.bw;
  bw.max = BW_MAX[c.mode];
  $('#rx-step').value = String(step);
  $('#rx-deemph').value = String(c.deemph);
  $('#rx-stereo').checked = c.stereo;
  $('#rx-stereo').disabled = c.mode !== 'wfm';
  const on = c.squelch != null;
  $('#rx-sql-on').checked = on;
  $('#rx-sql').disabled = !on;
  if (on) $('#rx-sql').value = c.squelch;
  $('#rx-sql-o').textContent = on ? c.squelch + ' dBFS' : 'apagado';
  const lb = $('#btn-listen');
  lb.setAttribute('aria-pressed', c.audio);
  lb.querySelector('span').textContent = c.audio ? 'Silenciar' : 'Escuchar';
}

export function tuneBy(hz) { setChannel({ freq: S.chan.freq + hz }, { user: true }); }

let peak = -120, peakT = 0;
function onLevel(buf) {
  const dv = new DataView(buf);
  S.level = { db: dv.getFloat32(4, true), flags: dv.getUint8(1) };
}
function paintMeter() {
  const { db, flags } = S.level, now = performance.now();
  const pos = clamp((db + 120) / 120, 0, 1);
  if (pos >= peak || now - peakT > 1500) { peak = Math.max(pos, peak - 0.01); if (pos >= peak) peakT = now; }
  $('#sm-fill').style.width = pos * 100 + '%';
  $('#sm-peak').style.left = `calc(${peak * 100}% - 1px)`;
  { const off = dispOff(); $('#sm-txt').textContent = isFinite(db) ? (db + off).toFixed(1) + ' ' + dispUnit() : '—'; }
  const set = (id, b) => $(id).classList.toggle('on', !!b);
  set('#fl-sql', flags & 1); set('#fl-st', flags & 2); set('#fl-oob', flags & 4); set('#fl-pil', flags & 8);
  requestAnimationFrame(paintMeter);
}

async function toggleListen() {
  if (S.chan.audio && audioOn()) { stopAudio(); setChannel({ audio: false }); return; }
  const ok = await startAudio();
  setChannel({ audio: ok });
}
export { toggleListen };

export function initReceiver() {
  const seg = $('#mode-seg');
  for (const [m, t] of MODES)
    seg.append(h('button', { type: 'button', role: 'radio', 'data-m': m, 'aria-checked': 'false', text: t, onclick: () => setChannel({ mode: m }, { user: true }) }));
  $('#rx-step').replaceChildren(...STEPS.map((s) => h('option', { value: s, text: fmtHz(s) })));
  $('#rx-step').addEventListener('change', (e) => { step = +e.target.value; });

  const fi = $('#freq-input');
  fi.addEventListener('keydown', (e) => { if (e.key === 'Enter') fi.blur(); if (e.key === 'Escape') { render(); fi.blur(); } });
  fi.addEventListener('focus', () => fi.select());
  fi.addEventListener('blur', () => {
    const v = parseHz(fi.value);
    if (v != null && v > 0) setChannel({ freq: v }, { user: true }); else render();
  });
  fi.addEventListener('wheel', (e) => { e.preventDefault(); tuneBy(Math.sign(-e.deltaY) * step); }, { passive: false });

  $('#rx-bw').addEventListener('change', (e) => setChannel({ bw: +e.target.value }));
  $('#rx-deemph').addEventListener('change', (e) => setChannel({ deemph: +e.target.value }));
  $('#rx-stereo').addEventListener('change', (e) => setChannel({ stereo: e.target.checked }));
  $('#rx-sql-on').addEventListener('change', (e) => setChannel({ squelch: e.target.checked ? +$('#rx-sql').value || -90 : null }));
  $('#rx-sql').addEventListener('input', (e) => setChannel({ squelch: +e.target.value }));
  $('#btn-listen').addEventListener('click', toggleListen);
  bus.on('audio:error', () => { import('./util.js').then((u) => u.toast('No se pudo iniciar el audio', 'err')); });

  bus.on('bin:audio', onLevel);
  // Reenvía el canal al reconectar (el servidor crea un canal nuevo por conexión)
  bus.on('ws', (up) => { if (up) { sendChan(); } });
  // Adopción de banda: tras conectar, y cada vez que cambia el centro o la tasa (perfil, config de
  // otro cliente…), si la frecuencia de escucha ha quedado fuera de la banda capturada se mueve a la
  // frecuencia de audio del perfil activo o, si no la tiene (ADS-B, DAB…), al centro. Si está dentro se respeta.
  let lastBand = '';
  const adopt = (m, first) => {
    const lo = m.center - m.rate / 2, hi = m.center + m.rate / 2;
    if (S.chan.freq >= lo && S.chan.freq <= hi) { if (first) sendChan(); return; }
    const p = m.profile && S.profiles.find((x) => x.id === m.profile);
    if (p && p.chan_freq && p.mode) setChannel({ freq: p.chan_freq, mode: p.mode, ...(p.bw ? { bw: p.bw } : {}) });
    else setChannel({ freq: Math.round(m.center / 1000) * 1000 });
  };
  bus.on('ws', (up) => { if (!up) lastBand = ''; });
  bus.on('msg:status', (m) => {
    const key = m.center + '/' + m.rate;
    if (key === lastBand) return;
    const first = lastBand === '';
    lastBand = key;
    adopt(m, first);
  });
  // «profiles» llega después del primer «status»: si el canal seguía fuera de banda, reintentar con el perfil ya conocido.
  bus.on('msg:profiles', () => { if (S.status) adopt(S.status, false); });
  render();
  requestAnimationFrame(paintMeter);
}
