// map.js — mapa en canvas sin teselas con una capa por protocolo.
//
// Dibuja CUALQUIER objeto de un array de «dec_state» que tenga latitud y longitud
// (aviones, barcos, estaciones APRS, nodos Meshtastic, sondas…), sin conocer los
// nombres de campo de cada protocolo: se buscan por una lista de alias.
//
// Proyección equirrectangular local alrededor del receptor (o, sin ubicación, del
// centroide de los puntos):
//     x = (lon − lon0)·cos(lat0)·111,32 km/°      y = (lat − lat0)·110,57 km/°
// Error < 1 % hasta ~400 km. Norte arriba; anillos con paso 1-2-5.
import { S, bus, $, h, clamp, throttle } from './util.js';
import { autoTable, fmtVal, isTable } from './tables.js';

const LAT = ['lat', 'latitude'], LON = ['lon', 'lng', 'long', 'longitude'];
const HDG = ['trk', 'cog', 'hdg', 'heading', 'course', 'track'];
const NAME = ['cs', 'callsign', 'call', 'name', 'long_name', 'short_name', 'ssid', 'shortname', 'id', 'mmsi', 'icao', 'addr', 'node', 'serial', 'sn'];
const KEY = ['icao', 'mmsi', 'id', 'addr', 'node', 'serial', 'sn', 'call', 'callsign', 'cs', 'name'];
const COLORS = ['#8fcdff', '#ffd37d', '#8ed6a5', '#ffb4ab', '#d0bcff', '#f2a6e8', '#9fe3e0', '#f4b27a'];
const first = (o, keys) => { for (const k of keys) if (o[k] != null && o[k] !== '') return o[k]; return null; };
// Color por altitud (ft): del naranja (suelo) al violeta (FL400), pasando por amarillo, verde y azul.
const altColor = (ft) => `hsl(${Math.round(30 + 250 * clamp(ft / 40000, 0, 1))} 85% 62%)`;
const num = (v) => (typeof v === 'number' && isFinite(v) ? v : null);

let cv, cx, W = 0, H = 0, dpr = 1;
let layers = [];                                  // [{id, proto, inst, key, rows, color}]
const hidden = new Set();                         // capas ocultas
const trails = new Map();                         // clave de objeto -> [[lat,lon],…]
const view = { dx: 0, dy: 0, range: 0 };         // desplazamiento (km) y radio visible (km); 0 = auto
let selected = null;                              // {layer, k}
let hits = [];
let labelSlots = new Map();                       // casillas de etiqueta ocupadas en este dibujo                                    // puntos pintados para el clic

const pos = (o) => { const la = num(first(o, LAT)), lo = num(first(o, LON)); return la != null && lo != null && Math.abs(la) <= 90 && Math.abs(lo) <= 180 ? [la, lo] : null; };
const rowKey = (o, i) => { const k = first(o, KEY); return k != null ? String(k) : '#' + i; };
const label = (o) => { const n = first(o, NAME); return n == null ? '' : String(n); };

/** Reconstruye las capas a partir de S.decState (una por protocolo; si un protocolo tiene varios arrays, uno por array). */
function collect() {
  const byProto = new Map();
  for (const s of Object.values(S.decState)) {
    for (const [k, rows] of Object.entries(s.state || {})) {
      if (!isTable(rows) || !rows.some((r) => pos(r))) continue;
      if (!byProto.has(s.proto)) byProto.set(s.proto, []);
      byProto.get(s.proto).push({ inst: s.inst, key: k, rows });
    }
  }
  const out = [];
  for (const [proto, arr] of byProto) {
    for (const a of arr) out.push({ id: proto + (arr.length > 1 ? ':' + a.key : '') + (arr.filter((x) => x.key === a.key).length > 1 ? '#' + a.inst : ''), proto, ...a });
  }
  out.sort((a, b) => a.id.localeCompare(b.id));
  out.forEach((l, i) => { l.color = COLORS[i % COLORS.length]; });
  return out;
}

function origin() {
  const loc = S.status && S.status.loc;
  if (loc) return { lat: loc.lat, lon: loc.lon, rx: true };
  const p = [];
  for (const l of layers) if (!hidden.has(l.id)) for (const r of l.rows) { const q = pos(r); if (q) p.push(q); }
  if (!p.length) return null;
  return { lat: p.reduce((s, a) => s + a[0], 0) / p.length, lon: p.reduce((s, a) => s + a[1], 0) / p.length, rx: false };
}
const proj = (o, lat, lon) => ({ x: (lon - o.lon) * Math.cos((o.lat * Math.PI) / 180) * 111.32, y: (lat - o.lat) * 110.57 });
const niceStep = (r) => { const p = Math.pow(10, Math.floor(Math.log10(r / 3))), m = r / 3 / p; return (m < 1.5 ? 1 : m < 3.5 ? 2 : m < 7.5 ? 5 : 10) * p; };

function visibleRange(o) {
  if (view.range) return view.range;
  let m = 0;
  for (const l of layers) if (!hidden.has(l.id)) for (const r of l.rows) { const q = pos(r); if (q) { const p = proj(o, q[0], q[1]); m = Math.max(m, Math.hypot(p.x, p.y)); } }
  return clamp(m * 1.15, 2, 3000);
}

function drawMap() {
  if (!cv || !cv.offsetParent) return;
  const c = cx; c.setTransform(dpr, 0, 0, dpr, 0, 0);
  c.fillStyle = '#0b0a0e'; c.fillRect(0, 0, W, H);
  const o = origin();
  c.font = '11px ui-monospace, monospace'; c.textAlign = 'center'; c.textBaseline = 'middle';
  hits = []; labelSlots = new Map();
  if (!o) {
    c.fillStyle = 'rgba(255,255,255,.55)'; c.fillText('Sin posiciones todavía: ningún decodificador ha publicado lat/lon', W / 2, H / 2); return;
  }
  const R = visibleRange(o), pxkm = Math.min(W, H) / 2 / R, cxp = W / 2 - view.dx * pxkm, cyp = H / 2 + view.dy * pxkm;
  const X = (x) => cxp + x * pxkm, Y = (y) => cyp - y * pxkm;
  // anillos de distancia
  const st = niceStep(R);
  c.strokeStyle = 'rgba(208,188,255,.18)'; c.fillStyle = 'rgba(208,188,255,.6)'; c.lineWidth = 1;
  for (let r = st; r < R * 3; r += st) { c.beginPath(); c.arc(cxp, cyp, r * pxkm, 0, 7); c.stroke(); if (cyp - r * pxkm > 8) c.fillText(+r.toFixed(2) + ' km', cxp, cyp - r * pxkm - 7); }
  c.strokeStyle = 'rgba(208,188,255,.12)'; c.beginPath(); c.moveTo(0, cyp); c.lineTo(W, cyp); c.moveTo(cxp, 0); c.lineTo(cxp, H); c.stroke();
  c.fillStyle = 'rgba(255,255,255,.6)'; c.fillText('N', W - 14, 14);
  // receptor
  c.fillStyle = o.rx ? '#8ed6a5' : 'rgba(255,255,255,.3)'; c.beginPath(); c.arc(cxp, cyp, 4, 0, 7); c.fill();
  if (o.rx) { c.strokeStyle = '#8ed6a5'; c.lineWidth = 1; c.beginPath(); c.arc(cxp, cyp, 8, 0, 7); c.stroke(); }
  // capas
  for (const l of layers) {
    if (hidden.has(l.id)) continue;
    l.rows.forEach((r, i) => {
      const q = pos(r); if (!q) return;
      const k = rowKey(r, i), tk = l.id + '|' + k, t = trails.get(tk);
      // Pista que publica el decodificador (ADS-B: [antigüedad, lat, lon, alt]): un tramo por par de puntos, coloreado por altitud.
      if (Array.isArray(r.trail) && r.trail.length > 1) {
        const pts = [...r.trail.filter((e) => Array.isArray(e) && num(e[1]) != null && num(e[2]) != null), q.length ? [0, q[0], q[1], num(r.alt)] : null].filter(Boolean);
        c.lineWidth = 1.8; c.globalAlpha = 0.75;
        for (let j = 1; j < pts.length; j++) {
          const p0 = proj(o, pts[j - 1][1], pts[j - 1][2]), p1 = proj(o, pts[j][1], pts[j][2]), al = num(pts[j][3]);
          c.strokeStyle = al != null ? altColor(al) : l.color;
          c.beginPath(); c.moveTo(X(p0.x), Y(p0.y)); c.lineTo(X(p1.x), Y(p1.y)); c.stroke();
        }
        c.globalAlpha = 1;
      } else if (t && t.length > 1) {
        c.strokeStyle = l.color; c.globalAlpha = 0.4; c.lineWidth = 1.2; c.beginPath();
        t.forEach(([la, lo], j) => { const p = proj(o, la, lo); j ? c.lineTo(X(p.x), Y(p.y)) : c.moveTo(X(p.x), Y(p.y)); });
        c.stroke(); c.globalAlpha = 1;
      }
      const p = proj(o, q[0], q[1]), x = X(p.x), y = Y(p.y), hd = num(first(r, HDG)), sel = selected && selected.layer === l.id && selected.k === k;
      const seen = num(r.seen), old = seen != null && seen > 60;        // dato antiguo: se atenúa
      c.save(); c.translate(x, y); c.globalAlpha = old ? 0.4 : 1;
      c.fillStyle = num(r.alt) != null && r.pos_src ? altColor(r.alt) : l.color; c.beginPath();
      if (hd != null) { c.rotate((hd * Math.PI) / 180); c.moveTo(0, -8); c.lineTo(5, 6); c.lineTo(0, 3); c.lineTo(-5, 6); } else c.arc(0, 0, 4, 0, 7);
      c.closePath(); c.fill();
      if (sel) { c.strokeStyle = '#fff'; c.lineWidth = 1.5; c.stroke(); }
      c.restore();
      const nm = label(r);
      if (nm && (sel || R < 150 || layers.length === 1)) {
        // Objetos en la misma posición: las etiquetas se apilan hacia abajo (y se marca «+n» en el primero).
        const key = Math.round(x / 10) + ',' + Math.round(y / 10), n = (labelSlots.get(key) || 0);
        labelSlots.set(key, n + 1);
        c.fillStyle = sel ? '#fff' : '#e6e0e9'; c.textAlign = 'left'; c.fillText(nm, x + 8, y - 6 + n * 12);
      }
      hits.push({ x, y, layer: l.id, k, row: r });
    });
  }
  c.textAlign = 'center';
}

function updateTrails() {
  const seen = new Set();
  for (const l of layers) l.rows.forEach((r, i) => {
    const q = pos(r); if (!q) return;
    const tk = l.id + '|' + rowKey(r, i); seen.add(tk);
    let t = trails.get(tk); if (!t) trails.set(tk, (t = []));
    const e = t[t.length - 1];
    if (!e || e[0] !== q[0] || e[1] !== q[1]) { t.push(q); if (t.length > 40) t.shift(); }
  });
  for (const k of trails.keys()) if (!seen.has(k)) trails.delete(k);
}

function renderLegend() {
  const box = $('#mp-layers');
  if (!layers.length) { box.replaceChildren(h('span', { class: 'muted', text: 'Sin capas: hace falta un decodificador que publique objetos con lat/lon (ADS-B, AIS, APRS, Meshtastic, radiosondas…).' })); return; }
  box.replaceChildren(...layers.map((l) => {
    const n = l.rows.filter((r) => pos(r)).length;
    return h('label', { class: 'chk layer' },
      h('input', { type: 'checkbox', checked: !hidden.has(l.id) ? true : null, onchange: (e) => { if (e.target.checked) hidden.delete(l.id); else hidden.add(l.id); drawMap(); } }),
      h('i', { class: 'sw', style: `background:${l.color}` }), h('span', { text: `${l.id} (${n})` }));
  }));
}

function renderSelected() {
  const box = $('#mp-sel');
  let row = null;
  if (selected) { const l = layers.find((x) => x.id === selected.layer); if (l) l.rows.forEach((r, i) => { if (rowKey(r, i) === selected.k) row = r; }); }
  if (!row) { box.replaceChildren(h('span', { class: 'muted', text: 'Haz clic en un objeto del mapa o de una tabla para ver todos sus campos.' })); return; }
  const src = row.pos_src ? h('div', { class: 'muted', text: `Posición ${row.pos_src === 'local' ? 'local (un solo mensaje CPR + posición del receptor)' : 'global (par par/impar)'}` + (num(row.seen_pos) != null ? ` · hace ${Math.round(row.seen_pos)} s` : '') + (Array.isArray(row.trail) ? ` · pista de ${row.trail.length} puntos` : '') }) : null;
  box.replaceChildren(...(src ? [src] : []), h('div', { class: 'kv' }, Object.entries(row).filter(([k]) => !k.startsWith('_') && k !== 'trail').flatMap(([k, v]) => [h('span', { class: 'k', text: k }), h('span', { class: 'v mono', text: fmtVal(v) })])));
}

function renderTables() {
  const box = $('#mp-tables');
  if (!layers.length) { box.replaceChildren(); return; }
  const open = new Set([...box.querySelectorAll('details[open]')].map((d) => d.dataset.id));
  const firstTime = !box.firstChild;
  box.replaceChildren(...layers.map((l) => h('details', { 'data-id': l.id, open: firstTime || open.has(l.id) ? true : null },
    h('summary', {}, h('i', { class: 'sw', style: `background:${l.color}` }), ` ${l.id} · ${l.key} (${l.rows.length})`),
    autoTable(l.rows, { id: 'map:' + l.id, limit: 300,
      isSel: (r, i) => !!selected && selected.layer === l.id && selected.k === rowKey(r, i),
      onRow: (r, i) => { selected = { layer: l.id, k: rowKey(r, i) }; renderSelected(); renderTables(); drawMap(); } }))));
}

function refresh() {
  layers = collect();
  for (const id of [...hidden]) if (!layers.some((l) => l.id === id)) hidden.delete(id);
  updateTrails();
  const n = layers.reduce((s, l) => s + l.rows.filter((r) => pos(r)).length, 0);
  $('#mp-sum').textContent = layers.length ? `${n} objetos · ${layers.length} capas` : '';
  const loc = S.status && S.status.loc;
  $('#mp-note').textContent = loc ? '' : 'Sin ubicación del receptor: el mapa se centra en los objetos. Indícala en Hardware para ver distancias.';
  renderLegend(); renderSelected(); renderTables(); drawMap();
}
// Con el panel oculto no se redibuja nada (tablas de cientos de filas cada segundo): se recalcula al abrirlo.
const mapVisible = () => { const p = document.getElementById('p-mapa'); return !p || p.style.display !== 'none'; };
const throttledRefresh = throttle(() => { if (mapVisible()) refresh(); }, 800);

function layout() {
  const r = cv.parentElement.getBoundingClientRect();
  dpr = Math.min(devicePixelRatio || 1, 2); W = Math.floor(r.width); H = Math.floor(r.height);
  cv.width = W * dpr; cv.height = H * dpr; drawMap();
}

export function initMap() {
  cv = $('#mp-map'); cx = cv.getContext('2d');
  new ResizeObserver(layout).observe(cv.parentElement);
  let drag = null;
  cv.addEventListener('pointerdown', (e) => { cv.setPointerCapture(e.pointerId); drag = { x: e.clientX, y: e.clientY, dx: view.dx, dy: view.dy, moved: false }; });
  cv.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const o = origin(); if (!o) return;
    const pxkm = Math.min(W, H) / 2 / visibleRange(o);
    if (Math.hypot(e.clientX - drag.x, e.clientY - drag.y) > 4) drag.moved = true;
    if (drag.moved) { if (!view.range) view.range = visibleRange(o); view.dx = drag.dx - (e.clientX - drag.x) / pxkm; view.dy = drag.dy + (e.clientY - drag.y) / pxkm; drawMap(); }
  });
  cv.addEventListener('pointerup', (e) => {
    if (drag && !drag.moved) {
      const r = cv.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
      let best = null, bd = 18;
      for (const p of hits) { const d = Math.hypot(p.x - x, p.y - y); if (d < bd) { bd = d; best = p; } }
      selected = best ? { layer: best.layer, k: best.k } : null; renderSelected(); renderTables(); drawMap();
    }
    drag = null;
  });
  cv.addEventListener('wheel', (e) => { e.preventDefault(); const o = origin(); if (!o) return; view.range = clamp(visibleRange(o) * (e.deltaY < 0 ? 0.8 : 1.25), 0.2, 5000); drawMap(); }, { passive: false });
  cv.addEventListener('dblclick', () => { view.dx = view.dy = 0; view.range = 0; drawMap(); });
  cv.addEventListener('keydown', (e) => { const o = origin(); if (!o) return; if (e.key === '+') view.range = visibleRange(o) * 0.8; else if (e.key === '-') view.range = visibleRange(o) * 1.25; else return; drawMap(); });
  bus.on('msg:dec_state', throttledRefresh);
  bus.on('msg:dec_list', throttledRefresh);
  bus.on('msg:status', throttledRefresh);
  bus.on('tab', (t) => { if (t === 'mapa') { layout(); refresh(); } });
  refresh();
}
