// tv.js — pestaña «TV»: ver y oír la TDT (AGENTS.md §8c, docs/arquitectura/WEBSOCKET.md).
//
// Flujo de datos (todo en little-endian):
//   WS binario tipo 5 (vídeo): [1] bit0 = clave, [2] códec (1 H.264, 2 MPEG-2, 3 HEVC), [3] nº de
//        secuencia (mod 256), [4..11] PTS i64, [12..19] DTS i64 (90 kHz), luego la unidad de acceso
//        en Annex B (los fotogramas clave llevan SPS+PPS).
//   WS binario tipo 6 (audio): [1] canales, [4..7] tasa, [8..15] PTS de la primera muestra, PCM int16.
//
// Vídeo: WebCodecs VideoDecoder 'avc1.PPCCLL' SIN «description» (con Annex B el decodificador
//   lee SPS/PPS del propio flujo); el códec se deduce del SPS del fotograma clave. Cada
//   VideoFrame decodificado se guarda en una cola ordenada por PTS (salen en orden de
//   presentación aunque haya B-frames) y se pinta en un <canvas>.
// Audio: tv-worklet.js (AudioWorklet propio de TV). Es el RELOJ: informa del PTS de la muestra
//   que suena y el vídeo se presenta cuando su PTS ≤ reloj de audio (se descartan los
//   fotogramas que ya llegan tarde). Sin audio (o antes de que arranque) se usa un reloj
//   propio anclado al primer fotograma.
// WebCodecs solo existe en contexto seguro (https o http://localhost): si falta se explica.
import { S, bus, $, h, toast } from './util.js';
import { send } from './ws.js';

const HAS_WC = typeof VideoDecoder !== 'undefined' && typeof EncodedVideoChunk !== 'undefined';
const CH_LO = 21, CH_HI = 48;
const chFreq = (ch) => (306 + 8 * ch) * 1e6;

/* ---------------- estado ---------------- */
let playing = false;
let vdec = null, vcodec = '', needKey = true, lastSeq = -1, h264ok = null;
let queue = [];                       // VideoFrame decodificados, por PTS ascendente
let frame0 = null;                    // fallback de reloj: {pts, t}
let ctx = null, node = null, gain = null, ana = null, clk = null, vol = 0.8, muted = false;
let wantAuto = false;                 // elegir un servicio en cuanto haya lista
let fileSeen = false, lastShown = 0;
let cur = null;                       // último mensaje "tv"
const st = { vin: 0, vdec: 0, vdrop: 0, vshown: 0, ain: 0, derr: 0, lastAV: 0, fps: 0, last: '', fpsN: 0, fpsT: 0, cw: 0, ch: 0 };

try { vol = parseFloat(localStorage.getItem('tvvol') ?? '0.8'); if (!isFinite(vol)) vol = 0.8; } catch { /* sin almacenamiento */ }

/* ---------------- audio ---------------- */
async function startAudio() {
  if (ctx) { try { await ctx.resume(); } catch { /* nada */ } return true; }
  try {
    ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('js/tv-worklet.js');
    node = new AudioWorkletNode(ctx, 'tv-player', { numberOfInputs: 0, outputChannelCount: [2] });
    gain = ctx.createGain();
    ana = ctx.createAnalyser();
    ana.fftSize = 4096;
    node.connect(gain).connect(ana).connect(ctx.destination);
    node.port.onmessage = (e) => { clk = e.data; };
    node.port.postMessage({ t: 'cfg', targetMs: 150 });
    applyVol();
    await ctx.resume();
    return true;
  } catch (e) {
    console.error(e);
    toast('No se pudo iniciar el audio de TV: ' + (e.message || e), 'err');
    try { ctx && ctx.close(); } catch { /* nada */ }
    ctx = node = gain = ana = null;
    return false;
  }
}
function stopAudio() {
  try { node && node.disconnect(); ctx && ctx.close(); } catch { /* nada */ }
  ctx = node = gain = ana = clk = null;
}
function applyVol() { if (gain) gain.gain.value = muted ? 0 : vol * vol; }

/** PTS (90 kHz) de lo que se está oyendo ahora mismo, o null si no hay reloj de audio. */
function audioPts() {
  if (!ctx || !clk || clk.pts == null || ctx.state !== 'running') return null;
  const dt = ctx.currentTime - clk.t;
  if (dt > 0.6) return null;                                  // el worklet se paró o se vació
  const lat = (ctx.baseLatency || 0) + (ctx.outputLatency || 0);
  return clk.pts + (dt - lat) * 90000;
}

function onAudio(buf) {
  if (!playing || !node || buf.byteLength < 20) return;
  const dv = new DataView(buf);
  const ch = dv.getUint8(1) || 2, rate = dv.getUint32(4, true);
  const pts = Number(dv.getBigInt64(8, true));
  const n = ((buf.byteLength - 16) >> 1) / ch | 0;
  if (!n) return;
  const pcm = new Int16Array(buf.slice(16, 16 + n * ch * 2));
  const l = new Float32Array(n), r = ch === 2 ? new Float32Array(n) : l;
  if (ch === 2) for (let i = 0; i < n; i++) { l[i] = pcm[2 * i] / 32768; r[i] = pcm[2 * i + 1] / 32768; }
  else for (let i = 0; i < n; i++) l[i] = pcm[i] / 32768;
  st.ain++;
  node.port.postMessage({ t: 'pcm', l, r: ch === 2 ? r : l.slice(), pts, rate }, ch === 2 ? [l.buffer, r.buffer] : [l.buffer]);
}

/* ---------------- vídeo ---------------- */
const hex2 = (v) => v.toString(16).padStart(2, '0');

/** Busca una NAL del tipo dado en Annex B; devuelve [inicio, fin) del cuerpo (con su cabecera). */
function findNal(d, type) {
  const n = d.length;
  for (let i = 0; i + 4 < n; i++) {
    if (d[i] === 0 && d[i + 1] === 0 && d[i + 2] === 1) {
      const s = i + 3;
      if ((d[s] & 0x1f) === type) {
        let e = s + 1;
        while (e + 2 < n && !(d[e] === 0 && d[e + 1] === 0 && (d[e + 2] === 1 || d[e + 2] === 0))) e++;
        return [s, Math.min(e, n)];
      }
      i = s;
    }
  }
  return null;
}

function resetVideo(keepDecoder) {
  for (const f of queue) f.frame.close();
  queue = [];
  frame0 = null;
  needKey = true;
  lastSeq = -1;
  if (vdec && !keepDecoder) { try { vdec.close(); } catch { /* nada */ } vdec = null; vcodec = ''; }
  else if (vdec && vdec.state === 'configured') { try { vdec.reset(); } catch { /* nada */ } vcodec = ''; }
}

function onFrameOut(frame) {
  st.vdec++;
  const pts = frame.timestamp * 0.09;                          // µs → ticks de 90 kHz
  // inserción ordenada (la salida del decodificador ya viene en orden de presentación)
  let i = queue.length;
  while (i > 0 && queue[i - 1].pts > pts) i--;
  queue.splice(i, 0, { pts, frame });
  while (queue.length > 24) { queue.shift().frame.close(); st.vdrop++; }
}

function ensureDecoder(codecStr) {
  if (vdec && vdec.state === 'configured' && vcodec === codecStr) return true;
  if (vdec && vdec.state !== 'closed') { try { vdec.close(); } catch { /* nada */ } }
  vdec = new VideoDecoder({
    output: onFrameOut,
    error: (e) => { st.derr++; st.last = 'Error del decodificador: ' + e.message; needKey = true; vdec = null; vcodec = ''; },
  });
  vdec.configure({ codec: codecStr, optimizeForLatency: true });
  vcodec = codecStr;
  return true;
}

function onVideo(buf) {
  if (!playing || !HAS_WC || buf.byteLength < 24) return;
  const dv = new DataView(buf);
  const key = (dv.getUint8(1) & 1) !== 0, codec = dv.getUint8(2), seq = dv.getUint8(3);
  const pts = Number(dv.getBigInt64(4, true));
  st.vin++;
  // hueco en la secuencia (el servidor descartó tramas por ir lento): esperar al siguiente clave
  if (lastSeq >= 0 && seq !== ((lastSeq + 1) & 255)) needKey = true;
  lastSeq = seq;
  if (needKey && !key) return;
  const data = new Uint8Array(buf, 20);
  if (codec !== 1) { st.last = codec === 2 ? 'Vídeo MPEG-2: WebCodecs no lo decodifica.' : 'Vídeo HEVC: aún no soportado.'; return; }
  if (key) {
    const sps = findNal(data, 7);
    if (!sps) return;
    const cs = 'avc1.' + hex2(data[sps[0] + 1]) + hex2(data[sps[0] + 2]) + hex2(data[sps[0] + 3]);
    try { ensureDecoder(cs); } catch (e) { st.last = 'configure: ' + e.message; return; }
    needKey = false;
  }
  if (!vdec || vdec.state !== 'configured') return;
  if (vdec.decodeQueueSize > 40) { needKey = true; st.vdrop++; return; }   // el decodificador no da abasto
  try {
    vdec.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: Math.round(pts * 100 / 9), data }));
  } catch (e) { st.last = 'decode: ' + e.message; needKey = true; }
}

/* ---------------- presentación ---------------- */
function present(now) {
  requestAnimationFrame(present);
  if (!playing || !queue.length) return;
  let clock = audioPts();
  const wantAudio = cur && cur.audio_ok;
  if (clock != null && now - lastShown > 3000 && queue[0].pts - clock > 90000) clock = null;   // reloj de audio atascado
  if (clock == null) {
    // sin reloj de audio: se espera hasta 1,5 s a que arranque; luego reloj propio (≈ 120 ms de retardo)
    if (!frame0) frame0 = { pts: queue[0].pts, t: now, since: now };
    if (wantAudio && now - frame0.since < 1500) return;
    clock = frame0.pts + (now - frame0.t) * 90 - 10800;
  } else frame0 = null;
  // el más reciente cuyo PTS ya toca; los anteriores se descartan (llegan tarde)
  let k = -1;
  for (let i = 0; i < queue.length; i++) if (queue[i].pts <= clock + 1800) k = i; else break;
  if (k < 0) {
    // el reloj va muy por delante o muy por detrás de la cola: resincronizar para no quedarse parado
    return;
  }
  for (let i = 0; i < k; i++) { queue[i].frame.close(); st.vdrop++; }
  const pick = queue[k];
  queue.splice(0, k + 1);
  const cv = $('#tv-canvas'), g = cv.getContext('2d');
  const w = pick.frame.displayWidth, hh = pick.frame.displayHeight;
  if (cv.width !== w || cv.height !== hh) { cv.width = w; cv.height = hh; st.cw = w; st.ch = hh; }
  g.drawImage(pick.frame, 0, 0, w, hh);
  pick.frame.close();
  st.vshown++;
  lastShown = now;
  st.lastAV = Math.round((clock - pick.pts) / 90);              // ms: >0 = el fotograma iba atrasado
  st.fpsN++;
  if (now - st.fpsT > 1000) { st.fps = st.fpsN * 1000 / (now - st.fpsT); st.fpsN = 0; st.fpsT = now; renderPlayerInfo(); }
  $('#tv-stage').classList.add('live');
}

/* ---------------- interfaz ---------------- */
const CODEC_NAME = { h264: 'H.264', mpeg2: 'MPEG-2', hevc: 'HEVC', mp2: 'MP2', aac: 'AAC', ac3: 'AC-3', eac3: 'E-AC-3' };

function fillChannels() {
  const sel = $('#tv-ch');
  if (sel.options.length) return;
  for (let ch = CH_LO; ch <= CH_HI; ch++) sel.append(h('option', { value: chFreq(ch), text: `Canal ${ch} · ${chFreq(ch) / 1e6} MHz` }));
  // Último múltiplex usado (o C32, el mejor de la zona de pruebas, si no hay ninguno).
  let last = 32; try { last = +localStorage.getItem('tvch') || 32; } catch { /* sin almacenamiento */ }
  sel.value = chFreq(Math.min(CH_HI, Math.max(CH_LO, last)));
}

function fmtBer(x) { return x == null ? '—' : x === 0 ? '0' : x.toExponential(1); }

function renderStats() {
  const box = $('#tv-stats');
  const m = cur;
  if (!m) { box.replaceChildren(h('span', { class: 'muted', text: 'Sin datos todavía.' })); return; }
  const s = m.stats || {}, t = m.ts || {};
  const mode = s.mode_k ? `${s.mode_k}k · guarda 1/${s.guard_den} · ${s.qam}-QAM · FEC ${s.cr_num}/${s.cr_den}` : '—';
  const rows = [
    ['Enganche', m.file ? 'fichero .ts (sin radio)' : s.lock ? 'sí' : 'no'],
    ['Frecuencia', m.freq ? `${(m.freq / 1e6).toFixed(3)} MHz${m.ch ? ' (canal ' + m.ch + ')' : ''}` : '—'],
    ['Modo', m.file ? '—' : mode],
    ['MER / SNR', m.file ? '—' : `${(s.mer_db ?? 0).toFixed(1)} dB / ${(s.snr_db ?? 0).toFixed(1)} dB`],
    ['Desvío (CFO)', m.file ? '—' : `${(s.cfo_hz ?? 0).toFixed(0)} Hz`],
    ['BER antes de Viterbi / RS', m.file ? '—' : `${fmtBer(s.ber_pre_viterbi)} / ${fmtBer(s.ber_pre_rs)}`],
    ['Paquetes RS ok / corregidos / perdidos', m.file ? '—' : `${s.rs_ok ?? 0} / ${s.rs_corrected ?? 0} / ${s.rs_failed ?? 0}`],
    ['Tasa de TS', `${((s.ts_bitrate || 0) / 1e6).toFixed(2)} Mb/s`],
    ['Paquetes TS / errores de continuidad', `${t.packets ?? 0} / ${t.cc_errors ?? 0}`],
    ['Red', m.network || '—'],
    ['Vídeo servidor (fotogramas / claves)', `${t.video_frames ?? 0} / ${t.key_frames ?? 0}`],
    ['Audio servidor (tramas)', `${t.audio_frames ?? 0}`],
  ];
  box.replaceChildren(...rows.flatMap(([k, v]) => [h('span', { class: 'k', text: k }), h('span', { class: 'v mono', text: v })]));
  const chip = $('#tv-lock');
  const ok = m.file || s.lock;
  chip.className = 'chip' + (ok ? '' : ' warn');
  chip.replaceChildren(h('i', { class: 'dot' + (ok ? '' : ' off') }), m.file ? 'Fichero' : ok ? `Enganchado · MER ${(s.mer_db ?? 0).toFixed(1)} dB` : (m.freq ? 'Sin señal' : 'Sin sintonizar'));
}

function svcLabel(sv) {
  const v = sv.video, a = (sv.audio || [])[0];
  const parts = [];
  if (v) parts.push(`${CODEC_NAME[v.codec] || v.codec}${v.w ? ` ${v.w}×${v.h}` : ''}`);
  else parts.push('radio');
  if (a) parts.push(`${CODEC_NAME[a.codec] || a.codec}${a.lang ? ' ' + a.lang : ''}`);
  return parts.join(' · ');
}

function renderServices() {
  const box = $('#tv-svcs');
  const list = (cur && cur.services) || [];
  if (!list.length) {
    box.replaceChildren(h('p', { class: 'muted', text: cur && (cur.file || (cur.stats && cur.stats.lock)) ? 'Esperando las tablas del múltiplex (PAT/SDT)…' : 'Sintoniza un canal o explora la banda para ver los servicios.' }));
    return;
  }
  const sorted = [...list].sort((a, b) => (a.lcn || 999) - (b.lcn || 999) || a.sid - b.sid);
  box.replaceChildren(...sorted.map((sv) => {
    const hasMp2 = (sv.audio || []).some((a) => a.codec === 'mp2');
    const note = !sv.video ? 'Servicio de radio' : sv.ca ? 'Cifrado: no se puede ver' : !hasMp2 && (sv.audio || []).length ? 'Sin audio: solo se decodifica MP2' : '';
    return h('button', {
      class: 'svc' + (cur.current === sv.sid ? ' on' : ''), type: 'button', 'aria-pressed': String(cur.current === sv.sid),
      title: note || svcLabel(sv), onclick: () => chooseService(sv.sid),
    },
      h('span', { class: 'lcn mono', text: sv.lcn || '' }),
      h('span', { class: 'nm', text: sv.name }),
      h('span', { class: 'sub muted', text: svcLabel(sv) + (note ? ' — ' + note : '') }));
  }));
}

function renderPlayerInfo() {
  const sv = cur && cur.services && cur.services.find((x) => x.sid === cur.current);
  const parts = [];
  if (sv) parts.push(sv.name);
  if (st.cw) parts.push(`${st.cw}×${st.ch}`);
  parts.push(`${st.fps.toFixed(0)} img/s`);
  parts.push(`A/V ${st.lastAV >= 0 ? '+' : ''}${st.lastAV} ms`);
  if (clk) parts.push(`audio ${clk.fillMs.toFixed(0)} ms en búfer`);
  $('#tv-info').textContent = parts.join(' · ') + (st.last ? ' · ' + st.last : '');
}

function renderScan(m) {
  const box = $('#tv-scanres');
  const bar = $('#tv-scanbar');
  if (m.running) {
    bar.hidden = false;
    bar.textContent = `Explorando… canal ${m.ch} (${m.pos + 1}/${m.total}) · ${m.found.length} múltiplex`;
  } else bar.hidden = true;
  $('#tv-scan').disabled = !!m.running;
  if (!m.found.length) { box.replaceChildren(m.running ? '' : h('p', { class: 'muted', text: m.done ? 'No se enganchó ningún múltiplex.' : '' })); return; }
  box.replaceChildren(...m.found.map((f) => h('button', {
    class: 'btn small mux', type: 'button', title: (f.names || []).join(', '),
    onclick: () => tuneMux(f.freq),
  }, `Canal ${f.ch} · ${(f.freq / 1e6).toFixed(0)} MHz · ${f.n} serv. · MER ${f.mer_db.toFixed(1)} dB`)));
}

/* ---------------- órdenes ---------------- */
async function play() {
  if (playing) return;
  if (!HAS_WC) return;
  playing = true;
  await startAudio();
  resetVideo(false);
  refreshButtons();
}
function pause() {
  playing = false;
  resetVideo(false);
  stopAudio();
  $('#tv-stage').classList.remove('live');
  refreshButtons();
}
function refreshButtons() {
  const b = $('#tv-play');
  b.textContent = playing ? 'Parar' : 'Ver';
  b.setAttribute('aria-pressed', String(playing));
}
function tuneMux(freq) {
  wantAuto = true;
  resetVideo(true);
  if (node) node.port.postMessage({ t: 'flush' });
  send({ cmd: 'tv', freq });
  $('#tv-ch').value = String(freq);
  if (!playing) play();
}
function chooseService(sid, user = true) {
  wantAuto = false;
  resetVideo(true);
  if (node) node.port.postMessage({ t: 'flush' });
  send({ cmd: 'tv_service', sid });
  if (user && !playing) play();     // el audio exige un gesto del usuario: la elección automática no arranca sola
}
function autoPick() {
  if (!wantAuto || !cur || cur.current != null || !(cur.services || []).length) return;
  const ok = (sv) => sv.video && !sv.ca && (sv.audio || []).some((a) => a.codec === 'mp2');
  const list = [...cur.services].sort((a, b) => (a.lcn || 999) - (b.lcn || 999));
  const sv = list.find(ok) || list.find((x) => x.video && !x.ca);
  if (sv) chooseService(sv.sid, false);
}

/* ---------------- arranque ---------------- */
export function initTv() {
  fillChannels();
  const secure = window.isSecureContext;
  if (!HAS_WC) {
    const w = $('#tv-nowc');
    w.hidden = false;
    w.replaceChildren(
      h('b', { text: 'Este navegador no puede reproducir la TV aquí. ' }),
      secure
        ? 'No ofrece WebCodecs (VideoDecoder). Usa un Chrome/Edge/Chromium reciente.'
        : `WebCodecs solo existe en contexto seguro y esta página se sirve por HTTP desde «${location.host}». Abre http://localhost:${location.port || 80}/ en la máquina del servidor, o accede por HTTPS (p. ej. túnel SSH: ssh -L ${location.port || 8090}:localhost:${location.port || 8090} servidor).`);
    $('#tv-stage').hidden = true;
    $('#tv-play').disabled = true;
  } else {
    VideoDecoder.isConfigSupported({ codec: 'avc1.640028' }).then((r) => {
      h264ok = !!r.supported;
      if (!h264ok) {
        const w = $('#tv-nowc');
        w.hidden = false;
        w.textContent = 'Este navegador tiene WebCodecs pero no decodifica H.264 (las versiones de Chromium sin códecs propietarios no lo hacen). Usa Google Chrome, Edge o Chromium con H.264.';
      }
    }).catch(() => { /* nada */ });
  }
  $('#tv-tune').addEventListener('click', () => { const f = parseFloat($('#tv-ch').value); try { localStorage.setItem('tvch', String(Math.round((f / 1e6 - 474) / 8 + 21))); } catch { /* nada */ } tuneMux(f); });
  $('#tv-scan').addEventListener('click', () => { send({ cmd: 'tv_scan' }); $('#tv-scan').disabled = true; });
  $('#tv-stop').addEventListener('click', () => { send({ cmd: 'tv_stop' }); pause(); wantAuto = false; });
  $('#tv-play').addEventListener('click', () => (playing ? pause() : play()));
  $('#tv-fs').addEventListener('click', toggleFs);
  $('#tv-canvas').addEventListener('dblclick', toggleFs);
  const v = $('#tv-vol');
  v.value = String(vol);
  v.addEventListener('input', () => { vol = parseFloat(v.value); muted = false; applyVol(); try { localStorage.setItem('tvvol', String(vol)); } catch { /* nada */ } });
  $('#tv-mute').addEventListener('click', () => { muted = !muted; $('#tv-mute').setAttribute('aria-pressed', String(muted)); applyVol(); });
  bus.on('bin:tvvideo', onVideo);
  bus.on('bin:tvaudio', onAudio);
  bus.on('msg:tv', (m) => {
    const prevCur = cur && cur.current;
    cur = m;
    if (m.current !== prevCur && prevCur != null) { resetVideo(true); if (node) node.port.postMessage({ t: 'flush' }); }
    if (m.file && !fileSeen) { fileSeen = true; wantAuto = true; }   // con un .ts no hay nada que sintonizar
    renderStats(); renderServices(); autoPick(); renderPlayerInfo();
    if (m.freq && !m.file) $('#tv-ch').value = String(m.freq);
    if (!m.scanning) $('#tv-scan').disabled = !!m.file;
    $('#tv-tune').disabled = !!m.file;
    $('#tv-ch').disabled = !!m.file;
    const fn = $('#tv-filenote'); if (fn) fn.hidden = !m.file;
  });
  bus.on('msg:tv_scan', renderScan);
  bus.on('tab', (id) => { if (id === 'tv') { renderStats(); renderServices(); } });
  requestAnimationFrame(present);
  renderStats(); renderServices(); refreshButtons();
  // ayuda para pruebas desde la consola / CDP
  window.__tvTone = () => {   // frecuencia dominante y nivel de lo que suena (diagnóstico)
    if (!ana || !ctx) return null;
    const f = new Float32Array(ana.frequencyBinCount), t = new Float32Array(ana.fftSize);
    ana.getFloatFrequencyData(f); ana.getFloatTimeDomainData(t);
    let k = 1; for (let i = 2; i < f.length; i++) if (f[i] > f[k]) k = i;
    let e = 0; for (const x of t) e += x * x;
    return { hz: k * ctx.sampleRate / ana.fftSize, db: f[k], rms: Math.sqrt(e / t.length) };
  };
  window.__tvAnalyser = () => {   // frecuencia con interpolación parabólica del pico (error << 1 bin)
    if (!ana || !ctx) return null;
    const f = new Float32Array(ana.frequencyBinCount);
    ana.getFloatFrequencyData(f);
    let k = 1; for (let i = 2; i < f.length - 1; i++) if (f[i] > f[k]) k = i;
    const a = f[k - 1], b = f[k], c2 = f[k + 1], d = (a - 2 * b + c2) || 1e-9;
    return +(((k + 0.5 * (a - c2) / d) * ctx.sampleRate / ana.fftSize).toFixed(2));
  };
  window.__tvDebug = () => ({
    playing, ...st, queue: queue.length, audioPts: audioPts(), clk, ctx: ctx && ctx.state, decoder: vdec && vdec.state,
    codec: vcodec, h264ok, secure, hasWC: HAS_WC, canvas: [$('#tv-canvas').width, $('#tv-canvas').height],
  });
}

function toggleFs() {
  const el = $('#tv-stage');
  if (document.fullscreenElement) document.exitFullscreen();
  else if (el.requestFullscreen) el.requestFullscreen().catch(() => toast('El navegador rechazó la pantalla completa', 'err'));
}
