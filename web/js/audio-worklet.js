// audio-worklet.js — reproductor de audio SDR en el hilo de audio.
//
// El servidor manda PCM a 48 kHz en bloques irregulares (jitter de red). Aquí:
//  1. Se acumulan en un búfer circular estéreo (L,R).
//  2. Se convierten a la tasa del AudioContext (44,1 / 48 / 96 kHz…) con
//     interpolación lineal: x(r) = a + (b−a)·frac, avanzando r en inRate/outRate.
//  3. Control de velocidad adaptativo: si el búfer se llena más del objetivo se
//     acelera la lectura hasta un 1 %; si se vacía se frena. Así el desfase entre
//     el reloj del servidor y el de la tarjeta de sonido no acaba en
//     desbordamiento ni en cortes (el 1 % solo desplaza el tono 17 cents como
//     máximo y en régimen estable la corrección es del orden de 0,01 %).
//  4. Hasta que el búfer no alcanza el objetivo no se empieza a reproducir
//     (precarga), y tras un vaciado se vuelve a precargar.
class SdrPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.inRate = 48000;
    this.size = 1 << 17;              // ~2,7 s a 48 kHz
    this.L = new Float32Array(this.size);
    this.R = new Float32Array(this.size);
    this.wr = 0;                      // muestras escritas (absoluto)
    this.rd = 0;                      // posición de lectura fraccionaria (absoluto)
    this.target = 4800;               // objetivo de ocupación (muestras de entrada)
    this.primed = false;
    this.adj = 0;
    this.under = 0; this.over = 0;
    this.tick = 0;
    // Silencio sin clics: `hold` es la última muestra emitida y decae exponencialmente (τ≈2 ms)
    // cuando falta audio; `g` sube de 0 a 1 en ~6 ms al reanudar. La suma hold + muestra·g es continua.
    this.hL = 0; this.hR = 0; this.g = 0; this.lL = 0; this.lR = 0;
    this.muted = false;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.t === 'pcm') { if (!this.muted) this.push(m.l, m.r); }
      else if (m.t === 'flush') { this.rd = this.wr; this.primed = false; this.adj = 0; }
      else if (m.t === 'mute') { this.muted = !!m.on; if (this.muted) { this.rd = this.wr; this.primed = false; } }
      else if (m.t === 'cfg') {
        if (m.inRate) this.inRate = m.inRate;
        if (m.targetMs) this.target = Math.max(480, Math.round(this.inRate * m.targetMs / 1000));
      }
    };
  }
  push(l, r) {
    const n = l.length, mask = this.size - 1;
    if (this.wr - this.rd + n > this.size * 0.9) {   // desbordamiento: tirar lo más viejo
      this.rd = this.wr - this.target;
      this.over++;
    }
    for (let i = 0; i < n; i++) { const k = (this.wr + i) & mask; this.L[k] = l[i]; this.R[k] = r[i]; }
    this.wr += n;
  }
  process(_in, outputs) {
    const out = outputs[0], oL = out[0], oR = out[1] || out[0], n = oL.length, mask = this.size - 1;
    let fill = this.wr - this.rd;
    if (!this.primed) {
      if (!this.muted && fill >= this.target) this.primed = true;
    } else if (fill < 2) { this.primed = false; this.under++; }
    const kd = Math.exp(-1 / (0.002 * sampleRate)), gs = 1 / (0.006 * sampleRate);
    let hL = this.hL, hR = this.hR, g = this.g, lL = this.lL, lR = this.lR;
    const ratio = (this.inRate / sampleRate) * (1 + this.adj);
    if (this.primed) {
      // corrección proporcional suave (±1 %)
      const err = (fill - this.target) / this.target;
      this.adj += (Math.max(-0.01, Math.min(0.01, err * 0.01)) - this.adj) * 0.05;
    }
    let rd = this.rd;
    for (let i = 0; i < n; i++) {
      if (this.primed && this.wr - rd >= 2) {
        const i0 = Math.floor(rd), f = rd - i0, a = i0 & mask, b = (i0 + 1) & mask;
        g = Math.min(1, g + gs);
        hL *= kd; hR *= kd;   // el residuo del fundido anterior sigue decayendo
        oL[i] = lL = hL + (this.L[a] + (this.L[b] - this.L[a]) * f) * g;
        oR[i] = lR = hR + (this.R[a] + (this.R[b] - this.R[a]) * f) * g;
        rd += ratio;
      } else {
        if (this.primed) { this.primed = false; this.under++; }   // vaciado a mitad de bloque
        g = 0;
        hL = lL * kd; hR = lR * kd;   // fundido desde la última muestra emitida
        oL[i] = lL = hL; oR[i] = lR = hR;
      }
    }
    if (Math.abs(lL) < 1e-6) { lL = 0; hL = 0; }
    if (Math.abs(lR) < 1e-6) { lR = 0; hR = 0; }
    this.lL = lL; this.lR = lR; this.hL = hL; this.hR = hR; this.g = g;
    if (this.primed) this.rd = rd;
    if (++this.tick % 64 === 0)
      this.port.postMessage({ fillMs: Math.max(0, (this.wr - this.rd) / this.inRate * 1000), under: this.under, over: this.over });
    return true;
  }
}
registerProcessor('sdr-player', SdrPlayer);
