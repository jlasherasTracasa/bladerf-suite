// protocols.js — hoja «Protocolos»: perfiles en tarjetas agrupadas (estilo del
// plugin material de legacy/) y catálogo de decodificadores para añadir instancias.
import { S, bus, $, h, fmtHz, throttle, toast } from './util.js';
import { send } from './ws.js';
import { setChannel } from './receiver.js';
import { startAudio, audioOn } from './audio.js';
import { addDecoder } from './decoders.js';

const groupBy = (arr, key) => { const m = new Map(); for (const x of arr) { const g = x[key] || 'Otros'; if (!m.has(g)) m.set(g, []); m.get(g).push(x); } return m; };
const mf = (f) => String(+(f / 1e6).toFixed(4));
const MODE = { wfm: 'WFM', nfm: 'NFM', am: 'AM', usb: 'USB', lsb: 'LSB', cw: 'CW' };
const mhz = (r) => (r / 1e6).toFixed(r % 1e5 ? 3 : 1).replace(/\.0$/, '') + ' Msps';

// Un aviso informativo por sesión cuando la fuente es un fichero y su centro no coincide con el del perfil.
let fileNoticeShown = false;
// Perfil con decodificador «follow» pendiente de sintonizar a la emisora detectada más fuerte.
let pendingFollow = null;
const FOLLOW_SPAN = 300e3;     // busca la señal a ±300 kHz del canal que propone el perfil
const FOLLOW_WAIT_MS = 5000;   // si no llegan señales a tiempo, se deja la frecuencia del perfil

/** Señal más fuerte (por SNR) a ±FOLLOW_SPAN de f0, o null. Solo si la lista de señales es de la banda del perfil. */
function strongestNear(p, f0) {
  const sg = S.signals;
  if (!sg || !sg.list || !sg.list.length || Math.abs(sg.center - p.center) > 1000) return null;
  let best = null;
  for (const [off, bw, , snr] of sg.list) {
    const f = sg.center + off;
    if (Math.abs(f - f0) <= FOLLOW_SPAN && snr >= 6 && (!best || snr > best.snr)) best = { f, bw, snr };
  }
  return best;
}
function tryFollow() {
  const pf = pendingFollow;
  if (!pf) return;
  const timeout = performance.now() - pf.t > FOLLOW_WAIT_MS;
  const best = strongestNear(pf.p, pf.p.chan_freq);
  if (!best && !timeout) return;
  pendingFollow = null;
  if (!best) return;
  const g = best.bw >= 100e3 ? 100e3 : 5e3, fr = Math.round(best.f / g) * g;
  setChannel({ freq: fr }, { user: true });   // user:true → «follow» mueve el RDS a la emisora
  toast(`Sintonizado en la señal más fuerte cerca de ${fmtHz(pf.p.chan_freq, 3)}: ${fmtHz(fr, 3)}`, 'ok');
}

// Arranca el audio con el gesto del clic del perfil. Si el navegador lo bloquea (autoplay), un único aviso claro.
let audioNoticeShown = false;
async function autoListen() {
  if (S.chan.audio && audioOn()) return;
  let ok = false;
  try { ok = await Promise.race([startAudio(), new Promise((r) => setTimeout(() => r(false), 2500))]); } catch { ok = false; }
  setChannel({ audio: !!ok });
  if (!ok && !audioNoticeShown) { audioNoticeShown = true; toast('El navegador no permite iniciar el audio solo: pulsa «Escuchar».'); }
}

function applyProfile(p) {
  if (!send({ cmd: 'profile', id: p.id })) { toast('Sin conexión con el servidor', 'err'); return; }
  const lo = p.center - p.rate / 2, hi = p.center + p.rate / 2;
  const st = S.status;
  const fileSrc = !!(st && st.device && st.device.can_tune === false);
  if (fileSrc) {
    // Fuente de fichero: el centro y la tasa no cambian, así que el canal de audio no se toca si el perfil cae fuera de la banda.
    const inBand = p.chan_freq && p.chan_freq >= st.center - st.rate / 2 && p.chan_freq <= st.center + st.rate / 2;
    if (inBand && p.mode) setChannel({ freq: p.chan_freq, mode: p.mode, ...(p.bw ? { bw: p.bw } : {}) });
    else setChannel({});   // el servidor mueve el canal a la frecuencia del perfil aunque quede fuera de banda (src/profiles.c:121): se reenvía el de la web para que ambos coincidan
    const same = Math.abs(st.center - p.center) < 1 && st.rate === p.rate;
    if (!same && !fileNoticeShown) {
      fileNoticeShown = true;
      toast(`Fuente de fichero: el centro queda en ${fmtHz(st.center, 3)}; solo se aplican los decodificadores del perfil «${p.name}» (los que caen fuera de la banda quedan «esperando»).`);
    } else if (same) toast(`Perfil «${p.name}» aplicado`, 'ok');
  } else {
    if (p.chan_freq && p.mode) setChannel({ freq: p.chan_freq, mode: p.mode, ...(p.bw ? { bw: p.bw } : {}) });
    else if (S.chan.freq < lo || S.chan.freq > hi) setChannel({ freq: Math.round(p.center / 1000) * 1000 });   // perfil sin audio: el centro
    toast(`Perfil «${p.name}» aplicado`, 'ok');
  }
  if (p.decoders && p.decoders.some((d) => d.follow) && p.chan_freq) {
    pendingFollow = { p, t: performance.now() };
    autoListen();
    tryFollow();
  }
}

function renderProfiles() {
  const box = $('#prof-groups');
  if (!S.profiles.length) { box.replaceChildren(h('p', { class: 'muted', text: 'El servidor no ha enviado perfiles.' })); return; }
  box.replaceChildren(...[...groupBy(S.profiles, 'group')].flatMap(([g, ps]) => [
    h('div', { class: 'group-h', text: g }),
    h('div', { class: 'cards' }, ps.map((p) => h('button', { class: 'pcard', type: 'button', 'data-id': p.id, 'aria-pressed': 'false', onclick: () => applyProfile(p) },
      h('span', { class: 't', text: p.name }),
      h('span', { class: 's mono', text: `${fmtHz(p.center, 3)} · ${mhz(p.rate)}` }),
      p.mode && p.chan_freq ? h('span', { class: 'd', text: `Audio ${MODE[p.mode] || p.mode} ${fmtHz(p.chan_freq, 4)}` }) : h('span', { class: 'd', text: p.spectrum_only ? 'Solo espectro' : 'Sin audio' }),
      p.decoders && p.decoders.length ? h('span', { class: 'chips' }, p.decoders.map((d) => h('span', { class: 'mini', title: d.follow ? 'Sigue a la frecuencia de audio' : d.auto ? 'Automático sobre señales detectadas' : '', text: d.proto + (d.auto ? ' (auto)' : d.freq ? '@' + mf(d.freq) : '') }))) : null)))]));
  markActive();
}
function markActive() {
  const id = S.status && S.status.profile;
  document.querySelectorAll('#prof-groups .pcard').forEach((b) => b.setAttribute('aria-pressed', b.dataset.id === id));
}

function renderCatalog() {
  const box = $('#cat-groups');
  if (!S.decoders.length) { box.replaceChildren(h('p', { class: 'muted', text: 'Catálogo vacío.' })); return; }
  box.replaceChildren(...[...groupBy(S.decoders, 'group')].flatMap(([g, ds]) => [
    h('div', { class: 'group-h', text: g }),
    h('div', { class: 'cards' }, ds.map((d) => {
      const wide = d.kind === 'wide';
      return h('div', { class: 'pcard dcard' },
        h('span', { class: 't' }, d.name, ' ', d.tx ? h('span', { class: 'mini', title: d.air_ok ? 'Emisión posible (también por aire si la política lo permite)' : 'Emisión solo a fichero/bucle', text: 'TX' }) : null),
        h('span', { class: 'd', text: d.desc }),
        h('span', { class: 's mono', text: wide ? (d.wide_rate ? `banda ancha · ${mhz(d.wide_rate)} exactos` : `banda ancha · ≥ ${mhz(d.wide_min_rate)}`) : `canal ${fmtHz(d.bw)} · ${fmtHz(d.rate)}` }),
        !wide && d.freqs && d.freqs.length ? h('span', { class: 'chips' }, d.freqs.filter(Boolean).map((f) => h('button', { class: 'mini', type: 'button', title: 'Añadir una instancia en esta frecuencia', text: mf(f), onclick: () => { addDecoder(d.id, f); toast(`Añadido ${d.name}`, 'ok'); } }))) : null,
        h('span', { class: 'chips' },
          h('button', { class: 'btn small', type: 'button', text: wide ? 'Añadir' : 'En frecuencia actual', onclick: () => { addDecoder(d.id, S.chan.freq); toast(`Añadido ${d.name}`, 'ok'); } })));
    }))]));
}

// «follow»: los decodificadores de un perfil con follow:true acompañan a la frecuencia de audio (p. ej. RDS)
const follow = throttle(() => {
  const id = S.status && S.status.profile, p = S.profiles.find((x) => x.id === id);
  if (!p || !p.decoders) return;
  for (const d of p.decoders.filter((x) => x.follow))
    for (const i of S.decList.filter((x) => x.owner === 1 && x.proto === d.proto))
      if (Math.abs(i.freq - S.chan.freq) > 1) send({ cmd: 'dec_tune', inst: i.inst, freq: Math.round(S.chan.freq) });
}, 300);

export function initProtocols() {
  bus.on('msg:profiles', renderProfiles);
  bus.on('msg:decoders', renderCatalog);
  bus.on('msg:status', () => { markActive(); tryFollow(); });
  bus.on('msg:signals', tryFollow);
  bus.on('chan', ({ user, patch }) => { if (user && patch.freq != null) follow(); });
  renderProfiles(); renderCatalog();
}
