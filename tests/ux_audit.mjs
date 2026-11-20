#!/usr/bin/env node
// ux_audit.mjs — auditoría de uso medible de la interfaz web (plan fase 4b, W7).
// Ejecutable aparte (no forma parte de `make webtest`, pero se puede enganchar: `node tests/ux_audit.mjs --enforce`).
//
// Qué hace: guioniza 12 tareas reales como las haría una persona (solo con ratón y teclado, a través de
// Chromium sin cabeza por CDP) contra servidores `bladerf-lab --replay` con señales sintéticas de `--gen`
// (FM con RDS, AIS, ADS-B…) y mide, por tarea:
//   - clics, pulsaciones de tecla (caracteres tecleados), arrastres y ruedas;
//   - tiempo real hasta que el criterio de éxito se cumple (sondeo cada 80 ms; incluye la latencia del servidor);
//   - tiempo estimado de una persona con el modelo KLM (clic 1,1 s, tecla 0,28 s, arrastre 1,5 s, 1,2 s de
//     pensar por cada cambio de pestaña/panel);
//   - si hizo falta escribir números a mano, errores o callejones sin salida (avisos de error, excepciones,
//     controles desactivados, mensajes que no explican nada);
//   - captura del estado final en build/w7/Tnn_*.png.
// Con `--enforce` compara con los OBJETIVOS (columna «obj.» de docs/calidad/UX.md) y devuelve 1 si alguna
// tarea los supera o falla: así W8–W14 pueden engancharla a webtest cuando lleguen a su meta.
//
// Puertos propios: servidores 8811…8830, Chromium CDP 9811. Opciones: --only T05,T09 · --out <dir> · --enforce · --keep.
// Resultado: build/w7/ux_audit.json (+ tabla por stdout). Requiere Node >= 22, sin dependencias.
import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
process.env.BIN = process.env.BIN || path.join(ROOT, 'build/w7/bladerf-lab');
process.env.CHROME = process.env.CHROME || path.join(process.env.HOME || '', '.cache/ms-playwright/chromium-1234/chrome-linux64/chrome');
const E = await import('./web_e2e.mjs');
const { Browser, waitHttp, launch, killAll, makeData } = E;
const sleep = E.sleep;

const argv = {}; { const a = process.argv.slice(2); for (let i = 0; i < a.length; i++) if (a[i].startsWith('--')) argv[a[i].slice(2)] = a[i + 1] && !a[i + 1].startsWith('--') ? a[++i] : true; }
const OUT = path.resolve(argv.out || path.join(ROOT, 'build/w7'));
const BIN = process.env.BIN;
const ONLY = argv.only ? String(argv.only).split(',') : null;

/* ---------- objetivos numéricos (W8–W14): acciones = clics + arrastres + pulsaciones de tecla sueltas; teclas = caracteres ---------- */
// act: máximo de acciones (clics+arrastres+ruedas+teclas de orden); chars: máximo de caracteres tecleados; klm: segundos estimados.
const OBJ = {
  T01: { act: 4, chars: 0, klm: 12 }, T02: { act: 3, chars: 0, klm: 8 }, T03: { act: 2, chars: 0, klm: 6 },
  T04: { act: 2, chars: 0, klm: 6 }, T05: { act: 2, chars: 0, klm: 6 }, T06: { act: 4, chars: 0, klm: 10 },
  T07: { act: 4, chars: 0, klm: 10 }, T08: { act: 2, chars: 0, klm: 4 }, T09: { act: 3, chars: 0, klm: 8 },
  T10: { act: 3, chars: 0, klm: 8 }, T11: { act: 1, chars: 0, klm: 3 }, T12: { act: 4, chars: 0, klm: 12 },
};

/* ---------- datos ---------- */
const DATA = path.join(OUT, 'data');
function gen(id, f, c, r, out) { execSync(`${BIN} --gen ${id} --gen-freq ${f} --gen-center ${c} --gen-rate ${r} --out ${out}`, { stdio: 'ignore' }); }
function prepare() {
  fs.mkdirSync(DATA, { recursive: true });
  makeData();                                                    // fm (99,6 y 100,4 MHz con RDS), marine (AIS+APRS), ism → /tmp/ux1-e2e-data
  if (!fs.existsSync(`${DATA}/adsb.sigmf-meta`)) gen('adsb', 1090000000, 1090000000, 2000000, `${DATA}/adsb`);
}
const SRC = (n) => (n === 'adsb' ? `${DATA}/adsb` : `/tmp/ux1-e2e-data/${n}`) + '.sigmf-meta';
let portN = 8811;
function server(replay, extra = []) {
  const port = portN++, rec = path.join(OUT, `rec${port}`);
  fs.rmSync(rec, { recursive: true, force: true }); fs.mkdirSync(rec, { recursive: true });
  const p = launch(BIN, ['--port', String(port), '--replay', SRC(replay), '--config', 'none', '--recdir', rec, '--web', path.join(ROOT, 'web'), ...extra]);
  p.port = port; p.rec = rec; live.push(p); return p;
}
const live = [];

/* ---------- medición: se envuelven las acciones de la página para contarlas ---------- */
class Meter {
  constructor(pg) {
    this.pg = pg; this.reset();
    const m = this, wrap = (name, fn) => { const orig = pg[name].bind(pg); pg[name] = (...a) => { fn(...a); return orig(...a); }; };
    wrap('click', () => m.clicks++);
    wrap('drag', () => m.drags++);
    wrap('wheel', () => m.wheels++);
    wrap('key', () => m.keys++);
    wrap('type', (t) => { m.chars += t.length; if (/\d/.test(t)) m.digits = true; });
    const f = pg.fill.bind(pg); pg.fill = async (sel, text, enter = true) => { m.clicks++; return f(sel, text, enter); };   // enfocar el campo es un clic
  }
  reset() { this.clicks = 0; this.drags = 0; this.wheels = 0; this.keys = 0; this.chars = 0; this.digits = false; this.think = 0; this.extra = 0; this.notes = []; this.dead = []; }
  /** Controles <select> nativos: abrir la lista y elegir son dos acciones. */
  async select(sel, value) {
    this.clicks += 2;
    await this.pg.ev(`(()=>{const s=document.querySelector(${JSON.stringify(sel)});s.value=${JSON.stringify(value)};s.dispatchEvent(new Event('change',{bubbles:true}))})()`);
  }
  tab(id) { this.think++; return this.pg.clickSel(`#tab-${id}`); }
}

const results = [];
async function task(id, title, fn) {
  if (ONLY && !ONLY.includes(id)) return;
  const t = { id, title, ok: false };
  let M = null, pg = null;
  const ctx = { open: async (port, w = 1440, h = 900, mobile = false) => {
    pg = await br.newPage(w, h);
    if (mobile) await pg.resize(w, h, true);
    await pg.goto(`http://127.0.0.1:${port}/`, 400);
    await pg.waitFor('window.__app && __app.S.connected && __app.S.spec.n > 0', 12000, 'conexión y primer espectro');
    await sleep(2500);                                             // la persona mira la pantalla antes de actuar: el detector ya ha marcado señales
    await pg.ev(`window.__confirms=0;window.confirm=()=>{__confirms++;return true};window.alert=()=>{};0`);
    M = new Meter(pg); ctx.pg = pg; ctx.M = M; return pg;
  } };
  const e0 = { ex: 0, ce: 0 };
  try {
    await fn(ctx);                                                 // abre la página (ctx.open) y fija ctx.M/ctx.pg
    M = ctx.M; pg = ctx.pg;
  } catch (e) { t.error = e.message.slice(0, 200); }
  M = ctx.M; pg = ctx.pg;
  if (M) {
    const toastErr = pg ? await pg.ev(`[...document.querySelectorAll('#toasts .toast.err')].map(x=>x.textContent)`).catch(() => []) : [];
    t.clicks = M.clicks; t.drags = M.drags; t.wheels = M.wheels; t.keys = M.keys; t.chars = M.chars; t.digits = M.digits;
    t.confirms = pg ? (await pg.ev('window.__confirms|0').catch(() => 0)) : 0;
    t.actions = M.clicks + M.drags + M.wheels + M.keys + t.confirms * 0;   // las confirmaciones nativas ya cuentan como clic si se añaden en la tarea
    t.ms = ctx.ms || null;
    t.klm = +(M.clicks * 1.1 + M.keys * 0.28 + M.drags * 1.5 + M.wheels * 0.5 + M.think * 1.2).toFixed(1);
    t.manual_numbers = M.digits;
    t.ok = !!ctx.ok;
    t.notes = M.notes; t.dead_ends = M.dead.concat(toastErr.map((x) => 'aviso de error: ' + x));
    if (pg) {
      t.exceptions = pg.exceptions.length; t.console_errors = pg.consoleErrors.length;
      fs.mkdirSync(OUT, { recursive: true });
      await pg.shot(path.join(OUT, `${id}_${ctx.slug || 'final'}.png`)).catch(() => {});
      pg.close();
    }
  }
  for (const p of live.splice(0)) { try { p.kill('SIGKILL'); } catch { /* ya terminado */ } }
  results.push(t);
  console.log(`${t.ok ? '  ok  ' : ' FALLA'} ${id} ${title}: ${t.actions} acciones (${t.clicks} clics, ${t.chars} teclas), ${t.ms ?? '—'} ms, KLM ${t.klm} s${t.error ? '  ERROR: ' + t.error : ''}`);
}
/** Cronometra desde ahora hasta que `expr` sea cierta (sondeo de 80 ms); devuelve ms o null. */
async function until(ctx, expr, ms = 10000) {
  const t0 = Date.now();
  try { await ctx.pg.waitFor(expr, ms, expr.slice(0, 60)); } catch { ctx.M.dead.push('no se cumplió en ' + ms / 1000 + ' s: ' + expr.slice(0, 80)); return false; }
  ctx.ms = (ctx.ms || 0) + (Date.now() - t0); return true;
}
const sigX = (pg, f) => pg.ev(`(()=>{const c=document.querySelectorAll('#spec-wrap canvas')[2]||document.querySelector('#spec-wrap canvas:last-child');const r=c.getBoundingClientRect();const x=__app.freqToX(${f});return {x:r.x+x,y:r.y,w:r.width,h:r.height}})()`);
const strongest = (pg, near) => pg.ev(`(()=>{const S=__app.S;const l=S.signals.list.map(s=>({f:S.signals.center+s[0],bw:s[1],snr:s[3]}));const c=l.filter(s=>${near ? `Math.abs(s.f-${near})<150e3` : 'true'}).sort((a,b)=>b.snr-a.snr);return c[0]||null})()`);
const t0clock = () => Date.now();

/* ============================================================ TAREAS ============================================================ */
let br;
async function main() {
  prepare(); fs.mkdirSync(OUT, { recursive: true });
  br = new Browser(9811); await br.start();
  const wait = async (p) => waitHttp(`http://127.0.0.1:${p.port}/`);

  // T01 — escuchar una emisora FM y ver su RDS (sin perfil previo: el camino de una persona nueva)
  await task('T01', 'Escuchar FM y ver el RDS', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await M.tab('protocolos'); await pg.clickSel('.pcard[data-id=fm]');
    await sleep(300); const chanAfterProfile = (await pg.ev('__app.S.chan.freq')) / 1e6;
    await M.tab('receptor');
    const sg = await strongest(pg, 100.4e6); const p = await sigX(pg, sg ? sg.f : 100.4e6);
    await pg.click(p.x, p.y + p.h * 0.45); M.think++;           // clic sobre la emisora en el espectro
    await pg.clickSel('#btn-listen');
    ctx.ok = await until(ctx, `!document.querySelector('#rds-card').hidden && Object.values(__app.S.decState).some(s=>s.proto==='rds'&&s.state&&s.state.ps&&s.state.ps.trim().length>0)`, 20000);
    ctx.ms = Date.now() - t0 - 0;
    M.notes.push(`Canal final ${(await pg.ev('__app.S.chan.freq')) / 1e6} MHz, modo ${await pg.ev('__app.S.chan.mode')}, audio ${await pg.ev(`document.querySelector('#btn-listen').getAttribute('aria-pressed')`)}`);
    M.notes.push('RDS: ' + await pg.ev(`document.querySelector('#rds-body').textContent.replace(/\\s+/g,' ').trim().slice(0,100)`));
    M.notes.push('Tras aplicar el perfil «FM comercial» el canal queda en ' + chanAfterProfile + ' MHz (no hay emisora ahí): hay que volver a sintonizar a mano.');
    ctx.slug = 'fm_rds'
  });

  // T02 — cambiar de modo y de ancho
  await task('T02', 'Cambiar de modo (NFM) y de ancho (25 kHz)', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await pg.clickSel('#mode-seg [data-m=nfm]');
    const bw0 = await pg.ev('__app.S.chan.bw');
    const vis = await pg.ev(`(()=>{const r=document.querySelector('#rx-bw').getBoundingClientRect();return r.top>=0&&r.bottom<=innerHeight})()`);
    if (!vis) M.dead.push('el campo «Anchura del filtro» queda fuera de la pantalla en 1440×900 (hay que desplazar)');
    await pg.fill('#rx-bw', '25000'); await pg.key('Tab');
    ctx.ok = await until(ctx, `__app.S.chan.mode==='nfm' && Math.abs(__app.S.chan.bw-25000)<=50`, 4000);
    ctx.ms = Date.now() - t0;
    M.notes.push(`Ancho tras elegir NFM: ${bw0} Hz (¿el modo ya trae uno razonable?). Alternativa sin teclado: arrastrar el borde del filtro en el espectro (no medido, depende de la precisión del puntero).`);
    M.notes.push('No hay preajustes de ancho (6 / 12,5 / 25 kHz) ni indicación de qué ancho es típico del modo.');
    ctx.slug = 'modo_ancho'
  });

  // T03 — ver aviones en el mapa
  await task('T03', 'Ver aviones en el mapa (ADS-B)', async (ctx) => {
    const s = server('adsb'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await M.tab('protocolos'); await pg.clickSel('.pcard[data-id=adsb]'); await M.tab('mapa');
    ctx.ok = await until(ctx, `Object.values(__app.S.decState).some(s=>s.proto==='adsb'&&s.state&&s.state.ac&&s.state.ac.length>0)`, 15000)
      && await until(ctx, `document.querySelector('#mp-sum').textContent.trim().length>0 && document.querySelectorAll('#mp-tables tr').length>1`, 6000);
    await sleep(600);
    ctx.ms = Date.now() - t0;
    M.notes.push('Sin ubicación del receptor guardada el mapa no tiene referencia local: ' + await pg.ev(`document.querySelector('#mp-note').textContent.trim().slice(0,120)||'(sin nota)'`));
    M.notes.push('Resumen del mapa: ' + await pg.ev(`document.querySelector('#mp-sum').textContent.trim()`));
    ctx.slug = 'adsb_mapa'
  });

  // T04 — ver barcos
  await task('T04', 'Ver barcos en el mapa (AIS)', async (ctx) => {
    const s = server('marine'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await M.tab('protocolos'); await pg.clickSel('.pcard[data-id=maritimo]'); await M.tab('mapa');
    ctx.ok = await until(ctx, `Object.values(__app.S.decState).some(s=>s.proto==='ais'&&s.state&&JSON.stringify(s.state).includes('mmsi'))`, 15000)
      && await until(ctx, `document.querySelector('#mp-sum').textContent.trim().length>0 && document.querySelectorAll('#mp-tables tr').length>1`, 6000);
    await sleep(600);
    ctx.ms = Date.now() - t0;
    const lst = await pg.ev(`__app.S.decList.map(i=>i.proto+'@'+i.freq+':'+i.state).join(', ')`);
    M.notes.push('Instancias creadas por el perfil: ' + lst);
    if (/waiting/.test(lst)) M.notes.push('El perfil avisa con un toast rojo de «error» aunque todo ha funcionado, y la instancia «esperando» queda sin explicar en el mapa.');
    ctx.slug = 'ais_mapa'
  });

  // T05 — añadir un decodificador sobre una señal detectada
  await task('T05', 'Añadir un decodificador sobre una señal detectada', async (ctx) => {
    const s = server('marine'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    const sg = await strongest(pg, 161.975e6) || await strongest(pg);
    if (!sg) { M.dead.push('el detector no marcó ninguna señal'); return; }
    M.notes.push(`Señal elegida ${(sg.f / 1e6).toFixed(4)} MHz, ancho ${Math.round(sg.bw)} Hz, SNR ${sg.snr.toFixed(0)} dB`);
    const p = await sigX(pg, sg.f);
    await pg.click(p.x, p.y + 8); M.think++;
    const open = await until(ctx, `!document.querySelector('#sig-menu').hidden`, 2000);
    if (open) {
      const info = await pg.ev(`(()=>{const m=document.querySelector('#sig-menu'),r=m.getBoundingClientRect(),b=[...m.querySelectorAll('button')];return {n:b.length,h:Math.round(r.height),vh:innerHeight,stars:b.filter(x=>x.textContent.includes('★')).map(x=>x.textContent.split('·')[0].trim()),idx:b.findIndex(x=>/^AIS/.test(x.textContent.trim()))}})()`);
      M.notes.push(`Menú: ${info.n} opciones, ${info.h}px de alto (ventana ${info.vh}px); sugeridas con ★: ${info.stars.join(', ') || 'ninguna'}; AIS es la opción nº ${info.idx + 1}.`);
      await pg.ev(`[...document.querySelectorAll('#sig-menu button')].find(x=>/^AIS/.test(x.textContent.trim()))?.scrollIntoView({block:'nearest'});0`);
      const r = await pg.ev(`(()=>{const b=[...document.querySelectorAll('#sig-menu button')].find(x=>/^AIS/.test(x.textContent.trim()));const r=b.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()`);
      await pg.click(r.x, r.y);
    } else M.dead.push('el clic sobre la marca de señal no abre el menú «decodificar como…»');
    ctx.ok = await until(ctx, `__app.S.decList.some(i=>i.proto==='ais'&&Math.abs(i.freq-${Math.round(sg.f)})<15000)`, 4000);
    ctx.ms = Date.now() - t0;
    M.notes.push('Instancia creada: ' + await pg.ev(`__app.S.decList.map(i=>i.proto+'@'+i.freq+':'+i.state).join(', ')`));
    M.notes.push('Tras elegir, el único aviso es un toast efímero: la persona no sabe dónde ver la instancia ni sus mensajes (la pestaña Decod. no se abre sola).');
    ctx.slug = 'decodificar_como'
  });

  // T06 — grabar y borrar la grabación
  await task('T06', 'Grabar IQ y borrar la grabación', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await M.tab('hardware'); await pg.clickSel('#rec-btn');
    await until(ctx, `__app.S.recording&&__app.S.recording.on`, 3000);
    await sleep(1500); await pg.clickSel('#rec-btn');             // parar
    const listed = await until(ctx, `document.querySelectorAll('#rec-list li a').length>=1`, 3000);
    if (!listed) { M.dead.push('la lista de grabaciones no se actualiza sola al parar: hay que pulsar «Actualizar lista»'); await pg.clickSel('#rec-refresh'); await until(ctx, `document.querySelectorAll('#rec-list li a').length>=1`, 3000); }
    const n = await pg.ev(`document.querySelectorAll('#rec-list li').length`);
    M.notes.push(`La grabación genera ${n} entradas en la lista (datos y metadatos).`);
    for (let i = 0; i < n && await pg.ev(`!!document.querySelector('#rec-list li button')`); i++) {
      const before = await pg.ev(`__confirms`); await pg.clickSel('#rec-list li button'); M.clicks++;   // el diálogo nativo confirm() cuenta como un clic más
      await until(ctx, `__confirms>${before}`, 1500); await sleep(700);
      await pg.ev(`document.querySelector('#rec-refresh').click()`); await sleep(300);
    }
    ctx.ok = (await pg.ev(`document.querySelectorAll('#rec-list li a').length`)) === 0;
    M.notes.push(`Pulsaciones de «Borrar»: ${await pg.ev('__confirms')} (un Borrar elimina datos y metadatos).`);
    ctx.ms = Date.now() - t0 - 1500;
    M.notes.push('Borrar usa confirm() nativo del navegador (bloquea la página; estilo ajeno a la interfaz).');
    ctx.slug = 'grabar_borrar'
  });

  // T07 — cambiar ganancia y FFT
  await task('T07', 'Cambiar la ganancia (manual) y el tamaño de FFT', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await M.tab('hardware');
    const st = await pg.ev(`JSON.stringify({dev:__app.S.status.device,gm:__app.S.status.gain_mode,fft:__app.S.status.fft})`);
    M.notes.push('status.device/gain_mode/fft: ' + st.slice(0, 160));
    const dis = await pg.ev(`['#hw-gmode','#hw-gain','#hw-fft','#hw-rate'].map(i=>i+(document.querySelector(i).disabled?':desactivado':':activo')).join(' ')`);
    M.notes.push('Estado de los controles con fuente de fichero: ' + dis);
    const gDis = await pg.ev(`document.querySelector('#hw-gmode').disabled`);
    if (gDis) M.dead.push('con fuente de fichero «Ganancia» está desactivada y la pantalla no dice por qué (no se puede medir el tramo de ganancia sin hardware)');
    else {
      await M.select('#hw-gmode', 'manual');
      const r = await pg.rect('#hw-gain'); if (r) await pg.click(r.x + r.w * 0.5, r.y + r.h / 2);
    }
    const fftBefore = await pg.ev(`document.querySelector('#hw-fft').value`);
    const fftNew = await pg.ev(`[...document.querySelector('#hw-fft').options].map(o=>o.value).find(v=>v!==document.querySelector('#hw-fft').value)`);
    await M.select('#hw-fft', fftNew);
    ctx.ok = await until(ctx, `document.querySelector('#hw-fft').value!==${JSON.stringify(fftBefore)} && document.querySelector('#spec-info').textContent.includes('FFT '+${JSON.stringify(fftNew)})`, 4000);
    if (!ctx.ok) M.dead.push(`tras elegir FFT ${fftNew} el pie del espectro sigue diciendo «${await pg.ev(`document.querySelector('#spec-info').textContent.trim()`)}»`);
    ctx.ms = Date.now() - t0;
    M.notes.push('Ganancia ' + await pg.ev(`document.querySelector('#hw-gain-o').textContent`) + ', FFT ' + await pg.ev(`document.querySelector('#hw-fft').value`) + '.');
    M.notes.push('El efecto de la FFT (resolución frente a velocidad) no se explica junto al control; el panel Hardware no muestra el espectro a la vista cuando hay que ajustar ganancia mirando el ruido.');
    ctx.slug = 'ganancia_fft'
  });

  // T08 — buscar un evento concreto
  await task('T08', 'Buscar un evento concreto en la lista (por MMSI)', async (ctx) => {
    const s = server('marine', ['--decode', 'ais@161975000']); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    await until(ctx, `__app.S.events? true : document.querySelectorAll('#ev-list .ev').length>=0`, 100);
    await pg.ev(`location.hash='decoders'`); await sleep(1500);
    const mmsi = await pg.ev(`(()=>{const t=document.querySelector('#ev-list').textContent;const m=t.match(/mmsi\\D{0,4}(\\d{9})/i);return m?m[1]:null})()`);
    const total = await pg.ev(`document.querySelectorAll('#ev-list .ev').length`);
    ctx.ms = 0; ctx.M.reset();                                     // la preparación no cuenta: la persona ya está en Decod. con eventos a la vista
    const t0 = t0clock();
    if (!mmsi) { M.dead.push('no hay eventos con MMSI visibles'); return; }
    await pg.fill('#ev-search', mmsi, false);
    ctx.ok = await until(ctx, `(()=>{const n=document.querySelectorAll('#ev-list .ev').length;return n>=1&&n<=${Math.max(total, 1)}&&document.querySelector('#ev-list').textContent.includes(${JSON.stringify(mmsi)})})()`, 3000);
    ctx.ms = Date.now() - t0;
    M.notes.push(`Eventos antes ${total}, después ${await pg.ev(`document.querySelectorAll('#ev-list .ev').length`)}; MMSI ${mmsi}.`);
    M.notes.push('La búsqueda es solo por texto: no hay filtro por campo (mmsi:, lat>), ni indicación del número de coincidencias, ni resaltado del término.');
    ctx.slug = 'buscar_evento'
  });

  // T09 — decodificador en error: entender por qué
  await task('T09', 'Entender un decodificador en error (ADS-B con 2,4 Msps)', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const t0 = t0clock();
    await M.tab('decoders');
    await M.select('#da-proto', 'adsb');
    await pg.clickSel('#da-add');
    const err = await until(ctx, `__app.S.decList.some(i=>i.state==='error')`, 4000);
    const txt = await pg.ev(`document.querySelector('#di-list').textContent.replace(/\\s+/g,' ').trim()`);
    ctx.ok = err && /exactamente|Msps|2 ?M/i.test(txt);
    ctx.ms = Date.now() - t0;
    M.notes.push('Texto de la instancia: «' + txt.slice(0, 200) + '»');
    const hint = await pg.ev(`document.querySelector('#da-hint').textContent.trim()`);
    M.notes.push('Pista del formulario: «' + hint.slice(0, 120) + '»');
    if (!err) M.dead.push('la instancia no llegó a estado de error');
    else if (!ctx.ok) M.dead.push('el estado «error» no explica la causa');
    // variante: narrow fuera de banda → «esperando» (no cuenta en las acciones de la tarea)
    const snap = { c: M.clicks, k: M.keys, ch: M.chars };
    await M.select('#da-proto', 'rs41'); await pg.fill('#da-freq', '403.5', false); await pg.clickSel('#da-add'); await until(ctx, `__app.S.decList.some(i=>i.state==='waiting')`, 3000);
    await sleep(1200);
    const w = await pg.ev(`(()=>{const i=[...document.querySelectorAll('#di-list .inst')].find(x=>/esperando/.test(x.textContent));return i?i.textContent.replace(/\\s+/g,' ').trim():''})()`);
    M.notes.push('Instancia «esperando» (fuera de banda): «' + w.slice(0, 160) + '» → ' + (/banda|fuera/i.test(w) ? 'explica el motivo' : 'no dice por qué espera ni qué hacer'));
    if (w && !/banda|fuera/i.test(w)) M.dead.push('«esperando» sin explicación (fuera de la banda capturada)');
    M.clicks = snap.c; M.keys = snap.k; M.chars = snap.ch;
    ctx.slug = 'decoder_error'
  });

  // T10 — generar una señal en modo file
  await task('T10', 'Generar una señal POCSAG en modo file', async (ctx) => {
    const s = server('fm', ['--enable-tx']); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    await pg.waitFor(`__app.S.txPolicy&&__app.S.txPolicy.enabled`, 4000);
    const t0 = t0clock();
    await M.tab('tx');
    await M.select('#tx-proto', 'pocsag');
    const mode = await pg.ev(`document.querySelector('#tx-mode').value`);
    M.notes.push(`Modo por defecto: ${mode}; frecuencia ${await pg.ev(`document.querySelector('#tx-freq').value`)}; centro ${await pg.ev(`document.querySelector('#tx-center').value`)}; tasa ${await pg.ev(`document.querySelector('#tx-rate').value`)}.`);
    await pg.clickSel('#tx-send');
    ctx.ok = await until(ctx, `document.querySelector('#tx-result').className.includes('ok') && /sigmf|Fichero/i.test(document.querySelector('#tx-result').textContent)`, 8000);
    ctx.ms = Date.now() - t0;
    const res = await pg.ev(`document.querySelector('#tx-result').textContent`);
    M.notes.push('Resultado: «' + res.replace(/\n/g, ' | ').slice(0, 200) + '»');
    const link = await pg.ev(`!!document.querySelector('#tx-result a')`);
    if (!link) M.dead.push('el resultado muestra la ruta del fichero pero no un enlace para descargarlo ni «reproducir/decodificar este fichero»');
    ctx.slug = 'emision_file'
  });

  // T11 — sintonizar con el ratón sobre el espectro
  await task('T11', 'Sintonizar una emisora con un clic en el espectro', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port), M = ctx.M;
    const sg = await strongest(pg, 99.6e6) || await strongest(pg);
    const t0 = t0clock();
    const p = await sigX(pg, sg.f);
    await pg.click(p.x, p.y + p.h * 0.5);
    await until(ctx, `Math.abs(__app.S.chan.freq-${Math.round(sg.f)})<600e3`, 500);
    await sleep(200);
    const f = await pg.ev('__app.S.chan.freq'), err = Math.abs(f - sg.f);
    ctx.ok = err <= 25e3; ctx.ms = Date.now() - t0 - 200;
    M.notes.push(`Pico detectado ${(sg.f / 1e6).toFixed(3)} MHz; canal tras el clic ${(f / 1e6).toFixed(3)} MHz (error ${(err / 1e3).toFixed(1)} kHz).`);
    const lab = await pg.ev(`document.querySelector('#freq-input').value`);
    M.notes.push('Campo de frecuencia tras el clic: ' + lab);
    ctx.slug = 'sintonia_raton'
  });

  // T12 — usar la web a 360 px
  await task('T12', 'Móvil 360 px: sintonizar, escuchar y llegar a Decod.', async (ctx) => {
    const s = server('fm'); await wait(s); const pg = await ctx.open(s.port, 360, 740, true), M = ctx.M;
    const t0 = t0clock();
    const over = await pg.ev(`document.documentElement.scrollWidth>innerWidth+1`);
    if (over) M.dead.push('desbordamiento horizontal de la página a 360 px');
    const inView = await pg.ev(`(()=>{const q=(s)=>{const r=document.querySelector(s).getBoundingClientRect();return r.top>=0&&r.bottom<=innerHeight&&r.left>=0&&r.right<=innerWidth};return {freq:q('#freq-input'),listen:q('#btn-listen'),spec:q('#spec-wrap'),tabs:q('#tabs')}})()`);
    M.notes.push('A la vista sin desplazar: ' + JSON.stringify(inView));
    await pg.fill('#freq-input', '100.4');
    await pg.clickSel('#btn-listen'); M.clicks += 0;
    const tabY = await pg.ev(`Math.round(document.querySelector('#tab-decoders').getBoundingClientRect().top)`);
    if (tabY > 740 - 30) { M.dead.push(`las pestañas empiezan en y=${tabY}px de 740: en la primera pantalla no se ve ningún panel, hay que desplazar`); await pg.ev('window.scrollBy(0,500)'); M.wheels++; await sleep(300); }
    await pg.ev(`document.querySelector('#tab-decoders').scrollIntoView({inline:'center',block:'nearest'})`);
    await M.tab('decoders');
    await sleep(500);
    const small = await pg.ev(`[...document.querySelectorAll('button,select,input,[role=tab]')].filter(e=>e.offsetParent&&!e.closest('[hidden]')).map(e=>{const r=e.getBoundingClientRect();return {id:e.id||e.textContent.trim().slice(0,14)||e.className,w:Math.round(r.width),h:Math.round(r.height)}}).filter(x=>x.h<36&&x.h>0&&x.w>0)`);
    M.notes.push(`Controles visibles con altura < 36 px: ${small.length} (${small.slice(0, 6).map((x) => x.id + ' ' + x.w + '×' + x.h).join('; ')}).`);
    const tabsVis = await pg.ev(`(()=>{const t=document.querySelector('#tabs');return {sw:t.scrollWidth,cw:t.clientWidth,n:t.querySelectorAll('[role=tab]').length}})()`);
    M.notes.push(`Barra de pestañas: ${tabsVis.n} pestañas en ${tabsVis.cw}px (contenido ${tabsVis.sw}px).`);
    const scrolled = await pg.ev(`scrollY`); M.notes.push(`Desplazamiento vertical de la página tras abrir Decod.: ${Math.round(scrolled)} px.`);
    const fin = { f: await pg.ev('__app.S.chan.freq'), tab: await pg.ev(`document.querySelector('[role=tab][aria-selected=true]').id`) };
    M.notes.push('Estado final: ' + JSON.stringify(fin));
    ctx.ok = !over && Math.abs(fin.f - 100.4e6) < 1e3 && fin.tab === 'tab-decoders';
    ctx.ms = Date.now() - t0;
    ctx.slug = 'movil_360'
  });

  /* ---------- informe ---------- */
  const rep = results.map((t) => {
    const o = OBJ[t.id] || {};
    t.target = o; t.within = t.ok && t.actions <= o.act && t.chars <= o.chars && t.klm <= o.klm;
    return t;
  });
  fs.writeFileSync(path.join(OUT, 'ux_audit.json'), JSON.stringify(rep, null, 1));
  console.log('\nTarea | ok | acciones (obj) | teclas (obj) | ms | KLM s (obj) | nº a mano | callejones');
  for (const t of rep) console.log(`${t.id} | ${t.ok ? 'sí' : 'NO'} | ${t.actions} (${t.target.act}) | ${t.chars} (${t.target.chars}) | ${t.ms ?? '—'} | ${t.klm} (${t.target.klm}) | ${t.manual_numbers ? 'sí' : 'no'} | ${(t.dead_ends || []).length}`);
  console.log(`\nJSON: ${path.join(OUT, 'ux_audit.json')}`);
  const bad = rep.filter((t) => !t.ok), over = rep.filter((t) => !t.within);
  if (!argv.keep) { br.stop(); killAll(); }
  if (argv.enforce) { console.log(`${over.length} tareas fuera de objetivo`); process.exit(over.length ? 1 : 0); }
  process.exit(bad.length === rep.length ? 1 : 0);                 // sin --enforce es una medición: solo falla si todo falla
}
main().catch((e) => { console.error(e); killAll(); process.exit(2); });
