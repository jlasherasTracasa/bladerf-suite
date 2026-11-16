// receiver.js — canal de audio de este navegador: frecuencia, modo, anchura,
// squelch, deénfasis, estéreo y S-meter. Es el dueño de S.chan; el resto de
// módulos lo cambian con setChannel().
import { S, bus, $, h, clamp, fmtHz, parseHz, throttle, editing, dispOff, dispUnit } from './util.js';
import { send } from './ws.js';
import { toast } from './util.js';
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
let pendingCenter = null, pendingT = 0;   // retuneo en curso: adopt() no debe deshacerlo

/** Si el canal (con su filtro) cae fuera de la banda capturada, retunea el hardware: centro con el canal a +rate/4. */
function retuneIfNeeded() {
  const st = S.status, c = S.chan;
  if (!st || !st.rate || st.center == null) return true;
  const [elo, ehi] = filterEdges(c), half = 0.45 * st.rate;   // 0,45·rate: margen por el filtro anti-alias
  if (elo >= st.center - half && ehi <= st.center + half) return true;
  const d = st.device || {};
  if (d.can_tune === false) { toast(`${fmtHz(c.freq, 3)} queda fuera de la banda capturada (${fmtHz(st.center - st.rate / 2, 1)} – ${fmtHz(st.center + st.rate / 2, 1)}) y una fuente de fichero no se puede sintonizar`, 'err'); return false; }
  let off = st.rate / 4;
  const edge = Math.max(Math.abs(c.freq - elo), Math.abs(ehi - c.freq));
  if (off + edge > half) off = Math.max(0, half - edge);
  let C = Math.round(c.freq - off);
  if (d.freq_min != null) C = Math.max(d.freq_min, C);
  if (d.freq_max != null) C = Math.min(d.freq_max, C);
  pendingCenter = C; pendingT = performance.now();
  send({ cmd: 'config', center: C });
  return true;
}

export function setChannel(patch, opts = {}) {
  const c = S.chan, oldMode = c.mode, oldFreq = c.freq;
  Object.assign(c, patch);
  if (patch.mode && patch.mode !== oldMode && patch.bw == null) c.bw = BW_DEF[c.mode];
  if (patch.mode && patch.mode !== oldMode) {
    step = STEP_DEF[c.mode];
    if (c.mode === 'wfm') c.stereo = true;
  }
  c.bw = clamp(Math.round(c.bw), BW_MIN, BW_MAX[c.mode]);
  c.freq = Math.round(c.freq);
  // Si no se puede retunear el hardware (fichero), el canal no se mueve: la frecuencia vuelve a la anterior.
  if (opts.user && (patch.freq != null || patch.mode != null) && !retuneIfNeeded() && patch.freq != null) c.freq = oldFreq;
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
    if (v != null && v > 0) setChannel({ freq: v }, { user: true });
    else { if (fi.value.trim()) toast('Frecuencia no válida: prueba «100.4», «433,92 MHz» o «162k»', 'err'); render(); }
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
  // Estado inicial sensato: si el perfil activo no fija frecuencia de audio y la persona aún no ha tocado nada,
  // el canal se coloca sobre la señal detectada más fuerte (la emisora, no un 100,3 MHz fijo).
  let autoDone = false, touched = false;
  bus.on('chan', ({ user }) => { if (user) touched = true; });
  bus.on('ws', (up) => { if (up) { autoDone = false; } });
  bus.on('msg:signals', (m) => {
    if (autoDone || touched || !m.list || !m.list.length || !S.status || S.status.sweep) return;
    const p = S.status.profile && S.profiles.find((x) => x.id === S.status.profile);
    if (p && p.chan_freq && p.mode) { autoDone = true; return; }
    const edge = 0.10 * S.status.rate, inner = m.list.filter((s) => Math.abs(m.center + s[0] - S.status.center) < S.status.rate / 2 - edge);   // sin falsos positivos de los bordes
    if (!inner.length) return;
    const best = inner.reduce((a, b) => (b[3] > a[3] ? b : a));
    const f = m.center + best[0], wide = best[1] >= 100000, g = wide ? 100000 : 5000;
    const lo = S.status.center - S.status.rate / 2, hi = S.status.center + S.status.rate / 2, fr = Math.round(f / g) * g;
    autoDone = true;
    if (fr < lo || fr > hi || best[3] < 6) return;
    setChannel(wide ? { freq: fr, mode: 'wfm' } : { freq: fr });
    toast(`Sintonizado en la señal más fuerte: ${fmtHz(fr, 3)}`, 'ok');
  });
  // Reenvía el canal al reconectar (el servidor crea un canal nuevo por conexión)
  bus.on('ws', (up) => { if (up) { sendChan(); } });
  // Adopción de banda: tras conectar, y cada vez que cambia el centro o la tasa (perfil, config de
  // otro cliente…), si la frecuencia de escucha ha quedado fuera de la banda capturada se mueve a la
  // frecuencia de audio del perfil activo o, si no la tiene (ADS-B, DAB…), al centro. Si está dentro se respeta.
  let lastBand = '';
  const adopt = (m, first) => {
    if (pendingCenter != null) {
      if (m.center === pendingCenter || performance.now() - pendingT > 5000 || (S.chan.freq >= m.center - m.rate / 2 && S.chan.freq <= m.center + m.rate / 2)) pendingCenter = null;
      else return;   // status anterior al retuneo: no mover el canal
    }
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
