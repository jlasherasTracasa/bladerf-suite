// hardware.js — panel de hardware: centro, tasa, ganancia, bias-tee, FFT, fps,
// umbral del detector, ubicación del receptor y grabaciones IQ.
import { S, bus, $, h, fmtHz, parseHz, fmtBytes, fillSelect, editing, toast } from './util.js';
import { send } from './ws.js';

const RATES = [0.25, 0.5, 1, 1.5, 2, 2.048, 2.4, 3, 4, 6, 8, 10, 12, 15.36, 16, 20, 24, 30.72, 40, 46.08, 61.44, 76.8, 92.16, 100, 122.88].map((m) => Math.round(m * 1e6));
const FFTS = [512, 1024, 2048, 4096, 8192, 16384];
const cfg = (o) => send({ cmd: 'config', ...o });

function fillRates(st) {
  // Por encima de rate_max solo con oversample (device.rate_max_oversample != null: FPGA ≥ 0.15, SC8)
  const d = st.device, top = d.rate_max_oversample || d.rate_max, set = new Set(RATES.filter((r) => r >= d.rate_min && r <= top));
  set.add(st.rate);
  const sel = $('#hw-rate');
  if (sel.dataset.sig === [...set].join()) return;
  sel.dataset.sig = [...set].join();
  fillSelect(sel, [...set].sort((a, b) => a - b).map((r) => [r, (r / 1e6).toFixed(r % 1e5 ? 3 : 2).replace(/\.?0+$/, '') + ' Msps']), st.rate);
}

// Fuente del espectro: CPU/GPU o motor de la imagen lab (FPGA). La opción FPGA solo se
// habilita si el servidor detectó la lab (status.fpga_lab.present).
function renderSpectrumSrc(st) {
  const sel = $('#hw-sspec'), note = $('#hw-sspec-note'), lab = st.fpga_lab || { present: false, active: false };
  const fpga = sel.querySelector('option[value="fpga"]');
  fpga.disabled = !lab.present;
  fpga.textContent = lab.present ? `FPGA (imagen lab ${lab.version || ''})`.trim() : 'FPGA (la imagen lab no está cargada)';
  if (!editing(sel)) sel.value = st.spectrum_src === 'fpga' ? 'fpga' : 'cpu';
  sel.querySelector('option[value="cpu"]').textContent = st.spectrum_src === 'gpu' ? 'GPU (FFT propia)' : 'CPU (FFT propia)';
  note.hidden = !lab.active;
  note.textContent = lab.active
    ? `Espectro de la FPGA: N=${lab.n}, K=${lab.k}, ${lab.frames} tramas${lab.gaps ? `, ${lab.gaps} huecos` : ''}. Sin muestras IQ: el audio, la grabación y los decodificadores esperan.`
    : '';
}

// Imagen de FPGA en RAM (F19): botón «Cargar imagen lab / de serie» con confirmación explícita. La carga es
// volátil (RAM de la FPGA): la flash no se toca y al desconectar la placa vuelve la de serie. Se deshabilita
// mientras se graba, hay TX o barrido, o la fuente es un fichero (el servidor también lo rechaza).
const LOAD_STATES = { ok: 'cargada en RAM', omitido: 'sin cambios', error: 'error' };
function ensureFpgaLoadUI() {
  if ($('#hw-fpga-btn')) return;
  const note = $('#hw-sspec-note');
  if (!note) return;
  const btn = h('button', { id: 'hw-fpga-btn', class: 'btn small', type: 'button', text: 'Cargar imagen lab en RAM' });
  const msg = h('p', { id: 'hw-fpga-note', class: 'muted', hidden: true });
  note.after(btn, msg);
  btn.addEventListener('click', () => {
    const to = btn.dataset.to;
    const txt = to === 'lab'
      ? 'Se cargará la imagen de FPGA «lab» (espectro en la FPGA, formato sc8g) en la RAM de la placa. Es volátil: la flash NO se toca y al desconectar la placa vuelve la imagen de serie.\n\nDurante 1-2 s se interrumpe la recepción y se reaplica la configuración. ¿Continuar?'
      : 'Se cargará la imagen de FPGA de serie en la RAM de la placa (la flash NO se toca). Durante 1-2 s se interrumpe la recepción. ¿Continuar?';
    if (!confirm(txt)) return;
    btn.disabled = true;
    send({ cmd: 'fpga_load', image: to });
  });
}

function renderFpgaLoad(st) {
  ensureFpgaLoadUI();
  const btn = $('#hw-fpga-btn'); if (!btn) return;
  const lab = st.fpga_lab || {}, file = st.device.kind === 'file';
  btn.hidden = file; $('#hw-fpga-note').hidden = file || !lab.load;
  if (file) return;
  btn.dataset.to = lab.present ? 'stock' : 'lab';
  btn.textContent = lab.present ? 'Cargar imagen de serie en RAM' : 'Cargar imagen lab en RAM';
  const rec = S.recording && S.recording.on, busy = rec || !!st.sweep;
  btn.disabled = busy;
  btn.title = busy ? 'No disponible mientras hay grabación, barrido o transmisión' : 'Carga en la RAM de la FPGA; la flash no se toca';
  if (lab.load) $('#hw-fpga-note').textContent = `Carga de la imagen lab: ${LOAD_STATES[lab.load.state] || lab.load.state}${lab.load.msg ? ' — ' + lab.load.msg : ''}`;
}

// Formato de las muestras por el USB (status.device.formats, format_req, format_note, sc8g). Los controles se
// crean aquí (no están en index.html): selector de formato junto a la tasa y, con sc8g, la ganancia digital
// GAIN8 (automática o 0..3 = +0/6/12/18 dB) con el % de muestras recortadas (CNT_SAT8 de la imagen lab 1.3).
const FMT_LABELS = {
  auto: 'Automático (según el USB)', sc16: 'SC16 (16 bit, con marcas de tiempo)',
  packed12: 'packed12 (12 bit, +33 % de tasa)', sc8g: 'sc8g (8 bit + ganancia digital, +100 %)',
};
function ensureFormatUI() {
  if ($('#hw-fmt')) return;
  const rate = $('#hw-rate')?.closest('label');
  if (!rate) return;
  const sel = h('select', { id: 'hw-fmt' });
  const lab = h('label', { id: 'hw-fmt-l', text: 'Formato USB' }, sel);
  rate.after(lab);
  const shift = h('select', { id: 'hw-g8' }, ...[['-1', 'Automática'], ['0', '0 dB (×1)'], ['1', '+6 dB (×2)'], ['2', '+12 dB (×4)'], ['3', '+18 dB (×8)']].map(([v, t]) => h('option', { value: v, text: t })));
  const lab2 = h('label', { id: 'hw-g8-l', hidden: true, text: 'Ganancia digital sc8g' }, shift);
  lab.after(lab2);
  const note = h('p', { id: 'hw-fmt-note', class: 'muted', hidden: true });
  const ovs = $('#hw-ovs-note');
  (ovs || rate.parentElement).after(note);
  sel.addEventListener('change', (e) => cfg({ format: e.target.value }));
  shift.addEventListener('change', (e) => cfg({ sc8g_shift: +e.target.value }));
}

function renderFormat(st) {
  ensureFormatUI();
  const sel = $('#hw-fmt'); if (!sel) return;
  const d = st.device, file = d.kind === 'file', list = d.formats || ['auto', 'sc16'];
  $('#hw-fmt-l').hidden = file;
  if (file) { $('#hw-g8-l').hidden = true; $('#hw-fmt-note').hidden = true; return; }
  const sig = list.join();
  if (sel.dataset.sig !== sig) {
    sel.dataset.sig = sig;
    fillSelect(sel, list.map((f) => [f, FMT_LABELS[f] || f]), st.format_req || 'auto');
  }
  if (!editing(sel)) sel.value = st.format_req || 'auto';
  const g = st.sc8g;
  $('#hw-g8-l').hidden = !list.includes('sc8g');
  if (g && !editing($('#hw-g8'))) $('#hw-g8').value = g.manual ? String(g.shift) : '-1';
  const parts = [];
  if (st.format_note) parts.push(st.format_note);
  if (g) parts.push(`Ganancia digital ${g.gain_db} dB (×${1 << g.shift}); recortes: ${g.sat_pct.toFixed(g.sat_pct < 0.1 ? 3 : 1)} % de las muestras${g.sat_pct > 0.5 ? ' — demasiados, baja la ganancia digital' : ''}.`);
  const note = $('#hw-fmt-note');
  note.hidden = !parts.length;
  note.textContent = parts.join(' ');
}

function renderStatus(st) {
  const d = st.device;
  fillRates(st);
  renderFormat(st);
  if (!editing($('#hw-center'))) $('#hw-center').value = fmtHz(st.center, 6);
  if (!editing($('#hw-rate'))) $('#hw-rate').value = st.rate;
  // Con fuente de fichero no hay filtro analógico que mostrar: se indica en vez de un guion
  if (!editing($('#hw-rfbw'))) $('#hw-rfbw').value = st.rf_bw > 0 ? fmtHz(st.rf_bw, 3) : d.kind === 'file' ? 'no aplica (fichero)' : '—';
  $('#hw-gmode').value = st.gain_mode;
  const agc = st.gain_mode !== 'manual';   // auto, fast, slow y hybrid: el servidor ajusta la ganancia
  const ovs = $('#hw-ovs-note');
  ovs.hidden = !(st.oversample || st.rate > d.rate_max);
  ovs.textContent = st.oversample ? `Modo oversample activo: ${((st.rate) / 1e6).toFixed(2)} Msps en ${st.format || 'SC8'} (8 bit por muestra: menor rango dinámico).` : '';
  const g = $('#hw-gain'); g.min = d.gain_min; g.max = d.gain_max;
  if (!editing(g)) g.value = st.gain;
  $('#hw-gain-o').textContent = agc ? `(${st.gain_mode}: ${st.gain_now} dB)` : `${st.gain} dB`;
  g.disabled = agc || !d.can_tune;
  $('#hw-gain-l').hidden = d.gain_max <= d.gain_min;           // un fichero no tiene ganancia
  $('#hw-bias-l').hidden = !d.biastee;
  $('#hw-bias').checked = !!st.biastee;
  const fs = $('#hw-fft');
  if (!fs.options.length) fillSelect(fs, FFTS.map((n) => [n, String(n)]));
  if (!editing(fs)) fs.value = String(st.fft);
  if (!editing($('#hw-fps'))) $('#hw-fps').value = st.fps;
  if (!editing($('#hw-det'))) $('#hw-det').value = st.det_snr;
  if (!editing($('#hw-lat')) && !editing($('#hw-lon'))) {
    $('#hw-lat').value = st.loc ? st.loc.lat : ''; $('#hw-lon').value = st.loc ? st.loc.lon : '';
  }
  for (const id of ['#hw-center', '#hw-rate', '#hw-rfbw', '#hw-gmode']) $(id).disabled = !d.can_tune;
  renderSpectrumSrc(st);
  renderFpgaLoad(st);
  const kv = [
    ['Tipo', d.kind], ['Placa', d.board], ['Serie', d.serial], ['Firmware FX3', d.fw], ['FPGA', d.fpga], ['libbladeRF', d.lib],
    ['Frecuencia', `${fmtHz(d.freq_min)} – ${fmtHz(d.freq_max)}`], ['Tasa', `${fmtHz(d.rate_min)} – ${fmtHz(d.rate_max)}`],
    ['Ancho RF', `${fmtHz(d.bw_min)} – ${fmtHz(d.bw_max)}`], ['Ganancia', `${d.gain_min} – ${d.gain_max} dB`],
    ['Formato de muestras', st.format ? `${st.format}${st.oversample ? ' (oversample)' : ''}` : '—'],
    ['Enlace USB', d.usb_speed ? `USB ${d.usb_speed >= 3 ? '3 (SuperSpeed)' : '2 (Hi-Speed)'}` : '—'],
    ['Recortes sc8g', st.sc8g ? `${st.sc8g.sat_pct.toFixed(3)} % (ganancia ${st.sc8g.gain_db} dB)` : '—'],
    ['Tasa máx. con oversample', d.rate_max_oversample ? fmtHz(d.rate_max_oversample) : 'no disponible'],
    ['Nivel en dBm', st.dbm_offset != null ? `sí (offset ${st.dbm_offset} dB)` : 'no (sin calibración de ganancia)'],
    ['Motor FFT', st.stats?.backend ?? '—'], ['Versión', `${st.app} ${st.version}`],
  ];
  $('#hw-info').replaceChildren(...kv.flatMap(([k, v]) => [h('span', { class: 'k', text: k }), h('span', { class: 'v', text: v || '—' })]));
}

/* ---- grabaciones ---- */
async function loadRecordings() {
  try {
    const r = await fetch('/api/recordings', { cache: 'no-store' });
    const j = await r.json();
    const ul = $('#rec-list');
    if (!j.files.length) { ul.replaceChildren(h('li', { class: 'muted', text: 'No hay grabaciones.' })); return; }
    ul.replaceChildren(...j.files.map((f) => h('li', {},
      h('a', { href: '/recordings/' + encodeURIComponent(f.name), download: f.name, text: f.name, title: new Date(f.mtime * 1000).toLocaleString('es-ES') }),
      h('span', { class: 'muted mono', text: fmtBytes(f.size) }),
      h('button', { class: 'btn small danger', 'aria-label': 'Borrar ' + f.name, text: 'Borrar', onclick: () => {
        if (!confirm(`¿Borrar ${f.name}?`)) return;
        send({ cmd: 'rec_delete', name: f.name }); setTimeout(loadRecordings, 500);
      } }))));
  } catch { $('#rec-list').replaceChildren(h('li', { class: 'muted', text: 'No se pudo leer la lista.' })); }
}

function renderRec(r) {
  if (!r) return;
  const b = $('#rec-btn');
  b.setAttribute('aria-pressed', r.on); b.textContent = r.on ? 'Parar' : 'Grabar';
  b.classList.toggle('primary', r.on);
  $('#rec-info').textContent = r.on ? `${r.name} · ${fmtBytes(r.bytes)} · ${r.seconds.toFixed(0)} s${r.dropped ? ` · ${r.dropped} perdidos` : ''}` : 'Parada';
}
export function toggleRecording() { send({ cmd: 'record', on: !(S.recording && S.recording.on) }); }

export function initHardware() {
  const num = (id, key, parse = parseHz) => {
    const el = $(id);
    const go = () => { const v = parse(el.value); if (v != null && isFinite(v)) cfg({ [key]: v }); else if (S.status) renderStatus(S.status); };
    el.addEventListener('keydown', (e) => { if (e.key === 'Enter') el.blur(); });
    el.addEventListener('blur', go);
  };
  num('#hw-center', 'center'); num('#hw-rfbw', 'rf_bw');
  $('#hw-rate').addEventListener('change', (e) => cfg({ rate: +e.target.value }));
  $('#hw-gmode').addEventListener('change', (e) => cfg({ gain_mode: e.target.value }));
  $('#hw-gain').addEventListener('input', (e) => { $('#hw-gain-o').textContent = e.target.value + ' dB'; });
  $('#hw-gain').addEventListener('change', (e) => cfg({ gain_mode: 'manual', gain: +e.target.value }));
  $('#hw-bias').addEventListener('change', (e) => {
    if (e.target.checked && !confirm('El bias-tee pone tensión en el conector de antena. Actívalo solo con una antena activa que lo necesite. ¿Continuar?')) { e.target.checked = false; return; }
    cfg({ biastee: e.target.checked });
  });
  $('#hw-sspec').addEventListener('change', (e) => {
    if (e.target.value === 'fpga' && !confirm('Con el espectro de la FPGA no hay muestras IQ: se pausan el audio, la grabación y los decodificadores. ¿Continuar?')) {
      if (S.status) renderSpectrumSrc(S.status);
      return;
    }
    cfg({ spectrum_src: e.target.value });
  });
  $('#hw-fft').addEventListener('change', (e) => cfg({ fft: +e.target.value }));
  $('#hw-fps').addEventListener('change', (e) => cfg({ fps: Math.max(1, Math.min(60, +e.target.value || 20)) }));
  $('#hw-det').addEventListener('change', (e) => cfg({ det_snr: +e.target.value }));

  const setLoc = () => {
    const lat = parseFloat($('#hw-lat').value.replace(',', '.')), lon = parseFloat($('#hw-lon').value.replace(',', '.'));
    if (!(Math.abs(lat) <= 90 && Math.abs(lon) <= 180)) { toast('Latitud o longitud no válidas', 'err'); return; }
    cfg({ loc: { lat, lon } });
  };
  $('#hw-loc-set').addEventListener('click', setLoc);
  $('#hw-loc-clear').addEventListener('click', () => { cfg({ loc: null }); $('#hw-lat').value = $('#hw-lon').value = ''; });
  $('#hw-geo').addEventListener('click', () => {
    if (!navigator.geolocation) { toast('Este navegador no ofrece geolocalización', 'err'); return; }
    navigator.geolocation.getCurrentPosition(
      (p) => { $('#hw-lat').value = p.coords.latitude.toFixed(5); $('#hw-lon').value = p.coords.longitude.toFixed(5); setLoc(); toast('Ubicación aplicada', 'ok'); },
      (e) => toast('No se pudo obtener la ubicación: ' + e.message + ' (requiere HTTPS o localhost)', 'err'),
      { enableHighAccuracy: false, timeout: 10000 });
  });

  $('#rec-btn').addEventListener('click', toggleRecording);
  $('#rec-refresh').addEventListener('click', loadRecordings);
  let was = false;
  bus.on('msg:recording', (r) => { renderRec(r); if (was && !r.on) setTimeout(loadRecordings, 600); was = r.on; });
  bus.on('msg:status', renderStatus);
  bus.on('tab', (t) => { if (t === 'hardware') loadRecordings(); });
  loadRecordings();
}
