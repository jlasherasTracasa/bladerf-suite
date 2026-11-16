// tv-worklet.js — reproductor de audio de TV en el hilo de audio, y RELOJ de la sincronía A/V.
//
// A diferencia del reproductor de radio (audio-worklet.js), aquí cada bloque de PCM llega
// con el PTS (90 kHz) de su primera muestra, y el worklet devuelve al hilo principal el PTS
// de la muestra que está sonando. El audio es el reloj: el vídeo se presenta cuando su PTS
// iguala al del audio (tv.js).
//
//  - Búfer circular estéreo; el PTS de cada trozo se guarda en una lista de segmentos
//    (muestra absoluta de inicio, PTS). La posición de lectura r (fraccionaria, en muestras
//    de entrada) se traduce a PTS = pts_seg + (r − inicio_seg)·90000/inRate.
//  - Conversión a la tasa del AudioContext con interpolación lineal y control adaptativo de
//    ±1 % (como en radio) para absorber la diferencia entre el reloj del servidor y el de la
//    tarjeta de sonido: el reloj A/V se mide sobre lo realmente consumido, así que el vídeo
//    sigue a ese ajuste.
//  - Si el PTS salta (cambio de servicio, bucle de un fichero) el segmento nuevo lo refleja:
//    el reloj salta con él. Una orden 'flush' vacía el búfer (cambio de servicio).
//  - Cada ~20 ms se envía {pts, t} con t = currentTime del AudioContext al inicio del bloque.
class TvPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.inRate = 48000;
    this.size = 1 << 17;
    this.L = new Float32Array(this.size);
    this.R = new Float32Array(this.size);
    this.wr = 0;
    this.rd = 0;
    this.target = 7200;          // 150 ms a 48 kHz
    this.primed = false;
    this.adj = 0;
    this.segs = [];              // [{s: muestra absoluta de inicio, pts}]
    this.under = 0; this.over = 0; this.tick = 0; this.muted = false;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.t === 'pcm') this.push(m);
      else if (m.t === 'flush') { this.rd = this.wr; this.segs = []; this.primed = false; }
      else if (m.t === 'cfg') { if (m.targetMs) this.target = Math.max(480, Math.round(this.inRate * m.targetMs / 1000)); }
    };
  }
  push(m) {
    if (m.rate && m.rate !== this.inRate) { this.inRate = m.rate; this.rd = this.wr; this.segs = []; this.primed = false; }
    const l = m.l, r = m.r, n = l.length, mask = this.size - 1;
    if (this.wr - this.rd + n > this.size * 0.9) {     // desbordamiento: tirar lo más viejo
      this.rd = this.wr - this.target;
      this.over++;
    }
    for (let i = 0; i < n; i++) { const k = (this.wr + i) & mask; this.L[k] = l[i]; this.R[k] = r[i]; }
    // un segmento nuevo solo si el PTS no continúa el anterior (ahorra trabajo en el caso normal)
    const last = this.segs[this.segs.length - 1];
    const expect = last ? last.pts + (this.wr - last.s) * 90000 / this.inRate : 0;
    if (!last || Math.abs(m.pts - expect) > 90) this.segs.push({ s: this.wr, pts: m.pts });
    this.wr += n;
  }
  ptsAt(pos) {
    const sg = this.segs;
    while (sg.length > 1 && sg[1].s <= pos) sg.shift();
    if (!sg.length) return null;
    return sg[0].pts + (pos - sg[0].s) * 90000 / this.inRate;
  }
  process(_in, outputs) {
    const out = outputs[0], oL = out[0], oR = out[1] || out[0], n = oL.length, mask = this.size - 1;
    const fill = this.wr - this.rd;
    if (!this.primed) {
      if (fill >= this.target) this.primed = true;
    } else if (fill < 2) { this.primed = false; this.under++; }
    if (!this.primed) { oL.fill(0); if (oR !== oL) oR.fill(0); }
    else {
      const err = (fill - this.target) / this.target;
      this.adj += (Math.max(-0.01, Math.min(0.01, err * 0.01)) - this.adj) * 0.05;
      const ratio = (this.inRate / sampleRate) * (1 + this.adj);
      let rd = this.rd;
      for (let i = 0; i < n; i++) {
        if (this.wr - rd < 2) { oL[i] = 0; oR[i] = 0; continue; }
        const i0 = Math.floor(rd), f = rd - i0, a = i0 & mask, b = (i0 + 1) & mask;
        oL[i] = this.L[a] + (this.L[b] - this.L[a]) * f;
        oR[i] = this.R[a] + (this.R[b] - this.R[a]) * f;
        rd += ratio;
      }
      this.rd = rd;
    }
    if (++this.tick % 8 === 0) {
      const pts = this.primed ? this.ptsAt(this.rd) : null;
      this.port.postMessage({ pts, t: currentTime, fillMs: Math.max(0, (this.wr - this.rd) / this.inRate * 1000),
                              under: this.under, over: this.over, got: this.wr });
    }
    return true;
  }
}
registerProcessor('tv-player', TvPlayer);
