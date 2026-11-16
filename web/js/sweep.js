// sweep.js — controles del barrido rápido de espectro (mensaje binario tipo 4).
// El dibujo lo hace spectrum.js (mismo lienzo); aquí solo están la barra de
// control (inicio, fin, FFT, iniciar/parar), el estado que viene en status.sweep
// y la acción «Sintonizar aquí» sobre un punto elegido con un clic.
// Mientras se barre el servidor no hace espectro normal, audio ni decodificación:
// la interfaz lo dice en la barra y no ofrece sintonizar el canal de audio.
import { S, bus, $, fmtHz, parseHz, fillSelect, toast } from './util.js';
import { send } from './ws.js';

const FFTS = [256, 512, 1024, 2048, 4096, 8192];
let picked = null, touched = false;

const isSweeping = () => !!(S.status && S.status.sweep);

function render() {
  const st = S.status;
  if (!st) return;
  const d = st.device, on = isSweeping(), sw = st.sweep;
  const tg = $('#sw-toggle');
  tg.disabled = false;   // se puede abrir para leer por qué no está disponible
  $('#sw-go').disabled = !d.can_tune;
  tg.title = d.can_tune ? 'Barrido rápido de espectro por toda la banda' : 'No disponible: una fuente de fichero no se puede barrer (necesita un bladeRF)';
  if (on && $('#sweepbar').hidden) setOpen(true);
  tg.classList.toggle('primary', on);
  $('#sw-go').textContent = on ? 'Reiniciar' : 'Iniciar'; if (!d.can_tune) $('#sw-go').title = 'No disponible con una fuente de fichero';
  $('#sw-stop').disabled = !on;
  if (!touched && !on) {
    $('#sw-start').value = fmtHz(Math.max(d.freq_min, 70e6), 3);
    $('#sw-end').value = fmtHz(Math.min(d.freq_max, 6e9), 3);
  }
  if (on) {
    $('#sw-start').value = fmtHz(sw.start, 3); $('#sw-end').value = fmtHz(sw.stop, 3); $('#sw-fft').value = String(sw.fft);
    const pct = S.sweepPct != null ? ` · ${S.sweepPct} % de la pasada` : '';
    $('#sw-info').textContent = `Barriendo: ${sw.hops} saltos a ${(sw.rate / 1e6).toFixed(2)} Msps, ${fmtHz(sw.rate / sw.fft, 1)}/bin, modo ${sw.mode === 'quick' ? 'rápido' : 'lento'}, ${sw.sweeps} pasadas${sw.restarts ? `, ${sw.restarts} resincronizaciones` : ''}${pct}. Sin audio ni decodificadores mientras tanto.`;
  } else {
    $('#sw-info').textContent = d.can_tune ? 'Parado. Recorre la banda saltando de frecuencia (pocos segundos por pasada).' : 'No disponible con una fuente de fichero: el barrido necesita un bladeRF que pueda saltar de frecuencia.';
  }
  const t = $('#sw-tune');
  t.hidden = picked == null;
  if (picked != null) t.textContent = `Sintonizar ${fmtHz(picked, 3)}`;
}

function setOpen(open) {
  $('#sweepbar').hidden = !open;
  $('#sw-toggle').setAttribute('aria-pressed', String(open));
}

function start() {
  const a = parseHz($('#sw-start').value), b = parseHz($('#sw-end').value);
  if (!a || !b || b <= a) { toast('Rango de barrido no válido (inicio < fin)', 'err'); return; }
  if (!send({ cmd: 'sweep', start: a, stop: b, fft: +$('#sw-fft').value })) toast('Sin conexión con el servidor', 'err');
}

export function initSweep() {
  fillSelect($('#sw-fft'), FFTS.map((n) => [n, String(n)]), 1024);
  $('#sw-toggle').addEventListener('click', () => {
    const open = $('#sweepbar').hidden;
    setOpen(open);
    if (!open && isSweeping()) send({ cmd: 'sweep_stop' });   // cerrar la barra con el barrido en marcha lo para
  });
  $('#sw-go').addEventListener('click', start);
  $('#sw-stop').addEventListener('click', () => send({ cmd: 'sweep_stop' }));
  for (const id of ['#sw-start', '#sw-end', '#sw-fft']) $(id).addEventListener('input', () => { touched = true; });
  for (const id of ['#sw-start', '#sw-end']) $(id).addEventListener('keydown', (e) => { if (e.key === 'Enter') start(); });
  $('#sw-tune').addEventListener('click', () => {
    if (picked == null) return;
    send({ cmd: 'sweep_stop' });
    send({ cmd: 'config', center: picked });
    toast(`Sintonizado en ${fmtHz(picked, 3)}`, 'ok');
    picked = null; render();
  });
  bus.on('sweep-pick', (f) => { picked = f; render(); });
  bus.on('msg:status', render);
  bus.on('bin:sweep', () => { if (isSweeping()) render(); });
}
