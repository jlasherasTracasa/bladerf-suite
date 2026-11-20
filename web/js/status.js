// status.js — estado del sistema y errores comprensibles (W10).
//  1. Barra de estado única: fuente, enlace USB, tasa, pérdidas, GPU/CPU, carga del DSP,
//     peor decodificador, temperatura, clientes. Cada elemento explica en un texto
//     emergente qué es y qué hacer cuando se pone en amarillo o rojo.
//  2. Avisos no bloqueantes (#notices): cada error conocido del servidor se traduce a un
//     aviso con un botón que lo resuelve («Cambiar a 2 Msps», «Mover centro aquí»…).
//     Los informativos (level:"info") salen en gris. Nunca se enseña el texto crudo sin contexto:
//     el fallback antepone lo que se intentaba hacer (S.lastCmd, lo anota ws.js).
//  3. explainInst(): motivo + acción para decodificadores en «waiting»/«error» (lo usa decoders.js).
//  4. Reconexión: aviso con cuenta atrás y «Reintentar ya»; al volver se restauran canal, modo,
//     audio, decodificadores manuales y, si el servidor se reinició, perfil o centro/tasa.
import { S, bus, $, h, toast, fmtHz } from './util.js';
import { send, reconnectNow } from './ws.js';
import { setChannel, filterEdges, BW_MIN } from './receiver.js';

const dev = () => (S.status && S.status.device) || {};
const canTune = () => dev().can_tune !== false;
const cfg = (o) => send({ cmd: 'config', ...o });
const goTab = (id) => { location.hash = id; };
export function fmtRate(r) {
  if (!(r > 0)) return '—';
  if (r >= 1e6) return `${(r / 1e6).toFixed(2)} Msps`;
  if (r >= 1e3) return `${(r / 1e3).toFixed(1)} ksps`;
  return `${Math.round(r)} sps`;
}
const msps = (v) => `${+(+v).toFixed(3)} Msps`;
/** Centro del hardware que deja f dentro de la banda (a −rate/4 del centro, como hace receiver.js). */
function centerFor(f) {
  const d = dev(), rate = (S.status && S.status.rate) || 2e6;
  let c = Math.round(f - rate / 4);
  if (d.freq_min != null) c = Math.max(d.freq_min, c);
  if (d.freq_max != null) c = Math.min(d.freq_max, c);
  return c;
}

/* ================= avisos ================= */
const notices = new Map();
export function dismissNotice(id) {
  const n = notices.get(id);
  if (!n) return;
  clearTimeout(n.timer); n.el.remove(); notices.delete(id);
}
/** spec: {id, level:'info'|'warn'|'err', title, body, actions:[{label, fn}], ttl (ms; 0 = hasta cerrarlo)} */
export function notice(spec) {
  const box = $('#notices');
  if (!box) { toast(spec.title, spec.level === 'err' ? 'err' : ''); return; }
  const id = spec.id || spec.title;
  dismissNotice(id);
  const level = spec.level || 'warn';
  const el = h('div', { class: 'notice ' + level, role: level === 'err' ? 'alert' : 'status', 'data-id': id },
    h('div', { class: 'nt' }, h('b', { text: spec.title }), spec.body ? h('span', { text: spec.body }) : null),
    h('div', { class: 'na' },
      ...(spec.actions || []).map((a) => h('button', { class: 'btn small' + (a.primary === false ? '' : ' primary'), type: 'button', text: a.label,
        onclick: () => { try { a.fn(); } finally { if (!a.keep) dismissNotice(id); } } })),
      h('button', { class: 'btn small ghost', type: 'button', text: '×', 'aria-label': 'Cerrar aviso', title: 'Cerrar', onclick: () => dismissNotice(id) })));
  box.append(el);
  const ttl = spec.ttl != null ? spec.ttl : { info: 10000, warn: 25000, err: 40000 }[level];
  const n = { el, timer: ttl ? setTimeout(() => dismissNotice(id), ttl) : 0 };
  notices.set(id, n);
  while (notices.size > 3) dismissNotice(notices.keys().next().value);   // como mucho tres a la vez
}

/* ---- traducción de errores del servidor ---- */
const CTX = {
  config: 'Al cambiar la configuración del receptor', channel: 'Al cambiar el canal de audio', profile: 'Al aplicar el perfil',
  dec_add: 'Al añadir el decodificador', dec_remove: 'Al quitar el decodificador', dec_tune: 'Al mover el decodificador',
  record: 'Al grabar IQ', rec_delete: 'Al borrar la grabación', tx: 'En la emisión', tx_stop: 'En la emisión', tx_play: 'Al reproducir la grabación',
  tx_cal: 'En la calibración del TX', sweep: 'En el barrido', sweep_stop: 'En el barrido', tv: 'En la TDT', tv_service: 'En la TDT', tv_scan: 'En la exploración de TDT',
};
const txMode = (v) => { const sel = $('#tx-mode'); if (sel && [...sel.options].some((o) => o.value === v)) sel.value = v; goTab('tx'); };

/** Devuelve {id, level, title, body, actions} para un mensaje de error conocido, o null. */
export function explain(msg) {
  const m = String(msg || '');
  let r;
  if (/falta .*FPGA|Espectro FPGA|espectro FPGA|FPGA lab|imagen lab/.test(m))
    return { id: 'fpga', level: /desactivado|Vuelvo/.test(m) ? 'info' : 'warn', title: 'Espectro de la FPGA', body: m, actions: [{ label: 'Abrir Hardware', fn: () => goTab('hardware') }] };
  if (/desactivada \(arrancar con --enable-tx\)/.test(m))
    return { id: 'tx-off', title: 'La emisión está desactivada en el servidor',
      body: 'Por seguridad solo emite si se arranca con --enable-tx. Mientras tanto, el modo «Fichero» guarda la señal sin radiar nada.',
      actions: [{ label: 'Usar modo Fichero', fn: () => txMode('file') }] };
  if (/por antena desactivada/.test(m))
    return { id: 'tx-air', title: 'La emisión por antena no está activada',
      body: 'Hace falta arrancar el servidor con --enable-tx --enable-tx-air y cumplir la política de bandas. «Fichero» y «Bucle interno» no radian.',
      actions: [{ label: 'Usar modo Fichero', fn: () => txMode('file') }] };
  const gm = m.match(/ganancia (-?\d+) dB por encima del máximo permitido \((-?\d+) dB/);
  if (gm)
    return { id: 'tx-gain', title: 'Ganancia de TX demasiado alta', body: `Pediste ${gm[1]} dB y el máximo permitido es ${gm[2]} dB (--tx-max-gain).`,
      actions: [{ label: `Bajar a ${gm[2]} dB`, fn: () => { const g = $('#tx-gain'); if (g) g.value = gm[2]; goTab('tx'); } }, { label: 'Usar modo Fichero', fn: () => txMode('file'), primary: false }] };
  if (/fuera de las bandas permitidas|no se puede radiar|indicativo|ciclo de trabajo|por orden|misma banda permitida|imagen especular|solo file o loopback/.test(m))
    return { id: 'tx-policy', title: 'Emisión por aire no permitida', body: `La política de transmisión la rechaza: ${m}. Radiar solo es posible en bandas ISM/SRD/radioaficionado y con protocolos permitidos.`,
      actions: [{ label: 'Usar modo Fichero', fn: () => txMode('file') }] };
  if (/esta fuente no puede transmitir|requiere el bladeRF|no funciona con ficheros|fuente de fichero|es un fichero/.test(m))
    return { id: 'src-file', level: 'info', title: 'Esta acción necesita la radio', body: `La fuente actual es un fichero. ${m}`,
      actions: [{ label: 'Ver fuente', fn: () => goTab('hardware') }] };
  if (/demasiadas instancias/.test(m))
    return { id: 'dec-max', title: 'Demasiados decodificadores', body: 'Se alcanzó el máximo de instancias. Quita alguna que no uses.', actions: [{ label: 'Ir a Decodificadores', fn: () => goTab('decoders') }] };
  if (/no se pudo abrir el bladeRF/.test(m))
    return { id: 'open-dev', level: 'err', title: 'No se pudo abrir el bladeRF',
      body: 'Otro programa (p. ej. OpenWebRX) puede tenerlo abierto, o no está conectado. Ciérralo y reintenta.', actions: [{ label: 'Reintentar', fn: () => location.reload() }] };
  if (/No se pudo (sintonizar|fijar la tasa|fijar la ganancia)|fuera de rango|barrido: rango/.test(m))
    return { id: 'hw-range', title: 'El equipo no acepta ese valor', body: m + '. Los límites están en la pestaña Hardware.', actions: [{ label: 'Abrir Hardware', fn: () => goTab('hardware') }] };
  if (/bias-tee/.test(m)) return { id: 'biastee', level: 'info', title: 'Sin bias-tee', body: m, actions: [] };
  if (/perfil desconocido/.test(m)) return { id: 'prof', title: 'Perfil desconocido', body: 'El servidor no conoce ese perfil (¿versión distinta?).', actions: [{ label: 'Ver perfiles', fn: () => goTab('protocolos') }] };
  if (/grabación|grabar/.test(m)) return { id: 'rec', title: 'Problema con la grabación', body: m, actions: [{ label: 'Abrir Hardware', fn: () => goTab('hardware') }] };
  if (/^tv|tv_|TDT|\.ts/.test(m)) return { id: 'tv', title: 'TDT', body: m, actions: [{ label: 'Abrir TV', fn: () => goTab('tv') }] };
  if ((r = m.match(/requiere (exactamente|al menos) ([\d.]+) Msps(?: \(ahora ([\d.]+) Msps\))?/)))
    return { id: 'dec-rate', title: 'Tasa de captura incompatible', body: `El decodificador necesita ${r[1]} ${r[2]} Msps${r[3] ? ` y se captura a ${r[3]}` : ''}.`, actions: rateActions(+r[2]) };
  return null;
}
function rateActions(mhz) {
  if (!canTune()) return [{ label: 'Ver fuente', fn: () => goTab('hardware') }];
  return [{ label: `Cambiar a ${msps(mhz)}`, fn: () => cfg({ rate: Math.round(mhz * 1e6) }) }];
}

function onServerError(m) {
  const info = m.level === 'info';
  if (m.chan_freq) return;   // aviso de perfil con canal movido: lo cuenta protocols.js
  const e = explain(m.msg);
  if (e) { notice({ ...e, level: info ? 'info' : e.level || 'warn' }); return; }
  const c = CTX[S.lastCmd && Date.now() - S.lastCmd.t < 4000 ? S.lastCmd.cmd : ''];
  notice({ id: 'srv:' + m.msg, level: info ? 'info' : 'warn', title: c ? `${c} el servidor respondió:` : 'El servidor avisa:', body: m.msg || 'Error desconocido',
    actions: info ? [] : [{ label: 'Entendido', fn: () => {}, primary: false }] });
}

/* ================= decodificadores en waiting/error ================= */
/** {why, actions[]} que explica por qué una instancia no está en «ok», o null si va bien. */
export function explainInst(i) {
  if (i.state === 'ok') return null;
  const d = S.decoders.find((x) => x.id === i.proto), name = d ? d.name : i.proto, e = String(i.err || '');
  const st = S.status || {}, rate = st.rate || 0, tune = canTune();
  let m;
  const dropBtn = { label: 'Quitar', primary: false, fn: () => send({ cmd: 'dec_remove', inst: i.inst }) };
  if (i.state === 'waiting') {
    if (/sin muestras IQ|FPGA/.test(e))
      return { why: 'Está parado porque el espectro de la FPGA está activo y no hay muestras IQ.', actions: [{ label: 'Volver a CPU/GPU', fn: () => cfg({ spectrum_src: 'cpu' }) }] };
    const lo = st.center - rate / 2, hi = st.center + rate / 2;
    if (/fuera de la banda/.test(e) || (!e && rate && (i.freq < lo || i.freq > hi))) {
      const why = `${fmtHz(i.freq, 4)} queda fuera de la banda capturada (${fmtHz(lo, 3)} – ${fmtHz(hi, 3)}); por eso espera.`;
      if (!tune) return { why: why + ' La fuente es un fichero y no se puede sintonizar.', actions: [dropBtn] };
      return { why, actions: [{ label: `Centrar el hardware en ${fmtHz(i.freq, 3)}`, fn: () => cfg({ center: centerFor(i.freq) }) },
        { label: 'Traer a la banda', primary: false, fn: () => send({ cmd: 'dec_tune', inst: i.inst, freq: Math.round(Math.min(hi - rate * 0.1, Math.max(lo + rate * 0.1, i.freq))) }) }] };
    }
    return { why: `Esperando: ${e || 'sin motivo informado'}.`, actions: [] };
  }
  if ((m = e.match(/requiere (exactamente|al menos) ([\d.]+) Msps(?: \(ahora ([\d.]+) Msps\))?/)))
    return { why: `${name} necesita capturar a ${m[1]} ${m[2]} Msps y ahora se captura a ${m[3] || +(rate / 1e6).toFixed(3)} Msps.`, actions: rateActions(+m[2]) };
  if ((m = e.match(/la tasa de captura \(([\d.]+) Msps\) es menor que la del canal \(([\d.]+) Msps\)/)))
    return { why: `${name} necesita al menos ${m[2]} Msps y ahora se captura a ${m[1]} Msps.`, actions: rateActions(+m[2]) };
  const retry = { label: 'Reintentar', fn: () => { send({ cmd: 'dec_remove', inst: i.inst }); send({ cmd: 'dec_add', proto: i.proto, freq: Math.round(i.freq) }); } };
  return { why: `${name} no pudo arrancar${e ? ': ' + e : ''}. Si se repite, mira el registro del servidor.`, actions: [retry, dropBtn] };
}

/* ================= barra de estado ================= */
const chip = (id, text, cls, title) => {
  const el = $(id); if (!el) return;
  if (el.textContent !== text) el.textContent = text;
  el.className = 'chip' + (el.classList.contains('hide-sm') ? ' hide-sm' : '') + (cls ? ' ' + cls : '');
  if (title) el.title = title;
};
let lastLoss = null, lossAt = -1e9;
const usbOf = (d) => {
  const v = d.usb_speed ?? d.usb ?? null;
  if (v == null) return null;
  if (typeof v === 'number') return v >= 3 ? 3 : 2;
  return /super|3/i.test(String(v)) ? 3 : 2;
};
function renderTop() {
  const up = S.connected;
  const conn = $('#st-conn');
  conn.replaceChildren(h('i', { class: 'dot' + (up ? '' : ' off') }), h('b', { text: up ? 'Conectado' : 'Sin conexión' }));
  conn.title = up ? 'Conexión con el servidor de BladeRF Lab: activa.' : 'Sin conexión con el servidor. La página reintenta sola; mira el aviso superior.';
  const st = S.status;
  if (!st) return;
  const d = st.device || {}, s = st.stats || {}, file = d.kind === 'file';
  // fuente
  chip('#st-dev', file ? `Fichero · ${String(d.board || '').replace(/^Fichero\s+/, '')}` : `Radio · ${d.board || d.kind}${d.serial ? ' · ' + d.serial.slice(-6) : ''}`, file ? 'info' : '',
    file ? 'Fuente: reproducción en bucle de un fichero IQ (--replay). No se puede sintonizar ni cambiar la ganancia; para otra banda usa otro fichero.'
      : `Fuente: radio ${d.board || d.kind}${d.serial ? ', n.º de serie ' + d.serial : ''}${d.fw ? ', FX3 ' + d.fw : ''}${d.fpga ? ', FPGA ' + d.fpga : ''}.`);
  // enlace USB
  const u = usbOf(d);
  chip('#st-usb', file ? 'USB n/a' : u ? `USB ${u}` : 'USB —', !file && u === 2 && st.rate > 11e6 ? 'warn' : '',
    file ? 'Enlace USB: no aplica, la fuente es un fichero.'
      : u === 3 ? 'Enlace USB 3 SuperSpeed: caudal suficiente para toda la banda del equipo.'
        : u === 2 ? 'Enlace USB 2 (Hi-Speed): ≈ 45 MB/s, unos 11 Msps. Por encima de esa tasa habrá pérdidas; usa un cable y un puerto USB 3.'
          : 'Enlace USB: el servidor aún no informa de la velocidad. Compruébalo con «bladeRF-cli -e info»; USB 2 limita a ≈ 11 Msps.');
  // tasa
  const meas = s.rate > 0, rate = meas ? s.rate : st.rate;
  chip('#st-rate', fmtRate(rate), meas ? '' : 'info',
    meas ? `Tasa de muestreo medida: ${Math.round(s.rate)} muestras/s (nominal ${fmtRate(st.rate)}). Si cae por debajo de la nominal, el sistema pierde muestras.`
      : `Tasa nominal ${fmtRate(st.rate)}; la medida real llega en unos 2 s.`);
  // pérdidas: hardware/USB (overruns) + bloques descartados por el DSP (drops)
  const ovr = s.overruns || 0, drp = s.drops || 0, tot = ovr + drp;
  if (lastLoss != null && tot > lastLoss) lossAt = performance.now();
  lastLoss = tot;
  const recent = performance.now() - lossAt < 6000;
  chip('#st-over', tot ? `Pérdidas ${tot}` : 'Pérdidas 0', recent ? 'bad' : tot ? 'warn' : '',
    `Muestras o bloques perdidos desde el arranque: ${ovr} por el hardware/USB (overruns) y ${drp} bloques que el DSP no llegó a procesar. `
    + (recent ? 'Está perdiendo ahora: baja la tasa, quita decodificadores o usa USB 3.' : tot ? 'Hubo pérdidas antes; si no suben, ya está estable.' : 'Sin pérdidas.'));
  // GPU / CPU
  const be = String(s.backend || st.spectrum_src || '').toUpperCase();
  chip('#st-gpu', /GPU|CUDA/.test(be) ? 'GPU' : /FPGA/.test(be) ? 'FPGA' : 'CPU', '',
    `Dónde se calcula el espectro: ${s.backend || 'CPU'}. ${/GPU|CUDA/.test(be) ? 'La GPU libera núcleos de la CPU para los decodificadores.' : 'Con GPU (CUDA) iría más holgado a tasas altas; ver --no-gpu.'}`);
  // carga del DSP
  chip('#st-load', `DSP ${((s.load || 0) * 100).toFixed(0)}%`, s.load > 0.9 ? 'bad' : s.load > 0.7 ? 'warn' : '',
    'Carga del hilo DSP (espectro, detector, canales de audio). Por encima del 90 % se pierden bloques: baja la tasa o la velocidad de la cascada (fps).');
  renderDec();
  chip('#st-temp', s.temp > 0 ? `${s.temp.toFixed(0)} °C` : '— °C', s.temp > 80 ? 'bad' : s.temp > 70 ? 'warn' : '',
    s.temp > 0 ? 'Temperatura del equipo. Por encima de 70 °C conviene ventilarlo.' : 'Temperatura: la fuente no la informa (ficheros).');
  chip('#st-clients', `${s.clients ?? '—'} cli.`, '', 'Navegadores conectados al servidor. Cada uno tiene su propio canal de audio.');
}
/** Peor instancia por carga de CPU + recuento de las que no están en «ok». */
function renderDec() {
  const l = S.decList || [], bad = l.filter((i) => i.state !== 'ok').length;
  let w = null;
  for (const i of l) if (i.state === 'ok' && (!w || i.load > w.load)) w = i;
  const load = w ? w.load : 0, d = w && S.decoders.find((x) => x.id === w.proto);
  const txt = !l.length ? 'Dec. 0' : `Dec. ${l.length}${w ? ` · máx ${(load * 100).toFixed(0)}%` : ''}${bad ? ` · ${bad}⚠` : ''}`;
  chip('#st-dec', txt, load > 0.9 ? 'bad' : load > 0.6 || bad ? 'warn' : '',
    !l.length ? 'Decodificadores en marcha: ninguno. Añade uno en la pestaña Decodificadores o elige un perfil.'
      : `${l.length} decodificadores. Carga de CPU de la peor instancia: ${w ? `#${w.inst} ${d ? d.name : w.proto} al ${(load * 100).toFixed(0)} %` : '—'}`
      + `${bad ? `. ${bad} están esperando o en error: ábrelos en Decodificadores para ver el motivo y la acción.` : '.'} Cada instancia usa su propio núcleo; si pasa del 90 % empezará a descartar bloques.`);
}

/* ================= bandera «FUERA» persistente ================= */
let oobSince = 0;
function checkOob() {
  const oob = S.connected && S.chan.audio && (S.level.flags & 4);
  if (!oob) { oobSince = 0; dismissNotice('oob'); return; }
  const now = performance.now();
  if (!oobSince) oobSince = now;
  if (now - oobSince < 2000 || notices.has('oob')) return;
  const st = S.status; if (!st) return;
  const c = S.chan, [lo, hi] = filterEdges(c), blo = st.center - st.rate / 2, bhi = st.center + st.rate / 2;
  const inside = c.freq >= blo && c.freq <= bhi;
  const actions = [];
  if (inside) actions.push({ label: 'Ajustar el filtro', fn: () => fitChannel(st) });
  else if (canTune()) actions.push({ label: 'Mover centro aquí', fn: () => cfg({ center: centerFor(c.freq) }) });
  else actions.push({ label: 'Ir a la banda del fichero', fn: () => setChannel({ freq: Math.round(st.center), mode: c.mode }, { user: true }) });
  notice({ id: 'oob', level: 'warn', title: 'El canal de audio está fuera de la banda capturada',
    body: inside ? `El filtro (${fmtHz(lo, 1)} – ${fmtHz(hi, 1)}) se sale de la banda ${fmtHz(blo, 1)} – ${fmtHz(bhi, 1)}.` : `${fmtHz(c.freq, 3)} no está en ${fmtHz(blo, 3)} – ${fmtHz(bhi, 3)}.`, actions, ttl: 0 });
}
/** Estrecha el filtro del canal (y pasa de WFM a NFM si hace falta) hasta que quepa en la banda. */
export function fitChannel(st) {
  const c = S.chan, lim = 0.49 * st.rate, lo = st.center - lim, hi = st.center + lim;
  if (c.freq < lo || c.freq > hi) return false;
  let max = c.mode === 'usb' ? hi - c.freq : c.mode === 'lsb' ? c.freq - lo : 2 * Math.min(c.freq - lo, hi - c.freq);
  max = Math.floor(max);
  if (c.bw <= max) return false;
  if (max < BW_MIN) return false;
  if (c.mode === 'wfm' && max < 100000) setChannel({ mode: 'nfm', bw: Math.min(12500, max) });
  else setChannel({ bw: max });
  return true;
}

/* ================= reconexión y restauración de sesión ================= */
const keep = { chan: null, status: null, decs: [], opts: new Map(), down: false, ever: false, pend: false };
/** Lo llama decoders.js al añadir una instancia manual: guarda las opciones (dec_list no las trae). */
export function rememberDecoder(proto, freq, opts) { if (opts) keep.opts.set(proto + '@' + Math.round(freq), opts); }

let connTimer = 0;
function onDown() {
  if (connTimer) return;
  keep.down = true; keep.chan = { ...S.chan }; keep.status = S.status;
  const tick = () => {
    connTimer = 0;
    if (S.connected) return;
    const secs = Math.max(0, Math.ceil(((S.retry && S.retry.at) - Date.now()) / 1000));
    const body = `Reintentando${secs ? ` en ${secs} s` : ' ahora'} (intento ${S.retry ? S.retry.n : 1}). Tu canal y tus decodificadores se restaurarán al volver.`;
    const n = notices.get('conn');
    if (n) n.el.querySelector('.nt span').textContent = body;   // se actualiza en sitio: el botón no parpadea
    else notice({ id: 'conn', level: 'err', title: 'Sin conexión con el servidor', body, actions: [{ label: 'Reintentar ya', keep: true, fn: () => reconnectNow() }], ttl: 0 });
    connTimer = setTimeout(tick, 1000);
  };
  tick();
}
function onUp() {
  clearTimeout(connTimer); connTimer = 0;
  dismissNotice('conn');
  renderTop();
  if (!keep.down) { keep.ever = true; return; }
  keep.down = false; keep.pend = true;
}
/** Tras volver: primer dec_list (el servidor ya envió status, perfiles y decodificadores). */
function restoreSession() {
  if (!keep.pend) return;
  keep.pend = false;
  const prev = keep.status, now = S.status, notes = [];
  let profileDone = false;
  if (prev && now && prev.profile && !now.profile) { send({ cmd: 'profile', id: prev.profile }); profileDone = true; notes.push('perfil'); }
  else if (prev && now && canTune() && (prev.center !== now.center || prev.rate !== now.rate) && now.profile == null) {
    cfg({ center: prev.center, rate: prev.rate }); notes.push('centro y tasa');
  }
  for (const k of keep.decs) {
    if (S.decList.some((i) => i.owner === 0 && i.proto === k.proto && Math.abs(i.freq - k.freq) < 1)) continue;
    if (profileDone && k.owner === 1) continue;
    const m = { cmd: 'dec_add', proto: k.proto, freq: Math.round(k.freq) };
    const o = keep.opts.get(k.proto + '@' + Math.round(k.freq)); if (o) m.opts = o;
    send(m); notes.push(k.proto);
  }
  if (keep.chan) { setChannel({ freq: keep.chan.freq, mode: keep.chan.mode, bw: keep.chan.bw, squelch: keep.chan.squelch, deemph: keep.chan.deemph, stereo: keep.chan.stereo, audio: keep.chan.audio }); }
  toast(`Conectado de nuevo al servidor${notes.length ? ' · restaurado: canal, ' + notes.join(', ') : ' · canal restaurado'}`, 'ok');
}
function snapshotDecs(m) {
  if (S.connected && !keep.pend) keep.decs = (m.list || []).filter((i) => i.owner === 0).map((i) => ({ proto: i.proto, freq: i.freq, owner: 0 }));
}

export function initStatus() {
  bus.on('ws', (up) => { if (up) onUp(); else { onDown(); renderTop(); } });
  bus.on('msg:status', renderTop);
  bus.on('msg:dec_list', (m) => { if (keep.pend) restoreSession(); else snapshotDecs(m); renderDec(); });
  bus.on('msg:error', onServerError);
  bus.on('msg:tx_result', (m) => { if (m && m.ok === false) { const e = explain(m.msg); if (e) notice(e); } });
  // el perfil activo cambia el centro/tasa: el aviso de «fuera de banda» ya no vale
  bus.on('msg:status', () => { if (S.status) fitChannel(S.status); });
  setInterval(checkOob, 500);
  const tb = $('.topbar'), setH = () => document.documentElement.style.setProperty('--topbar-h', tb.offsetHeight + 'px');
  setH(); if (window.ResizeObserver) new ResizeObserver(setH).observe(tb);
  renderTop();
}
