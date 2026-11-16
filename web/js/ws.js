// ws.js — conexión WebSocket con reconexión automática (retardo exponencial).
// Reparte los mensajes por el bus: 'msg:<type>' para JSON y 'bin:<tipo>' para
// binarios (1 espectro, 2 audio, 3 nivel, 4 barrido, 5 vídeo TV, 6 audio TV). Mantiene S al día.
import { S, bus } from './util.js';

let ws = null, delay = 500, timer = 0;

export function send(obj) {
  if (!ws || ws.readyState !== 1) return false;
  ws.send(JSON.stringify(obj));
  return true;
}
export const isOpen = () => !!ws && ws.readyState === 1;

function onJson(m) {
  switch (m.type) {
    case 'status': S.status = m; break;
    case 'tx_cal': break;                 // solo lo consume tx.js por el bus
    case 'signals': S.signals = m; break;
    case 'recording': S.recording = m; break;
    case 'decoders': S.decoders = m.list || []; break;
    case 'profiles': S.profiles = m.list || []; break;
    case 'dec_list': {
      S.decList = m.list || [];
      const alive = new Set(S.decList.map((i) => i.inst));
      for (const k of Object.keys(S.decState)) if (!alive.has(+k)) delete S.decState[k];
      break;
    }
    case 'dec_state': S.decState[m.inst] = m; break;
    case 'tx_policy': S.txPolicy = m; break;
  }
  bus.emit('msg:' + m.type, m);
}

function onBinary(buf) {
  const t = new Uint8Array(buf, 0, 1)[0];
  if (t === 1) bus.emit('bin:spectrum', buf);
  else if (t === 2 || t === 3) bus.emit('bin:audio', buf);
  else if (t === 4) bus.emit('bin:sweep', buf);
  else if (t === 5) bus.emit('bin:tvvideo', buf);
  else if (t === 6) bus.emit('bin:tvaudio', buf);
}

function open() {
  clearTimeout(timer);
  const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  try { ws = new WebSocket(url); } catch { retry(); return; }
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { delay = 500; S.connected = true; bus.emit('ws', true); };
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      try { onJson(JSON.parse(ev.data)); } catch (e) { console.warn('JSON inválido', e); }
    } else onBinary(ev.data);
  };
  ws.onclose = () => { S.connected = false; bus.emit('ws', false); retry(); };
  ws.onerror = () => { try { ws.close(); } catch { /* nada */ } };
}
function retry() {
  clearTimeout(timer);
  timer = setTimeout(open, delay);
  delay = Math.min(delay * 1.7, 6000);
}
export function connect() { open(); }
