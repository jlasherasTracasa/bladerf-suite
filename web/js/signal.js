// signal.js — panel «¿Qué es esta señal?».
//
// Se abre al pulsar un marcador del detector, con clic derecho o con pulsación larga
// sobre el espectro (spectrum.js emite «sig-menu»). Muestra frecuencia, ancho ocupado y
// SNR, un esbozo del espectro local, la forma espectral deducida en el cliente y los
// decodificadores propuestos con su probabilidad, cada uno con «Decodificar».
//
// Probabilidad = suma de tres indicios (máx. ≈ 8,5), normalizada a %:
//   1. Banda (0–4): la frecuencia cae en uno de los canales típicos del catálogo (+4) o
//      dentro de ~3 MHz de ellos (+2); además la tabla de asignaciones BANDAS añade +3 a
//      los protocolos que operan allí (canales marinos, aéreos, ISM, PMR, radioaficionado…).
//   2. Ancho ocupado frente al «bw» del decodificador (0–3): cociente 0,5–1,5 → +3; 0,25–2,5 → +2.
//   3. Forma espectral (0–1,5): portadora estrecha, dos lóbulos (FSK), plano (OFDM/banda
//      ancha) o campana (FM/PSK con filtrado) favorecen a ciertos protocolos.
// La forma sale de los bins del espectro (dBFS) en el ancho ocupado: cresta = pico menos
// media lineal (en dB) y la presencia de dos máximos separados por un valle ≥ 3 dB.
// Un chirp de LoRa (CSS) no se distingue con el espectro de potencia: se ve como un
// bloque plano de 125/250/500 kHz, y así se dice en el panel.
import { S, bus, $, h, fmtHz, toast, clamp } from './util.js';
import { addDecoder, snapFreq } from './decoders.js';
import { setChannel, toggleListen } from './receiver.js';
import { spectrumBins } from './spectrum.js';

/* Tabla de asignaciones: [lo, hi, nombre, modo de escucha, protocolos que operan allí]. */
const BANDAS = [
  [87.5e6, 108e6, 'Radiodifusión FM', 'wfm', ['rds']],
  [118e6, 137e6, 'Banda aérea (AM)', 'am', ['acars', 'vdl2']],
  [136.5e6, 137e6, 'VDL Modo 2', 'am', ['vdl2']],
  [137e6, 138e6, 'Satélites meteorológicos (APT)', 'nfm', []],
  [144e6, 146e6, 'Radioaficionado 2 m', 'nfm', ['aprs']],
  [156e6, 157.45e6, 'VHF marina (canales)', 'nfm', ['dsc']],
  [160.6e6, 162.6e6, 'VHF marina (AIS/canales)', 'nfm', ['ais']],
  [174e6, 240e6, 'TV/DAB banda III', null, ['dab']],
  [400.15e6, 406e6, 'Radiosondas meteorológicas', 'nfm', ['rs41']],
  [430e6, 440e6, 'Radioaficionado 70 cm', 'nfm', ['dmr']],
  [433.05e6, 434.79e6, 'ISM 433 MHz', 'nfm', ['ism']],
  [446e6, 446.2e6, 'PMR446 (walkie-talkies)', 'nfm', []],
  [466e6, 467e6, 'Buscapersonas', 'nfm', ['pocsag', 'flex']],
  [470e6, 694e6, 'TDT (TV digital)', null, ['dvbt']],
  [791e6, 862e6, 'LTE banda 20 / 800', null, ['lte']],
  [863e6, 870e6, 'SRD / LoRa 868 MHz', null, ['lora', 'meshtastic', 'ism']],
  [925e6, 960e6, 'GSM 900', null, ['gsm']],
  [1090e6 - 1e6, 1090e6 + 1e6, 'ADS-B / Mode S 1090 MHz', null, ['adsb']],
  [2400e6, 2483.5e6, 'WiFi / ISM 2,4 GHz', null, ['wifi']],
  [3300e6, 3800e6, '5G NR n78', null, ['nr']],
];
const bandOf = (f) => BANDAS.filter(([lo, hi]) => f >= lo && f <= hi).sort((a, b) => (a[1] - a[0]) - (b[1] - b[0]));

/* Forma espectral → protocolos favorecidos. */
const SHAPES = {
  carrier: { label: 'portadora estrecha', note: 'una raya: portadora, CW u OOK de baja velocidad', favours: ['ism', 'dsc'] },
  fsk2: { label: 'dos lóbulos (FSK)', note: 'dos picos separados por un valle: modulación FSK/AFSK', favours: ['pocsag', 'flex', 'rs41', 'ism', 'dsc', 'aprs', 'acars'] },
  flat: { label: 'bloque plano (OFDM / banda ancha)', note: 'techo plano y flancos abruptos: OFDM, CSS (LoRa; el chirp no se ve en el espectro de potencia) o DSSS', favours: ['dab', 'dvbt', 'wifi', 'lte', 'nr', 'lora', 'meshtastic', 'gsm'] },
  bell: { label: 'campana (FM / GMSK / PSK filtrado)', note: 'pico central con caída suave: FM analógica o modulación digital filtrada', favours: ['ais', 'aprs', 'dmr', 'pocsag', 'flex', 'rds', 'vdl2', 'acars'] },
};

/** Clasifica la forma dentro de [f − bw/2, f + bw/2]. */
export function classifyShape(B, f, bw) {
  const bh = B.rate / B.n, f0 = B.center - B.rate / 2;
  const i0 = clamp(Math.floor((f - bw / 2 - f0) / bh), 0, B.n - 1), i1 = clamp(Math.ceil((f + bw / 2 - f0) / bh), 0, B.n - 1);
  const N = i1 - i0 + 1;
  if (bw < 3000 || N < 4) return { id: 'carrier', ...SHAPES.carrier };
  const p = Array.from(B.db.subarray(i0, i1 + 1));
  const peak = Math.max(...p), mean = p.reduce((a, v) => a + Math.pow(10, v / 10), 0) / N, crest = peak - 10 * Math.log10(mean);
  const t = Math.floor(N / 3), lm = Math.max(...p.slice(0, t || 1)), rm = Math.max(...p.slice(N - (t || 1))), mid = Math.max(...p.slice(t, N - t || undefined));
  const valley = Math.min(...p.slice(t, N - t || undefined));
  if (N >= 9 && Math.min(lm, rm) - valley >= 3 && Math.abs(lm - rm) <= 4 && mid <= Math.max(lm, rm) + 1) return { id: 'fsk2', ...SHAPES.fsk2 };
  if (N >= 8 && crest < 2.5) return { id: 'flat', ...SHAPES.flat };
  return { id: 'bell', ...SHAPES.bell };
}

/** Sin marcador del detector: busca el pico cerca de f y mide su ancho a −10 dB. */
export function estimateAt(B, f, tol) {
  const bh = B.rate / B.n, f0 = B.center - B.rate / 2, idx = (x) => clamp(Math.round((x - f0) / bh), 0, B.n - 1);
  const s = []; const st = Math.max(1, Math.floor(B.n / 1024)); for (let i = 0; i < B.n; i += st) s.push(B.db[i]);
  s.sort((a, b) => a - b); const noise = s[s.length >> 1];
  let k = idx(f), a = idx(f - tol), b = idx(f + tol);
  for (let i = a; i <= b; i++) if (B.db[i] > B.db[k]) k = i;
  const peak = B.db[k], thr = Math.max(noise + 3, peak - 10), maxW = Math.floor(3e6 / bh);
  let lo = k, hi = k;
  while (lo > 0 && k - lo < maxW && B.db[lo - 1] > thr) lo--;
  while (hi < B.n - 1 && hi - k < maxW && B.db[hi + 1] > thr) hi++;
  return { freq: f0 + ((lo + hi) / 2 + 0.5) * bh, bw: Math.max(bh * 2, (hi - lo + 1) * bh), peak, snr: peak - noise, estimated: true };
}

/** Candidatos ordenados: [{d, score, pct, why[]}]. */
export function candidates(sig, shape) {
  const bands = bandOf(sig.freq), out = [];
  for (const d of S.decoders) {
    let sc = 0; const why = [];
    const near = (d.freqs || []).filter(Boolean).reduce((m, t) => Math.min(m, Math.abs(t - sig.freq)), Infinity);
    const inBand = bands.some((b) => b[4].includes(d.id));
    if (near < Math.max(d.bw || 0, 25e3)) { sc += 4; why.push(`en uno de sus canales (${fmtHz(snapFreq(d, sig.freq), 4)})`); }
    else if (near < 3e6) { sc += 2; why.push('dentro de su banda'); }
    if (inBand) { sc += 3; why.push(`opera en ${bands.find((b) => b[4].includes(d.id))[2]}`); }
    const ref = d.kind === 'narrow' ? d.bw : (d.wide_rate || d.wide_min_rate || 0) * 0.8;
    if (ref > 0) {
      const r = sig.bw / ref;
      if (r >= 0.5 && r <= 1.5) { sc += 3; why.push(`ancho ${fmtHz(sig.bw)} ≈ ${fmtHz(ref)} del protocolo`); }
      else if (r >= 0.25 && r <= 2.5) { sc += 2; why.push(`ancho ${fmtHz(sig.bw)} compatible con ${fmtHz(ref)}`); }
    }
    if (shape.favours.includes(d.id)) { sc += 1.5; why.push('forma: ' + shape.label); }
    if (sc >= 2) out.push({ d, score: sc, pct: Math.min(99, Math.round((sc / 8.5) * 100)), why });
  }
  return out.sort((a, b) => b.score - a.score || a.d.name.localeCompare(b.d.name));
}

function guessMode(sig) {
  const b = bandOf(sig.freq).find((x) => x[3]);
  if (b) return b[3];
  return sig.bw >= 100e3 ? 'wfm' : sig.bw >= 7e3 ? 'nfm' : 'usb';
}

/* ---------- panel ---------- */
let open = false;
const close = () => { const m = $('#sig-panel'); m.hidden = true; open = false; };

function sketch(B, sig) {
  const cv = h('canvas', { width: 340, height: 56, 'aria-label': 'Esbozo del espectro alrededor de la señal', role: 'img' });
  const c = cv.getContext('2d'), bh = B.rate / B.n, f0 = B.center - B.rate / 2;
  const span = Math.max(sig.bw * 3, bh * 24), a = clamp(Math.floor((sig.freq - span / 2 - f0) / bh), 0, B.n - 2), b = clamp(Math.ceil((sig.freq + span / 2 - f0) / bh), a + 1, B.n - 1);
  const v = Array.from(B.db.subarray(a, b + 1)), lo = Math.min(...v) - 2, hi = Math.max(...v) + 2;
  const X = (i) => (i / (v.length - 1)) * 340, Y = (d) => 54 - ((d - lo) / (hi - lo || 1)) * 50;
  const xa = X((sig.freq - sig.bw / 2 - f0) / bh - a), xb = X((sig.freq + sig.bw / 2 - f0) / bh - a);
  c.fillStyle = 'rgba(208,188,255,.18)'; c.fillRect(xa, 0, Math.max(2, xb - xa), 56);
  c.strokeStyle = '#d0bcff'; c.lineWidth = 1.2; c.beginPath();
  v.forEach((d, i) => (i ? c.lineTo(X(i), Y(d)) : c.moveTo(X(i), Y(d)))); c.stroke();
  return cv;
}

function decodeNow(cd, sig) {
  const d = cd.d, f = snapFreq(d, sig.freq), before = new Set(S.decList.map((i) => i.inst));
  bus.emit('ev-focus', { proto: d.id, before });
  addDecoder(d.id, f); close();
  toast(`Añadido ${d.name} en ${fmtHz(f, 4)}: los eventos aparecen en Decodificadores`, 'ok');
  const tab = document.getElementById('tab-decoders'); if (tab) tab.click();
}
async function listen(sig) {
  const mode = guessMode(sig);
  setChannel({ freq: Math.round(sig.freq / 100) * 100, mode, ...(mode === 'wfm' ? {} : {}) }, { user: true });
  close();
  if (!S.chan.audio) await toggleListen();
}

function show({ sig, freq, tol, x, y }) {
  const B = spectrumBins();
  if (!B.n || !B.rate || B.sweep) return;
  const s = sig || estimateAt(B, freq, tol);
  const weak = s.estimated && s.snr < 4;
  if (weak) s.bw = Math.max(s.bw, 12500);
  const shape = classifyShape(B, s.freq, s.bw), list = candidates(s, shape);
  const mode = guessMode(s), bands = bandOf(s.freq);
  const cand = (c) => h('div', { class: 'cand' },
    h('span', { class: 'nm', text: `${c.d.name} · ${c.pct} %` }),
    h('button', { class: 'btn small primary', type: 'button', text: 'Decodificar', 'aria-label': 'Decodificar como ' + c.d.name, onclick: () => decodeNow(c, s) }),
    h('div', { class: 'bar' }, h('i', { style: `width:${c.pct}%` })),
    h('div', { class: 'why', text: c.why.join(' · ') + (c.d.kind === 'wide' ? ' · banda ancha' : '') }));
  const top = list.slice(0, 3), rest = list.slice(3, 12);
  const m = $('#sig-panel');
  m.replaceChildren(
    h('div', { class: 'sp-h' }, h('b', { text: fmtHz(s.freq, 4) }), h('span', { class: 'muted', text: bands.length ? bands[0][2] : 'banda sin asignación conocida' }),
      h('button', { class: 'btn small sp-x', type: 'button', text: '✕', 'aria-label': 'Cerrar el panel', onclick: close })),
    h('div', { class: 'sp-m' }, h('span', { class: 'chip', text: `ancho ${fmtHz(s.bw)}` }), h('span', { class: 'chip', text: `SNR ${s.snr.toFixed(0)} dB` }), ...(s.estimated ? [h('span', { class: 'chip', title: 'No hay marcador del detector aquí: medido en el cliente sobre el espectro', text: 'medido aquí' })] : [])),
    sketch(B, s),
    h('div', { class: 'sp-shape' }, 'Forma: ', h('b', { text: shape.label }), ` — ${shape.note}.`),
    h('div', { class: 'row' }, h('button', { class: 'btn small', type: 'button', text: `Escuchar (${mode.toUpperCase()})`, onclick: () => listen(s) })),
    h('div', { class: 'sp-h2', text: weak ? 'Sin señal clara: propuestas solo por banda' : 'Decodificadores propuestos' }),
    ...(top.length ? top.map(cand) : [h('p', { class: 'muted', text: 'Ningún protocolo del catálogo encaja con esta banda y este ancho.' })]),
    ...(rest.length ? [h('details', {}, h('summary', { text: `Más protocolos (${rest.length})` }), ...rest.map(cand))] : []));
  m.hidden = false; open = true;
  const r = m.getBoundingClientRect();
  m.style.left = Math.max(4, Math.min(x, innerWidth - r.width - 4)) + 'px';
  m.style.top = Math.max(4, Math.min(y + 8, innerHeight - r.height - 4)) + 'px';
  (m.querySelector('.cand button') || m.querySelector('.sp-x')).focus();
}

export function initSignal() {
  bus.on('sig-menu', show);
  document.addEventListener('pointerdown', (e) => { if (open && !e.target.closest('#sig-panel')) close(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && open) close(); });
  window.addEventListener('scroll', () => { if (open) close(); }, true);
}
