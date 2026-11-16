#!/usr/bin/env node
// web_mock.js — servidor de pruebas de la interfaz web (Node puro, sin npm).
//
// Sirve web/ con las mismas cabeceras de seguridad que src/net/server.c y habla
// el protocolo WebSocket de docs/PROTOCOLO.md, el que implementa src/net/server.c
// (saludo RFC 6455 con http + crypto): espectro binario con ruido y señales, audio
// (tonos), signals, decoders, profiles, dec_list, dec de varios protocolos,
// dec_state con tablas (aviones, barcos, estaciones APRS, nodos, sondas, RDS) y
// tx_policy, enviados en el mismo orden que el servidor real al conectar. Registra las órdenes recibidas (consola y GET /_log).
//
// Uso: node tests/web_mock.js [puerto]      (por defecto 8099)
'use strict';
const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PORT = +(process.argv[2] || process.env.PORT || 8099);
const WEB = path.join(__dirname, '..', 'web');
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const CSP = "default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; connect-src 'self' ws: wss:; style-src 'self' 'unsafe-inline'; worker-src 'self' blob:";
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon' };

/* ---------------- estado simulado ---------------- */
const received = [];                       // órdenes recibidas
const status = {
  type: 'status', app: 'BladeRF Lab', version: '2.0.0',
  device: { kind: 'bladerf', board: 'bladeRF 2.0 micro (mock)', serial: 'MOCK0123456789ABCDEF', fw: '2.6.0', fpga: '0.16.0', lib: '2.6.1',
    freq_min: 70000000, freq_max: 6000000000, rate_min: 520834, rate_max: 61440000, bw_min: 200000, bw_max: 56000000, gain_min: -15, gain_max: 60,
    can_tune: true, biastee: true, can_tx: true },
  center: 100000000, rate: 2400000, rf_bw: 2000000, gain_mode: 'auto', gain: 30, gain_now: 38, biastee: false, fft: 2048, fps: 20, det_snr: 8,
  profile: 'fm', loc: { lat: 40.4168, lon: -3.7038 },
  stats: { rate: 2400000, overruns: 0, temp: 47.5, load: 0.21, clients: 1, sent: 0, backend: 'CPU' },
};
const DEC = (id, name, group, desc, kind, rate, bw, wr, wmin, freqs, tx, air) => ({ id, name, group, desc, kind, rate, bw, wide_rate: wr, wide_min_rate: wmin, freqs, tx, air_ok: air });
const decoders = [
  DEC('adsb', 'ADS-B / Mode S', 'Aviación', 'Aeronaves en 1090 MHz', 'wide', 0, 0, 2000000, 0, [1090000000], true, false),
  DEC('acars', 'ACARS', 'Aviación', 'Mensajes de datos aeronáuticos', 'narrow', 48000, 12500, 0, 0, [131525000, 131725000, 131825000], true, false),
  DEC('vdl2', 'VDL Modo 2', 'Aviación', 'D8PSK 10500 Bd', 'narrow', 105000, 25000, 0, 0, [136975000, 136875000], true, false),
  DEC('rds', 'RDS', 'Radiodifusión', 'Datos en FM comercial', 'narrow', 250000, 100000, 0, 0, [100300000], true, false),
  DEC('dab', 'DAB / DAB+', 'Radiodifusión', 'Conjunto, servicios y subcanales', 'wide', 0, 0, 2048000, 0, [227360000], true, false),
  DEC('ais', 'AIS', 'Marítimo', 'Barcos GMSK 9600', 'narrow', 48000, 12500, 0, 0, [161975000, 162025000], true, false),
  DEC('dsc', 'DSC VHF', 'Marítimo', 'Llamada selectiva digital canal 70', 'narrow', 48000, 12500, 0, 0, [156525000], true, false),
  DEC('aprs', 'APRS', 'Radioafición', 'AX.25 AFSK 1200', 'narrow', 48000, 12500, 0, 0, [144800000], true, true),
  DEC('pocsag', 'POCSAG', 'Buscapersonas', '512/1200/2400 bps', 'narrow', 48000, 12500, 0, 0, [466075000], true, false),
  DEC('flex', 'FLEX', 'Buscapersonas', '1600/3200/6400', 'narrow', 48000, 12500, 0, 0, [466075000], true, false),
  DEC('dmr', 'DMR', 'Radio digital', 'Solo metadatos', 'narrow', 48000, 12500, 0, 0, [], true, false),
  DEC('ism', 'ISM genérico', 'ISM', 'Sensores, TPMS, mandos', 'narrow', 250000, 200000, 0, 0, [433920000, 868300000], true, true),
  DEC('rs41', 'Radiosonda RS41', 'Meteorología', 'Posición y PTU', 'narrow', 48000, 12500, 0, 0, [403000000], true, false),
  DEC('lora', 'LoRa / LoRaWAN', 'LoRa', 'SF7-12', 'narrow', 500000, 150000, 0, 0, [868100000, 868300000, 868500000], true, true),
  DEC('meshtastic', 'Meshtastic', 'LoRa', 'Clave pública por defecto', 'narrow', 500000, 150000, 0, 0, [869525000], true, true),
  DEC('lte', 'LTE', 'Celular', 'Búsqueda de celda', 'narrow', 1920000, 1400000, 0, 0, [], true, false),
  DEC('nr', '5G NR', 'Celular', 'SSB en la rejilla GSCN', 'wide', 0, 0, 0, 15360000, [3500000000], true, false),
  DEC('gsm', 'GSM', 'Celular', 'FCCH, SCH', 'narrow', 1083333, 200000, 0, 0, [], true, false),
  DEC('wifi', 'WiFi beacons', 'Redes', 'SSID, BSSID, canal', 'wide', 0, 0, 20000000, 0, [2437000000], true, false),
  DEC('dvbt', 'DVB-T', 'Radiodifusión', 'TPS, modulación', 'narrow', 9142857, 8000000, 0, 0, [474000000], false, false),
];
const P = (id, name, group, center, rate, mode, cf, bw, dd, so) => ({ id, name, group, center, rate, mode, chan_freq: cf, bw, decoders: dd || [], spectrum_only: !!so });
const profiles = [
  P('fm', 'FM comercial', 'Radiodifusión', 100000000, 2400000, 'wfm', 100300000, 180000, [{ proto: 'rds', freq: 100300000, follow: true, auto: false }]),
  P('airband', 'Banda aérea', 'Aviación', 127200000, 2400000, 'am', 127500000, 9000),
  P('adsb', 'ADS-B', 'Aviación', 1090000000, 2000000, null, 0, 0, [{ proto: 'adsb', freq: 1090000000, follow: false, auto: false }]),
  P('acars', 'ACARS', 'Aviación', 131300000, 1000000, 'am', 131725000, 9000, [{ proto: 'acars', freq: 131525000 }, { proto: 'acars', freq: 131725000 }]),
  P('vdl2', 'VDL2', 'Aviación', 136600000, 1000000, null, 0, 0, [{ proto: 'vdl2', freq: 136975000 }]),
  P('maritimo', 'VHF marina', 'Marítimo', 159300000, 8000000, 'nfm', 156800000, 12500, [{ proto: 'ais', freq: 161975000 }, { proto: 'ais', freq: 162025000 }, { proto: 'dsc', freq: 156525000 }]),
  P('dab', 'DAB / DAB+', 'Radiodifusión', 227360000, 2048000, null, 0, 0, [{ proto: 'dab', freq: 227360000 }]),
  P('busca', 'Buscapersonas', 'Servicios', 466300000, 1000000, 'nfm', 466075000, 12500, [{ proto: 'pocsag', freq: 466075000 }, { proto: 'flex', freq: 466075000 }]),
  P('aprs', 'APRS 2 m', 'Radioafición', 145000000, 1000000, 'nfm', 144800000, 12500, [{ proto: 'aprs', freq: 144800000 }]),
  P('ham70', '70 cm', 'Radioafición', 433000000, 2400000, 'nfm', 433500000, 12500),
  P('dmr', 'DMR 70 cm', 'Radioafición', 438500000, 4000000, null, 0, 0),
  P('ism433', 'ISM 433', 'ISM', 433420000, 2000000, null, 0, 0, [{ proto: 'ism', freq: 433920000 }]),
  P('ism868', 'ISM 868', 'ISM', 868000000, 2000000, null, 0, 0, [{ proto: 'ism', freq: 868300000 }, { proto: 'ism', freq: 868950000 }]),
  P('lora', 'LoRaWAN EU868', 'ISM', 867700000, 2000000, null, 0, 0, [{ proto: 'lora', freq: 868100000 }, { proto: 'lora', freq: 868300000 }]),
  P('sonda', 'Radiosondas', 'Meteorología', 403000000, 6000000, null, 0, 0, [{ proto: 'rs41', freq: 403000000, auto: true }]),
  P('lte', 'LTE banda 20', 'Celular', 806000000, 20000000, null, 0, 0, [], true),
  P('wifi', 'WiFi 2,4 GHz', 'Redes', 2437000000, 20000000, null, 0, 0, [{ proto: 'wifi', freq: 2437000000 }]),
];
// Instancias de ejemplo. El mock da por buenas las manuales para poder ver mapa y tablas aunque
// su frecuencia esté fuera de la banda; el servidor real las marcaría «waiting». La del ADS-B
// (banda ancha en 1090 MHz) cae fuera de la captura de FM y sirve para probar que la web no la pinta.
let decList = [
  { inst: 1, proto: 'rds', freq: 100300000, rate: 250000, owner: 1, state: 'ok', err: '', events: 14, drops: 0, load: 0.04 },
  { inst: 2, proto: 'adsb', freq: 1090000000, rate: 2000000, owner: 0, state: 'waiting', err: '', events: 0, drops: 0, load: 0 },
  { inst: 3, proto: 'pocsag', freq: 466075000, rate: 48000, owner: 0, state: 'ok', err: '', events: 0, drops: 0, load: 0 },
  { inst: 4, proto: 'ais', freq: 100100000, rate: 48000, owner: 0, state: 'ok', err: '', events: 0, drops: 0, load: 0.01 },
  { inst: 5, proto: 'aprs', freq: 100500000, rate: 48000, owner: 0, state: 'ok', err: '', events: 0, drops: 0, load: 0.01 },
  { inst: 6, proto: 'meshtastic', freq: 100600000, rate: 500000, owner: 0, state: 'ok', err: '', events: 0, drops: 0, load: 0.01 },
  { inst: 8, proto: 'acars', freq: 131525000, rate: 48000, owner: 0, state: 'ok', err: '', events: 0, drops: 0, load: 0.01 },
  { inst: 7, proto: 'rs41', freq: 100700000, rate: 48000, owner: 0, state: 'ok', err: '', events: 0, drops: 0, load: 0.01 },
];
let nextInst = 9;
const txPolicy = { type: 'tx_policy', enabled: true, air: true, callsign: 'EA4XYZ', max_gain: 0,
  bands: [[433050000, 434790000, 'ISM 433'], [868000000, 868600000, 'SRD 868'], [144000000, 146000000, 'VHF 2 m (indicativo)']] };
let recFiles = [{ name: 'iq_20261115_101500.sigmf-data', size: 48e6, mtime: Date.now() / 1000 - 3600 }, { name: 'iq_20261115_101500.sigmf-meta', size: 812, mtime: Date.now() / 1000 - 3600 }];
let rec = { type: 'recording', on: false, name: '', bytes: 0, seconds: 0, dropped: 0 };
const recent = [];                       // últimos eventos dec

/* ---------------- ADS-B simulado ---------------- */
const planes = Array.from({ length: 9 }, (_, i) => ({
  icao: (0x34a000 + i * 4097).toString(16).toUpperCase().padStart(6, '0'), cs: ['IBE', 'RYR', 'VLG', 'AEA', 'EZY', 'TAP', 'AFR', 'DLH', 'BAW'][i] + (100 + i * 37),
  lat: 40.4 + (Math.random() - 0.5) * 2.2, lon: -3.7 + (Math.random() - 0.5) * 3, alt: 8000 + Math.random() * 30000, gs: 250 + Math.random() * 250, trk: Math.random() * 360,
  vr: Math.round((Math.random() - 0.5) * 3000), sq: String(1000 + Math.floor(Math.random() * 6000)), msgs: 10,
}));
planes[3].sq = '7700';
function adsbState() {
  const R = Math.PI / 180;
  for (const p of planes) {
    const d = (p.gs * 1.852 / 3600) / 111; p.lat += Math.cos(p.trk * R) * d; p.lon += Math.sin(p.trk * R) * d / Math.cos(p.lat * R);
    p.trk = (p.trk + (Math.random() - 0.5) * 3 + 360) % 360; p.msgs += 3 + Math.floor(Math.random() * 5);
  }
  return { msgs: planes.reduce((s, p) => s + p.msgs, 0), rate: 120.5, rx: true, ac: planes.map((p) => ({
    icao: p.icao, cs: p.cs, cat: 'A3', alt: Math.round(p.alt), gs: +p.gs.toFixed(1), trk: +p.trk.toFixed(1), vr: p.vr, sq: p.sq, emerg: p.sq === '7700' ? 1 : 0,
    lat: +p.lat.toFixed(5), lon: +p.lon.toFixed(5), seen_pos: 0.8,
    gnd: false, msgs: p.msgs, seen: +(Math.random() * 3).toFixed(1), rssi: -20 - Math.random() * 15, adsb: true })) };
}

/* ---------------- estados con tablas de otros protocolos ---------------- */
// Objetos con lat/lon (+ rumbo y nombre) bajo claves distintas por protocolo: la web no debe depender de ellas.
const mover = (n, lat0, lon0, spread, names, speed) => Array.from({ length: n }, (_, i) => ({ id: i, nm: names[i % names.length] + (i + 1), lat: lat0 + (Math.random() - 0.5) * spread, lon: lon0 + (Math.random() - 0.5) * spread, hd: Math.random() * 360, sp: speed * (0.5 + Math.random()) }));
const fleets = { ais: mover(6, 40.2, -3.3, 0.6, ['MV ', 'SS '], 12), aprs: mover(5, 40.4, -3.7, 0.5, ['EA4', 'EC4'], 30), meshtastic: mover(4, 40.45, -3.6, 0.2, ['!a1b2c', '!d4e5f'], 0), rs41: mover(2, 40.3, -3.9, 0.3, ['T1230', 'S4567'], 20) };
function stepFleet(f) {
  const R = Math.PI / 180;
  for (const o of f) { const d = (o.sp * 1.852 / 3600) / 111; o.lat += Math.cos(o.hd * R) * d; o.lon += Math.sin(o.hd * R) * d / Math.cos(o.lat * R); o.hd = (o.hd + (Math.random() - 0.5) * 6 + 360) % 360; }
}
const STATE_GEN = {
  adsb: adsbState,
  rds: () => ({ pi: '0xE2A1', ps: 'RADIO 3', rt: 'Ahora suena una canción de prueba', pty: 10, pty_name: 'Pop', tp: true, ta: false, ms: true, af: [98.1, 99.3, 100.3, 101.7], ct: '2026-11-15 22:10', groups: 153 }),
  ais: () => { stepFleet(fleets.ais); return { msgs: 410, vessels: fleets.ais.map((o) => ({ mmsi: 224000000 + o.id * 1111, name: o.nm, lat: +o.lat.toFixed(5), lon: +o.lon.toFixed(5), cog: +o.hd.toFixed(1), sog: +o.sp.toFixed(1), type: 70, seen: +(Math.random() * 20).toFixed(0) })) }; },
  aprs: () => { stepFleet(fleets.aprs); return { packets: 55, stations: fleets.aprs.map((o) => ({ call: o.nm + '-9', lat: +o.lat.toFixed(4), lon: +o.lon.toFixed(4), course: +o.hd.toFixed(0), speed: +o.sp.toFixed(0), symbol: '>', comment: 'móvil', seen: 12 })) }; },
  meshtastic: () => ({ packets: 9, nodes: fleets.meshtastic.map((o) => ({ id: o.nm, long_name: 'Nodo ' + o.nm, lat: +o.lat.toFixed(5), lon: +o.lon.toFixed(5), snr: +(Math.random() * 10 - 3).toFixed(1), battery: 80 })) }),
  rs41: () => { stepFleet(fleets.rs41); return { frames: 300, sondes: fleets.rs41.map((o) => ({ serial: o.nm, lat: +o.lat.toFixed(5), lon: +o.lon.toFixed(5), alt: 12000 + Math.round(Math.random() * 100), hdg: +o.hd.toFixed(0), temp: -31.5, hum: 12 })) }; },
};
const stateMsg = (i) => (i.state === 'ok' && STATE_GEN[i.proto] ? { type: 'dec_state', inst: i.inst, proto: i.proto, t: Date.now() / 1000, state: STATE_GEN[i.proto]() } : null);

/* ---------------- eventos de ejemplo ---------------- */
const rnd = (a) => a[Math.floor(Math.random() * a.length)];
const SAMPLES = {
  rds: () => ({ pi: '0xE2A1', ps: rnd(['RADIO 3', 'COPE', 'RNE 1']), pty: 'Pop', rt: 'Ahora suena una canción', text: 'PS=RADIO 3  RT=Ahora suena una canción' }),
  pocsag: () => ({ addr: 1234567, func: 0, baud: 1200, kind: 'alpha', message: 'Urgencia: acudir al punto de encuentro habitual, llamada ' + Math.floor(Math.random() * 99), text: 'POCSAG 1234567 alfa: Urgencia: acudir al punto de encuentro habitual' }),
  meshtastic: () => ({ from: '!a1b2c3', to: '^all', ch: 0, text: 'Mensaje de texto Meshtastic número ' + Math.floor(Math.random() * 99) + ' desde el repetidor de la sierra' }),
  ais: () => ({ type: 1, mmsi: 224000000 + Math.floor(Math.random() * 999999), lat: 40.1 + Math.random() * 0.1, lon: -3.2, sog: +(Math.random() * 12).toFixed(1), text: 'AIS tipo 1 MMSI 224xxxxxx' }),
  acars: () => ({ reg: '.EC-MXY', label: 'H1', flight: 'IB3141', msg_text: 'POSICION 4010N00350W FL380 ETA 1432', text: 'ACARS H1 EC-MXY IB3141: POSICION 4010N00350W FL380 ETA 1432' }),
  ism: () => ({ model: 'Nexus-TH', id: 42, temp_c: +(15 + Math.random() * 10).toFixed(1), hum: 55, text: 'Nexus-TH id 42 ' }),
  aprs: () => ({ src: 'EA4XYZ-9', dst: 'APRS', path: 'WIDE1-1', lat: 40.41, lon: -3.7, text: 'EA4XYZ-9>APRS: posición 40.41N 3.70W' }),
};
function emitDec() {
  const inst = rnd(decList.filter((i) => SAMPLES[i.proto] && i.state === 'ok'));
  if (!inst) return;
  const msg = SAMPLES[inst.proto]();
  if (msg.text && inst.proto === 'ism') msg.text += msg.temp_c + ' °C';
  inst.events++; inst.load = +(Math.random() * 0.2).toFixed(3);
  const ev = { type: 'dec', inst: inst.inst, proto: inst.proto, freq: inst.freq, t: Date.now() / 1000, msg };
  recent.push(ev); if (recent.length > 100) recent.shift();
  broadcastJson(ev);
}

/* ---------------- WebSocket mínimo ---------------- */
const clients = new Set();
function frame(op, payload) {
  const len = payload.length;
  const head = len < 126 ? Buffer.from([0x80 | op, len]) : len < 65536 ? Buffer.from([0x80 | op, 126, len >> 8, len & 255]) : (() => { const b = Buffer.alloc(10); b[0] = 0x80 | op; b[1] = 127; b.writeBigUInt64BE(BigInt(len), 2); return b; })();
  return Buffer.concat([head, payload]);
}
const sendText = (c, obj) => c.sock.writable && c.sock.write(frame(1, Buffer.from(JSON.stringify(obj))));
const sendBin = (c, buf) => c.sock.writable && c.sock.write(frame(2, buf));
const broadcastJson = (o) => { for (const c of clients) sendText(c, o); };

function parseFrames(c) {
  for (;;) {
    const b = c.buf;
    if (b.length < 2) return;
    const op = b[0] & 15; let len = b[1] & 127, pos = 2;
    if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); pos = 4; }
    else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); pos = 10; }
    const masked = !!(b[1] & 128);
    if (!masked) { c.sock.destroy(); return; }               // RFC 6455: el cliente siempre enmascara
    if (b.length < pos + 4 + len) return;
    const mask = b.subarray(pos, pos + 4), data = Buffer.from(b.subarray(pos + 4, pos + 4 + len));
    for (let i = 0; i < len; i++) data[i] ^= mask[i & 3];
    c.buf = b.subarray(pos + 4 + len);
    if (op === 1) { try { onCommand(c, JSON.parse(data.toString('utf8'))); } catch (e) { console.log('JSON inválido', e.message); } }
    else if (op === 8) { c.sock.end(frame(8, Buffer.alloc(0))); return; }
    else if (op === 9) c.sock.write(frame(10, data));
  }
}

function sendSnapshot(c) {
  sendText(c, status);
  sendText(c, { type: 'signals', center: status.center, noise: -96, list: sigList() });
  sendText(c, rec);
  sendText(c, { type: 'decoders', list: decoders });
  sendText(c, { type: 'profiles', list: profiles });
  sendText(c, txPolicy);
  sendText(c, { type: 'dec_list', list: decList });
  for (const i of decList) { const m = stateMsg(i); if (m) sendText(c, m); }
  for (const e of recent) sendText(c, { ...e, replay: true });
}

/* ---------------- órdenes ---------------- */
function onCommand(c, m) {
  received.push({ t: Date.now(), ...m });
  console.log('<-', JSON.stringify(m));
  switch (m.cmd) {
    case 'config':
      for (const k of ['center', 'rate', 'rf_bw', 'gain_mode', 'gain', 'biastee', 'fft', 'fps', 'det_snr']) if (m[k] !== undefined) status[k] = m[k];
      if (m.rate !== undefined) status.stats.rate = m.rate;
      if (m.loc !== undefined) status.loc = m.loc;
      if (m.center !== undefined || m.rate !== undefined) status.profile = null;
      broadcastJson(status); broadcastJson({ type: 'signals', center: status.center, noise: -96, list: sigList() });
      break;
    case 'channel': c.chan = { ...c.chan, ...m }; break;
    case 'profile': {
      const p = profiles.find((x) => x.id === m.id);
      if (!p) { sendText(c, { type: 'error', msg: 'perfil desconocido: ' + m.id }); break; }
      status.center = p.center; status.rate = p.rate; status.rf_bw = Math.min(p.rate, 56e6); status.profile = p.id; status.stats.rate = p.rate;
      decList = decList.filter((i) => i.owner !== 1);
      for (const d of p.decoders) decList.push({ inst: nextInst++, proto: d.proto, freq: d.freq, rate: 48000, owner: 1, state: 'ok', err: '', events: 0, drops: 0, load: 0.02 });
      broadcastJson(status); broadcastJson({ type: 'dec_list', list: decList });
      break;
    }
    case 'dec_add': {
      const d = decoders.find((x) => x.id === m.proto);
      if (!d) { sendText(c, { type: 'error', msg: 'protocolo desconocido' }); break; }
      const lo = status.center - status.rate / 2, hi = status.center + status.rate / 2;
      const state = d.kind === 'wide' && d.wide_rate && d.wide_rate !== status.rate ? 'error' : m.freq < lo || m.freq > hi ? 'waiting' : 'ok';
      decList.push({ inst: nextInst++, proto: m.proto, freq: m.freq, rate: d.rate, owner: 0, state, err: state === 'error' ? `requiere exactamente ${d.wide_rate / 1e6} Msps` : '', events: 0, drops: 0, load: 0 });
      broadcastJson({ type: 'dec_list', list: decList }); break;
    }
    case 'dec_remove': decList = decList.filter((i) => i.inst !== m.inst); broadcastJson({ type: 'dec_list', list: decList }); break;
    case 'dec_tune': { const i = decList.find((x) => x.inst === m.inst); if (i) i.freq = m.freq; broadcastJson({ type: 'dec_list', list: decList }); break; }
    case 'record':
      rec = m.on ? { type: 'recording', on: true, name: 'iq_mock_' + Date.now() + '.sigmf-data', bytes: 0, seconds: 0, dropped: 0 } : { ...rec, on: false };
      if (!m.on) recFiles.unshift({ name: rec.name, size: rec.bytes, mtime: Date.now() / 1000 });
      broadcastJson(rec); break;
    case 'rec_delete': recFiles = recFiles.filter((f) => f.name !== m.name); break;
    case 'tx':
      setTimeout(() => sendText(c, { type: 'tx_result', ok: m.mode !== 'air' || m.gain <= txPolicy.max_gain, msg: m.mode === 'air' && m.gain > txPolicy.max_gain ? 'ganancia por encima del máximo' : `modo ${m.mode}: ${m.proto} generado (simulado)`,
        file: m.mode === 'file' ? `recordings/tx_${m.proto}_mock.sigmf-data` : null, seconds: 1.234 }), 300);
      break;
    case 'tx_stop': sendText(c, { type: 'tx_result', ok: true, msg: 'emisión detenida', file: null, seconds: 0 }); break;
  }
}

/* ---------------- generadores ---------------- */
const SIGS = [[-0.8e6, 180e3, -45], [-0.3e6, 15e3, -62], [0.15e6, 200e3, -40], [0.55e6, 12.5e3, -70], [0.9e6, 9e3, -75]];
const sigList = () => SIGS.map(([off, bw, p]) => [off, bw, p + (Math.random() - 0.5), +(p + 96).toFixed(1)]);
let tick = 0;
function spectrumFrame() {
  const n = status.fft, buf = Buffer.alloc(32 + n), rate = status.rate, dbmin = -130, dbstep = 0.5;
  buf[0] = 1; buf.writeUInt32LE(n, 4); buf.writeDoubleLE(status.center, 8); buf.writeDoubleLE(rate, 16); buf.writeFloatLE(dbmin, 24); buf.writeFloatLE(dbstep, 28);
  const fade = 1 + Math.sin(tick / 15) * 0.15;
  for (let i = 0; i < n; i++) {
    const off = ((i + 0.5) / n - 0.5) * rate;
    let p = Math.pow(10, (-96 + (Math.random() - 0.5) * 5) / 10);
    for (const [so, bw, sp] of SIGS) {
      const x = (off - so) / (bw / 2);
      if (Math.abs(x) < 1.5) p += Math.pow(10, (sp + (Math.random() - 0.5) * 4) / 10) * Math.exp(-x * x * 3) * fade;
    }
    if (Math.abs(off) < rate / n * 2.5) p = Math.pow(10, -96 / 10);          // pico central suprimido
    buf[32 + i] = Math.max(0, Math.min(255, Math.round((10 * Math.log10(p) - dbmin) / dbstep)));
  }
  return buf;
}
function audioFrames(c) {
  const ch = c.chan || {}; if (!ch.audio) return;
  const stereo = ch.mode === 'wfm' && ch.stereo !== false ? 2 : 1, frames = 960;
  const buf = Buffer.alloc(8 + frames * stereo * 2);
  buf[0] = 2; buf[1] = 1 | (stereo === 2 ? 2 | 8 : 0); buf[2] = stereo; buf.writeFloatLE(-48 + Math.sin(tick / 9) * 6, 4);
  for (let i = 0; i < frames; i++) {
    const t = (c.phase = (c.phase || 0) + 1) / 48000;
    const l = Math.sin(2 * Math.PI * 440 * t) * 0.25 * (0.6 + 0.4 * Math.sin(2 * Math.PI * 2 * t));
    const r = Math.sin(2 * Math.PI * 660 * t) * 0.25;
    buf.writeInt16LE(Math.round(l * 32767), 8 + i * stereo * 2);
    if (stereo === 2) buf.writeInt16LE(Math.round(r * 32767), 8 + i * 4 + 2);
  }
  sendBin(c, buf);
}
function meterFrame(c) {
  const b = Buffer.alloc(8); b[0] = 3; b[1] = 0; b[2] = 1; b.writeFloatLE(-85 + Math.random() * 20, 4); sendBin(c, b);
}

let specTimer = null;
function schedule() {
  clearInterval(specTimer);
  specTimer = setInterval(() => {
    tick++;
    if (!clients.size) return;
    const sp = spectrumFrame();
    for (const c of clients) { sendBin(c, sp); }
    if (rec.on) { rec.bytes += status.rate * 4 / status.fps; rec.seconds += 1 / status.fps; }
  }, 1000 / (status.fps || 20));
}
schedule();
setInterval(() => { // 50 fps de audio = 960 muestras/20 ms (aprox.)
  for (const c of clients) { if (c.chan && c.chan.audio) audioFrames(c); }
}, 20);
setInterval(() => { for (const c of clients) if (!(c.chan && c.chan.audio)) meterFrame(c); }, 120);
setInterval(() => {
  status.stats.load = +(0.2 + Math.random() * 0.1).toFixed(3); status.stats.temp = +(47 + Math.random() * 2).toFixed(1); status.stats.clients = clients.size;
  if (Math.random() < 0.05) status.stats.overruns++;
  broadcastJson(status);
  broadcastJson({ type: 'signals', center: status.center, noise: -96, list: sigList() });
  broadcastJson({ type: 'dec_list', list: decList });
  for (const i of decList) { const m = stateMsg(i); if (m) broadcastJson(m); }
  if (rec.on) broadcastJson(rec);
}, 1000);
setInterval(() => { if (clients.size) emitDec(); }, 1800);
setInterval(schedule, 5000);

/* ---------------- HTTP ---------------- */
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x'), p = decodeURIComponent(url.pathname);
  const base = { 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': CSP, 'Cache-Control': 'no-cache' };
  const json = (o, code = 200) => { res.writeHead(code, { ...base, 'Content-Type': 'application/json' }); res.end(JSON.stringify(o)); };
  if (p === '/api/status') return json(status);
  if (p === '/api/recordings') return json({ files: recFiles });
  if (p === '/_log') return json(received);
  if (p.startsWith('/recordings/')) {
    const f = recFiles.find((x) => x.name === p.slice(12));
    if (!f) return json({ error: 'no existe' }, 404);
    res.writeHead(200, { ...base, 'Content-Type': 'application/octet-stream', 'Content-Disposition': `attachment; filename="${f.name}"` });
    return res.end(Buffer.alloc(1024));
  }
  let rel = p === '/' ? '/index.html' : p;
  const file = path.normalize(path.join(WEB, rel));
  if (!file.startsWith(WEB + path.sep) || rel.includes('..')) return json({ error: 'prohibido' }, 403);
  fs.readFile(file, (err, data) => {
    if (err) return json({ error: 'no encontrado' }, 404);
    res.writeHead(200, { ...base, 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
});
server.on('upgrade', (req, sock) => {
  if (new URL(req.url, 'http://x').pathname !== '/ws' || !req.headers['sec-websocket-key']) { sock.destroy(); return; }
  const acc = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + GUID).digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${acc}\r\n\r\n`);
  const c = { sock, buf: Buffer.alloc(0), chan: { audio: false } };
  clients.add(c);
  sock.setNoDelay(true);
  sock.on('data', (d) => { c.buf = Buffer.concat([c.buf, d]); parseFrames(c); });
  const gone = () => clients.delete(c);
  sock.on('close', gone); sock.on('error', gone);
  sendSnapshot(c);
});
server.listen(PORT, '127.0.0.1', () => console.log(`web_mock escuchando en http://127.0.0.1:${PORT}/  (órdenes recibidas: /_log)`));
