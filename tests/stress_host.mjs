// Estrés del host de decodificadores por WebSocket: añade, mueve y quita instancias
// estrechas a toda prisa, como el clic masivo de la web, mientras el DDC de GPU está activo.
//
// Qué estresa: el ciclo de vida de las instancias (dec_add / dec_tune / dec_remove) cuando
// se solapan con el procesado del hilo DSP. Cada instancia tiene su propio hilo y su
// propio DDC; los bloques de muestras se reparten con contador de referencias.
//
// Por qué: fallo histórico con GPU: SIGSEGV al quitar o mover una instancia mientras
// su hilo aún usaba un bloque que ya se había reciclado (use-after-free del bloque
// compartido). Este script reproduce ese patrón de forma determinista en el número
// de rondas, así que una regresión se ve como un servidor que cierra la conexión.
//
// Uso: node tests/stress_host.mjs <url ws> [rondas=100]
// Códigos de salida: 0 = el servidor sigue vivo al final; 3 = el servidor cerró la
// conexión durante el estrés (caída); 1 = error de uso o de conexión inicial.

const [url, roundsArg] = process.argv.slice(2);
const rounds = Number(roundsArg ?? 100);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

if (!url || !Number.isInteger(rounds) || rounds < 1) {
    console.error('uso: node tests/stress_host.mjs <url ws> [rondas=100]');
    process.exit(1);
}

async function main() {
    const ws = new WebSocket(url);
    let insts = [];
    let narrow = [];
    let closed = false;

    ws.onmessage = (e) => {
        if (typeof e.data !== 'string') return;
        const m = JSON.parse(e.data);
        if (m.type === 'decoders') narrow = m.list.filter((d) => d.kind === 'narrow').map((d) => d.id);
        if (m.type === 'dec_list') insts = m.list.map((x) => x.inst);
    };
    ws.onclose = () => { closed = true; };

    // Conexión inicial: si falla, el evento onerror/onclose la resuelve como error.
    await new Promise((resolve, reject) => {
        ws.onopen = resolve;
        ws.onerror = () => reject(new Error('no se pudo conectar a ' + url));
        ws.onclose = () => reject(new Error('conexión cerrada antes de abrir'));
    });
    ws.onclose = () => { closed = true; };

    await sleep(1500);
    console.log('estrechos:', narrow.join(','));

    for (let i = 0; i < rounds; i++) {
        if (closed) {
            console.log(`CERRADO por el servidor en la ronda ${i}`);
            return 3;
        }
        for (const p of narrow) ws.send(JSON.stringify({ cmd: 'dec_add', proto: p, freq: 100300000 }));
        await sleep(i % 3 === 0 ? 0 : 50);
        for (const n of insts) {
            ws.send(JSON.stringify({ cmd: 'dec_tune', inst: n, freq: 100500000 }));
            ws.send(JSON.stringify({ cmd: 'dec_remove', inst: n }));
        }
        await sleep(20);
    }

    await sleep(2000);
    if (closed) {
        console.log('CERRADO por el servidor tras el estrés');
        return 3;
    }
    console.log('vivo tras', rounds, 'rondas; instancias:', insts.length);
    ws.close();
    return 0;
}

main()
    .then((code) => process.exit(code))
    .catch((err) => {
        console.error(err.message);
        process.exit(1);
    });
