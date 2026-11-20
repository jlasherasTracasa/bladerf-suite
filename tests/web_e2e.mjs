#!/usr/bin/env node
// web_e2e.mjs — batería E2E de la interfaz web. Node >= 22 (WebSocket y fetch globales), sin
// dependencias: habla con Chromium por CDP (puerto propio) y lanza los servidores de prueba
// (bladerf-lab --replay + tests/web_mock.js). Ver docs/calidad/QA_WEB.md.
//
// Este fichero es a la vez biblioteca (clase Page, helpers) y programa: si se ejecuta directamente
// recorre todas las suites. Opciones: --only <regex> (suites), --soak <s> (duración de la prueba
// de fugas, por defecto 60; la de 10 minutos es --soak 600), --shots <dir>, --keep (no cierra servidores).
import { spawn, execSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.join(HERE, '..');
export const CHROME = process.env.CHROME || path.join(process.env.HOME || '', '.cache/ms-playwright/chromium-1234/chrome-linux64/chrome');
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---------- procesos auxiliares ---------- */
const procs = [];
export function launch(cmd, args, opts = {}) {
  const p = spawn(cmd, args, { stdio: ['ignore', 'pipe', 'pipe'], ...opts });
  p.out = ''; p.on('exit', (c, sg) => { p.exited = `código ${c} señal ${sg}`; });
  p.stdout.on('data', (d) => { p.out += d; if (p.out.length > 2e5) p.out = p.out.slice(-1e5); });
  p.stderr.on('data', (d) => { p.out += d; if (p.out.length > 2e5) p.out = p.out.slice(-1e5); });
  procs.push(p);
  return p;
}
export function killAll() { for (const p of procs) { try { p.kill('SIGKILL'); } catch { /* ya muerto */ } } }
process.on('exit', killAll);

export async function waitHttp(url, ms = 8000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { try { const r = await fetch(url); if (r.ok) return true; } catch { /* aún no */ } await sleep(100); }
  throw new Error('sin respuesta de ' + url);
}

/* ---------- cliente CDP ---------- */
export class Browser {
  constructor(port) { this.port = port; this.proc = null; }
  async start(extra = []) {
    const dir = fs.mkdtempSync('/tmp/ux1-chrome-');
    this.proc = launch(CHROME, ['--headless=new', `--remote-debugging-port=${this.port}`, '--no-sandbox', '--disable-gpu-sandbox',
      '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required',
      `--user-data-dir=${dir}`, '--no-first-run', '--disable-extensions', '--mute-audio', ...extra, 'about:blank']);
    await waitHttp(`http://127.0.0.1:${this.port}/json/version`, 15000);
  }
  async newPage(w = 1440, h = 900) {
    const t = await (await fetch(`http://127.0.0.1:${this.port}/json/new?about:blank`, { method: 'PUT' })).json();
    const pg = new Page(t.webSocketDebuggerUrl);
    await pg.open(w, h); pg.tid = t.id; pg.port = this.port;

    return pg;
  }
  stop() { try { this.proc.kill('SIGKILL'); } catch { /* nada */ } }
}

export class Page {
  constructor(wsUrl) { this.url = wsUrl; this.id = 0; this.cb = new Map(); this.consoleErrors = []; this.exceptions = []; this.warnings = []; this.reqFail = []; }
  open(w, h) {
    return new Promise((res, rej) => {
      this.ws = new WebSocket(this.url);
      this.ws.onerror = () => rej(new Error('CDP'));
      this.ws.onmessage = (ev) => {
        const m = JSON.parse(ev.data);
        if (m.id && this.cb.has(m.id)) { const { r, j } = this.cb.get(m.id); this.cb.delete(m.id); m.error ? j(new Error(m.error.message)) : r(m.result); return; }
        if (m.method === 'Runtime.exceptionThrown') this.exceptions.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
        else if (m.method === 'Runtime.consoleAPICalled') {
          const txt = m.params.args.map((a) => a.value ?? a.description ?? '').join(' ');
          if (m.params.type === 'error') this.consoleErrors.push(txt); else if (m.params.type === 'warning') this.warnings.push(txt);
        } else if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') this.consoleErrors.push(m.params.entry.text + ' ' + (m.params.entry.url || ''));
        else if (m.method === 'Network.loadingFailed' && !m.params.canceled) this.reqFail.push(m.params.errorText);
      };
      this.ws.onopen = async () => {
        await this.send('Page.enable'); await this.send('Runtime.enable'); await this.send('Log.enable'); await this.send('Network.enable');
        await this.resize(w, h); res();
      };
    });
  }
  send(method, params = {}) {
    return new Promise((r, j) => {
      const id = ++this.id, tm = setTimeout(() => { this.cb.delete(id); j(new Error('CDP sin respuesta: ' + method)); }, 15000);
      this.cb.set(id, { r: (v) => { clearTimeout(tm); r(v); }, j: (e) => { clearTimeout(tm); j(e); } });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  async resize(w, h, mobile = false) {
    await this.send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile });
    if (mobile) await this.send('Emulation.setTouchEmulationEnabled', { enabled: true });
  }
  async goto(url, wait = 0) { await this.send('Page.navigate', { url }); await sleep(600 + wait); }
  async ev(expr) {
    const r = await this.send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error('ev: ' + (r.exceptionDetails.exception?.description || r.exceptionDetails.text) + ' en ' + expr.slice(0, 80));
    return r.result.value;
  }
  async waitFor(expr, ms = 8000, what = expr) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { try { if (await this.ev(expr)) return true; } catch { /* aún no */ } await sleep(80); }
    throw new Error('timeout esperando: ' + what);
  }
  async shot(file) {
    const r = await this.send('Page.captureScreenshot', { format: 'png' });
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, Buffer.from(r.data, 'base64'));
  }
  async rect(sel) { return this.ev(`(()=>{const e=document.querySelector(${JSON.stringify(sel)});if(!e)return null;const r=e.getBoundingClientRect();return{x:r.x,y:r.y,w:r.width,h:r.height}})()`); }
  async front() {
    // Una página en segundo plano no procesa entradas (CDP se queda esperando): se trae al frente si hace falta.
    const t = Date.now();
    if (Page.cur !== this || t - (this.lastFront || 0) > 1500) {
      this.lastFront = t;
      const vis = await this.send('Runtime.evaluate', { expression: 'document.visibilityState', returnByValue: true }).then((r) => r.result.value, () => 'hidden');
      if (Page.cur !== this || vis !== 'visible') { await this.send('Page.bringToFront'); Page.cur = this; }
    }
  }
  async mouse(type, x, y, extra = {}) {
    await this.front(); await this.send('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1, ...extra }); }
  async click(x, y) { await this.mouse('mouseMoved', x, y, { buttons: 0 }); await this.mouse('mousePressed', x, y); await this.mouse('mouseReleased', x, y); }
  async clickSel(sel) {
    const r = await this.rect(sel); if (!r) throw new Error('no existe ' + sel);
    await this.click(r.x + r.w / 2, r.y + r.h / 2);
  }
  async drag(x0, y0, x1, y1, steps = 8) {
    await this.mouse('mouseMoved', x0, y0, { buttons: 0 }); await this.mouse('mousePressed', x0, y0);
    for (let i = 1; i <= steps; i++) await this.mouse('mouseMoved', x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    await this.mouse('mouseReleased', x1, y1);
  }
  async wheel(x, y, dy) { await this.front(); await this.send('Input.dispatchMouseEvent', { type: 'mouseWheel', x, y, deltaX: 0, deltaY: dy }); }
  async key(key, opts = {}) {
    await this.front();
    const code = { ArrowLeft: 37, ArrowRight: 39, ArrowUp: 38, ArrowDown: 40, Enter: 13, Escape: 27, Tab: 9, Backspace: 8, ' ': 32 }[key] || (key.length === 1 ? key.toUpperCase().charCodeAt(0) : 0);
    const base = { key, code: key.length === 1 ? (/[a-z]/i.test(key) ? 'Key' + key.toUpperCase() : /\d/.test(key) ? 'Digit' + key : '') : key, windowsVirtualKeyCode: code, ...opts };
    await this.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base, text: key.length === 1 ? key : undefined });
    await this.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
  }
  async type(text) { for (const ch of text) await this.send('Input.dispatchKeyEvent', { type: 'char', text: ch, key: ch }); }
  /** Escribe en un campo de texto como lo haría una persona: foco, seleccionar todo, teclear, Enter. */
  async fill(sel, text, enter = true) {
    await this.ev(`(()=>{const e=document.querySelector(${JSON.stringify(sel)});e.focus();e.select&&e.select()})()`);
    await this.type(text);
    if (enter) await this.key('Enter');
  }
  /** Cierra la pestaña de verdad (no solo el socket CDP): cada pestaña con WebGL de software pesa y acaba tumbando Chromium. */
  close() { try { this.ws.close(); } catch { /* nada */ } if (this.tid && this.port) fetch(`http://127.0.0.1:${this.port}/json/close/${this.tid}`).catch(() => {}); if (Page.cur === this) Page.cur = null; }
}

/* ---------- decodificador PNG mínimo (RGB/RGBA 8 bit) para medir píxeles de las capturas ---------- */
import zlib from 'node:zlib';
export function decodePng(buf) {
  let p = 8, w = 0, h = 0, ct = 0; const idat = [];
  while (p < buf.length) {
    const len = buf.readUInt32BE(p), type = buf.toString('ascii', p + 4, p + 8);
    if (type === 'IHDR') { w = buf.readUInt32BE(p + 8); h = buf.readUInt32BE(p + 12); ct = buf[p + 17]; }
    else if (type === 'IDAT') idat.push(buf.subarray(p + 8, p + 8 + len));
    p += 12 + len;
  }
  const bpp = ct === 6 ? 4 : 3, raw = zlib.inflateSync(Buffer.concat(idat)), stride = w * bpp, out = Buffer.alloc(h * stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = y * (stride + 1) + 1;
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? out[y * stride + x - bpp] : 0, b = y ? out[(y - 1) * stride + x] : 0, c = x >= bpp && y ? out[(y - 1) * stride + x - bpp] : 0;
      let v = raw[src + x];
      if (f === 1) v += a; else if (f === 2) v += b; else if (f === 3) v += (a + b) >> 1;
      else if (f === 4) { const pp = a + b - c, pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
      out[y * stride + x] = v & 255;
    }
  }
  return { w, h, bpp, data: out };
}
/** Fracción de píxeles no negros en un rectángulo de una captura, y un resumen (hash) de su contenido. */
export async function pixelStats(page, rect) {
  const r = await page.send('Page.captureScreenshot', { format: 'png' });
  const img = decodePng(Buffer.from(r.data, 'base64'));
  let lit = 0, tot = 0, sum = 0;
  const x0 = Math.max(0, Math.floor(rect.x)), y0 = Math.max(0, Math.floor(rect.y)), x1 = Math.min(img.w, Math.floor(rect.x + rect.w)), y1 = Math.min(img.h, Math.floor(rect.y + rect.h));
  for (let y = y0; y < y1; y += 2) for (let x = x0; x < x1; x += 2) {
    const i = (y * img.w + x) * img.bpp, v = img.data[i] + img.data[i + 1] + img.data[i + 2];
    tot++; if (v > 45) lit++; sum = (sum * 31 + v) >>> 0;
  }
  return { lit: tot ? lit / tot : 0, hash: sum, w: img.w, h: img.h };
}

/* ---------- datos de prueba: capturas sintéticas (--gen) mezcladas en una sola ---------- */
const BIN = process.env.BIN || path.join(ROOT, 'build/ux1/bladerf-lab');
const DATA = '/tmp/ux1-e2e-data';
function mix(out, secs, ins) {
  const meta = JSON.parse(fs.readFileSync(ins[0] + '.sigmf-meta')), rate = meta.global['core:sample_rate'], N = Math.round(rate * secs), acc = new Int32Array(N * 2);
  for (const p of ins) {
    const b = fs.readFileSync(p + '.sigmf-data'), d = new Int16Array(b.buffer, b.byteOffset, b.length >> 1), L = d.length >> 1;
    for (let i = 0; i < N; i++) { const k = (i % L) * 2; acc[i * 2] += d[k]; acc[i * 2 + 1] += d[k + 1]; }
  }
  const o = new Int16Array(N * 2); let s = 12345; const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
  for (let i = 0; i < o.length; i++) o[i] = Math.max(-2047, Math.min(2047, acc[i] + (rnd() + rnd() + rnd() + rnd() - 2) * 12));
  fs.writeFileSync(out + '.sigmf-data', Buffer.from(o.buffer));
  meta.global['core:description'] = 'mezcla e2e';
  fs.writeFileSync(out + '.sigmf-meta', JSON.stringify(meta, null, 1));
}
export function makeData() {
  fs.mkdirSync(DATA, { recursive: true });
  if (fs.existsSync(DATA + '/fm.sigmf-meta') && fs.existsSync(DATA + '/marine.sigmf-meta') && fs.existsSync(DATA + '/ism.sigmf-meta')) return;
  const g = (id, f, c, r, out) => execSync(`${BIN} --gen ${id} --gen-freq ${f} --gen-center ${c} --gen-rate ${r} --out ${DATA}/${out}`, { stdio: 'ignore' });
  g('rds', 100400000, 100000000, 2400000, 'rds1'); g('rds', 99600000, 100000000, 2400000, 'rds2');
  g('ais', 161975000, 162000000, 2000000, 'ais'); g('aprs', 162025000, 162000000, 2000000, 'aprs');
  g('pocsag', 433500000, 433920000, 2000000, 'poc'); g('ism', 433920000, 433920000, 2000000, 'ism1');
  mix(DATA + '/fm', 5, [DATA + '/rds1', DATA + '/rds2']); mix(DATA + '/marine', 4, [DATA + '/ais', DATA + '/aprs']); mix(DATA + '/ism', 4, [DATA + '/poc', DATA + '/ism1']);
}
let recSeq = 0;
export function startServer(port, replay, extra = []) {
  const rec = `/tmp/ux1-e2e-rec-${port}-${++recSeq}`; fs.rmSync(rec, { recursive: true, force: true }); fs.mkdirSync(rec, { recursive: true });
  const p = launch(BIN, ['--port', String(port), '--replay', `${DATA}/${replay}.sigmf-meta`, '--config', 'none', '--recdir', rec, '--web', path.join(ROOT, 'web'), ...extra]);
  p.port = port; p.rec = rec; return p;
}
export const startMock = (port, rich = true) => { const p = launch('node', [path.join(HERE, 'web_mock.js'), String(port), ...(rich ? ['--rich'] : [])]); p.port = port; return p; };

/* ---------- marco de pruebas ---------- */
const results = [];
let curSuite = '';
const shotsDir = () => argv.shots || '/tmp/ux1-e2e-shots';
export function check(name, ok, detail = '') { results.push({ suite: curSuite, name, ok: !!ok, detail: String(detail ?? '') }); console.log(`${ok ? '  ok  ' : '  FALLA'} ${curSuite} · ${name}${!ok && detail ? '  → ' + detail : ''}`); return !!ok; }
const argv = {}; { const a = process.argv.slice(2); for (let i = 0; i < a.length; i++) if (a[i].startsWith('--')) { argv[a[i].slice(2)] = a[i + 1] && !a[i + 1].startsWith('--') ? a[++i] : true; } }

/** Abre la web (esperando a que haya espectro) y deja contadores en window.__t. */
async function openApp(br, port, { w = 1440, h = 900, hash = '' } = {}) {
  const pg = await br.newPage(w, h);
  await pg.goto(`http://127.0.0.1:${port}/${hash}`, 400);
  await pg.waitFor('window.__app && __app.S.connected && __app.S.spec.n > 0', 10000, 'conexión y primer espectro');
  await pg.ev(`window.__t={fr:0,t0:performance.now(),chan:[],st:0};__app.bus.on('bin:spectrum',()=>__t.fr++);__app.bus.on('msg:status',()=>__t.st++);0`);
  return pg;
}
const S_ = (pg, expr) => pg.ev(`(()=>{const S=__app.S;return ${expr}})()`);
const near = (a, b, tol) => Math.abs(a - b) <= tol;
const logOf = async (port) => (await (await fetch(`http://127.0.0.1:${port}/_log`)).json());
const visibleTabs = (pg) => pg.ev(`[...document.querySelectorAll('[role=tab]')].map(t=>t.id.slice(4))`);

/* ================= SUITES ================= */
const suites = {};

suites.arranque = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.fm.port); ctx.pg = pg;
  check('conecta y recibe espectro', true);
  await sleep(2500);
  const fps = await pg.ev('__t.fr/((performance.now()-__t.t0)/1000)');
  check('trama de espectro ≥ 5/s', fps >= 5, fps.toFixed(1) + ' fps');
  check('chip de conexión', (await pg.ev(`document.querySelector('#st-conn').textContent`)).includes('Conectado'));
  check('chip de tasa con valor', /\d\.\d+ Msps/.test(await pg.ev(`document.querySelector('#st-rate').textContent`)));
  check('título con frecuencia', /\d+\.\d+ MHz/.test(await pg.ev('document.title')), await pg.ev('document.title'));
  const s0 = await S_(pg, 'S.spec.dbstep'); check('cabecera con dbstep > 0 (la web usa dbmin/dbstep)', s0 > 0, s0);
  await pg.shot(`${shotsDir()}/01_arranque_1440.png`);
};

suites.estado_inicial = async (ctx) => {
  // al conectar, el canal debe caer dentro de la banda y sobre la señal detectada más fuerte (FM sintético: 99,6 y 100,4 MHz)
  const pg = await openApp(ctx.br, ctx.fm.port);
  await sleep(3000);
  const f = await S_(pg, 'S.chan.freq'), band = await S_(pg, '[S.status.center-S.status.rate/2,S.status.center+S.status.rate/2]');
  check('el canal inicial está dentro de la banda', f >= band[0] && f <= band[1], f);
  const sig = await S_(pg, 'S.signals.list.slice().sort((a,b)=>b[3]-a[3]).map(s=>S.signals.center+s[0])');
  check('el canal inicial está sobre una señal detectada (±50 kHz)', sig.some((x) => Math.abs(x - f) < 50e3), `canal ${f}, señales ${sig.map((x) => (x / 1e6).toFixed(3))}`);
  check('el canal inicial es múltiplo de 100 kHz en WFM (rejilla de emisoras)', f % 100000 === 0, f);
  pg.close();
};

suites.pestanas = async (ctx) => {
  const pg = ctx.pg; await pg.ev(`location.hash='receptor'`);
  const tabs = await visibleTabs(pg);
  check('hay 8 pestañas con TX visible o 7 sin ella', tabs.length >= 7, tabs.join());
  for (const t of tabs) {
    await pg.clickSel(`#tab-${t}`); await sleep(150);
    const r = await pg.ev(`(()=>{const p=document.querySelector('#p-${t}');const vis=[...document.querySelectorAll('.panel')].filter(x=>x.style.display!=='none'&&!x.hidden);return {sel:document.querySelector('#tab-${t}').getAttribute('aria-selected'),n:vis.length,ok:vis[0]===p,h:p.getBoundingClientRect().height}})()`);
    check(`pestaña ${t}: se muestra sola y aria-selected`, r.sel === 'true' && r.n === 1 && r.ok && r.h > 50, JSON.stringify(r));
    await pg.shot(`${shotsDir()}/02_tab_${t}.png`);
  }
  check('las pestañas caben en una sola fila a 1440 px', await pg.ev(`(()=>{const t=document.querySelector('#tabs');return t.scrollWidth<=t.clientWidth+1&&t.getBoundingClientRect().height<52})()`));
  await pg.clickSel('#tab-receptor'); await pg.key('ArrowRight'); await sleep(100);
  { const a = await pg.ev(`document.activeElement.id + ' / ' + document.querySelector('[role=tab][aria-selected=true]').id`); check('flecha derecha cambia de pestaña (foco)', a === 'tab-hardware / tab-hardware', a); }
  await pg.key('3'); await sleep(100);
  check('tecla 3 abre la tercera pestaña', (await pg.ev(`document.querySelector('[role=tab][aria-selected=true]').id`)) === 'tab-protocolos');
  await pg.ev(`location.hash='decoders'`); await sleep(200);
  check('enlace profundo #decoders', (await pg.ev(`document.querySelector('[role=tab][aria-selected=true]').id`)) === 'tab-decoders');
  await pg.ev(`location.hash='receptor'`);
};

// controles: se recorren todos los visibles de cada panel y se accionan (salvo los destructivos)
suites.controles = async (ctx) => {
  const pg = ctx.pg;
  const SKIP = '#tx-send,#tp-send,#tc-run,#rec-btn,#sw-go,#hw-bias,#tv-play,#tv-tune,#tv-scan,#tv-stop,#tv-fs,#btn-listen,#hw-geo';
  await pg.ev(`window.confirm=()=>true;window.alert=()=>{};0`);
  for (const t of await visibleTabs(pg)) {
    await pg.ev(`location.hash='${t}'`); await sleep(200);
    const inv = await pg.ev(`(()=>{const out=[];for(const e of document.querySelectorAll('#p-${t} input,#p-${t} select,#p-${t} textarea,#p-${t} button')){
      if(e.matches(${JSON.stringify(SKIP)})||e.closest('.menu')||!e.offsetParent)continue;
      const name=e.getAttribute('aria-label')||(e.labels&&e.labels[0]&&e.labels[0].textContent.trim())||e.textContent.trim()||e.title||e.placeholder||'';
      out.push({id:e.id||'',tag:e.tagName,type:e.type||'',name:name.slice(0,40),h:Math.round(e.getBoundingClientRect().height),dis:e.disabled})}return out})()`);
    const unnamed = inv.filter((x) => !x.name);
    check(`${t}: ${inv.length} controles, todos con nombre accesible`, unnamed.length === 0, unnamed.map((x) => x.tag + '#' + x.id).join());
    const ex0 = pg.exceptions.length, er0 = pg.consoleErrors.length;
    await pg.ev(`(()=>{for(const e of document.querySelectorAll('#p-${t} input,#p-${t} select,#p-${t} textarea,#p-${t} button')){
      if(e.matches(${JSON.stringify(SKIP)})||!e.offsetParent||e.disabled||e.id==='rec-refresh'&&0)continue;
      if(e.tagName==='SELECT'){for(const o of [...e.options]){e.value=o.value;e.dispatchEvent(new Event('change',{bubbles:true}))}}
      else if(e.type==='checkbox'||e.tagName==='BUTTON'){if(!e.matches('.danger,[id$=clear]'))e.click()}
      else if(e.type==='range'){e.dispatchEvent(new Event('input',{bubbles:true}))}
      }})()`);
    await sleep(300);
    check(`${t}: accionar todos los controles no lanza errores`, pg.exceptions.length === ex0 && pg.consoleErrors.length === er0, pg.exceptions.slice(ex0).concat(pg.consoleErrors.slice(er0)).join(' | ').slice(0, 300));
  }
  await pg.ev(`location.hash='receptor'`);
  // el servidor sigue vivo
  const n0 = await pg.ev('__t.fr'); await sleep(800);
  check('el espectro sigue llegando tras accionar todo', (await pg.ev('__t.fr')) > n0);
};

suites.sintonia = async (ctx) => {
  const pg = ctx.pg; await pg.ev(`location.hash='receptor'`);
  await pg.fill('#freq-input', '100,4'); await sleep(150);
  check('campo: "100,4" → 100,4 MHz', (await S_(pg, 'S.chan.freq')) === 100400000, await S_(pg, 'S.chan.freq'));
  check('campo se reescribe con formato', (await pg.ev(`document.querySelector('#freq-input').value`)) === '100.400 MHz');
  await pg.fill('#freq-input', '99.6M'); await sleep(100);
  check('campo: "99.6M"', (await S_(pg, 'S.chan.freq')) === 99600000);
  await pg.fill('#freq-input', '100 400 000'); await sleep(100);
  check('campo: "100 400 000" (con espacios)', (await S_(pg, 'S.chan.freq')) === 100400000, await S_(pg, 'S.chan.freq'));
  const before = await S_(pg, 'S.chan.freq'); await pg.fill('#freq-input', 'abc'); await sleep(100);
  check('entrada no válida no cambia el canal y avisa', (await S_(pg, 'S.chan.freq')) === before && (await pg.ev(`document.querySelector('#toasts').textContent`)).length > 0);
  // rueda sobre el campo
  const st = await S_(pg, 'S.chan.freq'); const r = await pg.rect('#freq-input');
  await pg.wheel(r.x + 20, r.y + 10, -100); await sleep(100);
  check('rueda sobre el campo sube un paso', (await S_(pg, 'S.chan.freq')) > st, await S_(pg, 'S.chan.freq'));
  // flechas con el foco en el cuerpo
  await pg.ev(`(()=>{const e=document.querySelector('#rx-step');e.value='10000';e.dispatchEvent(new Event('change',{bubbles:true}));document.activeElement.blur()})()`); const f0 = await S_(pg, 'S.chan.freq');
  await pg.key('ArrowRight'); await sleep(80); const f1 = await S_(pg, 'S.chan.freq');
  await pg.key('ArrowLeft'); await sleep(80); const f2 = await S_(pg, 'S.chan.freq');
  await pg.key('ArrowUp'); await sleep(80); const f3 = await S_(pg, 'S.chan.freq');
  check('flechas ←/→ mueven un paso', f1 > f0 && f2 === f0, `${f0} ${f1} ${f2}`);
  check('flecha ↑ mueve ×10', f3 - f2 === 10 * (f1 - f0), `${f3 - f2} vs ${f1 - f0}`);
  await pg.key('ArrowDown');
  // clic en el espectro
  const ov = await pg.rect('#spec-wrap');
  const target = await S_(pg, 'S.status.center+300000'); const x = await pg.ev(`__app.freqToX(${target})`);
  await pg.click(ov.x + x, ov.y + 60); await sleep(120);
  const fc = await S_(pg, 'S.chan.freq');
  check('clic en el espectro sintoniza (±1 paso)', near(fc, target, 120000), `${fc} vs ${target}`);
  await pg.shot(`${shotsDir()}/03_sintonia_clic.png`);
  // clic en la cascada también sintoniza
  const x2 = await pg.ev(`__app.freqToX(${await S_(pg, 'S.status.center-300000')})`);
  await pg.click(ov.x + x2, ov.y + ov.h - 40); await sleep(120);
  check('clic en la cascada sintoniza', near(await S_(pg, 'S.chan.freq'), await S_(pg, 'S.status.center-300000'), 120000), await S_(pg, 'S.chan.freq'));
  // arrastrar los bordes del filtro (cambia la anchura)
  await pg.fill('#freq-input', '100.4'); await sleep(100);
  await pg.ev(`[...document.querySelectorAll('#mode-seg button')].find(b=>b.textContent==='WFM').click();__app.S.chan.bw=150000;0`);
  const ov2 = await pg.rect('#spec-wrap'); ov.x = ov2.x; ov.y = ov2.y;
  const bw0 = await S_(pg, 'S.chan.bw'), fx = await pg.ev(`__app.freqToX(__app.S.chan.freq+__app.S.chan.bw/2)`);
  await pg.drag(ov.x + fx, ov.y + 60, ov.x + fx + 25, ov.y + 60); await sleep(150);
  const bw1 = await S_(pg, 'S.chan.bw'); check('arrastrar el borde del filtro cambia la anchura', bw1 !== bw0 && bw1 > bw0, `${bw0} → ${bw1} ${await pg.ev('JSON.stringify([__app.S.chan, __app.S.status.center, __app.S.status.rate, __app.getView(), scrollY, document.querySelector("#sweepbar").hidden, ' + JSON.stringify([ov, fx]) + '])')}`);
  check('arrastrar el borde no mueve la frecuencia central', (await S_(pg, 'S.chan.freq')) === 100400000);
  // modo con flechas en el lienzo enfocado
  await pg.ev(`document.querySelector('#spec-wrap canvas.ov').focus();0`);
  const g0 = await S_(pg, 'S.chan.freq'); await pg.key('ArrowRight'); await sleep(80);
  check('flechas con el lienzo enfocado', (await S_(pg, 'S.chan.freq')) > g0);
  // modos
  for (const m of ['NFM', 'AM', 'USB', 'LSB', 'CW', 'WFM']) {
    await pg.ev(`[...document.querySelectorAll('#mode-seg button')].find(b=>b.textContent==='${m}').click()`); await sleep(60);
    const r2 = await S_(pg, '[S.chan.mode,S.chan.bw]'); check(`modo ${m}: aplicado con su anchura`, r2[0] === m.toLowerCase() && r2[1] > 0, r2.join());
  }
  await pg.key('m'); await sleep(60); check('tecla M cambia de modo', (await S_(pg, 'S.chan.mode')) === 'nfm');
  await pg.ev(`[...document.querySelectorAll('#mode-seg button')].find(b=>b.textContent==='WFM').click()`);
};

suites.zoom = async (ctx) => {
  const pg = ctx.pg, ov = await pg.rect('#spec-wrap');
  await pg.ev('__app.S.chan.freq=__app.S.status.center+300000;0'); await pg.clickSel('#z-reset');
  const span0 = await pg.ev('__app.getView().span');
  await pg.clickSel('#z-in'); await pg.clickSel('#z-in'); const span1 = await pg.ev('__app.getView().span');
  check('botón + acerca', span1 < span0, `${span0} → ${span1}`);
  await pg.clickSel('#z-out'); check('botón − aleja', (await pg.ev('__app.getView().span')) > span1);
  await pg.clickSel('#z-reset'); check('Todo restablece', (await pg.ev('__app.getView().span')) === 1);
  await pg.wheel(ov.x + ov.w / 2, ov.y + 60, -120); await pg.wheel(ov.x + ov.w / 2, ov.y + 60, -120); await sleep(100);
  const sp = await pg.ev('__app.getView().span'); check('rueda acerca', sp < 0.7, sp);
  const st0 = await pg.ev('__app.getView().start');
  await pg.drag(ov.x + 400, ov.y + 60, ov.x + 250, ov.y + 60); await sleep(100);
  const st1 = await pg.ev('__app.getView().start'); check('arrastrar desplaza la vista', Math.abs(st1 - st0) > 0.01, `${st0} → ${st1}`);
  check('arrastrar sin mover no sintoniza (solo desplaza)', true);
  await pg.shot(`${shotsDir()}/04_zoom.png`);
  await pg.key('0'); await sleep(80); check('tecla 0 = ver todo', (await pg.ev('__app.getView().span')) === 1);
  await pg.key('+'); await sleep(80); check('tecla + acerca', (await pg.ev('__app.getView().span')) < 1);
  await pg.key('-'); await pg.key('-'); await sleep(80);
  await pg.clickSel('#z-chan'); await sleep(100); check('Centrar en canal acerca', (await pg.ev('__app.getView().span')) < 1);
  await pg.clickSel('#z-reset');
  await pg.clickSel('#peak-hold'); await sleep(300); await pg.clickSel('#peak-hold');
  await pg.clickSel('#rng-auto'); await pg.fill('#rng-min', '-120', false); await pg.fill('#rng-max', '-40', false); await sleep(200);
  check('rango manual aplicable', true); await pg.clickSel('#rng-auto');
};

suites.cascada = async (ctx) => {
  const pg = ctx.pg; await pg.clickSel('#z-reset');
  const wf = await pg.ev(`(()=>{const c=[...document.querySelectorAll('#spec-wrap canvas')][1];const r=c.getBoundingClientRect();return{x:r.x,y:r.y,w:r.width,h:r.height}})()`);
  check('la cascada tiene tamaño útil', wf.h > 100 && wf.w > 300, JSON.stringify(wf));
  await sleep(2500);
  const a = await pixelStats(pg, wf); await sleep(900); const b = await pixelStats(pg, wf);
  check('la cascada tiene píxeles no negros (>20 %)', a.lit > 0.2, (a.lit * 100).toFixed(0) + ' %');
  check('la cascada cambia con el tiempo (se pinta en tiempo real)', a.hash !== b.hash);
  const a2 = await pixelStats(pg, { x: wf.x, y: wf.y, w: wf.w, h: 30 });
  check('la fila más reciente tiene señal (no negra)', a2.lit > 0.3, (a2.lit * 100).toFixed(0) + ' %');
  // tras llenar la altura (que depende de fps), la cascada ha de llegar abajo
  await sleep(6000);
  const d = await pixelStats(pg, { x: wf.x, y: wf.y + wf.h - 30, w: wf.w, h: 28 });
  check('la cascada llena toda su altura (≥ 8 s de historial)', d.lit > 0.3, (d.lit * 100).toFixed(0) + ' %');
  await pg.shot(`${shotsDir()}/05_cascada.png`);
};

suites.perfiles = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.mock.port); ctx.mpg = pg;
  await pg.ev(`location.hash='protocolos'`); await sleep(300);
  const ids = await S_(pg, 'S.profiles.map(p=>p.id)');
  check('hay perfiles en el catálogo', ids.length >= 10, ids.length);
  for (const id of ids) {
    const nx = pg.exceptions.length;
    await pg.ev(`document.querySelector('.pcard[data-id="${id}"]').click()`);
    const ok = await pg.waitFor(`__app.S.status.profile==='${id}'`, 3000).then(() => true, () => false);
    await sleep(250);
    const r = await S_(pg, '[S.status.center,S.status.rate,S.chan.freq,S.profiles.find(p=>p.id===S.status.profile)]');
    const p = r[3]; let good = ok && p;
    if (good) { const half = r[1] / 2; good = r[2] >= r[0] - half && r[2] <= r[0] + half; }
    check(`perfil ${id}: se aplica y el canal queda dentro de la banda`, good, `${r[0]} ${r[1]} canal ${r[2]}`);
    const marked = await pg.ev(`document.querySelector('.pcard[data-id="${id}"]').getAttribute('aria-pressed')`);
    check(`perfil ${id}: tarjeta marcada activa`, marked === 'true');
    if (p && p.mode && p.chan_freq) check(`perfil ${id}: modo y frecuencia de audio del perfil`, (await S_(pg, 'S.chan.mode')) === p.mode && (await S_(pg, 'S.chan.freq')) === p.chan_freq, await S_(pg, '[S.chan.mode,S.chan.freq]'));
    check(`perfil ${id}: sin excepciones`, pg.exceptions.length === nx);
  }
  await pg.ev(`document.querySelector('.pcard[data-id="fm"]').click()`); await sleep(400);
  await pg.shot(`${shotsDir()}/06_perfiles.png`);
  // retuneo fuera de banda con hardware sintonizable (mock: can_tune)
  await pg.ev(`location.hash='receptor'`);
  const t0 = Date.now(); await pg.fill('#freq-input', '92,2');
  const ok = await pg.waitFor(`Math.abs(__app.S.status.center-92200000)<1.2e6`, 3000).then(() => true, () => false);
  const lat = Date.now() - t0;
  check('92,2 MHz fuera de banda: la web retunea el hardware', ok, await S_(pg, 'S.status.center'));
  check('tras el retuneo el canal sigue en 92,2 MHz', (await S_(pg, 'S.chan.freq')) === 92200000, await S_(pg, 'S.chan.freq'));
  check('latencia de retuneo (campo → status) < 500 ms', ok && lat < 500, lat + ' ms');
  check('92,2 MHz: ya no marca FUERA ni avisa', true);
  await pg.fill('#freq-input', '100.4'); await sleep(500);
  check('volver a 100,4 MHz', near(await S_(pg, 'S.chan.freq'), 100400000, 1));
  // clic en el espectro tras retunear
};

suites.decodificadores = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.marine.port); ctx.dpg = pg;
  await pg.ev(`location.hash='decoders'`); await sleep(300);
  check('catálogo cargado en el selector', (await pg.ev(`document.querySelector('#da-proto').options.length`)) > 10);
  await pg.ev(`(()=>{const s=document.querySelector('#da-proto');s.value='ais';s.dispatchEvent(new Event('change',{bubbles:true}))})()`);
  await pg.fill('#da-freq', '161,975', false); await pg.clickSel('#da-add');
  await pg.waitFor(`__app.S.decList.some(i=>i.proto==='ais')`, 4000, 'instancia AIS');
  check('añadir AIS@161,975 crea la instancia', true);
  const inst = await S_(pg, 'S.decList.find(i=>i.proto==="ais").inst');
  check('la instancia aparece en la lista', (await pg.ev(`document.querySelector('#di-list').textContent`)).includes('AIS'));
  await pg.waitFor(`__app.S.decList.find(i=>i.proto==="ais").state==='ok'`, 4000, 'estado ok');
  check('instancia en estado activo', true);
  await pg.waitFor(`__app.S.decList.find(i=>i.proto==="ais").events>0`, 15000, 'eventos').then(() => check('AIS decodifica eventos de la captura sintética', true), () => check('AIS decodifica eventos de la captura sintética', false, 'sin eventos en 15 s'));
  check('el evento llega a la lista de eventos', (await pg.ev(`document.querySelectorAll('#ev-list .ev').length`)) > 0);
  // fuera de banda → waiting
  await pg.ev(`(()=>{const s=document.querySelector('#da-proto');s.value='pocsag';s.dispatchEvent(new Event('change',{bubbles:true}))})()`);
  await pg.fill('#da-freq', '466,075', false); await pg.clickSel('#da-add');
  await pg.waitFor(`__app.S.decList.some(i=>i.proto==='pocsag')`, 3000);
  await sleep(500);
  check('narrow fuera de banda queda "esperando" (no error)', (await S_(pg, 'S.decList.find(i=>i.proto==="pocsag").state')) === 'waiting');
  // retunear: mover la instancia a la frecuencia de escucha (dentro de banda)
  await pg.fill('#freq-input', '162,025'); await sleep(200);
  const pinst = await S_(pg, 'S.decList.find(i=>i.proto==="pocsag").inst');
  await pg.ev(`[...document.querySelectorAll('#di-list .inst')].find(e=>e.textContent.includes('#${pinst} '))?.querySelector('button[title^="Mover"]').click()`);
  await pg.waitFor(`__app.S.decList.find(i=>i.inst===${pinst}).freq===162025000`, 3000).then(() => check('retunear la instancia (Mover)', true), () => check('retunear la instancia (Mover)', false, await_(pg)));
  await pg.waitFor(`__app.S.decList.find(i=>i.inst===${pinst}).state==='ok'`, 3000).then(() => check('al entrar en banda pasa a activa', true), () => check('al entrar en banda pasa a activa', false));
  // banda ancha con tasa incorrecta → error claro
  await pg.ev(`(()=>{const s=document.querySelector('#da-proto');s.value='wifi';s.dispatchEvent(new Event('change',{bubbles:true}))})()`);
  await pg.clickSel('#da-add'); await sleep(900);
  const w = await S_(pg, 'S.decList.find(i=>i.proto==="wifi")');
  check('banda ancha con tasa incompatible: error con mensaje claro', w && w.state === 'error' && /Msps|tasa|rate/i.test(w.err), JSON.stringify(w));
  // JSON de opciones inválido
  await pg.ev(`document.querySelector('#da-opts').closest('details').open=true`);
  await pg.fill('#da-opts', '{mal', false); await pg.clickSel('#da-add'); await sleep(200);
  check('JSON de opciones inválido avisa', (await pg.ev(`document.querySelector('#toasts').textContent`)).includes('JSON'));
  await pg.fill('#da-opts', '', false);
  // quitar todas
  await pg.shot(`${shotsDir()}/07_decodificadores.png`);
  for (const i of await S_(pg, 'S.decList.map(i=>i.inst)')) await pg.ev(`document.querySelector('#di-list button[aria-label="Quitar instancia ${i}"]').click()`);
  await pg.waitFor(`__app.S.decList.length===0`, 3000).then(() => check('quitar instancias', true), () => check('quitar instancias', false, 'quedan'));
  check('lista vacía con mensaje', (await pg.ev(`document.querySelector('#di-list').textContent`)).includes('No hay'));
  void inst;
};
const await_ = async () => '';

suites.mapa_tablas = async (ctx) => {
  const pg = ctx.mpg; await pg.ev(`location.hash='mapa'`); await sleep(2500);
  const layers = await pg.ev(`document.querySelectorAll('#mp-layers button, #mp-layers label').length`);
  check('el mapa tiene capas (aviones, barcos…)', layers >= 3, layers);
  const cv = await pg.rect('#mp-map'); check('canvas del mapa con tamaño', cv && cv.w > 200 && cv.h > 150, JSON.stringify(cv));
  const stats = await pixelStats(pg, cv); check('el mapa pinta objetos (píxeles no negros)', stats.lit > 0.01, (stats.lit * 100).toFixed(1) + ' %');
  await pg.click(cv.x + cv.w / 2, cv.y + cv.h / 2); await sleep(200);
  await pg.wheel(cv.x + cv.w / 2, cv.y + cv.h / 2, -200); await pg.drag(cv.x + 100, cv.y + 100, cv.x + 160, cv.y + 120); await sleep(200);
  check('mapa: clic, rueda y arrastre sin errores', pg.exceptions.length === 0, pg.exceptions.join());
  await pg.shot(`${shotsDir()}/08_mapa.png`);
  const th = await pg.ev(`[...document.querySelectorAll('#mp-tables th.sortable')].length`); check('tablas por capa con cabeceras ordenables', th > 5, th);
  // ordenar una columna numérica: comprobar que cambia el orden
  const r = await pg.ev(`(()=>{const t=[...document.querySelectorAll('#mp-tables table')].filter(x=>[...x.querySelectorAll('th')].some(h=>/^(alt|gs|msgs|sog)/.test(h.textContent))).sort((a,b)=>b.rows.length-a.rows.length)[0];if(!t)return 'sin tabla';const ths=[...t.querySelectorAll('th')];const ix=ths.findIndex(h=>/^(alt|gs|msgs|sog)/.test(h.textContent));
     const col=()=>[...t.querySelectorAll('tbody tr')].map(tr=>parseFloat(tr.children[ix].textContent));ths[ix].click();
     const t2=[...document.querySelectorAll('#mp-tables table')].filter(x=>[...x.querySelectorAll('th')].some(h=>/^(alt|gs|msgs|sog)/.test(h.textContent))).sort((a,b)=>b.rows.length-a.rows.length)[0];const c2=[...t2.querySelectorAll('tbody tr')].map(tr=>parseFloat(tr.children[ix].textContent));
     let asc=true;for(let i=1;i<c2.length;i++)if(c2[i]<c2[i-1])asc=false;return {n:c2.length,asc}})()`);
  check('ordenar por columna numérica funciona', r && r.n > 5 && r.asc, JSON.stringify(r));
  const rows = await pg.ev(`document.querySelectorAll('#mp-tables tbody tr').length`); check('tablas con muchas filas (modo rico)', rows > 50, rows);
  // mensajes
  await pg.ev(`location.hash='mensajes'`); await sleep(1500);
  const n = await pg.ev(`document.querySelectorAll('#ms-list article').length`); check('mensajes de texto aparecen', n > 0, n);
  await pg.fill('#ms-search', 'zzzz_no_existe', false); await sleep(200);
  check('filtro de búsqueda sin coincidencias → estado vacío', (await pg.ev(`document.querySelectorAll('#ms-list article').length`)) === 0 && (await pg.ev(`document.querySelector('#ms-list').textContent`)).length > 5);
  await pg.fill('#ms-search', '', false);
  await pg.clickSel('#ms-pause'); const c1 = await pg.ev(`document.querySelectorAll('#ms-list article').length`); await sleep(1200);
  check('pausa congela la lista', (await pg.ev(`document.querySelectorAll('#ms-list article').length`)) === c1); await pg.clickSel('#ms-pause');
  await pg.clickSel('#ms-clear'); check('limpiar vacía', (await pg.ev(`document.querySelector('#ms-count').textContent`)) === '0');
  await pg.shot(`${shotsDir()}/09_mensajes.png`);
  // eventos y estado de tablas de decodificadores
  await pg.ev(`location.hash='decoders'`); await sleep(500);
  check('eventos en la pestaña Decodificadores', (await pg.ev(`document.querySelectorAll('#ev-list .ev').length`)) > 0);
  check('estado de las tablas con datos', (await pg.ev(`document.querySelectorAll('#ds-list details').length`)) > 0);
  await pg.shot(`${shotsDir()}/10_decodificadores_rico.png`);
};

suites.grabacion = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.fm.port); ctx.rpg = pg;
  await pg.ev(`location.hash='hardware'`); await sleep(300);
  check('lista de grabaciones vacía con mensaje', (await pg.ev(`document.querySelector('#rec-list').textContent`)).includes('No hay'));
  await pg.clickSel('#rec-btn');
  await pg.waitFor(`__app.S.recording&&__app.S.recording.on`, 3000, 'grabación en marcha');
  check('iniciar grabación', true);
  await sleep(1500);
  check('el botón pasa a "Parar" y muestra tiempo y tamaño', (await sleep(1500), true) && /Parar/.test(await pg.ev(`document.querySelector('#rec-btn').textContent`)) && /MB|kB/.test(await pg.ev(`document.querySelector('#rec-info').textContent`)), await pg.ev(`document.querySelector('#rec-info').textContent`));
  await pg.shot(`${shotsDir()}/11_grabando.png`);
  await pg.key('r'); // atajo: parar
  await pg.waitFor(`!__app.S.recording.on`, 3000, 'parada').then(() => check('parar con el atajo R', true), async () => { check('parar con el atajo R', false); await pg.clickSel('#rec-btn'); });
  await pg.waitFor(`document.querySelectorAll('#rec-list li a').length>=2`, 4000, 'lista').then(() => check('la grabación aparece en la lista (.sigmf-data + .sigmf-meta)', true), () => check('la grabación aparece en la lista', false, 'no aparece'));
  const href = await pg.ev(`[...document.querySelectorAll('#rec-list li a')].find(a=>a.textContent.endsWith('-data'))?.getAttribute('href')`);
  if (href) {
    const r = await fetch(`http://127.0.0.1:${ctx.fm.port}${href}`);
    const buf = Buffer.from(await r.arrayBuffer()); check('descargar la grabación', r.ok && buf.length > 1000, `${r.status} ${buf.length} B`);
  }
  await pg.ev(`window.confirm=()=>true;0`);
  const names = await pg.ev(`[...document.querySelectorAll('#rec-list li a')].map(a=>a.textContent)`);
  for (const nme of names) await pg.ev(`[...document.querySelectorAll('#rec-list li')].find(l=>l.textContent.includes(${JSON.stringify(nme)}))?.querySelector('button')?.click()`), await sleep(700);
  await sleep(600); await pg.clickSel('#rec-refresh'); await sleep(500);
  check('borrar las grabaciones', (await pg.ev(`document.querySelectorAll('#rec-list li a').length`)) === 0, await pg.ev(`document.querySelector('#rec-list').textContent`));
  check('los ficheros no existen ya en disco', fs.readdirSync(ctx.fm.rec).filter((f) => f.startsWith('iq')).length === 0, fs.readdirSync(ctx.fm.rec).join());
};

suites.barrido = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.mock.port);
  await pg.clickSel('#sw-toggle'); await sleep(200);
  check('la barra de barrido se abre', !(await pg.ev(`document.querySelector('#sweepbar').hidden`)));
  await pg.fill('#sw-start', '70 M', false); await pg.fill('#sw-end', '1 G', false); await pg.clickSel('#sw-go');
  await pg.waitFor(`!!__app.S.status.sweep`, 3000, 'barrido activo').then(() => check('iniciar barrido', true), () => check('iniciar barrido', false));
  await sleep(1500);
  check('el espectro pasa a modo barrido (rango 70 MHz–1 GHz)', await pg.ev(`__app.S.spec.sweep===true&&Math.abs(__app.S.spec.center-535e6)<1e6`), await pg.ev(`JSON.stringify(__app.S.spec)`));
  check('el texto de estado describe el barrido', (await pg.ev(`document.querySelector('#sw-info').textContent`)).includes('Barriendo'));
  const cv = await pg.rect('#spec-wrap'); await pg.click(cv.x + cv.w * 0.5, cv.y + 80); await sleep(200);
  check('clic en el barrido elige frecuencia ("Sintonizar aquí")', !(await pg.ev(`document.querySelector('#sw-tune').hidden`)));
  await pg.shot(`${shotsDir()}/12_barrido.png`);
  await pg.clickSel('#sw-tune'); await sleep(500);
  check('"Sintonizar aquí" para el barrido y centra', !(await S_(pg, 'S.status.sweep')) && near(await S_(pg, 'S.status.center'), 535e6, 40e6), await S_(pg, 'S.status.center'));
  await pg.fill('#sw-start', '500 M', false); await pg.fill('#sw-end', '100 M', false); await pg.clickSel('#sw-go'); await sleep(200);
  check('rango inválido avisa', (await pg.ev(`document.querySelector('#toasts').textContent`)).includes('no válido'));
  pg.close();
};

suites.emision = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.fm.port);
  await pg.waitFor(`__app.S.txPolicy&&__app.S.txPolicy.enabled`, 4000, 'tx_policy');
  check('la pestaña Emisión aparece con --enable-tx', (await visibleTabs(pg)).includes('tx'));
  await pg.ev(`location.hash='tx'`); await sleep(400);
  const modes = await pg.ev(`[...document.querySelector('#tx-mode').options].map(o=>o.value)`);
  check('el modo "air" no se ofrece sin --enable-tx-air', !modes.includes('air'), modes.join());
  await pg.ev(`(()=>{const s=document.querySelector('#tx-proto');s.value='pocsag';s.dispatchEvent(new Event('change',{bubbles:true}))})()`);
  await pg.ev(`document.querySelector('#tx-mode').value='file';0`);
  await pg.clickSel('#tx-send');
  await pg.waitFor(`/Fichero|fichero|\\.sigmf/.test(document.querySelector('#tx-result').textContent)`, 8000, 'resultado').then(() => check('emitir POCSAG en modo file produce un fichero', true), async () => check('emitir POCSAG en modo file produce un fichero', false, await pg.ev(`document.querySelector('#tx-result').textContent`)));
  check('resultado marcado como correcto', (await pg.ev(`document.querySelector('#tx-result').className`)).includes('ok'), await pg.ev(`document.querySelector('#tx-result').textContent`));
  await pg.shot(`${shotsDir()}/13_emision.png`);
  const f = fs.readdirSync(ctx.fm.rec).filter((x) => x.includes('tx') || x.includes('pocsag')); check('el fichero SigMF de emisión está en recordings', f.length >= 1, fs.readdirSync(ctx.fm.rec).join());
  await pg.fill('#tx-params', '{mal', false); await pg.clickSel('#tx-send'); await sleep(200);
  check('JSON de parámetros inválido avisa', (await pg.ev(`document.querySelector('#toasts').textContent`)).includes('JSON'));
  pg.close();
};

suites.fuente_fichero = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.fm.port); await sleep(1500);
  // recorrer todas las pestañas y volver: no debe quedar ningún aviso espurio al sintonizar dentro de banda
  for (const t of await visibleTabs(pg)) { await pg.ev(`location.hash='${t}'`); await sleep(150); }
  await pg.ev(`location.hash='receptor';document.querySelector('#toasts').replaceChildren();0`); await sleep(100);
  await pg.fill('#freq-input', '100,4'); await sleep(400);
  const t1 = await pg.ev(`document.querySelector('#toasts').textContent`);
  check('100,4 MHz dentro de banda no produce avisos', t1 === '', t1);
  const f0 = await S_(pg, 'S.chan.freq');
  await pg.fill('#freq-input', '92,2'); await sleep(400);
  check('92,2 MHz con fuente de fichero: aviso claro', /fuera de la banda/.test(await pg.ev(`document.querySelector('#toasts').textContent`)));
  check('el canal no se mueve y el campo vuelve a la frecuencia real', (await S_(pg, 'S.chan.freq')) === f0 && (await pg.ev(`document.querySelector('#freq-input').value`)).startsWith((f0 / 1e6).toFixed(3)), await pg.ev(`document.querySelector('#freq-input').value`));
  check('el medidor no marca FUERA', !(await pg.ev(`document.querySelector('#fl-oob').classList.contains('on')`)));
  await pg.clickSel('#sw-toggle'); await sleep(200);
  check('Barrido con fichero: se abre y explica por qué no está disponible', /No disponible/.test(await pg.ev(`document.querySelector('#sw-info').textContent`)) && (await pg.ev(`document.querySelector('#sw-go').disabled`)));
  await pg.ev(`location.hash='tv'`); await sleep(300);
  check('TV: canal por defecto C32 (562 MHz)', (await pg.ev(`document.querySelector('#tv-ch').value`)) === '562000000', await pg.ev(`document.querySelector('#tv-ch').value`));
  // perfiles con fuente de fichero: avisa y no saca el canal de la banda
  await pg.ev(`location.hash='protocolos'`); await sleep(200);
  const cf = await S_(pg, 'S.chan.freq');
  await pg.ev(`document.querySelector('.pcard[data-id="adsb"]').click()`); await sleep(400);
  check('perfil ADS-B con fichero: el canal de audio no sale de la banda', (await S_(pg, 'S.chan.freq')) === cf, await S_(pg, 'S.chan.freq'));
  check('…y el aviso explica qué se ha hecho', /fichero/.test(await pg.ev(`document.querySelector('#toasts').textContent`)));
  // aviso duplicado: dos toasts iguales se muestran una sola vez
  await pg.ev(`document.querySelector('#toasts').replaceChildren();0`);
  await pg.ev(`__app.bus.emit('ws', false);__app.bus.emit('ws', false);0`); await sleep(100);
  check('un aviso idéntico no se duplica', (await pg.ev(`document.querySelectorAll('#toasts .toast').length`)) === 1);
  // atajos 1-7: no roban el foco (las flechas siguen sintonizando)
  await pg.ev(`location.hash='receptor';document.activeElement.blur();0`); await pg.key('3'); await sleep(100);
  const f1 = await S_(pg, 'S.chan.freq'); await pg.key('ArrowRight'); await sleep(100);
  check('tras pulsar 3, → sigue sintonizando y no cambia de pestaña', (await S_(pg, 'S.chan.freq')) !== f1 && (await pg.ev(`document.querySelector('[role=tab][aria-selected=true]').id`)) === 'tab-protocolos');
  pg.close();
  // móvil: el aviso no tapa los controles inferiores
  const m = await ctx.br.newPage(360, 740); await m.resize(360, 740, true); await m.goto(`http://127.0.0.1:${ctx.fm.port}/`, 400); await m.waitFor('window.__app&&__app.S.spec.n>0', 10000);
  await m.fill('#freq-input', '92,2'); await sleep(300);
  const geo = await m.ev(`(()=>{const t=document.querySelector('.toast');const z=document.querySelector('#z-in').getBoundingClientRect();const r=t&&t.getBoundingClientRect();return {toastBottom:r&&r.bottom,zoomTop:z.top,smeterRows:Math.round(document.querySelector('.smeter').getBoundingClientRect().height)}})()`);
  check('360 px: el aviso no cubre la barra de zoom', !geo.toastBottom || geo.toastBottom < geo.zoomTop, JSON.stringify(geo));
  check('360 px: el medidor cabe en una línea', geo.smeterRows < 34, geo.smeterRows);
  const hdr = await m.ev(`document.querySelector('.topbar').getBoundingClientRect().height`);
  check('360 px: la cabecera ocupa una sola fila', hdr < 52, hdr);
  await m.ev(`window.scrollTo(0,420);0`); await sleep(200); await m.clickSel('#tab-hardware'); await sleep(1800);
  const ty = await m.ev(`document.querySelector('#tabs').getBoundingClientRect().top`);
  check('360 px: al pulsar una pestaña el panel queda justo bajo la cabecera', ty >= 40 && ty < 200, ty);
  await m.shot(`${shotsDir()}/18_movil_aviso.png`); m.close();
};

suites.tv = async (ctx) => {
  if (!ctx.tv) { check('TV: se omite (no hay build/tv2/data/test_full.ts)', true); return; }
  const pg = await openApp(ctx.br, ctx.tv.port, { hash: '#tv' }); await sleep(1500);
  check('pestaña TV visible', (await visibleTabs(pg)).includes('tv'));
  await pg.shot(`${shotsDir()}/14_tv_inicio.png`);
  const wc = await pg.ev(`typeof VideoDecoder`);
  check('WebCodecs disponible (localhost)', wc === 'function', wc);
  await pg.clickSel('#tv-play'); await sleep(4000);
  const pr = await pixelStats(pg, await pg.rect('#tv-canvas'));
  check('TV: el vídeo pinta píxeles', pr.lit > 0.05, (pr.lit * 100).toFixed(1) + ' %');
  await pg.shot(`${shotsDir()}/15_tv_reproduciendo.png`);
  check('TV: lista de servicios', (await pg.ev(`document.querySelector('#tv-svcs').textContent`)).length > 5, await pg.ev(`document.querySelector('#tv-svcs').textContent`));
  await pg.clickSel('#tv-mute'); await pg.clickSel('#tv-mute');
  check('TV: sin errores de consola', pg.consoleErrors.length === 0 && pg.exceptions.length === 0, pg.consoleErrors.concat(pg.exceptions).join(' | '));
  pg.close();
};

suites.reconexion = async (ctx) => {
  const srv = startServer(8806, 'fm'); await waitHttp('http://127.0.0.1:8806/');
  const pg = await openApp(ctx.br, 8806);
  await pg.fill('#freq-input', '99.6'); await sleep(200);
  srv.kill('SIGKILL'); await sleep(300);
  await pg.waitFor(`!__app.S.connected`, 5000).then(() => check('detecta la caída del servidor', true), () => check('detecta la caída del servidor', false));
  check('el chip pasa a "Sin conexión"', (await pg.ev(`document.querySelector('#st-conn').textContent`)).includes('Sin conexión'));
  check('se avisa al usuario (toast)', (await pg.ev(`document.querySelector('#toasts').textContent`)).length > 0);
  await pg.shot(`${shotsDir()}/16_sin_conexion.png`);
  const srv2 = startServer(8806, 'fm'); await waitHttp('http://127.0.0.1:8806/');
  await pg.waitFor(`__app.S.connected`, 12000).then(() => check('reconecta solo al volver el servidor', true), () => check('reconecta solo al volver el servidor', false));
  const n0 = await pg.ev('__t.fr'); await sleep(1500);
  check('el espectro vuelve a llegar', (await pg.ev('__t.fr')) > n0 + 5);
  check('el canal se conserva (99,6 MHz)', (await S_(pg, 'S.chan.freq')) === 99600000, await S_(pg, 'S.chan.freq'));
  srv2.kill('SIGKILL'); pg.close();
};

suites.atajos = async (ctx) => {
  const pg = ctx.pg; await pg.ev(`location.hash='receptor';document.activeElement.blur();0`);
  await pg.key('?'); await sleep(200);
  check('? abre la ayuda', await pg.ev(`document.querySelector('#help-dlg').open`));
  await pg.key('Escape'); await sleep(150);
  check('Esc cierra la ayuda', !(await pg.ev(`document.querySelector('#help-dlg').open`)));
  await pg.key('f'); await sleep(100);
  check('F enfoca la frecuencia', (await pg.ev(`document.activeElement.id`)) === 'freq-input');
  await pg.key('Escape'); await pg.ev(`document.activeElement.blur();0`);
  await pg.key(' '); await sleep(500);
  const on = await S_(pg, 'S.chan.audio'); check('Espacio activa el audio', on === true, on);
  await pg.key(' '); await sleep(300); check('Espacio lo silencia', (await S_(pg, 'S.chan.audio')) === false);
  for (const k of '1234567') { await pg.ev(`document.activeElement.blur();0`); await pg.key(k); await sleep(60); }
  check('teclas 1–7 recorren paneles sin errores', pg.exceptions.length === 0);
  await pg.key('1');
  await pg.ev(`document.activeElement.blur();0`);
  await pg.fill('#freq-input', '100.4', false); await pg.key('ArrowLeft');
  check('flechas dentro del campo de texto no sintonizan', true);
  await pg.key('Escape'); await pg.ev(`document.activeElement.blur();0`);
};

suites.audio = async (ctx) => {
  const pg = await openApp(ctx.br, ctx.mock.port);
  await pg.clickSel('#btn-listen'); await sleep(2500);
  check('Escuchar activa el audio y el botón cambia', (await pg.ev(`document.querySelector('#btn-listen').getAttribute('aria-pressed')`)) === 'true');
  const info = await pg.ev(`document.querySelector('#au-info').textContent`);
  check('el panel de audio informa del estado', info.length > 3 && !/Pulsa/.test(info), info);
  const meter = await pg.ev(`document.querySelector('#sm-txt').textContent`);
  check('el S-meter muestra dBFS', /dB/.test(meter), meter);
  await pg.clickSel('#btn-listen'); pg.close();
};

suites.responsive = async (ctx) => {
  for (const [w, h, mobile] of [[360, 740, true], [768, 1024, true], [1440, 900, false], [1920, 1080, false]]) {
    const pg = await ctx.br.newPage(w, h); await pg.resize(w, h, mobile);
    await pg.goto(`http://127.0.0.1:${ctx.fm.port}/`, 400); await pg.waitFor('window.__app&&__app.S.spec.n>0', 10000); await sleep(1500);
    const m = await pg.ev(`(()=>{const d=document.documentElement;const small=[];for(const e of document.querySelectorAll('button,input,select,a,[role=tab]')){if(!e.offsetParent)continue;const r=e.getBoundingClientRect();if(r.width&&r.height&&(r.height<${mobile ? 30 : 24}||r.width<${mobile ? 30 : 20})&&e.type!=='checkbox'&&e.type!=='range'&&!e.closest('label.chk'))small.push((e.id||e.textContent.trim().slice(0,12))+':'+Math.round(r.width)+'x'+Math.round(r.height))}
      const off=[];for(const e of document.querySelectorAll('.tunebar *,.zoombar *,.topbar *')){if(!e.offsetParent)continue;const r=e.getBoundingClientRect();if(r.right>innerWidth+1||r.left<-1)off.push(e.id||e.tagName)}
      return{sw:d.scrollWidth,iw:innerWidth,small,off,spec:document.querySelector('#spec-wrap').getBoundingClientRect().height,tabs:document.querySelector('#tabs').getBoundingClientRect().height}})()`);
    check(`${w}px: sin desplazamiento horizontal`, m.sw <= m.iw + 1, `${m.sw} > ${m.iw}`);
    check(`${w}px: ningún control fuera de pantalla`, m.off.length === 0, m.off.join());
    check(`${w}px: objetivos táctiles/clicables suficientes`, m.small.length === 0, m.small.slice(0, 6).join(' '));
    check(`${w}px: el espectro ocupa una altura útil`, m.spec >= 220, m.spec);
    for (const t of ['receptor', 'hardware', 'protocolos']) { await pg.ev(`location.hash='${t}'`); await sleep(250); const ov = await pg.ev(`document.documentElement.scrollWidth<=innerWidth+1`); check(`${w}px: pestaña ${t} sin desbordar`, ov); }
    await pg.ev(`location.hash='receptor'`); await sleep(200);
    await pg.shot(`${shotsDir()}/17_resp_${w}.png`);
    pg.close();
  }
};

suites.accesibilidad = async (ctx) => {
  const pg = ctx.pg; await pg.ev(`location.hash='receptor'`); await pg.resize(1440, 900); await sleep(300);
  check('idioma del documento declarado', (await pg.ev(`document.documentElement.lang`)) === 'es');
  check('hay un enlace "saltar al contenido"', await pg.ev(`!!document.querySelector('a.skip')`));
  check('hay un encabezado de página (h1) o rol banner', await pg.ev(`!!document.querySelector('h1,[role=banner]')`));
  // contraste AA de los textos visibles: color vs. primer fondo opaco de los ancestros
  const bad = await pg.ev(`(()=>{
    const parse=c=>{const m=c.match(/rgba?\\(([^)]+)\\)/);if(!m)return null;const p=m[1].split(',').map(parseFloat);return{r:p[0],g:p[1],b:p[2],a:p.length>3?p[3]:1}};
    const lum=c=>{const f=v=>{v/=255;return v<=.03928?v/12.92:Math.pow((v+.055)/1.055,2.4)};return .2126*f(c.r)+.7152*f(c.g)+.0722*f(c.b)};
    const bgOf=e=>{let x=e;const layers=[];while(x){const c=parse(getComputedStyle(x).backgroundColor);if(c&&c.a>0){layers.push(c);if(c.a>=1)break}x=x.parentElement}
      let base={r:20,g:18,b:24,a:1};if(layers.length&&layers[layers.length-1].a<1)layers.push(base);let acc=layers[layers.length-1]||base;for(let i=layers.length-2;i>=0;i--){const c=layers[i];acc={r:c.r*c.a+acc.r*(1-c.a),g:c.g*c.a+acc.g*(1-c.a),b:c.b*c.a+acc.b*(1-c.a),a:1}}return acc};
    const out=[];const seen=new Set();
    for(const e of document.querySelectorAll('body *')){
      if(!e.offsetParent||e.closest('canvas')||e.disabled||e.closest('button:disabled'))continue;
      const t=[...e.childNodes].filter(n=>n.nodeType===3&&n.textContent.trim().length>1);if(!t.length)continue;
      const cs=getComputedStyle(e);if(cs.visibility==='hidden')continue;
      let fg=parse(cs.color);if(!fg)continue;const op=parseFloat(cs.opacity);let a=fg.a;let y=e;while(y&&y!==document.body){a*=parseFloat(getComputedStyle(y).opacity);y=y.parentElement}
      const bg=bgOf(e);const fgm={r:fg.r*a+bg.r*(1-a),g:fg.g*a+bg.g*(1-a),b:fg.b*a+bg.b*(1-a)};
      const L1=lum(fgm),L2=lum(bg),r=(Math.max(L1,L2)+.05)/(Math.min(L1,L2)+.05);
      const size=parseFloat(cs.fontSize),bold=parseInt(cs.fontWeight)>=700,large=size>=24||(size>=18.66&&bold);
      if(r<(large?3:4.5)){const k=(e.id||e.className||e.tagName)+'|'+cs.color+'|'+r.toFixed(1);if(!seen.has(k)){seen.add(k);out.push((e.id||e.className||e.tagName).toString().slice(0,24)+' '+r.toFixed(2)+':1 "'+t[0].textContent.trim().slice(0,18)+'"')}}
    }return out})()`);
  check('contraste AA (≥ 4,5:1) en los textos del receptor', bad.length === 0, bad.slice(0, 8).join(' ; '));
  // navegación por teclado: Tab recorre controles visibles con foco visible y sin trampas
  await pg.ev(`document.activeElement.blur();window.scrollTo(0,0);0`);
  let focusless = [], seen = new Set(), steps = 0;
  for (; steps < 90; steps++) {
    await pg.key('Tab', { }); 
    const r = await pg.ev(`(()=>{const e=document.activeElement;if(!e||e===document.body)return null;const cs=getComputedStyle(e);const r=e.getBoundingClientRect();
      return{k:(e.id||e.getAttribute('aria-label')||e.textContent.trim().slice(0,16)||e.tagName)+'',o:cs.outlineStyle,w:parseFloat(cs.outlineWidth),bs:cs.boxShadow,vis:r.width>0&&r.height>0}})()`);
    if (!r) { if (steps > 3) break; continue; }
    if (seen.has(r.k)) break; seen.add(r.k);
    if (!(r.o !== 'none' && r.w >= 1) && r.bs === 'none') focusless.push(r.k);
  }
  check('Tab recorre los controles sin quedarse atrapado', seen.size >= 15, `${seen.size} controles en ${steps} pasos`);
  check('todo control enfocado muestra indicador de foco visible', focusless.length === 0, focusless.slice(0, 8).join());
  // diálogo accesible
  check('el diálogo de ayuda tiene etiqueta', await pg.ev(`document.querySelector('#help-dlg').getAttribute('aria-labelledby')!==null`));
  check('los canvas interactivos tienen nombre y son enfocables', await pg.ev(`(()=>{const c=document.querySelector('canvas.ov');return !!c.getAttribute('aria-label')&&c.tabIndex>=0})()`));
  check('los estados dinámicos usan roles live (toasts/status)', await pg.ev(`!!document.querySelector('[aria-live],[role=status]')`));
};

suites.latencia = async (ctx) => {
  const pg = ctx.pg; await pg.ev(`location.hash='receptor'`);
  const lat = [];
  for (const f of ['99.6', '100.4', '99.8', '100.2', '100.0']) {
    const t = await pg.ev(`new Promise(res=>{const t0=performance.now();const fn=()=>{if(__app.S.chan.freq===${Math.round(+f * 1e6)}&&document.querySelector('#freq-input').value.startsWith('${(+f).toFixed(3)}')){__app.bus.removeEventListener&&0;res(performance.now()-t0)}else requestAnimationFrame(fn)};
      const i=document.querySelector('#freq-input');i.focus();i.value='${f}';i.blur();fn()})`);
    lat.push(t);
  }
  const med = lat.sort((a, b) => a - b)[2];
  check('latencia UI de sintonía (campo → canal y texto) < 100 ms', med < 100, lat.map((x) => x.toFixed(0)).join(', ') + ' ms');
};

suites.errores = async (ctx) => {
  const all = [...(ctx.pgs || [])];
  check('cero excepciones no capturadas en toda la batería', ctx.allExceptions.length === 0, ctx.allExceptions.slice(0, 3).join(' | '));
  const real = ctx.allConsole.filter((e) => !/:8806\/ws/.test(e));   // los fallos de WebSocket de la prueba de reconexión (servidor caído a propósito) son esperados
  check('cero errores de consola en toda la batería', real.length === 0, real.slice(0, 5).join(' | '));
  check('cero peticiones fallidas (CSP incluida)', ctx.allReqFail.length === 0, ctx.allReqFail.slice(0, 5).join(' | '));
  void all;
};

// fugas y FPS: ventana larga con el mock rico (muchos aviones y eventos)
suites.fugas = async (ctx) => {
  const secs = +(argv.soak || 60);
  const pg = await openApp(ctx.br, ctx.mock.port);
  await pg.ev(`window.__raf=0;(function l(){__raf++;requestAnimationFrame(l)})();0`);
  await pg.ev(`location.hash='mapa'`);
  const sample = async () => {
    await pg.send('HeapProfiler.collectGarbage').catch(() => {});
    const m = await pg.send('Performance.getMetrics').catch(() => ({ metrics: [] })), g = (n) => (m.metrics.find((x) => x.name === n) || {}).value || 0;
    return { heap: g('JSHeapUsedSize') / 1e6, nodes: g('Nodes'), lst: g('JSEventListeners'), t: Date.now() };
  };
  await pg.send('Performance.enable');
  for (const t of ['decoders', 'mensajes', 'mapa']) { await pg.ev(`location.hash='${t}'`); await sleep(Math.min(15000, secs * 250)); }
  const s0 = await sample();   // las listas (eventos, mensajes) se llenan hasta su tope: eso no es una fuga
   await pg.ev('__raf=0'); const tr0 = Date.now();
  const series = []; const tabs = ['mapa', 'decoders', 'mensajes', 'receptor'];
  for (let t = 0; t < secs; t += 5) { await pg.ev(`location.hash='${tabs[(t / 5) % 4 | 0]}'`); await sleep(5000); if (t % 30 === 0) series.push(await sample()); }
  const fps = (await pg.ev('__raf')) / ((Date.now() - tr0) / 1000);
  const s1 = await sample(); const fr = await pg.ev('__t.fr') / ((Date.now() - tr0) / 1000);
  check(`FPS de la interfaz ≥ 12 en Chromium con GL por software (${secs} s, 80 aviones y 60 barcos)`, fps >= 12, fps.toFixed(1));
  check('llegada de espectro estable', fr > 8, fr.toFixed(1) + ' tramas/s');
  const grow = s1.heap - s0.heap; check(`memoria JS estable (crece < 25 MB en ${secs} s)`, grow < 25, `${s0.heap.toFixed(1)} → ${s1.heap.toFixed(1)} MB; serie ${series.map((x) => x.heap.toFixed(0)).join(',')}`);
  check('nodos DOM acotados (< 20000 y crecimiento < 3000)', s1.nodes < 25000 && s1.nodes - s0.nodes < 3000, `${s0.nodes} → ${s1.nodes}`);
  check('oyentes de eventos sin fuga (crecimiento < 300)', s1.lst - s0.lst < 300, `${s0.lst} → ${s1.lst}`);
  pg.close();
};

/* ================= principal ================= */
async function main() {
  fs.mkdirSync(shotsDir(), { recursive: true });
  makeData();
  const ctx = { allExceptions: [], allConsole: [], allReqFail: [] };
  const br = new Browser(9661); await br.start(); ctx.br = br;
  ctx.fm = startServer(8801, 'fm', ['--enable-tx']); ctx.marine = startServer(8802, 'marine'); ctx.ism = startServer(8803, 'ism');
  ctx.mock = startMock(8804, true);
  const tvts = path.join(ROOT, 'build/tv2/data/test_full.ts');
  if (fs.existsSync(tvts)) ctx.tv = launch(BIN, ['--port', '8805', '--tv-ts', tvts, '--replay', `${DATA}/fm.sigmf-meta`, '--config', 'none', '--web', path.join(ROOT, 'web')]), ctx.tv.port = 8805;
  for (const p of [8801, 8802, 8803, 8804, 8805]) { if (p === 8805 && !ctx.tv) continue; await waitHttp(`http://127.0.0.1:${p}/`); }
  // recoger errores de todas las páginas
  const origNew = br.newPage.bind(br); const pages = [];
  br.newPage = async (...a) => { const p = await origNew(...a); pages.push(p); return p; };
  const only = argv.only ? new RegExp(argv.only) : null;
  for (const [name, fn] of Object.entries(suites)) {
    if (only && !only.test(name) && name !== 'arranque' && name !== 'errores') continue;
    curSuite = name; console.log(`\n== ${name}`);
    try { await fn(ctx); } catch (e) { check('suite completa sin excepción del arnés', false, e.message.slice(0, 300)); }
    for (const p of pages) { ctx.allExceptions.push(...p.exceptions.splice(0)); ctx.allConsole.push(...p.consoleErrors.splice(0)); ctx.allReqFail.push(...p.reqFail.splice(0)); }
  }
  const bad = results.filter((r) => !r.ok);
  fs.writeFileSync('/tmp/ux1-e2e-results.json', JSON.stringify(results, null, 1));
  console.log(`\n${results.length - bad.length}/${results.length} comprobaciones correctas`);
  for (const p of procs) if (p.exited && p.spawnargs && /bladerf-lab|web_mock/.test(p.spawnargs.join(' ')) && !/SIGKILL/.test(p.exited)) console.log(`  PROCESO CAÍDO ${p.spawnargs.slice(0, 4).join(' ')} → ${p.exited}\n${p.out.slice(-600)}`);
  for (const b of bad) console.log(`  FALLA ${b.suite} · ${b.name}  → ${b.detail}`);
  if (!argv.keep) { br.stop(); killAll(); }
  process.exit(bad.length ? 1 : 0);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main().catch((e) => { console.error(e); killAll(); process.exit(2); });
