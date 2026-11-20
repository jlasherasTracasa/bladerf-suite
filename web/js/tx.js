// tx.js — panel de emisión. Solo aparece si el servidor anuncia tx_policy.enabled.
// Los modos: «file» escribe SigMF (siempre seguro), «loopback» usa el bucle
// interno del RFIC (nada sale por la antena) y «air» radia de verdad: solo se
// ofrece si la política lo permite y el modulador es radiable (air_ok).
import { S, bus, $, h, fmtHz, parseHz, fillSelect, toast } from './util.js';
import { send } from './ws.js';

const cur = () => S.decoders.find((d) => d.id === $('#tx-proto').value);

function renderPolicy() {
  const p = S.txPolicy, panel = $('#p-tx');
  const on = !!(p && p.enabled);
  const changed = panel.hidden === on;
  panel.hidden = !on;
  if (changed) bus.emit('tabs-changed');
  if (!on) return;
  const bands = (p.bands || []).map(([lo, hi, n]) => `${n}: ${fmtHz(lo, 3)}–${fmtHz(hi, 3)}`).join('; ');
  $('#tx-pol').textContent = `Aire: ${p.air ? 'permitido' : 'desactivado'} · Indicativo: ${p.callsign || '(ninguno)'} · Ganancia máx.: ${p.max_gain} dB` + (bands ? ` · Bandas: ${bands}` : '');
  fillProtos();
}
function fillProtos() {
  const sel = $('#tx-proto'), keep = sel.value;
  const list = S.decoders.filter((d) => d.tx);
  fillSelect(sel, list.map((d) => [d.id, `${d.name} (${d.group})`]));
  if (keep && list.some((d) => d.id === keep)) sel.value = keep;
  onProto();
}
/* Centro y tasa efectivos del fichero: los mismos que calcula el envío (la frecuencia cabe en la banda → se conserva el centro; si no, se centra en ella).
 * Se rellenan al elegir protocolo/frecuencia mientras la persona no los haya tocado a mano. */
function syncCenterRate(force) {
  const rate = (S.status && S.status.rate) || 2e6, freq = parseHz($('#tx-freq').value);
  const cEl = $('#tx-center'), rEl = $('#tx-rate');
  if (force || cEl.dataset.auto === '1') {
    const c0 = S.status ? S.status.center : null;
    const c = freq && (c0 == null || Math.abs(freq - c0) >= 0.4 * rate) ? freq : c0;
    if (c) { cEl.value = fmtHz(c, 6); cEl.dataset.auto = '1'; }
  }
  if (force || rEl.dataset.auto === '1') { rEl.value = fmtHz(rate, 3); rEl.dataset.auto = '1'; }
}
function onProto() {
  const d = cur(), p = S.txPolicy;
  if (!d) return;
  const modes = [['file', 'Fichero SigMF (seguro)'], ['loopback', 'Bucle interno (sin antena)']];
  if (p && p.air && d.air_ok) modes.push(['air', 'Aire (radia por la antena)']);
  const changed = $('#tx-proto').dataset.last !== d.id;
  const prev = $('#tx-mode').value;
  fillSelect($('#tx-mode'), modes, modes.some((m) => m[0] === prev) ? prev : 'file');
  if (!$('#tx-freq').value || changed) $('#tx-freq').value = fmtHz((d.freqs && d.freqs[0]) || S.chan.freq, 6);
  $('#tx-proto').dataset.last = d.id;
  syncCenterRate(changed);
  if (p) $('#tx-gain').max = p.max_gain;
}

function result(ok, text) { const r = $('#tx-result'); r.className = 'result ' + (ok ? 'ok' : 'err'); r.textContent = text; }

/* ---- tx_play: reproducir una grabación (solo file y loopback; nunca air) ---- */
async function loadRecs() {
  const sel = $('#tp-name'), keep = sel.value;
  try {
    const j = await (await fetch('/api/recordings', { cache: 'no-store' })).json();
    const names = j.files.map((f) => f.name).filter((n) => n.endsWith('.sigmf-meta')).map((n) => n.slice(0, -'.sigmf-meta'.length));
    fillSelect(sel, names.length ? names.map((n) => [n, n]) : [['', '(no hay grabaciones)']]);
    if (keep && names.includes(keep)) sel.value = keep;
  } catch { fillSelect(sel, [['', '(no se pudo leer la lista)']]); }
}
function playRec() {
  const name = $('#tp-name').value, mode = $('#tp-mode').value;
  if (!name) { toast('Elige una grabación', 'err'); return; }
  if (mode !== 'file' && mode !== 'loopback') return;            // el servidor tampoco lo aceptaría
  result(true, 'Enviando orden…');
  if (!send({ cmd: 'tx_play', name, mode, repeat: Math.max(1, +$('#tp-repeat').value || 1) })) result(false, 'Sin conexión con el servidor');
}

/* ---- tx_cal ---- */
function calResult(ok, text) { const r = $('#tc-result'); r.className = 'result ' + (ok ? 'ok' : 'err'); r.textContent = text; }
function runCal() {
  const freq = parseHz($('#tc-freq').value), rate = parseHz($('#tc-rate').value);
  if (!freq || !rate) { toast('Frecuencia o tasa no válidas', 'err'); return; }
  if (!confirm('La calibración sintoniza el receptor un momento y emite en el bucle interno (sin antena). ¿Continuar?')) return;
  calResult(true, 'Calibrando…');
  if (!send({ cmd: 'tx_cal', freq, rate })) calResult(false, 'Sin conexión con el servidor');
}
function onCal(m) {
  const f = (x) => (typeof x === 'number' ? x.toFixed(1) : '—');
  calResult(!!m.ok, `${m.msg || ''}\nImagen: ${f(m.irr_before_db)} → ${f(m.irr_after_db)} dB · Fuga de LO: ${f(m.lo_before_dbc)} → ${f(m.lo_after_dbc)} dBc${m.saved ? '\nCalibración guardada.' : ''}`);
}

export function initTx() {
  $('#tp-send').addEventListener('click', playRec);
  $('#tp-refresh').addEventListener('click', loadRecs);
  $('#tc-run').addEventListener('click', runCal);
  bus.on('msg:tx_cal', onCal);
  bus.on('tab', (t) => { if (t === 'tx') loadRecs(); });
  bus.on('msg:recording', (r) => { if (r && !r.on) setTimeout(loadRecs, 700); });
  loadRecs();
  $('#tx-proto').addEventListener('change', onProto);
  $('#tx-freq').addEventListener('input', () => syncCenterRate(false));
  for (const id of ['#tx-center', '#tx-rate']) $(id).addEventListener('input', (e) => { if (e.isTrusted) e.target.dataset.auto = '0'; });
  $('#tx-send').addEventListener('click', () => {
    const d = cur(); if (!d) return;
    const mode = $('#tx-mode').value, freq = parseHz($('#tx-freq').value);
    if (!freq) { toast('Frecuencia no válida', 'err'); return; }
    let params = null; const t = $('#tx-params').value.trim();
    if (t) { try { params = JSON.parse(t); } catch (e) { toast('JSON de parámetros no válido: ' + e.message, 'err'); return; } }
    if (mode === 'air' && !confirm(`Vas a RADIAR por la antena en ${fmtHz(freq, 4)}. Comprueba que tienes autorización y que la antena es adecuada. ¿Emitir?`)) return;
    const m = { cmd: 'tx', proto: d.id, mode, freq, gain: +$('#tx-gain').value, repeat: Math.max(1, +$('#tx-repeat').value || 1) };
    const c = parseHz($('#tx-center').value), r = parseHz($('#tx-rate').value);
    if (mode === 'file') {
      // Si la frecuencia no cabe en la banda indicada (por defecto la capturada ahora), se centra la captura sintética en ella.
      const rr = r || (S.status && S.status.rate) || 2e6;
      m.rate = rr; m.center = c && Math.abs(freq - c) < 0.4 * rr ? c : freq;
    }
    if (params) m.params = params;
    result(true, 'Enviando orden…');
    if (!send(m)) result(false, 'Sin conexión con el servidor');
  });
  $('#tx-stop').addEventListener('click', () => { send({ cmd: 'tx_stop' }); result(true, 'Parada solicitada'); });
  bus.on('msg:status', () => syncCenterRate(false));
  bus.on('msg:tx_policy', renderPolicy);
  bus.on('msg:decoders', () => { if (S.txPolicy && S.txPolicy.enabled) fillProtos(); });
  bus.on('msg:tx_result', (m) => {
    result(!!m.ok, `${m.msg || ''}${m.file ? `\nFichero: ${m.file}` : ''}${m.seconds ? `\nDuración: ${(+m.seconds).toFixed(2)} s` : ''}`);
    if (!m.ok || !m.file) return;
    // Enlaces para descargar la señal generada y la orden para decodificarla de punta a punta con --replay.
    const base = String(m.file).split('/').pop().replace(/\.sigmf-(meta|data)$/, ''), r = $('#tx-result'), proto = ($('#tx-proto') || {}).value || '';
    const cmd = `build/bladerf-lab --replay recordings/${base}.sigmf-meta --decode ${proto}@${Math.round(parseHz($('#tx-freq').value) || 0)} --print`;
    r.append(h('div', { class: 'row wrap', style: 'margin-top:6px;gap:8px' },
      h('a', { href: `/recordings/${base}.sigmf-meta`, download: `${base}.sigmf-meta`, text: 'Descargar metadatos' }),
      h('a', { href: `/recordings/${base}.sigmf-data`, download: `${base}.sigmf-data`, text: 'Descargar datos IQ' }),
      h('button', { class: 'btn small', type: 'button', text: 'Copiar orden para decodificarlo', title: cmd,
        onclick: () => { try { navigator.clipboard.writeText(cmd); toast('Orden copiada', 'ok'); } catch { toast(cmd); } } })));
  });
  renderPolicy();
}
