// specworker.js — worker del espectro y la cascada (ver specrender.js).
// Recibe los dos OffscreenCanvas, las tramas binarias (transferidas, sin copia) y la vista.
// Solo hay un protocolo de mensajes, todos {t:…}:
//   init {plot, wf, force2d} · size {W,plotH,wfH,dpr} · view {start,span,lo,hi,hold}
//   spec {buf, sweep} · pal {name} · stats (pide {frames,ms} acumulados; responde {t:'stats'})
import { SpecRenderer } from './specrender.js';

let r = null;
self.onmessage = (e) => {
  const m = e.data;
  switch (m.t) {
    case 'init':
      try { r = new SpecRenderer(m.plot, m.wf, { force2d: m.force2d }, (ev) => self.postMessage(ev)); }
      catch (err) { self.postMessage({ t: 'fail', msg: String(err) }); }
      break;
    case 'size': if (r && !r.failed) r.setSize(m.W, m.plotH, m.wfH, m.dpr); break;
    case 'view': if (r && !r.failed) r.setView(m.start, m.span, m.lo, m.hi, m.hold); break;
    case 'spec': if (r && !r.failed) r.spectrum(m.buf, m.sweep); self.postMessage({ t: 'ack' }); break;
    case 'pal': if (r && !r.failed) r.setPalette(m.name); break;
    case 'stats': if (r) self.postMessage({ t: 'stats', ...r.stats }); break;
  }
};
