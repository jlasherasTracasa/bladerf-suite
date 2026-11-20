// Clientes WebSocket para tsan.sh: piden canal de audio y espectro y cambian de ajustes un rato.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const url = process.argv[2];
const mk = async (mode, freq) => {
  const ws = new WebSocket(url); ws.binaryType = 'arraybuffer'; let n = 0;
  ws.onmessage = () => { n++; };
  await new Promise((r) => (ws.onopen = r));
  ws.send(JSON.stringify({ cmd: 'channel', freq, mode, bw: 200000, audio: true }));
  return { ws, get n() { return n; } };
};
const cs = [await mk('wfm', 100300000), await mk('nfm', 100100000), await mk('am', 100500000)];
for (let i = 0; i < 40; i++) {
  cs.forEach((c, k) => c.ws.send(JSON.stringify({ cmd: 'channel', freq: 100000000 + (i * 7 + k) * 30000, mode: i % 2 ? 'wfm' : 'nfm', audio: true })));
  if (i % 10 === 5) cs[0].ws.send(JSON.stringify({ cmd: 'config', fft: i % 20 ? 2048 : 4096, fps: 20 }));
  await sleep(100);
}
cs[2].ws.close();
await sleep(500);
console.log('mensajes', cs.map((c) => c.n).join(','));
process.exit(0);
