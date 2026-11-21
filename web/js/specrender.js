// specrender.js — dibujo del espectro (traza) y de la cascada, sin tocar el DOM.
//
// Este módulo lo carga tanto el hilo principal (alternativa cuando el navegador no tiene
// OffscreenCanvas) como el worker `specworker.js`, que es el camino normal: los dos lienzos del
// espectro se ceden con transferControlToOffscreen() y todo el trabajo por imagen (conversión de
// bytes a dB, media exponencial, retención de picos, subida de la fila nueva a la textura, sombreado
// de la cascada y trazado de la curva) ocurre fuera del hilo de la interfaz. El hilo principal solo
// manda la VISTA (inicio/anchura del zoom, rango de dB, tamaño) y los datos binarios del WebSocket
// por transferencia (sin copia), de modo que la interacción (rueda, arrastre, pellizco) no espera
// a nadie.
//
// Cascada. WebGL2 (o WebGL1 si no hay): una textura LUMINANCE de n × 1024 usada como anillo (cada
// trama escribe UNA fila con texSubImage2D) y una textura de paleta de 256 × 1 (la «1D» de WebGL: alto
// 1). El shader aplica zoom (x0..x1), recorre el anillo y colorea según el rango de dB. Sin WebGL, un
// canvas 2D desplazado como anillo (drawImage con dos tramos).
//
// Codificación interna (1 byte): q = (dB − QMIN)/QSTEP; 0 queda reservado a «sin datos». Cada trama se
// convierte con SU dbmin/dbstep (lut), así cambiar el rango de colores no obliga a reescribir el
// historial: el color se decide en el shader, no al guardar la fila.
//
// Traza. Bins → píxeles: con más bins que píxeles, máximo por columna (no se pierden picos
// estrechos); con menos, interpolación lineal. La media es exponencial (α = 0,55) salvo en barrido.

export const QMIN = -160, QSTEP = 0.7, WF_ROWS = 1024, MAX_2D_W = 4096, WF_MAX_SWEEP = 4096, AXIS_H = 18;
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);

/* Paletas de la cascada (RGBA, 256 entradas, interpoladas entre puntos de control [t, r, g, b]).
 * «classic» es la de siempre (negro, azul, cian, verde, amarillo, rojo, blanco): útil pero mezcla verde y rojo.
 * «viridis» (van der Walt y Smith, matplotlib) y «cividis» (Nuñez, Anderton y Renslow 2018, PLoS ONE 13(7)) son
 * perceptualmente uniformes: la luminosidad crece de forma monótona con el nivel, así que el orden se lee igual
 * con visión normal que con deuteranopía, protanopía o en escala de grises. Los puntos de control son muestras
 * de los mapas oficiales cada 1/8. */
const PALETTES = {
  classic: [[0, 0, 0, 20], [0.15, 10, 20, 120], [0.35, 20, 150, 200], [0.5, 40, 200, 90], [0.7, 240, 230, 50], [0.88, 240, 70, 30], [1, 255, 255, 255]],
  viridis: [[0, 68, 1, 84], [0.125, 72, 36, 117], [0.25, 65, 68, 135], [0.375, 53, 95, 141], [0.5, 42, 120, 142], [0.625, 34, 168, 132], [0.75, 122, 209, 81], [0.875, 189, 223, 38], [1, 253, 231, 37]],
  cividis: [[0, 0, 32, 77], [0.125, 0, 51, 112], [0.25, 49, 68, 107], [0.375, 87, 92, 108], [0.5, 124, 123, 120], [0.625, 160, 153, 118], [0.75, 188, 175, 111], [0.875, 222, 205, 93], [1, 255, 234, 70]],
  gray: [[0, 0, 0, 0], [1, 255, 255, 255]],
};
export const PALETTE_NAMES = Object.keys(PALETTES);
export function buildPalette(name = 'classic') {
  const stops = PALETTES[name] || PALETTES.classic;
  const p = new Uint8Array(256 * 4);
  for (let i = 0; i < 256; i++) {
    const t = i / 255;
    let k = 0; while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
    const a = stops[k], b = stops[k + 1], f = clamp((t - a[0]) / (b[0] - a[0]), 0, 1);
    for (let c = 0; c < 3; c++) p[i * 4 + c] = a[c + 1] + (b[c + 1] - a[c + 1]) * f;
    p[i * 4 + 3] = 255;
  }
  return p;
}

/* ---- cascada WebGL ---- */
class WfGL {
  constructor(canvas, palette, onLost) {
    this.ok = false; this.canvas = canvas; this.palette = palette; this.w = 0; this.n = 0; this.tex = null;
    const opt = { antialias: false, alpha: false, depth: false, stencil: false, preserveDrawingBuffer: false, powerPreference: 'high-performance' };
    const gl = canvas.getContext('webgl2', opt) || canvas.getContext('webgl', opt);
    if (!gl) return;
    this.gl = gl; this.v2 = typeof WebGL2RenderingContext !== 'undefined' && gl instanceof WebGL2RenderingContext;
    this.maxTex = gl.getParameter(gl.MAX_TEXTURE_SIZE);
    if (!this.build()) return;
    this.ok = true;
    canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); this.lost = true; });
    canvas.addEventListener('webglcontextrestored', () => { this.lost = false; this.tex = null; if (this.build() && this.n) this.setN(this.n); onLost && onLost(); });
  }
  /** Cambia la paleta en caliente: el color se decide en el shader, así que el historial entero se repinta. */
  setPalette(pal) {
    this.palette = pal;
    const gl = this.gl;
    if (!gl || this.lost || !this.palTex) return;
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, this.palTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, pal);
    gl.activeTexture(gl.TEXTURE0);
  }
  build() {
    const gl = this.gl;
    const vs = 'attribute vec2 p;varying vec2 v;void main(){v=vec2(p.x*.5+.5,.5-p.y*.5);gl_Position=vec4(p,0.,1.);}';
    const fs = `#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v;uniform sampler2D t,pal;
uniform float x0,x1,w,rows,lo,hi;
void main(){
  float x=mix(x0,x1,v.x);
  float r=floor(v.y*rows);
  float row=mod(w-1.-r+${WF_ROWS}.,${WF_ROWS}.);
  float q=texture2D(t,vec2(x,(row+.5)/${WF_ROWS}.)).r*255.;
  if(q<.5){gl_FragColor=vec4(0.,0.,0.,1.);return;}   // fila aún sin datos: negra, no color de «-140 dB»
  float db=${QMIN}.+q*${QSTEP.toFixed(2)};
  float l=clamp((db-lo)/(hi-lo),0.,1.);
  gl_FragColor=vec4(texture2D(pal,vec2(l*.99+.005,.5)).rgb,1.);
}`;
    const sh = (type, src) => { const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s)); return s; };
    try {
      const pr = gl.createProgram();
      gl.attachShader(pr, sh(gl.VERTEX_SHADER, vs)); gl.attachShader(pr, sh(gl.FRAGMENT_SHADER, fs));
      gl.linkProgram(pr);
      if (!gl.getProgramParameter(pr, gl.LINK_STATUS)) throw new Error('enlace');
      gl.useProgram(pr); this.pr = pr;
    } catch (e) { console.warn('WebGL descartado:', e); return false; }
    const buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(this.pr, 'p'); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    this.u = {}; for (const k of ['t', 'pal', 'x0', 'x1', 'w', 'rows', 'lo', 'hi']) this.u[k] = gl.getUniformLocation(this.pr, k);
    gl.activeTexture(gl.TEXTURE1);
    this.palTex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, this.palTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, this.palette);
    this.params();
    gl.uniform1i(this.u.pal, 1);
    gl.activeTexture(gl.TEXTURE0); gl.uniform1i(this.u.t, 0);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    return true;
  }
  params() {
    const gl = this.gl;
    for (const [k, v] of [[gl.TEXTURE_MIN_FILTER, gl.LINEAR], [gl.TEXTURE_MAG_FILTER, gl.LINEAR], [gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE]]) gl.texParameteri(gl.TEXTURE_2D, k, v);
  }
  setN(nn) {
    const gl = this.gl;
    if (nn > this.maxTex) return false;
    if (this.tex) gl.deleteTexture(this.tex);
    this.tex = gl.createTexture(); this.n = nn; this.w = 0;
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, nn, WF_ROWS, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, new Uint8Array(nn * WF_ROWS));
    this.params();
    return true;
  }
  push(q) {
    if (this.lost) return;
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.w, this.n, 1, gl.LUMINANCE, gl.UNSIGNED_BYTE, q);   // solo la fila nueva
    this.w = (this.w + 1) % WF_ROWS;
  }
  resize(bw, bh) { if (this.canvas.width !== bw || this.canvas.height !== bh) { this.canvas.width = bw; this.canvas.height = bh; } this.gl.viewport(0, 0, bw, bh); }
  draw(x0, x1, lo, hi) {
    if (this.lost || !this.tex) return;
    const gl = this.gl, u = this.u;
    gl.uniform1f(u.x0, x0); gl.uniform1f(u.x1, x1); gl.uniform1f(u.w, this.w);
    gl.uniform1f(u.rows, Math.min(WF_ROWS, this.canvas.height)); gl.uniform1f(u.lo, lo); gl.uniform1f(u.hi, hi);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }
}

/* ---- cascada 2D (alternativa) ---- */
class Wf2D {
  constructor(canvas, palette) {
    this.ok = true; this.cv = canvas; this.palette = palette; this.ctx = canvas.getContext('2d', { alpha: false });
    this.off = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(1, 1) : document.createElement('canvas'); this.p = 0; this.wd = 0;
    this.lo = -110; this.hi = -30;
  }
  setN(nn) {
    this.n = nn; this.wd = Math.min(nn, MAX_2D_W);
    this.off.width = this.wd; this.off.height = WF_ROWS;
    this.octx = this.off.getContext('2d', { alpha: false });
    this.octx.fillStyle = '#000'; this.octx.fillRect(0, 0, this.wd, WF_ROWS);
    this.img = this.octx.createImageData(this.wd, 1); this.p = 0; return true;
  }
  push(q) {
    // el más nuevo en la fila p; los anteriores en p+1, p+2… (módulo)
    this.p = (this.p - 1 + WF_ROWS) % WF_ROWS;
    const d = this.img.data, wd = this.wd, ratio = this.n / wd, span = this.hi - this.lo || 1, pal = this.palette;
    for (let x = 0; x < wd; x++) {
      let m = 0;
      if (ratio <= 1) m = q[x];
      else { const a = Math.floor(x * ratio), b = Math.min(this.n, Math.floor((x + 1) * ratio)); for (let i = a; i < b; i++) if (q[i] > m) m = q[i]; }
      const l = clamp((QMIN + m * QSTEP - this.lo) / span, 0, 1);
      const k = (l * 255) | 0, j = x * 4;
      d[j] = pal[k * 4]; d[j + 1] = pal[k * 4 + 1]; d[j + 2] = pal[k * 4 + 2]; d[j + 3] = 255;
    }
    this.octx.putImageData(this.img, 0, this.p);
  }
  resize(bw, bh) { if (this.cv.width !== bw || this.cv.height !== bh) { this.cv.width = bw; this.cv.height = bh; } }
  draw(x0, x1, lo, hi) {
    this.lo = lo; this.hi = hi;
    const c = this.ctx, bw = this.cv.width, bh = this.cv.height, rows = Math.min(WF_ROWS, bh);
    if (!this.wd) return;
    const sx = x0 * this.wd, sw = Math.max(1, (x1 - x0) * this.wd);
    c.imageSmoothingEnabled = true;
    const first = Math.min(rows, WF_ROWS - this.p);
    c.drawImage(this.off, sx, this.p, sw, first, 0, 0, bw, first);
    if (rows > first) c.drawImage(this.off, sx, 0, sw, rows - first, 0, first, bw, rows - first);
  }
}

/* ---- traza WebGL ----
 * La curva (relleno con degradado + línea + retención de picos) se tesela en tiras de triángulos: una
 * pareja de vértices por columna de píxeles. Con MSAA da bordes suaves y, sobre todo, cuesta una fracción
 * de lo que cuesta rasterizar tres trazados con degradado en Canvas 2D (medido: ~2/3 de toda la CPU de
 * la GPU de software del navegador). Las coordenadas van en px CSS; el vértice las pasa a clip space. */
class TraceGL {
  constructor(canvas, onLost) {
    this.ok = false; this.canvas = canvas; this.W = 0; this.H = 0;
    const opt = { antialias: true, alpha: false, depth: false, stencil: false, preserveDrawingBuffer: false };
    const gl = canvas.getContext('webgl2', opt) || canvas.getContext('webgl', opt);
    if (!gl) return;
    this.gl = gl;
    if (!this.build()) return;
    this.ok = true;
    canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); this.lost = true; });
    canvas.addEventListener('webglcontextrestored', () => { this.lost = false; this.build(); onLost && onLost(); });
  }
  build() {
    const gl = this.gl;
    const vs = 'attribute vec2 p;uniform vec2 res;varying float vy;void main(){vy=p.y;gl_Position=vec4(p.x/res.x*2.-1.,1.-p.y/res.y*2.,0.,1.);}';
    const fs = 'precision mediump float;varying float vy;uniform vec4 c0,c1;uniform vec2 g;void main(){vec4 c=mix(c0,c1,clamp((vy-g.x)/(g.y-g.x),0.,1.));gl_FragColor=vec4(c.rgb*c.a,c.a);}';
    const sh = (type, src) => { const x = gl.createShader(type); gl.shaderSource(x, src); gl.compileShader(x); if (!gl.getShaderParameter(x, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(x)); return x; };
    try {
      const pr = gl.createProgram();
      gl.attachShader(pr, sh(gl.VERTEX_SHADER, vs)); gl.attachShader(pr, sh(gl.FRAGMENT_SHADER, fs));
      gl.linkProgram(pr);
      if (!gl.getProgramParameter(pr, gl.LINK_STATUS)) throw new Error('enlace');
      gl.useProgram(pr); this.pr = pr;
    } catch (e) { console.warn('WebGL de la traza descartado:', e); return false; }
    this.buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, this.buf);
    const loc = gl.getAttribLocation(this.pr, 'p'); gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    this.u = {}; for (const k of ['res', 'c0', 'c1', 'g']) this.u[k] = gl.getUniformLocation(this.pr, k);
    gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    this.verts = new Float32Array(0);
    return true;
  }
  resize(pw, ph, W, H) {
    if (this.canvas.width !== pw || this.canvas.height !== ph) { this.canvas.width = pw; this.canvas.height = ph; }
    this.W = W; this.H = H; this.gl.viewport(0, 0, pw, ph);
  }
  begin() {
    const gl = this.gl;
    gl.clearColor(11 / 255, 10 / 255, 14 / 255, 1); gl.clear(gl.COLOR_BUFFER_BIT);
    gl.uniform2f(this.u.res, this.W, this.H);
  }
  /** ys: y (px CSS) por columna. mode 'fill': hasta bot, con degradado; 'line': banda de ±hw px. */
  strip(ys, W, mode, bot, hw, c0, c1) {
    const gl = this.gl;
    if (this.verts.length < W * 4) this.verts = new Float32Array(W * 4);
    const v = this.verts;
    for (let x = 0; x < W; x++) {
      const y = ys[x];
      v[x * 4] = x; v[x * 4 + 1] = mode === 'fill' ? y : y - hw;
      v[x * 4 + 2] = x; v[x * 4 + 3] = mode === 'fill' ? bot : y + hw;
    }
    gl.bufferData(gl.ARRAY_BUFFER, v.subarray(0, W * 4), gl.DYNAMIC_DRAW);
    gl.uniform4fv(this.u.c0, c0); gl.uniform4fv(this.u.c1, c1); gl.uniform2f(this.u.g, 0, bot);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, W * 2);
  }
}

/** Estado y dibujo del espectro + cascada sobre dos lienzos (normales u OffscreenCanvas). */
export class SpecRenderer {
  /** opts: {force2d}. onEvent(obj) avisa de {t:'info', gl} o {t:'fail'} (el worker lo reenvía). */
  constructor(plotCanvas, wfCanvas, opts = {}, onEvent = () => {}) {
    this.plotC = plotCanvas; this.wfC = wfCanvas; this.onEvent = onEvent;
    this.palette = buildPalette();
    this.wf = null; this.failed = false; this.tgl = null; this.px = null; this.ys = new Float32Array(0);
    if (!opts.force2d) { const t = new TraceGL(plotCanvas, () => this.request()); if (t.ok) this.tgl = t; else if (t.gl) { this.failed = true; onEvent({ t: 'fail' }); return; } }
    if (!this.tgl) this.px = plotCanvas.getContext('2d', { alpha: false });
    if (!opts.force2d) {
      const g = new WfGL(wfCanvas, this.palette, () => this.request());
      if (g.ok) this.wf = g;
      else if (g.gl) { this.failed = true; onEvent({ t: 'fail' }); return; }   // el lienzo ya tiene contexto GL: no admite 2D, que lo recree el hilo principal
    }
    if (!this.wf) this.wf = new Wf2D(wfCanvas, this.palette);
    onEvent({ t: 'info', gl: this.wf instanceof WfGL && !!this.tgl });
    this.n = 0; this.wfN = 0; this.rate = 0; this.center = 0; this.sweep = false;
    this.avg = new Float32Array(0); this.hold = new Float32Array(0); this.lut = new Uint8Array(256);
    this.v = { start: 0, span: 1, lo: -110, hi: -30, holdOn: false, W: 50, plotH: 110, wfH: 100, dpr: 1 };
    this.pending = false; this.sized = false; this.rows = 0;
    this.stats = { frames: 0, ms: 0 };
  }
  /** Paleta de la cascada por nombre (la cascada 2D solo aplica la nueva a las filas siguientes: el color va ya cocido en cada fila). */
  setPalette(name) {
    this.palette = buildPalette(name);
    if (this.wf.setPalette) this.wf.setPalette(this.palette); else this.wf.palette = this.palette;
    this.request();
  }
  /** Tamaño lógico (W × plotH y wfH, en px CSS) y densidad de píxeles. */
  setSize(W, plotH, wfH, dpr) {
    const v = this.v; v.W = W; v.plotH = plotH; v.wfH = wfH; v.dpr = dpr;
    const pw = Math.round(W * dpr), ph = Math.round(plotH * dpr);
    if (this.tgl) this.tgl.resize(pw, ph, W, plotH);
    else if (this.plotC.width !== pw || this.plotC.height !== ph) { this.plotC.width = pw; this.plotC.height = ph; }
    this.wf.resize(pw, Math.round(wfH * dpr)); this.sized = true; this.request();
  }
  setView(start, span, lo, hi, holdOn) {
    const v = this.v; v.start = start; v.span = span; v.lo = lo; v.hi = hi;
    this.wf.lo = lo; this.wf.hi = hi;                  // la cascada 2D hornea el color al subir la fila
    if (holdOn !== v.holdOn) { v.holdOn = holdOn; this.hold.fill(-200); }
    this.request();
  }
  /** Una trama binaria de tipo 1 (espectro) o 4 (barrido). */
  spectrum(buf, isSweep) {
    if (buf.byteLength < 32) return;
    const dv = new DataView(buf);
    const nn = dv.getUint32(4, true), center = dv.getFloat64(8, true), rate = dv.getFloat64(16, true);
    const dbmin = dv.getFloat32(24, true), dbstep = dv.getFloat32(28, true);
    if (!nn || buf.byteLength < 32 + nn || !(rate > 0)) return;
    const complete = !isSweep || (dv.getUint8(2) & 1) === 1;
    const modeChanged = this.sweep !== isSweep;
    const rateChanged = modeChanged || Math.abs(rate - this.rate) > 1 || Math.abs(center - this.center) > 1;
    const wn = isSweep ? Math.min(nn, WF_MAX_SWEEP) : nn;      // anchura de la cascada
    if (nn !== this.n) { this.n = nn; this.avg = new Float32Array(nn).fill(NaN); this.hold = new Float32Array(nn).fill(-200); }
    if (wn !== this.wfN) { this.wfN = wn; this.q = new Uint8Array(wn); if (!this.wf.setN(wn)) { this.failed = true; this.onEvent({ t: 'fail' }); return; } }
    if ((rate !== this.rate && this.rate) || modeChanged) this.avg.fill(NaN);
    if (rateChanged) this.hold.fill(-200);
    this.rate = rate; this.center = center; this.sweep = isSweep;
    const n = this.n, avg = this.avg, hold = this.hold, lut = this.lut, v = new Uint8Array(buf, 32, n);
    for (let i = 0; i < 256; i++) lut[i] = clamp(Math.round((dbmin + i * dbstep - QMIN) / QSTEP), 1, 255);   // 0 queda reservado a «sin datos»
    const q = this.q, ratio = n / this.wfN; q.fill(0);
    for (let i = 0; i < n; i++) {
      const db = dbmin + v[i] * dbstep;
      avg[i] = isSweep || Number.isNaN(avg[i]) ? db : avg[i] + (db - avg[i]) * 0.55;
      hold[i] = Math.max(db, hold[i] - 0.08);
      const j = ratio > 1 ? Math.floor(i / ratio) : i, l = lut[v[i]];
      if (l > q[j]) q[j] = l;                          // máximo por columna si se reduce
    }
    if (complete) this.wf.push(q);
    this.request();
  }
  /** Agrupa los cambios: un solo dibujo por imagen del navegador, pase lo que pase. */
  request() {
    if (this.pending || this.failed) return;
    this.pending = true;
    (typeof requestAnimationFrame === 'function' ? requestAnimationFrame : (f) => setTimeout(f, 16))(() => { this.pending = false; this.draw(); });
  }
  dbY(db) { const v = this.v; return (1 - (db - v.lo) / (v.hi - v.lo)) * (v.plotH - AXIS_H); }
  draw() {
    if (!this.sized || this.failed) return;
    const t0 = performance.now(), v = this.v;
    this.drawTrace();
    this.wf.draw(v.start, v.start + v.span, v.lo, v.hi);
    this.stats.frames++; this.stats.ms += performance.now() - t0;
  }
  /** y (px CSS) de cada columna: máximo por columna si hay más bins que píxeles; si no, interpolación lineal. */
  columns(arr) {
    const v = this.v, W = v.W, n = this.n, bot = v.plotH - AXIS_H;
    if (this.ys.length < W) this.ys = new Float32Array(W);
    const ys = this.ys, bpp = (n * v.span) / W, b0 = v.start * n;
    for (let x = 0; x < W; x++) {
      let val;
      if (bpp > 1.2) { const a = Math.floor(b0 + x * bpp), b = Math.min(n, Math.max(a + 1, Math.floor(b0 + (x + 1) * bpp))); val = -1e9; for (let i = a; i < b; i++) if (arr[i] > val) val = arr[i]; }
      else { const p = clamp(b0 + (x / W) * n * v.span - 0.5, 0, n - 1), i = Math.floor(p), f = p - i; val = arr[i] + ((arr[Math.min(n - 1, i + 1)] - arr[i]) * f); }
      ys[x] = clamp(this.dbY(val), 0, bot);
    }
    return ys;
  }
  drawTrace() {
    if (this.tgl) return this.drawTraceGL();
    const c = this.px, v = this.v, W = v.W, plotH = v.plotH, n = this.n;
    c.setTransform(v.dpr, 0, 0, v.dpr, 0, 0);
    c.fillStyle = '#0b0a0e'; c.fillRect(0, 0, W, plotH);
    if (!n || !this.rate) return;
    const bot = plotH - AXIS_H;
    const trace = (arr, close) => {
      const ys = this.columns(arr);
      c.beginPath();
      for (let x = 0; x < W; x++) { if (x) c.lineTo(x, ys[x]); else c.moveTo(x, ys[x]); }
      if (close) { c.lineTo(W, bot); c.lineTo(0, bot); c.closePath(); }
    };
    if (v.holdOn) { trace(this.hold, false); c.strokeStyle = 'rgba(255,211,125,.6)'; c.lineWidth = 1; c.stroke(); }
    trace(this.avg, true);
    const g = c.createLinearGradient(0, 0, 0, bot); g.addColorStop(0, 'rgba(208,188,255,.55)'); g.addColorStop(1, 'rgba(208,188,255,.04)');
    c.fillStyle = g; c.fill();
    trace(this.avg, false); c.strokeStyle = '#d0bcff'; c.lineWidth = 1.3; c.stroke();
  }
  drawTraceGL() {
    const t = this.tgl, v = this.v, W = v.W, bot = v.plotH - AXIS_H;
    if (t.lost) return;
    t.begin();
    if (!this.n || !this.rate) return;
    const HOLD = [1, 211 / 255, 125 / 255, 0.6], LINE = [208 / 255, 188 / 255, 1, 1];
    if (v.holdOn) t.strip(this.columns(this.hold), W, 'line', bot, 0.5, HOLD, HOLD);
    const ys = this.columns(this.avg);
    t.strip(ys, W, 'fill', bot, 0, [208 / 255, 188 / 255, 1, 0.55], [208 / 255, 188 / 255, 1, 0.04]);
    t.strip(ys, W, 'line', bot, 0.85, LINE, LINE);
  }
}
