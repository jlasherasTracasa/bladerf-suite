// Generador de la presentación-curso de BladeRF Lab (reproducible):
//   NODE_PATH=build/ppt1/node_modules node docs/presentacion/src/generar_presentacion.js
// Todos los datos del proyecto salen de README.md, AGENTS.md y docs/. La teoría general
// (SDR, IQ, FFT, modulaciones, FEC) está explicada con analogías y es la estándar.
// Las imágenes de docs/img se reducen a <= 1600 px de ancho con sharp (build/ppt1/img).
// Deck estructurado: tema + layouts (defineSlideMaster, uno por módulo para el pie) +
// placeholders + secciones. Requiere pptxgenjs y sharp (en build/ppt1/node_modules o globales).
const path = require("path");
const fs = require("fs");
const ROOT = path.resolve(__dirname, "../../..");
const req = (m) => require(require.resolve(m, { paths: [path.join(ROOT, "build/ppt1"), __dirname] }));
const pptxgen = req("pptxgenjs");
const sharp = req("sharp");
// El tema (colores del tema de PowerPoint) lo escribe un apply_theme.js externo, indicado en la
// variable de entorno PPTX_TEMA_JS. Si no está definida, el pptx se escribe sin aplicar el tema.
const TEMA_JS = process.env.PPTX_TEMA_JS;
const applyTheme = TEMA_JS ? require(path.resolve(TEMA_JS)).applyTheme : null;

const IMGDIR = path.join(ROOT, "build/ppt1/img");
const OUT = path.join(ROOT, "docs/presentacion/BladeRF_Lab.pptx");

const THEME = {
  name: "BladeRF Lab",
  headFontFace: "Cambria",
  bodyFontFace: "Calibri",
  colors: {
    dk1: "1B1424", lt1: "FFFFFF", dk2: "3B2A66", lt2: "FBF8FF",
    accent1: "6B4FA8", accent2: "0E6B5C", accent3: "B8651A",
    accent4: "B3392F", accent5: "2F5FB0", accent6: "5E5870",
    hlink: "2F5FB0", folHlink: "6B4FA8",
  },
};
const HEX = { violet: "6B4FA8", teal: "0E6B5C", amber: "B8651A", red: "B3392F", blue: "2F5FB0", grey: "5E5870", line: "DDD6EA", tint: "EFE9FA", amberTint: "FBEBD6", dark: "1B1424" };

const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE"; // 13.33 x 7.5
pres.theme = { headFontFace: THEME.headFontFace, bodyFontFace: THEME.bodyFontFace };
pres.author = "BladeRF Lab";
pres.company = "BladeRF Lab";
pres.title = "Curso BladeRF Lab: del IQ al protocolo";
pres.subject = "Curso didáctico sobre radio definida por software y el proyecto BladeRF Lab: fundamentos, arquitectura, procesado de señal, 21 protocolos, TDT, emisión, CUDA, FPGA y validación";
const C = pres.SchemeColor;
const MX = 0.6, CW = 12.13;

// ---------------------------------------------------------------- módulos y layouts
const MODS = {
  0: "Módulo 0 · Fundamentos de radio y DSP", 1: "Módulo 1 · El hardware", 2: "Módulo 2 · Arquitectura",
  3: "Módulo 3 · Interfaz web", 4: "Módulo 4 · Procesado de señal propio", 5: "Módulo 5 · Protocolos",
  6: "Módulo 6 · TDT", 7: "Módulo 7 · Emisión y seguridad", 8: "Módulo 8 · Rendimiento y CUDA",
  9: "Módulo 9 · FPGA", 10: "Módulo 10 · Validación y estado", A: "Anexos · Glosario y mapa de lectura", X: "Curso BladeRF Lab",
};
pres.defineSlideMaster({
  title: "PORTADA", background: { color: HEX.dark },
  objects: [
    { placeholder: { options: { name: "title", type: "title", x: MX, y: 4.55, w: CW, h: 1.0, fontSize: 30, bold: true, color: C.background1, margin: 0, valign: "top", align: "left" }, text: "" } },
    { placeholder: { options: { name: "body", type: "body", x: MX, y: 5.65, w: CW, h: 0.9, fontSize: 20, color: "CFC4E8", margin: 0, valign: "top", align: "left" }, text: "" } },
  ],
});
pres.defineSlideMaster({
  title: "SEPARADOR", background: { color: HEX.dark },
  objects: [
    { placeholder: { options: { name: "title", type: "title", x: 0.9, y: 1.25, w: 11.5, h: 1.2, fontSize: 40, bold: true, color: C.background1, margin: 0, valign: "middle", align: "left" }, text: "" } },
    { placeholder: { options: { name: "body", type: "body", x: 0.9, y: 3.35, w: 11.5, h: 3.3, fontSize: 20, color: "E6DEF7", margin: 0, valign: "top", align: "left" }, text: "" } },
  ],
  slideNumber: { x: 12.13, y: 7.03, w: 0.6, h: 0.3, fontSize: 10, color: "CFC4E8" },
});
for (const [k, label] of Object.entries(MODS)) {
  pres.defineSlideMaster({
    title: "CONTENIDO_" + k, background: { color: "FBF8FF" },
    objects: [
      { placeholder: { options: { name: "title", type: "title", x: MX, y: 0.4, w: CW, h: 0.85, fontSize: 32, bold: true, color: C.text1, margin: 0, valign: "middle", align: "left" }, text: "" } },
      { text: { text: label, options: { x: MX, y: 7.03, w: 8, h: 0.3, fontSize: 10, color: C.accent6, margin: 0, isTextBox: true } } },
    ],
    slideNumber: { x: 12.13, y: 7.03, w: 0.6, h: 0.3, fontSize: 10, color: C.accent6 },
  });
}

// ---------------------------------------------------------------- imágenes
const SRC = (r) => path.join(ROOT, "docs/img", r);
const D = (id, k) => `decoders/${id}-${k}-light.png`;
const IMGS = {
  banner: "banner-dark.png",
  arq: "arquitectura-light.png", marco: "marco-decodificadores-light.png",
  secBloque: "arquitectura/secuencia-bloque-light.png", hilos: "arquitectura/hilos-cerrojos-light.png",
  msgBin: "protocolo/msg-binarios-light.png", secWs: "protocolo/secuencia-ws-light.png",
  canal: "canal-audio-light.png", firdec: "dsp/firdec-respuesta-light.png", cfir: "dsp/cfir-canal-light.png",
  resamp: "dsp/resamp-respuesta-light.png", bh: "dsp/ventana-blackman-harris-light.png",
  calib: "dsp/calibracion-espectro-light.png", ber: "dsp/ber-viterbi-light.png",
  bandas: "mapa-bandas-light.png", matriz: "matriz-validacion-light.png",
  tdt: "cadena-tdt-light.png", politica: "politica-tx-light.png", ancho: "tx/ancho-ocupado-light.png",
  gpu: "gpu-light.png", cudaDdc: "cuda/cuda-ddc-light.png", cudaNr: "cuda/cuda-espectro-nr-pfb-light.png",
  rendBlade: "rendimiento/rendimiento-bladerf-light.png",
  fpgaLab: "fpga-lab-light.png", fpgaMotor: "fpga/fpga-motor-espectro-light.png",
  fpgaReg: "fpga/fpga-registros-light.png", fpgaTrama: "fpga/fpga-trama-light.png",
  valA: "validacion/validacion-grupo-a-light.png", valB: "validacion/validacion-grupo-b-light.png",
  guiaFlujo: "guia/flujo-sin-antena-light.png",
  qaAntes: "qa/antes_receptor_1440.png", qaDespues: "qa/despues_receptor_1440.png",
  fAm: D("acars", "senal"), fFm: D("pocsag", "senal"), fGmsk: D("ais", "ojo").replace("-ojo", "-ojo"), fBpsk: D("rds", "senal"),
  fOfdm: D("dab", "senal"), fOfdm2: D("dvbt", "estructura"), fCss: D("lora", "senal"),
};
const INFO = {};
// Capturas anotadas de docs/img/guia: la pantalla va arriba y la leyenda numerada debajo.
// Se recorta la pantalla (la leyenda se pasa a texto de la diapositiva para que se lea).
const CAPS = { receptor: 900, "fuera-de-banda": 900, hardware: 1100, protocolos: 900, decodificadores: 1100, "decodificar-como": 900, mapa: 1100, "mapa-demo": 900, mensajes: 1100, barrido: 900, emision: 1100, tv: 1150 };
async function prepCaps() {
  fs.mkdirSync(IMGDIR, { recursive: true });
  for (const [n, y] of Object.entries(CAPS)) {
    const out = path.join(IMGDIR, "cs_" + n + ".png");
    const r = await sharp(path.join(ROOT, "docs/img/guia/captura-" + n + ".png")).extract({ left: 0, top: 0, width: 1440, height: y }).png({ compressionLevel: 9 }).toFile(out);
    INFO["cs_" + n] = { file: out, ratio: r.width / r.height };
  }
}
async function prepImages(list) {
  fs.mkdirSync(IMGDIR, { recursive: true });
  for (const [k, rel] of Object.entries(list)) {
    if (!rel) continue;
    const out = path.join(IMGDIR, k + ".png");
    const r = await sharp(SRC(rel)).resize({ width: 1600, withoutEnlargement: true })
      .png({ compressionLevel: 9, palette: !rel.startsWith("screens/") }).toFile(out);
    INFO[k] = { file: out, ratio: r.width / r.height };
  }
}

// ---------------------------------------------------------------- ayudas de dibujo
function img(slide, key, box, alt, align = "center") {
  const { file, ratio } = INFO[key];
  let w = box.w, h = w / ratio;
  if (h > box.h) { h = box.h; w = h * ratio; }
  const x = align === "left" ? box.x : align === "right" ? box.x + box.w - w : box.x + (box.w - w) / 2;
  const y = box.y + (box.h - h) / 2;
  slide.addImage({ path: file, x, y, w, h, altText: alt || key, objectName: "img_" + key });
  return { x, y, w, h };
}
function cap(slide, text, x, y, w, h = 0.45, size = 11) {
  slide.addText(text, { x, y, w, h, fontSize: size, italic: true, color: C.accent6, margin: 0, valign: "top", isTextBox: true, objectName: "pie" });
}
function bullets(slide, items, box, size = 15, gap = 8) {
  const runs = [];
  items.forEach((it, i) => {
    const last = i === items.length - 1;
    if (typeof it === "string") runs.push({ text: it, options: { bullet: { indent: 16 }, breakLine: !last, paraSpaceAfter: gap } });
    else {
      runs.push({ text: it.b + " ", options: { bold: true, bullet: { indent: 16 }, color: C.accent1 } });
      runs.push({ text: it.t, options: { breakLine: !last, paraSpaceAfter: gap } });
    }
  });
  slide.addText(runs, { x: box.x, y: box.y, w: box.w, h: box.h, fontSize: size, color: C.text1, margin: 0, valign: "top", isTextBox: true, objectName: "puntos" });
}
function chip(slide, text, color, box, size = 14) {
  slide.addShape(pres.ShapeType.roundRect, { x: box.x, y: box.y, w: box.w, h: box.h, rectRadius: 0.12, fill: { color }, line: { color, width: 0.5 }, objectName: "etiqueta" });
  slide.addText(text, { x: box.x + 0.1, y: box.y, w: box.w - 0.2, h: box.h, fontSize: size, bold: true, color: "FFFFFF", margin: 0, valign: "middle", align: "center", isTextBox: true });
}
function card(slide, box, name = "tarjeta") {
  slide.addShape(pres.ShapeType.roundRect, { x: box.x, y: box.y, w: box.w, h: box.h, rectRadius: 0.1, fill: { color: C.background1 }, line: { color: HEX.line, width: 1 }, shadow: { type: "outer", color: "3B2A66", opacity: 0.12, blur: 6, offset: 2, angle: 90 }, objectName: name });
}
// Recuadro tintado (sin barra lateral): "Idea clave" (violeta) u "Ojo" (ámbar).
function callout(slide, kind, text, box, size = 14) {
  const idea = kind === "idea";
  slide.addShape(pres.ShapeType.roundRect, { x: box.x, y: box.y, w: box.w, h: box.h, rectRadius: 0.1, fill: { color: idea ? HEX.tint : HEX.amberTint }, line: { color: idea ? HEX.tint : HEX.amberTint, width: 0.5 }, objectName: idea ? "idea_clave" : "ojo" });
  slide.addText([{ text: idea ? "Idea clave. " : "Ojo. ", options: { bold: true, color: idea ? C.accent1 : "8A4A10" } }, { text }], { x: box.x + 0.18, y: box.y + 0.08, w: box.w - 0.36, h: box.h - 0.16, fontSize: size, color: C.text1, margin: 0, valign: "middle", isTextBox: true });
}
// Pasos numerados (esquema paso a paso).
function steps(slide, items, box, size = 15, start = 0) {
  const rowH = box.h / items.length;
  items.forEach((t, i) => {
    const y = box.y + i * rowH;
    slide.addShape(pres.ShapeType.ellipse, { x: box.x, y: y + (rowH - 0.5) / 2 - 0.05, w: 0.5, h: 0.5, fill: { color: C.accent1 }, line: { color: C.accent1 }, objectName: "paso_num" });
    slide.addText(String(i + 1 + start), { x: box.x, y: y + (rowH - 0.5) / 2 - 0.05, w: 0.5, h: 0.5, fontSize: 16, bold: true, color: C.background1, align: "center", valign: "middle", margin: 0, isTextBox: true });
    const runs = typeof t === "string" ? [{ text: t }] : [{ text: t.b + " ", options: { bold: true, color: C.accent1 } }, { text: t.t }];
    slide.addText(runs, { x: box.x + 0.7, y, w: box.w - 0.7, h: rowH - 0.05, fontSize: size, color: C.text1, margin: 0, valign: "middle", isTextBox: true });
  });
}
// Flujo horizontal de cajas con flechas.
function flow(slide, labels, box, size = 14, fill = HEX.tint) {
  const n = labels.length, gap = 0.45;
  const w = (box.w - gap * (n - 1)) / n;
  labels.forEach((l, i) => {
    const x = box.x + i * (w + gap);
    slide.addShape(pres.ShapeType.roundRect, { x, y: box.y, w, h: box.h, rectRadius: 0.1, fill: { color: fill }, line: { color: HEX.violet, width: 1 }, objectName: "caja_flujo" });
    slide.addText(l, { x: x + 0.05, y: box.y, w: w - 0.1, h: box.h, fontSize: size, color: C.text1, align: "center", valign: "middle", margin: 0, isTextBox: true });
    if (i < n - 1) slide.addShape(pres.ShapeType.line, { x: x + w + 0.04, y: box.y + box.h / 2, w: gap - 0.08, h: 0, line: { color: HEX.violet, width: 2, endArrowType: "triangle" }, objectName: "flecha" });
  });
}
function code(slide, lines, box, size = 14) {
  slide.addShape(pres.ShapeType.roundRect, { x: box.x, y: box.y, w: box.w, h: box.h, rectRadius: 0.08, fill: { color: HEX.dark }, line: { color: HEX.dark }, objectName: "bloque_codigo" });
  slide.addText(lines.map((l, i) => ({ text: l, options: { breakLine: i < lines.length - 1 } })), { x: box.x + 0.2, y: box.y + 0.1, w: box.w - 0.4, h: box.h - 0.2, fontFace: "Courier New", fontSize: size, color: "E6DEF7", margin: 0, valign: "middle", isTextBox: true });
}

let curSection = null, curMod = "X";
function section(title, mod) { pres.addSection({ title }); curSection = title; curMod = String(mod); }
function content(title, notes) {
  const s = pres.addSlide({ masterName: "CONTENIDO_" + curMod, sectionTitle: curSection });
  s.addText(title, { placeholder: "title" });
  if (notes) s.addNotes(notes);
  return s;
}
// Separador de módulo con «Qué vas a aprender».
function moduleStart(num, title, learn, notes) {
  const s = pres.addSlide({ masterName: "SEPARADOR", sectionTitle: curSection });
  s.addText("MÓDULO " + num, { x: 0.9, y: 0.7, w: 6, h: 0.45, fontSize: 18, bold: true, color: "B9A4F0", margin: 0, isTextBox: true, charSpacing: 3 });
  s.addText(title, { placeholder: "title" });
  s.addText("Qué vas a aprender", { x: 0.9, y: 2.75, w: 8, h: 0.5, fontSize: 22, bold: true, color: "B9A4F0", margin: 0, isTextBox: true });
  s.addText(learn.map((t, i) => ({ text: t, options: { bullet: { type: "number" }, breakLine: i < learn.length - 1, paraSpaceAfter: 8 } })), { placeholder: "body" });
  s.addNotes(notes || "Al empezar cada módulo se enumeran los objetivos; al terminar hay un resumen y un ejercicio práctico que puedes hacer sin antena.");
  return s;
}
// Resumen + «Pruébalo tú».
function wrapUp(summary, cmds, tryText, notes) {
  const s = content("Resumen y pruébalo tú", notes || "Resumen del módulo y un ejercicio práctico. Las órdenes salen de README.md y docs/uso/GUIA.md; todas funcionan sin antena.");
  card(s, { x: MX, y: 1.5, w: 5.7, h: 5.25 }, "resumen");
  s.addText("Resumen", { x: MX + 0.3, y: 1.65, w: 5.1, h: 0.5, fontSize: 20, bold: true, color: C.accent1, margin: 0, isTextBox: true });
  bullets(s, summary, { x: MX + 0.3, y: 2.25, w: 5.1, h: 4.35 }, 15, 8);
  card(s, { x: MX + 6.0, y: 1.5, w: 6.13, h: 5.25 }, "pruebalo");
  s.addText("Pruébalo tú", { x: MX + 6.3, y: 1.65, w: 5.5, h: 0.5, fontSize: 20, bold: true, color: C.accent1, margin: 0, isTextBox: true });
  s.addText(tryText, { x: MX + 6.3, y: 2.25, w: 5.53, h: 1.3, fontSize: 15, color: C.text1, margin: 0, valign: "top", isTextBox: true });
  code(s, cmds, { x: MX + 6.3, y: 3.65, w: 5.53, h: 2.9 }, 13);
  return s;
}
// Diapositiva didáctica genérica: imagen a un lado y texto al otro.
function lesson(o) {
  const s = content(o.title, o.notes);
  const iw = o.iw || 7.2, side = o.side || "L";
  const ix = side === "L" ? MX : MX + CW - iw;
  const tx = side === "L" ? MX + iw + 0.35 : MX, tw = CW - iw - 0.35;
  const boxes = (o.idea ? 1 : 0) + (o.ojo ? 1 : 0);
  const bh = o.boxH || 1.45;
  const top = 1.5, bottom = 6.85, textH = bottom - top - boxes * (bh + 0.2);
  if (o.imgs) {
    const n = o.imgs.length, ih = (o.ih || 4.7);
    o.imgs.forEach((im, i) => {
      const b = { x: ix, y: top + i * ((ih) / n + 0.15), w: iw, h: ih / n };
      img(s, im.k, b, im.alt, side === "L" ? "left" : "right");
    });
    if (o.cap) cap(s, o.cap, ix, top + (o.ih || 4.7) + 0.1, iw, 0.7);
  }
  if (o.steps) steps(s, o.steps, { x: tx, y: top, w: tw, h: textH }, o.size || 15);
  else if (o.bullets) bullets(s, o.bullets, { x: tx, y: top, w: tw, h: textH }, o.size || 15, o.gap || 8);
  let y = top + textH + 0.2;
  if (o.idea) { callout(s, "idea", o.idea, { x: tx, y, w: tw, h: bh }, o.cs || 14); y += bh + 0.2; }
  if (o.ojo) callout(s, "ojo", o.ojo, { x: tx, y, w: tw, h: bh }, o.cs || 14);
  return s;
}
function table(s, rows, box, colW, size = 14, rowH = 0.5) {
  const hdr = (t) => ({ text: t, options: { bold: true, color: "FFFFFF", fill: { color: HEX.violet } } });
  const data = rows.map((r, i) => (i === 0 ? r.map(hdr) : r));
  s.addTable(data, { x: box.x, y: box.y, w: box.w, colW, fontSize: size, color: C.text1, border: { type: "solid", pt: 1, color: HEX.line }, fill: { color: C.background1 }, valign: "middle", rowH, margin: [0.03, 0.1, 0.03, 0.1], objectName: "tabla" });
}
const chartBase = (o) => ({ showTitle: !!o.title, title: o.title, titleFontSize: 14, titleColor: C.text1, titleFontFace: "+mn-lt",
  showValue: true, dataLabelPosition: "outEnd", dataLabelFontSize: 12, dataLabelColor: C.text1, dataLabelFontFace: "+mn-lt", dataLabelFormatCode: o.fmt || "0.0",
  catAxisLabelColor: C.text1, catAxisLabelFontSize: 12, catAxisLabelFontFace: "+mn-lt", valAxisLabelColor: C.accent6, valAxisLabelFontSize: 11, valAxisLabelFontFace: "+mn-lt",
  valGridLine: { color: HEX.line, size: 0.5 }, catGridLine: { style: "none" }, showLegend: !!o.legend, legendPos: "b", legendFontSize: 12, legendColor: C.text1, legendFontFace: "+mn-lt", valAxisMinVal: 0 });

const CAT = { aire: HEX.teal, real: HEX.blue, sint: HEX.grey, pend: HEX.amber };
const PROTOS = [
  { id: "adsb", t: "ADS-B / Mode S: aviones a 1090 MHz", sig: "adsb-trama", est: "adsb-estructura", cadena: "decoders/adsb-cadena-light.png", sigCap: "Módulo de una trama DF17 a 2 Msps. Señal sintética (--gen adsb).", estCap: "Campos de la trama DF17 y del ME de posición.",
    f: [{ b: "Wide a 2 Msps exactos.", t: "PPM: preámbulo de 8 µs y 112 bits; decide comparando dos muestras por bit." }, { b: "CRC-24 (0xFFF409).", t: "Corrige errores de 1 y 2 bits; posición por CPR, velocidad y altitud." }, { b: "Tabla de aeronaves.", t: "Estado con indicativo, altitud, rumbo y posición; caducan a los 60 s." }],
    meta: "Wide · 2 000 000 S/s · 1090 MHz", cat: "sint", chip: "Sintético: dump1090-fa; sin tráfico real",
    n: "ADS-B es el decodificador original del proyecto y ahora vive en el host como los demás. Trabaja con el módulo de la señal, por eso no le afecta el desvío de frecuencia. Las mejoras incluyen confianza por bit, corrección de 1 y 2 bits y entramado alternativo por CRC. Validación: dump1090-fa decodifica las mismas cuatro tramas DF17 de la señal generada; con la antena actual no llega tráfico real a 1090 MHz, así que sigue sin validarse con aire. Con la posición del receptor conocida, un solo mensaje de posición basta para situar el avión. Fuente: docs/protocolos/adsb.md." },
  { id: "rds", t: "RDS: datos dentro de la FM comercial", sig: "rds-senal", est: "rds-estructura", sigCap: "Espectro MPX con piloto y subportadora de 57 kHz, y constelación BPSK. Señal sintética.", estCap: "Estructura de los grupos 0A y 2A.",
    f: [{ b: "Subportadora de 57 kHz", t: "(3 × piloto), BPSK bifase a 1187,5 bit/s; bloques de 26 bits con código cíclico (26,16)." }, { b: "Muestra PI, PS, RadioText, PTY, AF y hora.", t: "Grupos 0A/0B, 2A/2B, 4A y 10A." }, { b: "Sensibilidad.", t: "Completo con CNR ≥ 18–20 dB; a 10 dB no decodifica (límite físico del discriminador)." }],
    meta: "Narrow · 250 kS/s · FM comercial", cat: "aire", chip: "En el aire: Radio Pamplona (PI E239, PS «SER»)",
    n: "El discriminador FM convierte cualquier desvío de frecuencia en una continua del MPX, así que no hace falta estimar el CFO. Después se mezcla a -57 kHz, se diezma, se engancha con un lazo de Costas y se sincroniza el reloj con Gardner. La sincronía de bloque usa las palabras de desplazamiento A, B, C, C' y D. Validado con redsea sobre la salida de --gen rds y con RDS real captado con el bladeRF en Radio Pamplona (100,4 MHz). Fuente: docs/protocolos/rds.md." },
  { id: "dab", t: "DAB / DAB+: conjunto y servicios", sig: "dab-senal", est: "dab-estructura", sigCap: "Símbolo nulo, espectro y constelación D-QPSK de una grabación real.", estCap: "Trama de transmisión del modo I y FIB.",
    f: [{ b: "Wide a 2 048 000 S/s exactos.", t: "Modo I: trama de 96 ms, 76 símbolos, 1536 portadoras separadas 1 kHz." }, { b: "Lee el FIC:", t: "identificador del conjunto, servicios, subcanales, tasa y protección. No decodifica el audio (MSC)." }, { b: "Real:", t: "ensemble 5001 «Towercom», servicio «Radio Slovensko»; 36/36 FIB válidos." }],
    meta: "Wide · 2 048 000 S/s · banda III", cat: "real", chip: "Datos reales (grabación de sigidwiki)",
    n: "Sincroniza con el símbolo nulo y el símbolo de referencia de fase, demodula D-QPSK, desentrelaza en frecuencia y tiempo, deshace el punzonado y aplica Viterbi, dispersión y CRC-16 del FIB (ETSI EN 300 401). Las tablas de punzonado y entrelazado eran el principal riesgo y la grabación real no obligó a cambiarlas. Sin validar: tablas UEP/EEP de varios subcanales y otros modos. Fuente: docs/protocolos/dab.md." },
  { id: "ais", t: "AIS: barcos en VHF marina", sig: "ais-ojo", est: "ais-estructura", sigCap: "Izquierda: audio FM real (sigidwiki, Long Beach) remodulado. Derecha: ojo GMSK de una señal sintética.", estCap: "Ráfaga AIS y mensaje 1/2/3.",
    f: [{ b: "GMSK BT 0,4 a 9600 bd", t: "en 161,975 y 162,025 MHz; 5 muestras por símbolo a 48 kS/s." }, { b: "NRZI + HDLC + CRC-16/X-25.", t: "Cinco fases de reloj en paralelo; tipos 1-3, 4, 5, 18, 19, 21, 24." }, { b: "Defecto real corregido:", t: "orden de bits de los campos M.1371; ya salen MMSI y posiciones coherentes." }],
    meta: "Narrow · 48 kS/s · 161,975 / 162,025 MHz", cat: "real", chip: "Datos reales (AIS Sound y AIS IQ)",
    n: "Con grabaciones públicas de sigidwiki, el decodificador pasó de producir mensajes tipo 8 con MMSI basura a tipo 1 con MMSI 367003560 y posiciones coherentes: el error era el orden de bits dentro de los octetos HDLC. Mantiene una tabla de barcos. Con la convención de posición corregida gana 2,3 dB de sensibilidad (SNR al 50 %: ≈ 6,6 → 4,3 dB). Pendiente: tipos 5/18/19/21/24 contrastados con tráfico real y la mejora PLVA. Fuente: docs/protocolos/ais.md y VALIDACION.md." },
  { id: "dsc", t: "DSC: llamada selectiva digital (canal 70)", sig: "dsc-senal", est: "dsc-estructura", sigCap: "Cascada y frecuencia instantánea FSK de 1200 bd. Señal sintética.", estCap: "Secuencia de símbolos de una llamada.",
    f: [{ b: "FSK de 1200 bd", t: "en 156,525 MHz: 2100 Hz = 0, 1300 Hz = 1; socorro, rutina y MMSI." }, { b: "Validada con la norma ITU-R M.493-15:", t: "bits de comprobación MSB primero, especificador de formato duplicado, EOS triple." }, { b: "Sin grabación real", t: "pública encontrada: sin validar con aire." }],
    meta: "Narrow · 48 kS/s · 156,525 MHz", cat: "sint", chip: "Sintético + tablas de la norma; sin aire",
    n: "No existe una grabación pública de DSC VHF que se haya encontrado y ningún decodificador de referencia instalado lo decodifica. Se leyó la recomendación ITU-R M.493-15 y se contrastaron las constantes con la Tabla A1-1: el modulador enviaba los bits de comprobación al revés y faltaban el especificador de formato duplicado y el EOS triple. Fuente: docs/protocolos/dsc.md." },
  { id: "aprs", t: "APRS / AX.25: radioaficionados a 1200 bd", sig: "aprs-senal", est: "aprs-estructura", sigCap: "Cascada y audio con los tonos Bell 202. Señal sintética.", estCap: "Trama AX.25 UI e información de posición.",
    f: [{ b: "AFSK Bell 202:", t: "1200 Hz = 1 y 2200 Hz = 0, 1200 Bd sobre FM de banda estrecha (144,800 MHz)." }, { b: "NRZI + HDLC + FCS CRC-16/X-25;", t: "trama UI con destino, origen y hasta 8 repetidores." }, { b: "Real:", t: "tramas AX.25 idénticas a multimon-ng y Dire Wolf. La posición no se ha contrastado." }],
    meta: "Narrow · 48 kS/s · 144,800 MHz", cat: "real", chip: "Datos reales (AFSK1200); posición sin contrastar",
    n: "Referencias: APRS Protocol Reference 1.0.1, AX.25 v2.2 y Bell 202. Con la grabación AFSK1200 de sigidwiki ninguna constante estaba mal: HDLC, NRZI, CRC, tonos y polaridad coincidieron a la primera. Las tramas de esa grabación no son APRS de posición, así que el análisis de posición y clima sigue sin contrastarse con tráfico real. Fuente: docs/protocolos/aprs.md." },
  { id: "pocsag", t: "POCSAG: buscapersonas", sig: "pocsag-senal", est: "pocsag-estructura", sigCap: "Cascada y ojo FSK a 1200 bd (audio real remodulado).", estCap: "Transmisión, lote y palabra POCSAG.",
    f: [{ b: "FSK de 2 niveles a 512, 1200 o 2400 bit/s,", t: "canal de 12,5 kHz." }, { b: "Palabra de 32 bits:", t: "21 de datos + BCH(31,21) con corrección + paridad; sincronía 0x7CD215D8." }, { b: "Real:", t: "512, 1200 y 2400 bps coinciden con multimon-ng; numérico y alfanumérico." }],
    meta: "Narrow · 48 kS/s · p. ej. 466,075 MHz", cat: "real", chip: "Datos reales (3/3 = multimon-ng; sin erróneos)",
    n: "Recomendación ITU-R M.584 (Radio-paging code No. 1). Con las tres muestras públicas del repositorio de pruebas de multimon-ng el resultado fue idéntico (RIC 273040 y 1022869) y no hubo que cambiar ninguna constante. Aviso legal: interceptar o divulgar mensajes que no son públicos puede no ser legal. Fuente: docs/protocolos/pocsag.md." },
  { id: "flex", t: "FLEX: buscapersonas de Motorola", sig: "flex-senal", est: "flex-estructura", sigCap: "Cascada y histograma FSK de 2 niveles (audio real remodulado).", estCap: "Trama FLEX.",
    f: [{ b: "1600/3200/6400 bps, 2 o 4 niveles,", t: "BCH(31,21); la especificación de Motorola no es pública." }, { b: "1600/2 y 6400/4 contrastados:", t: "20 y 24 mensajes idénticos a multimon-ng." }, { b: "3200 pendiente:", t: "3200/4 da mensajes plausibles sin referencia; 3200/2 sin grabación." }],
    meta: "Narrow · 48 kS/s · p. ej. 466,075 MHz", cat: "real", chip: "Real en 1600/2 y 6400/4; 3200 pendiente",
    n: "La grabación real corrigió tres cosas: la palabra A del Sync 1 es el código más 0xA6C6 (no el complemento), la primera copia invertida de A producía una falsa sincronía, y la primera palabra de datos lleva 7 bits de cabecera. El FIW se confirmó (suma de nibbles, ciclo, trama). Fuente: docs/protocolos/flex.md y VALIDACION.md." },
  { id: "dmr", t: "DMR: solo metadatos, nunca voz", sig: "dmr-senal", est: "dmr-estructura", sigCap: "Cascada de la grabación real y niveles 4FSK de una señal generada.", estCap: "Ráfaga TDMA y metadatos.",
    f: [{ b: "4FSK a 4800 símbolos/s", t: "(h = 0,27); sincronías, color code, ranura y LC con origen y destino." }, { b: "ETSI TS 102 361-1:", t: "matrices, polinomios y entrelazado tomados de la norma." }, { b: "Sin voz:", t: "las tramas de voz solo sirven para saber que hay llamada. Sin validar con aire." }],
    meta: "Narrow · 48 kS/s · 70 cm", cat: "pend", chip: "Pendiente: sin validar con aire",
    n: "La política de ética del proyecto prohíbe decodificar voz y extraer identidades de abonados. Con la grabación de sigidwiki solo apareció una sincronía y ninguna ráfaga; no hay evidencia de que esa señal sea DMR estándar y no hay decodificador de referencia instalado. Fuente: docs/protocolos/dmr.md." },
  { id: "acars", t: "ACARS: mensajes avión-tierra en VHF", sig: "acars-senal", est: "acars-estructura", sigCap: "Cascada y envolvente AM de la grabación real.", estCap: "Bloque ACARS.",
    f: [{ b: "AM de baja profundidad con MSK a 2400 bit/s", t: "(1200/2400 Hz) en 131,525; 131,725 y 131,825 MHz." }, { b: "Codificación diferencial:", t: "hallazgo con la grabación real; paridad impar y BCS CRC-16/KERMIT." }, { b: "Real:", t: "11 de 11 bloques, igual que acarsdec." }],
    meta: "Narrow · 48 kS/s · 131,525 / 131,725 / 131,825 MHz", cat: "real", chip: "Datos reales (11/11 = acarsdec)",
    n: "Antes de la grabación real no se decodificaba ningún bloque: se leía 1200 Hz como 1 y 2400 Hz como 0, pero la codificación es diferencial (2400 Hz significa bit igual al anterior). Pendiente: mensajes multibloque (ETB) e interpretación de etiquetas. Fuente: docs/protocolos/acars.md." },
  { id: "vdl2", t: "VDL Modo 2: datos digitales de aviación", sig: "vdl2-senal", est: "vdl2-estructura", sigCap: "Cascada real y constelación D8PSK diferencial, sintética y real.", estCap: "Ráfaga D8PSK y trama AVLC.",
    f: [{ b: "D8PSK a 10 500 Bd (31,5 kbit/s),", t: "RRC α = 0,6, canales 136,725–136,975 MHz." }, { b: "Cadena:", t: "ráfaga, Reed-Solomon, AVLC y ACARS sobre AVLC." }, { b: "Real:", t: "38 de 41 tramas de dumpvdl2 (93 %) con la grabación de sigidwiki." }],
    meta: "Narrow · 105 kS/s · 136,725–136,975 MHz", cat: "real", chip: "Datos reales (38/41 tramas = dumpvdl2)",
    n: "Con la grabación real de sigidwiki se confirmaron la velocidad de símbolo, el filtro y que el WAV trae I/Q intercambiados. Hubo que corregir el aleatorizador (sobre cabecera y datos), la matriz de la cabecera, el orden de octetos RS, el entrelazado y la dirección AVLC. Quedan 3 de 41 tramas sin sacar. Fuente: docs/protocolos/vdl2.md." },
  { id: "ism", t: "ISM: sensores y mandos OOK/FSK", sig: "ism-senal", est: "ism-estructura", sigCap: "Envolvente OOK real y tren de pulsos con el umbral adaptativo.", estCap: "Esquema de un sensor meteorológico OOK.",
    f: [{ b: "Detecta ráfagas OOK y FSK,", t: "extrae el tren de pulsos y prueba protocolos conocidos con suma o CRC." }, { b: "Si ninguno encaja,", t: "analizador automático de modulación tipo Universal Radio Hacker (PWM/PPM/Manchester)." }, { b: "Real:", t: "Acurite, Nexus, F007TH, WH1080, TX141 y Oregon: iguales a rtl_433." }],
    meta: "Narrow · 250 kS/s · 433,92 / 868 MHz", cat: "real", chip: "Datos reales (= rtl_433)",
    n: "Se contrastó con el repositorio público rtl_433_tests y con rtl_433 usado como caja negra, que además decodifica los 8 modelos generados con --gen ism. La validación corrigió tiempos, polaridad y longitudes en WH1080, TX141 y Oregon v2.1. Fuente: docs/protocolos/ism.md." },
  { id: "rs41", t: "RS41: radiosondas Vaisala", sig: "rs41-senal", est: "rs41-estructura", sigCap: "Cascada y ojo GFSK a 4800 bd de la grabación real.", estCap: "Trama de 312 octetos.",
    f: [{ b: "GFSK 4800 Bd, ±2,4 kHz,", t: "una trama por segundo en 403–406 MHz; desaleatorizado y CRC-16 por bloque." }, { b: "Reed-Solomon", t: "sobre GF(0x11D), fcr 0, 24 raíces: confirmado con datos reales." }, { b: "Real:", t: "38/38 tramas por grabación; posición, altura y tiempo GPS coherentes." }],
    meta: "Narrow · 48 kS/s · 403–406 MHz", cat: "real", chip: "Datos reales (38/38 tramas)",
    n: "Las grabaciones de rs1729/RS son audio FM demodulado, no IQ, y se remodulan para probar. El hallazgo clave: la paridad RS va en bloque (no entrelazada). Sondas K1930308 y L1830070, posiciones en torno a 45,67°N y 46,02°N. Fuente: docs/protocolos/rs41.md." },
  { id: "lora", t: "LoRa y LoRaWAN: capa física CSS", sig: "lora-senal", est: "lora-estructura", sigCap: "Cascada de chirps y picos tras el dechirp (referencia de loratx).", estCap: "Paquete LoRa y cabecera explícita.",
    f: [{ b: "CSS, SF7–SF12, BW 125/250 kHz,", t: "cabecera explícita, CR 4/5–4/8, LDRO y CRC de carga." }, { b: "LoRaWAN:", t: "analiza MHDR y DevAddr; no descifra ni verifica el MIC." }, { b: "Contrastado", t: "con loratx/lorarx en ambos sentidos (CRC sin blanquear)." }],
    meta: "Narrow · 500 kS/s · 868,1 / 868,3 / 868,5 MHz", cat: "sint", chip: "Sintético: contraste con loratx / lorarx",
    n: "No hay grabaciones públicas con bytes verificados, así que se usó un transmisor y receptor independientes ya instalados. El fallo hallado: el CRC de la carga va sin blanquear. Antes solo se decodificaba SF12 y SF10/11; ahora SF7 a SF12, CR 1-4 y BW 125/250. Fuente: docs/protocolos/lora.md." },
  { id: "meshtastic", t: "Meshtastic: malla LoRa", sig: "meshtastic-senal", est: "meshtastic-estructura", sigCap: "Cascada y picos tras el dechirp (SF11, BW 250 kHz). Señal sintética.", estCap: "Cabecera de 16 bytes y nonce de AES-CTR.",
    f: [{ b: "Capa LoRa", t: "con presets (LongFast SF11/BW 250 por defecto) y cabecera de 16 bytes con hash de canal." }, { b: "AES-CTR con la clave pública por defecto:", t: "texto, posición y nodeinfo." }, { b: "Sin validar con aire:", t: "descifrado y protobuf (cabecera y hash sí confirmados)." }],
    meta: "Narrow · 500 kS/s · 869,525 MHz", cat: "sint", chip: "Sintético: cabecera confirmada; sin aire",
    n: "La clave por defecto es pública por diseño (excepción recogida en AGENTS.md §0.12). lorarx decodifica nuestras tramas y muestra destino, origen, hash 08 y salto, que coinciden. No hay captura real con bytes verificados y lorarx no descifra. Fuente: docs/protocolos/meshtastic.md." },
  { id: "lte", t: "LTE: búsqueda de celdas", sig: "lte-pss", est: "lte-estructura", sigCap: "Espectro y picos de correlación con la PSS cada 5 ms (grabación real).", estCap: "Posición de PSS, SSS y PBCH.",
    f: [{ b: "PSS/SSS → PCI,", t: "FDD/TDD, prefijo cíclico, CFO, RSRP y SINR aproximadas." }, { b: "MIB del PBCH:", t: "ancho de banda, PHICH y SFN. Solo difusión." }, { b: "Real:", t: "PCI 65 y PCI 44 con MIB; 4 celdas con MIB en el aire en Pamplona." }],
    meta: "Narrow · 1 920 000 S/s · banda 20 / 3", cat: "aire", chip: "En el aire: 4 celdas con MIB (Pamplona)",
    n: "El CRC del PBCH solo pasa si la codificación convolucional tail-biting, la adaptación de velocidad, el aleatorizado y la máscara de puertos son los de TS 36.212/36.211, así que las constantes quedaron confirmadas sin cambios. Sin validar: TDD, prefijo cíclico extendido y SIB. Fuente: docs/protocolos/lte.md." },
  { id: "nr", t: "5G NR: búsqueda de bloques SSB", sig: "nr-pss", est: "nr-estructura", sigCap: "Espectro de la grabación real y correlación con la PSS en una señal generada.", estCap: "Bloque SSB e identidad de celda.",
    f: [{ b: "Wide ≥ 15,36 Msps", t: "(múltiplo de 7,68); recorre la rejilla GSCN." }, { b: "PSS/SSS → PCI (0–1007),", t: "SCS 15/30 kHz, CFO y potencia. Sin PBCH polar." }, { b: "Correlación opcional en GPU:", t: "de 15–24 ms a 1,1 ms de CPU por ventana." }],
    meta: "Wide · ≥ 15,36 Msps · n78 (3500 MHz)", cat: "pend", chip: "Pendiente: sin celda en grabaciones reales",
    n: "Se probaron dos grabaciones de sigidwiki sin conocer la frecuencia central y no apareció ninguna celda, así que PSS/SSS de NR siguen sin contrastarse con aire real. Fuente: docs/protocolos/nr.md y docs/arquitectura/CUDA.md §6." },
  { id: "gsm", t: "GSM: sincronía y BSIC", sig: "gsm-fcch", est: "gsm-estructura", sigCap: "Cascada y detector de FCCH sobre la grabación real.", estCap: "Multitrama de 51 y ráfaga normal.",
    f: [{ b: "FCCH y SCH → BSIC y número de trama;", t: "SI3 del BCCH daría MCC, MNC, LAC y Cell ID." }, { b: "Real:", t: "211/211 SCH correctos, BSIC 23 (NCC 2, BCC 7) y FN que avanza bien." }, { b: "BCCH/SI3 reales:", t: "no decodifican todavía. Sin canales de aviso ni tráfico." }],
    meta: "Narrow · 1 083 333 S/s · 942,5 MHz", cat: "pend", chip: "SCH real sí; BCCH/SI3 pendiente",
    n: "Hallazgo con grabación real: el orden de los campos del SCH estaba mal (el BSIC bien, el número de trama saltaba ±1000). Se corrigió en gsm_phy.c. Por ética solo se tocan FCCH, SCH y las cuatro ráfagas del BCCH. Fuente: docs/protocolos/gsm.md." },
  { id: "wifi", t: "WiFi: balizas 802.11", sig: "wifi-senal", est: "wifi-estructura", sigCap: "Cascada y ráfagas de balizas 802.11b en la grabación real.", estCap: "PPDU y trama Beacon.",
    f: [{ b: "Wide a 20 Msps exactos:", t: "descubre redes por Beacon y Probe Response (SSID, BSSID, canal)." }, { b: "DSSS 1 Mb/s validado:", t: "8 BSSID reales; OFDM 6 Mb/s sin validar." }, { b: "Solo tramas de gestión de difusión;", t: "datos y control se descartan." }],
    meta: "Wide · 20 000 000 S/s · 2437 MHz", cat: "real", chip: "Datos reales (DSSS); OFDM pendiente",
    n: "Se decodifican primero solo ocho bytes del PSDU para leer el control de trama. Confirmados: preámbulo/SFD, CRC-16 del PLCP, DBPSK/Barker, aleatorizador y FCS. Fuente: docs/protocolos/wifi.md." },
  { id: "dvbt", t: "DVB-T: parámetros TPS de la TDT", sig: "dvbt-senal", est: "dvbt-estructura", sigCap: "Espectro y prefijo cíclico reales del C32 de Pamplona, y por qué 64-QAM a −9 dB no demodula (simulada).", estCap: "Símbolo OFDM 8k y bits TPS.",
    f: [{ b: "OFDM 2k/8k a 64/7 MHz:", t: "modulación, tasa, guarda y cell ID desde la señalización TPS." }, { b: "Receptor completo hasta TS:", t: "8k 1/4 64-QAM 2/3, 19,9 Mb/s con 0,64–0,8 núcleos." }, { b: "Real:", t: "referencia gr-dtv y TPS en el aire; la SNR de ≈ −9 dB impide sacar TS." }],
    meta: "Narrow · 9 142 857 S/s (64/7 MHz)", cat: "real", chip: "gr-dtv + TPS en el aire; sin TS real",
    n: "El receptor reproduce un TS idéntico al de gr-dtv y engancha el C32 de Pamplona, pero con la antena actual la relación señal/ruido es de unos -9 dB y 64-QAM no puede demodularse. Viterbi con AVX2 a unos 100 Mb/s. Fuente: docs/protocolos/dvbt.md y docs/protocolos/tdt.md." },
  { id: "tdt", t: "TDT: vídeo y audio de la televisión", sig: "dvbt-senal", est: "tdt-estructura", cadena: "decoders/tdt-cadena-light.png", sigCap: "Señal real del C32 de Pamplona (misma figura que DVB-T).", estCap: "Paquete TS y pila de tablas.",
    f: [{ b: "DVB-T → TS → demux, PSI/SI, PES y PTS;", t: "audio MP2 propio y vídeo H.264 por WebCodecs." }, { b: "Sincronía A/V:", t: "el audio es el reloj; el vídeo se presenta cuando su PTS ≤ reloj." }, { b: "Limitaciones:", t: "solo H.264 y MP2; AAC/AC-3 sin audio; sin descifrado." }],
    meta: "Narrow · 64/7 MHz · canales 21–48", cat: "sint", chip: "Sintético y IQ de gr-dtv; TS real pendiente",
    n: "La parte de TV2 se verificó con un TS de prueba en bucle (--tv-ts): el decodificador mp2 tiene error rms < 0,1 LSB frente a ffmpeg usado solo como referencia. Con la antena actual no hay TS en el aire. Fuente: docs/protocolos/tdt.md." },
];

const PTXT = {
  adsb: ["Cada avión emite cada medio segundo su identidad, posición y velocidad en 1090 MHz.", "Es lo que usan las webs de seguimiento de vuelos: ver el tráfico aéreo sobre tu cabeza.", "Mide pulsos a 2 Msps, busca el preámbulo de 8 µs, lee 112 bits y comprueba un CRC de 24 bits.", "Tabla de aviones con indicativo, altitud, rumbo y posición en el mapa."],
  rds: ["Datos digitales escondidos en una subportadora de 57 kHz de cada emisora de FM.", "El coche muestra el nombre de la emisora y el texto de la canción gracias a él.", "Demodula la FM, baja 57 kHz, recupera la fase con un lazo de Costas y valida bloques de 26 bits.", "Nombre (PS), texto (RT), tipo de programa, frecuencias alternativas y hora. Real: «SER» en 100,4 MHz."],
  dab: ["Radio digital: muchos servicios (una multiplex) dentro de un solo canal OFDM.", "Más emisoras y mejor calidad que la FM en el mismo espacio de banda.", "Encuentra el símbolo nulo, demodula 1536 portadoras D-QPSK y corrige con Viterbi para leer el FIC.", "Nombre del conjunto y de cada servicio, con tasa y protección (sin audio)."],
  ais: ["Los barcos emiten su identificador (MMSI), posición y rumbo en dos canales VHF.", "Evitar abordajes y seguir barcos en un mapa.", "Demodula GMSK a 9600 bd, deshace NRZI y HDLC y valida un CRC-16 en cinco fases de reloj.", "Tabla de barcos con MMSI, posición y rumbo, y su tipo de mensaje."],
  dsc: ["Llamada selectiva digital: un botón de socorro en la VHF marina (canal 70).", "Avisar de un socorro o llamar a un barco concreto sin hablar.", "Recibe FSK de 1200 bd, quita el especificador duplicado y comprueba los bits de cada símbolo.", "Formato de la llamada (socorro, rutina) y MMSI de origen y destino."],
  aprs: ["Mensajes cortos de radioaficionados a 1200 baudios sobre FM (144,800 MHz en Europa).", "Posiciones, estado, telemetría y mensajes de estaciones de aficionados.", "Distingue dos tonos (1200 y 2200 Hz), deshace NRZI y HDLC y valida el CRC-16 de AX.25.", "Indicativos de origen y destino y el texto de la trama."],
  pocsag: ["Sistema clásico de buscapersonas: ráfagas FSK de 512 a 2400 bit/s.", "Avisar a hospitales y servicios de emergencia.", "Detecta la palabra de sincronía, lee palabras de 32 bits y corrige errores con BCH(31,21).", "Dirección (RIC) y mensaje numérico o de texto."],
  flex: ["Buscapersonas de Motorola, más rápido que POCSAG (1600 a 6400 bit/s).", "Avisos a más usuarios con mensajes de texto más largos.", "Busca la sincronía, lee la palabra de información de la trama y corrige con BCH.", "Direcciones y mensajes alfanuméricos. Probado a 1600 bit/s."],
  dmr: ["Radio digital con dos ranuras de tiempo (TDMA) por canal.", "Comunicaciones de empresas, servicios y radioaficionados.", "Demodula 4FSK, busca sincronías, corrige con códigos Hamming y lee el control de enlace.", "Solo metadatos: color code, ranura, origen y destino. Nunca voz."],
  acars: ["Mensajes de texto entre aviones y tierra por VHF.", "Informes de vuelo, meteorología y avisos de mantenimiento.", "Demodula AM, distingue 1200/2400 Hz con codificación diferencial, comprueba paridad y CRC-16.", "Matrícula, vuelo, etiqueta y texto del bloque."],
  vdl2: ["Sucesor digital de ACARS: datos a 31,5 kbit/s con D8PSK.", "Enlace de datos aire-tierra de la aviación moderna.", "Sincroniza 10 500 baudios, demodula 8 fases, desaleatoriza y aplica Reed-Solomon.", "Tramas AVLC con direcciones y ACARS encapsulado (sin validar aún con aire)."],
  ism: ["Sensores y mandos de 433 y 868 MHz: estaciones meteo, TPMS, timbres.", "Leer la temperatura y la humedad de los sensores de casa.", "Mide anchuras de pulso, prueba protocolos conocidos con suma de control y analiza el resto.", "Modelo, identificador, temperatura, humedad y batería."],
  rs41: ["Radiosonda meteorológica que sube en globo y emite GFSK de 4800 bd.", "Medir presión, temperatura y humedad en altura; seguir y recuperar la sonda.", "Demodula GFSK, desaleatoriza la trama, corrige con Reed-Solomon y valida el CRC de cada bloque.", "Posición, altura, velocidad y datos de PTU; una trama por segundo."],
  lora: ["Modulación por chirps de largo alcance y bajo consumo.", "Sensores IoT y redes LoRaWAN.", "Multiplica por un chirp inverso, hace una FFT y el pico indica el símbolo; luego desentrelaza y corrige.", "Parámetros (SF, BW), carga con CRC y campos LoRaWAN, sin descifrar."],
  meshtastic: ["Malla de mensajería de bajo consumo sobre LoRa.", "Mensajes de texto y posición entre nodos sin infraestructura.", "Demodula LoRa, lee la cabecera de 16 bytes y descifra con AES-CTR (clave pública por defecto).", "Texto, posición y datos de nodo (descifrado aún sin validar en el aire)."],
  lte: ["Telefonía 4G: cada celda emite señales de sincronía conocidas.", "Saber qué celdas hay cerca y con qué potencia.", "Correla la PSS cada 5 ms, detecta la SSS, deduce el PCI y decodifica el MIB del PBCH.", "PCI, ancho de banda, SFN, CFO y potencia. Real: 4 celdas en Pamplona."],
  nr: ["5G NR: bloques SSB periódicos que anuncian cada celda.", "Detectar celdas 5G sin un móvil.", "Recorre la rejilla GSCN, correla la PSS, detecta la SSS y obtiene el PCI (0–1007).", "PCI, separación de subportadoras, CFO y potencia. Sin validar con aire."],
  gsm: ["Telefonía 2G: la portadora BCCH emite un tono de sincronía y un canal con la identidad de la celda.", "Identificar celdas GSM cercanas.", "Detecta el tono FCCH, decodifica el SCH con CRC y extrae BSIC y número de trama.", "BSIC (NCC y BCC) y número de trama. Solo información de difusión."],
  wifi: ["Las balizas (Beacon) que cada punto de acceso emite unas diez veces por segundo.", "Ver qué redes WiFi hay alrededor.", "Detecta el preámbulo, demodula DSSS (802.11b) u OFDM, valida el FCS y lee SSID y BSSID.", "SSID, BSSID, canal y seguridad. Real: 8 redes en DSSS."],
  dvbt: ["Estándar de la televisión digital terrestre europea (OFDM 2k/8k).", "Entender cómo se transmite la TDT y comprobar que se recibe.", "Detecta el prefijo cíclico, estima la frecuencia y lee los bits TPS de los pilotos.", "Modo, guarda, modulación, tasa y cell ID."],
  tdt: ["Televisión digital: paquetes de transporte MPEG-2 (TS) con vídeo H.264 y audio MP2.", "Ver y oír canales de TDT en el navegador.", "DVB-T entrega el TS; se separan servicios con PAT/PMT, se reensambla el PES y se sincroniza con el PTS.", "Lista de canales y reproducción de vídeo y audio (sobre un TS de prueba)."],
};

(async () => {
  const toPrep = { ...IMGS };
  for (const p of PROTOS) {
    toPrep["c_" + p.id] = p.cadena || D(p.id, "cadena");
    toPrep["s_" + p.id] = `decoders/${p.sig}-light.png`;
    toPrep["e_" + p.id] = `decoders/${p.est}-light.png`;
    toPrep["w_" + p.id] = `decoders/${p.id}-web.png`;
  }
  await prepImages(toPrep);
  await prepCaps();
  let s;

  // ------------------------------------------------------------ portada
  s = pres.addSlide({ masterName: "PORTADA" });
  img(s, "banner", { x: 0, y: 0, w: 13.333, h: 4.17 }, "Banner de BladeRF Lab: espectro y cascada");
  s.addText("Curso: un servidor SDR propio para el bladeRF 2.0 micro", { placeholder: "title" });
  s.addText("Del IQ al protocolo, desde cero en C · de los fundamentos de radio a la FPGA", { placeholder: "body" });
  s.addNotes("Este curso está pensado para leerse de corrido, sin nadie al lado. Supone que sabes programar y tienes un bladeRF, pero no que domines la teoría de radio ni de DSP. Cada módulo empieza con lo que vas a aprender y termina con un resumen y un ejercicio que puedes hacer sin antena. BladeRF Lab es un receptor SDR completo para el bladeRF 2.0 micro, escrito desde cero en C11 con aceleración CUDA opcional y manejado desde el navegador.");

  // ------------------------------------------------------------ resumen y cifras
  section("Presentación", "X");
  s = content("El proyecto en ocho cifras",
    "Todas las cifras salen del README y de la documentación. Cero bibliotecas DSP de terceros: FFT, filtros, FEC y sincronía están implementados y comentados en el repositorio; solo se enlaza libbladeRF. Con 16 instancias a 20 Msps, pasar el DDC a la GPU baja el consumo de 5,2 a 0,32 núcleos. Con ficheros, el motor sostiene 122,88 Msps sin descartes; el límite real con el bladeRF es el USB. La imagen FPGA «lab» está simulada pero aún no probada en la placa.");
  const tiles = [
    ["21", "protocolos decodificados, del ADS-B a la TDT", 54], ["22", "perfiles listos: un clic ajusta centro, tasa y audio", 54],
    ["0", "bibliotecas DSP de terceros: todo implementado aquí", 54], ["1", "binario en C11 que sirve también la interfaz web", 54],
    ["5,2 → 0,3", "núcleos con 16 decodificadores a 20 Msps al usar el DDC en GPU (CUDA)", 34], ["122,88", "Msps sostenidos con ficheros (cs8) sin descartes", 40],
    ["47 MHz–6 GHz", "barrido rápido del espectro", 28], ["FPGA propia", "imagen «lab» con motor de espectro; simulada y probada en la placa (solo RAM)", 28],
  ];
  tiles.forEach((t, i) => {
    const x = MX + (i % 4) * (2.85 + 0.243), y = 1.55 + Math.floor(i / 4) * 2.65;
    card(s, { x, y, w: 2.85, h: 2.4 }, "cifra");
    s.addText(t[0], { x: x + 0.15, y: y + 0.15, w: 2.55, h: 1.0, fontSize: t[2], bold: true, color: C.accent1, margin: 0, valign: "middle", fit: "shrink", isTextBox: true });
    s.addText(t[1], { x: x + 0.15, y: y + 1.2, w: 2.55, h: 1.1, fontSize: 14, color: C.text1, margin: 0, valign: "top", isTextBox: true });
  });

  s = content("Cómo leer este curso",
    "El curso va de lo básico a lo avanzado. El módulo 0 explica la teoría necesaria; el 1 compara el hardware; del 2 al 4 se ve cómo está construido el servidor; el 5 es el catálogo de protocolos; del 6 al 10 se tratan la TDT, la emisión, el rendimiento, la FPGA y la validación. Al final hay un glosario y un mapa de lectura de la documentación.");
  const idx = [["0", "Fundamentos", "SDR, IQ, FFT, DDC, modulaciones y errores"], ["1", "El hardware", "qué cuesta un SDR y por qué"], ["2", "Arquitectura", "hilos, cerrojos y WebSocket"],
    ["3", "Interfaz web", "qué ves en cada pestaña"], ["4", "Procesado de señal", "filtros, remuestreo, ventana y FEC"], ["5", "Protocolos", "21 fichas, de la señal al dato"],
    ["6", "TDT", "ver y oír la televisión"], ["7", "Emisión", "modos y política de seguridad"], ["8", "Rendimiento y CUDA", "medidas con ficheros y con el bladeRF"],
    ["9", "FPGA", "motor de espectro, registros y trama"], ["10", "Validación y estado", "qué está probado y qué falta"], ["A", "Anexos", "glosario y mapa de lectura"]];
  idx.forEach((t, i) => {
    const x = MX + (i % 4) * (2.85 + 0.243), y = 1.5 + Math.floor(i / 4) * 1.78;
    card(s, { x, y, w: 2.85, h: 1.6 }, "modulo");
    s.addShape(pres.ShapeType.ellipse, { x: x + 0.15, y: y + 0.15, w: 0.55, h: 0.55, fill: { color: C.accent1 }, line: { color: C.accent1 }, objectName: "letra" });
    s.addText(t[0], { x: x + 0.15, y: y + 0.15, w: 0.55, h: 0.55, fontSize: 16, bold: true, color: C.background1, align: "center", valign: "middle", margin: 0, isTextBox: true });
    s.addText(t[1], { x: x + 0.8, y: y + 0.15, w: 1.95, h: 0.55, fontSize: 15, bold: true, color: C.text1, margin: 0, valign: "middle", isTextBox: true });
    s.addText(t[2], { x: x + 0.15, y: y + 0.82, w: 2.55, h: 0.7, fontSize: 13, color: C.accent6, margin: 0, valign: "top", isTextBox: true });
  });

  // ============================================================ MÓDULO 0
  section("Módulo 0. Fundamentos", 0);
  moduleStart(0, "Fundamentos de radio y DSP",
    ["Qué es un SDR y qué parte del trabajo hace el software.", "Qué son las muestras IQ y la frecuencia de muestreo.", "Qué son el espectro, la FFT y la cascada, y cómo leerlos.", "Cómo se baja un canal a banda base (filtrar, diezmar, DDC).", "Cómo se codifica información en una onda (AM, FM, FSK, PSK, OFDM, LoRa) y cómo se corrigen errores."]);

  s = content("Qué es un SDR",
    "Una radio clásica tiene un circuito por cada función: filtro, mezclador, demodulador. En una radio definida por software (SDR) el hardware se limita a convertir en números una porción del espectro y el resto (filtrar, demodular, decodificar) es software que corre en el ordenador. El bladeRF cubre 47 MHz a 6 GHz con hasta 56 MHz de ancho y 12 bits. Fuente: README.md y docs/hardware/HARDWARE.md.");
  flow(s, ["Antena", "Radiofrecuencia: amplifica y baja a banda base", "Conversor analógico-digital (ADC)", "FPGA y USB 3", "Tu ordenador: software"], { x: MX, y: 1.6, w: CW, h: 1.5 }, 14);
  bullets(s, [{ b: "Hardware:", t: "convierte la señal de radio en una corriente de números." }, { b: "Software:", t: "todo lo demás. Cambiar de protocolo es cambiar de programa, no de radio." }, { b: "Ejemplo del proyecto:", t: "el mismo bladeRF y el mismo binario decodifican aviones, barcos, buscapersonas y televisión." }], { x: MX, y: 3.5, w: 7.2, h: 3.3 }, 16, 10);
  callout(s, "idea", "Todo lo que verás en este curso es software que procesa números. No hay magia: cada bloque es una fórmula que puedes leer en src/.", { x: 8.2, y: 3.5, w: 4.53, h: 2.0 }, 15);
  callout(s, "ojo", "Un SDR solo ve una ventana de frecuencias a la vez: su ancho es la tasa de muestreo.", { x: 8.2, y: 5.7, w: 4.53, h: 1.1 }, 14);

  // IQ: fasor dibujado con formas
  s = content("Muestras IQ: dos números por instante",
    "Una onda de radio se describe en cada instante con dos números, I (en fase) y Q (en cuadratura), que forman el número complejo I + jQ. Su módulo es la amplitud y su ángulo es la fase. Con IQ se distingue una frecuencia por encima del centro (el fasor gira a un lado) de otra por debajo (gira al otro), algo imposible con una sola muestra real. En el proyecto las muestras crudas son enteros con fondo de escala ±2048 (12 bits, formato Q11) y 0 dBFS equivale a un tono complejo de amplitud 1,0 (docs/arquitectura/WEBSOCKET.md, AGENTS.md §4.1).");
  {
    const cx = 3.6, cy = 4.1, r = 2.1;
    s.addShape(pres.ShapeType.ellipse, { x: cx - r, y: cy - r, w: 2 * r, h: 2 * r, fill: { color: "FFFFFF" }, line: { color: HEX.violet, width: 1.5 }, objectName: "circulo_iq" });
    s.addShape(pres.ShapeType.line, { x: cx - r - 0.3, y: cy, w: 2 * r + 0.6, h: 0, line: { color: HEX.grey, width: 1, endArrowType: "triangle" }, objectName: "eje_i" });
    s.addShape(pres.ShapeType.line, { x: cx, y: cy - r - 0.3, w: 0, h: 2 * r + 0.6, line: { color: HEX.grey, width: 1, beginArrowType: "triangle" }, objectName: "eje_q" });
    const th = 40 * Math.PI / 180, px = cx + r * Math.cos(th), py = cy - r * Math.sin(th);
    s.addShape(pres.ShapeType.line, { x: cx, y: py, w: px - cx, h: cy - py, flipV: true, line: { color: HEX.red, width: 3, endArrowType: "triangle" }, objectName: "fasor" });
    s.addShape(pres.ShapeType.line, { x: px, y: py, w: 0, h: cy - py, line: { color: HEX.grey, width: 1, dashType: "dash" }, objectName: "proy_q" });
    s.addShape(pres.ShapeType.line, { x: cx, y: py, w: px - cx, h: 0, line: { color: HEX.grey, width: 1, dashType: "dash" }, objectName: "proy_i" });
    s.addText("I (en fase)", { x: cx + r - 0.2, y: cy + 0.05, w: 1.3, h: 0.35, fontSize: 14, color: C.text1, margin: 0, isTextBox: true });
    s.addText("Q (cuadratura)", { x: cx + 0.1, y: cy - r - 0.45, w: 1.8, h: 0.35, fontSize: 14, color: C.text1, margin: 0, isTextBox: true });
    s.addText("x = I + jQ", { x: px - 0.2, y: py - 0.45, w: 1.5, h: 0.35, fontSize: 14, bold: true, color: HEX.red, margin: 0, isTextBox: true });
    s.addText("fase", { x: cx + 0.7, y: cy - 0.55, w: 0.8, h: 0.3, fontSize: 13, color: C.accent6, margin: 0, isTextBox: true });
  }
  bullets(s, [{ b: "Amplitud", t: "= longitud de la flecha (√(I² + Q²))." }, { b: "Fase", t: "= ángulo de la flecha; cambia con el tiempo si hay frecuencia." }, { b: "Giro:", t: "una flecha que gira a la izquierda es una frecuencia más alta que el centro; a la derecha, más baja." }, { b: "En el proyecto:", t: "enteros de 12 bits por componente; fondo de escala ±2048; 0 dBFS = amplitud 1,0." }], { x: 7.3, y: 1.5, w: 5.43, h: 3.6 }, 15);
  callout(s, "idea", "Con IQ la radio se convierte en una lista de números complejos: lo que hace cada decodificador es operar con ellos.", { x: 7.3, y: 5.3, w: 5.43, h: 1.5 }, 15);

  // Muestreo
  s = content("Frecuencia de muestreo: cuántas fotos por segundo",
    "La frecuencia de muestreo fs es el número de muestras IQ por segundo. Para señales reales hace falta fs mayor que el doble de la frecuencia máxima (criterio de Nyquist); con IQ complejo, una tasa fs captura un ancho de banda de fs: de −fs/2 a +fs/2 respecto a la frecuencia central. Por eso el bladeRF a 61,44 Msps ve unos 56 MHz y a 2,4 Msps ve 2,4 MHz. Lo que queda fuera se pliega dentro (aliasing) si no se filtra antes. Fuente: teoría estándar de muestreo; cifras del proyecto en README.md y docs/arquitectura/RENDIMIENTO.md.");
  const nS = 16, sv = []; for (let i = 0; i < nS; i++) sv.push(Math.round(Math.sin((2 * Math.PI * i) / nS) * 100) / 100);
  s.addChart(pres.charts.BAR, [{ name: "Muestras", labels: sv.map((_, i) => String(i)), values: sv }], {
    x: MX, y: 1.5, w: 6.6, h: 3.9, barDir: "col", chartColors: [HEX.violet], showTitle: true, title: "Una senoide vista como 16 números", titleFontSize: 14, titleColor: C.text1, titleFontFace: "+mn-lt",
    showValue: false, catAxisLabelColor: C.text1, catAxisLabelFontSize: 11, catAxisLabelFontFace: "+mn-lt", valAxisLabelColor: C.accent6, valAxisLabelFontSize: 11, valAxisLabelFontFace: "+mn-lt",
    valGridLine: { color: HEX.line, size: 0.5 }, catGridLine: { style: "none" }, showLegend: false, catAxisLabelPos: "low", objectName: "grafica_muestras" });
  cap(s, "Cada barra es una muestra: el ordenador no ve la curva, solo estos números.", MX, 5.45, 6.6, 0.4);
  bullets(s, [{ b: "fs", t: "= muestras por segundo (Msps = millones)." }, { b: "Ancho capturado ≈ fs:", t: "de −fs/2 a +fs/2 alrededor de la frecuencia central." }, { b: "Ejemplos:", t: "2,4 Msps ve 2,4 MHz (una emisora de FM); 61,44 Msps ve unos 56 MHz." }], { x: 7.5, y: 1.5, w: 5.23, h: 3.0 }, 15);
  callout(s, "ojo", "Si hay señales fuera de ±fs/2 se pliegan dentro de la banda (aliasing). Por eso el hardware filtra antes de muestrear.", { x: 7.5, y: 4.7, w: 5.23, h: 1.4 }, 14);
  callout(s, "idea", "Más tasa = más ancho de banda, pero también más datos que procesar.", { x: MX, y: 5.95, w: 6.6, h: 0.85 }, 14);

  // FFT
  lesson({ title: "Espectro y FFT: qué frecuencias hay", imgs: [{ k: "bh", alt: "Ventana Blackman-Harris" }], iw: 6.2, ih: 2.6, cap: "Ventana Blackman-Harris propia (N = 1024): lóbulos laterales a −93 dB. Medida con el código real.",
    steps: ["Toma N muestras IQ seguidas (por ejemplo 4096).", "Multiplícalas por una ventana: evita que los bordes del trozo ensucien el resultado.", "Aplica la FFT: N muestras en el tiempo pasan a N «cajones» de frecuencia (bins).", "Calcula |X|² y pásalo a dB (dBFS).", "Promedia varias tramas y dibújalo: es la curva de espectro."], size: 15, side: "R", iw2: 0,
    idea: "Resolución de un bin = fs / N. Con 2,4 Msps y N = 4096, cada bin mide unos 586 Hz; más N da más detalle pero tarda más.",
    notes: "La FFT calcula la transformada de Fourier discreta. Antes se aplica una ventana de Blackman-Harris, cuyos lóbulos laterales caen a −93 dB frente a −13 dB de la rectangular, a cambio de un lóbulo principal más ancho (ENBW = 2,006 bins). La FFT es propia, en CPU o en CUDA. Fuente: docs/arquitectura/DSP.md." });

  // Cascada
  s = content("Leer la cascada (waterfall)",
    "El espectro dice qué hay ahora; la cascada apila los espectros uno bajo otro y añade el tiempo. Eje horizontal: frecuencia; eje vertical: tiempo (lo nuevo entra por arriba); color: potencia. Una portadora fija es una línea vertical; una ráfaga corta (un mensaje de buscapersonas o un paquete ISM) es un trazo corto; una emisora de FM ocupa unos 200 kHz y se ve como una banda ancha con variaciones. Captura de la interfaz con una FM generada con --gen. Fuente: README.md y docs/uso/GUIA.md.");
  img(s, "cs_receptor", { x: MX, y: 1.45, w: 7.3, h: 4.56 }, "Pestaña Receptor con espectro y cascada", "left");
  cap(s, "Pestaña Receptor: espectro arriba, cascada debajo. FM generada con --gen rds.", MX, 6.1, 7.3, 0.6);
  bullets(s, [{ b: "Horizontal:", t: "frecuencia." }, { b: "Vertical:", t: "tiempo; lo nuevo entra por arriba." }, { b: "Color:", t: "potencia (más claro = más fuerte)." }, { b: "Línea vertical:", t: "portadora continua." }, { b: "Trazo corto:", t: "ráfaga (un mensaje)." }], { x: 8.2, y: 1.5, w: 4.53, h: 3.4 }, 15);
  callout(s, "idea", "La cascada es la forma más rápida de saber qué protocolo es una señal antes de decodificarla.", { x: 8.2, y: 5.1, w: 4.53, h: 1.5 }, 15);

  // DDC
  s = content("Bajar a banda base: el DDC",
    "Un DDC (Digital Down Converter) extrae un canal estrecho de una captura ancha. Primero un oscilador numérico (NCO) multiplica por e^{-jωn} para mover el canal deseado a 0 Hz; después se filtra con un paso bajo; como el canal ocupa poco, se diezma (se calcula solo 1 de cada D salidas); por último un remuestreador fraccionario ajusta a la tasa exacta que pide el decodificador. La cadena del proyecto: NCO → firdec en etapas (÷8 máx.) → cfir de ancho bw → remuestreador complejo cresamp. Fuente: docs/arquitectura/DSP.md.");
  steps(s, [{ b: "Mezclar:", t: "multiplicar por un oscilador numérico (NCO) para llevar el canal a 0 Hz." }, { b: "Filtrar:", t: "un paso bajo deja solo el canal y elimina el resto." }, { b: "Diezmar:", t: "quedarse con 1 de cada D muestras, porque ya sobran." }, { b: "Remuestrear:", t: "ajustar a la tasa exacta del decodificador (por ejemplo 48 000 S/s)." }], { x: MX, y: 1.5, w: 6.5, h: 3.7 }, 16);
  flow(s, ["Captura 2,4 Msps", "NCO + filtro", "Diezmado", "Canal a 48 kS/s"], { x: MX, y: 5.45, w: 6.5, h: 1.2 }, 13);
  callout(s, "idea", "Ejemplo del proyecto: AIS ocupa unos 25 kHz dentro de una captura de 2,4 MHz. Tras el DDC se procesan 48 000 muestras por segundo en lugar de 2,4 millones: 50 veces menos.", { x: 7.6, y: 1.5, w: 5.13, h: 2.3 }, 15);
  img(s, "canal", { x: 7.6, y: 4.0, w: 5.13, h: 2.0 }, "Cadena de un canal de audio");
  cap(s, "Cadena de un canal de audio del proyecto (NCO, diezmado, filtro, demodulador, remuestreo).", 7.6, 6.1, 5.13, 0.6);

  // Filtro FIR
  lesson({ title: "Filtrar: una media ponderada que se desliza", imgs: [{ k: "firdec", alt: "Respuesta del diezmador FIR" }], iw: 7.2, ih: 3.1,
    cap: "Etapa ÷8 de un DDC con canal de 200 kHz a 2,4 Msps (209 taps) y a 61,44 Msps (73 taps). Medida con el código real.",
    bullets: [{ b: "Filtro FIR:", t: "cada salida es la suma de las últimas N muestras, cada una multiplicada por un coeficiente (tap)." }, { b: "Los coeficientes", t: "determinan qué frecuencias pasan y cuáles se atenúan." }, { b: "Diseño de Kaiser:", t: "una sola fórmula da la atenuación y la transición que buscas." }, { b: "Diezmador:", t: "solo calcula 1 de cada D salidas; el coste depende de la tasa de salida." }],
    idea: "Lo que aliasea sobre la banda útil queda por debajo de −68 dB en el diezmador del proyecto.", side: "L",
    notes: "El diseño usa la fórmula de Kaiser: beta = 0,1102 (A − 8,7) para A > 50 dB y N ≈ (A − 8) / (2,285 · 2π · Δf/fs). Todos los filtros se normalizan a ganancia 1 en continua. Fuente: docs/arquitectura/DSP.md." });

  // Modulación AM/FM
  s = content("Modular: AM y FM",
    "Modular es escribir información sobre una portadora. AM varía la amplitud: la envolvente de la señal copia al audio. FM varía la frecuencia instantánea: lo que importa es cuánto se desvía del centro, no el volumen. FM es más inmune al ruido de amplitud. En el proyecto: ACARS usa AM de baja profundidad (el audio MSK a 1200/2400 Hz va en la envolvente) y POCSAG usa FM con FSK; la FM comercial lleva además RDS. Fuente: docs/protocolos/acars.md, pocsag.md y rds.md.");
  img(s, "fAm", { x: MX, y: 1.5, w: 5.95, h: 2.4 }, "Envolvente AM de ACARS");
  img(s, "fFm", { x: MX + 6.18, y: 1.5, w: 5.95, h: 2.4 }, "Cascada y ojo FSK de POCSAG");
  cap(s, "AM: cascada y envolvente de una grabación real de ACARS.", MX, 3.95, 5.95, 0.4);
  cap(s, "FM: cascada y ojo FSK de POCSAG (audio real remodulado).", MX + 6.18, 3.95, 5.95, 0.4);
  bullets(s, [{ b: "AM = gritar más o menos fuerte:", t: "la información va en la amplitud." }, { b: "FM = cantar más agudo o más grave:", t: "la información va en la frecuencia." }], { x: MX, y: 4.5, w: 7.3, h: 2.3 }, 16, 10);
  callout(s, "idea", "Para decodificar FM se calcula la variación de fase entre muestras IQ consecutivas (un discriminador).", { x: 8.2, y: 4.5, w: 4.53, h: 2.2 }, 15);

  // FSK GMSK PSK
  s = content("Modulación digital: FSK, GMSK y PSK",
    "En FSK cada bit elige entre dos frecuencias (por ejemplo 1200 y 2200 Hz en APRS). GMSK es una FSK de fase continua con un filtro gaussiano que suaviza las transiciones para ocupar menos ancho (AIS: BT = 0,4 a 9600 bd). En PSK la información está en la fase: BPSK usa dos fases (RDS), D8PSK usa ocho (VDL2: 3 bits por símbolo). Un símbolo es lo que se transmite de una vez; baudio = símbolos por segundo, y bits/s = baudios × bits por símbolo. Fuente: docs/protocolos/ais.md, rds.md y vdl2.md.");
  img(s, "fGmsk", { x: MX, y: 1.5, w: 5.95, h: 2.4 }, "Ojo GMSK de AIS");
  img(s, "fBpsk", { x: MX + 6.18, y: 1.5, w: 5.95, h: 2.4 }, "Constelación BPSK de RDS");
  cap(s, "GMSK de AIS: cascada de audio real remodulado y ojo de una señal generada.", MX, 3.95, 5.95, 0.4);
  cap(s, "BPSK de RDS: espectro MPX y constelación (señal sintética).", MX + 6.18, 3.95, 5.95, 0.4);
  bullets(s, [{ b: "FSK:", t: "dos tonos, uno para el 0 y otro para el 1, como un silbido agudo o grave." }, { b: "GMSK:", t: "FSK suavizada para ocupar menos ancho (AIS, 9600 bd)." }, { b: "PSK:", t: "el dato es la fase; BPSK tiene 2 estados y D8PSK, 8." }], { x: MX, y: 4.5, w: 7.3, h: 2.3 }, 15, 8);
  callout(s, "idea", "Baudio = símbolos por segundo. Bit/s = baudios × bits por símbolo (D8PSK: 3).", { x: 8.2, y: 4.5, w: 4.53, h: 2.2 }, 15);

  // OFDM
  s = content("OFDM: muchas portadoras lentas en paralelo",
    "OFDM reparte los datos entre muchas subportadoras ortogonales, cada una con símbolos largos y lentos. Un eco más corto que el prefijo cíclico (copia del final del símbolo puesta delante) no mezcla símbolos consecutivos. DAB modo I usa 1536 portadoras separadas 1 kHz con D-QPSK; DVB-T usa 2k u 8k portadoras; WiFi y LTE también son OFDM. La FFT es el demodulador natural. Fuente: docs/protocolos/dab.md y dvbt.md.");
  img(s, "fOfdm", { x: MX, y: 1.5, w: 5.95, h: 2.4 }, "Espectro y constelación D-QPSK de DAB");
  img(s, "fOfdm2", { x: MX + 6.18, y: 1.5, w: 5.95, h: 2.4 }, "Símbolo OFDM 8k de DVB-T");
  cap(s, "DAB: símbolo nulo, espectro y constelación D-QPSK de una grabación real.", MX, 3.95, 5.95, 0.4);
  cap(s, "DVB-T: símbolo OFDM 8k y bits TPS.", MX + 6.18, 3.95, 5.95, 0.4);
  bullets(s, [{ b: "Analogía:", t: "en vez de un camión muy rápido, 1536 bicicletas lentas en paralelo; si una se pincha, las demás llegan." }, { b: "Prefijo cíclico:", t: "copia del final del símbolo que absorbe los ecos." }, { b: "Se usa en:", t: "DAB, DVB-T, WiFi y LTE." }], { x: MX, y: 4.5, w: 7.3, h: 2.3 }, 15, 8);
  callout(s, "idea", "La FFT que dibuja el espectro es también la que separa las portadoras OFDM.", { x: 8.2, y: 4.5, w: 4.53, h: 2.2 }, 15);

  // CSS LoRa
  lesson({ title: "LoRa: el dato es un silbido que sube", imgs: [{ k: "fCss", alt: "Cascada de chirps y picos tras dechirp" }], iw: 7.0, ih: 2.9,
    cap: "Cascada de chirps y picos tras el dechirp (referencia de loratx). Figura del proyecto.", side: "L",
    steps: ["El transmisor emite un chirp: una frecuencia que sube de grave a agudo.", "El valor del símbolo es dónde empieza el chirp.", "El receptor multiplica por el chirp inverso (dechirp).", "Una FFT convierte cada símbolo en un pico; la posición del pico es el dato."],
    idea: "Con SF = 12 hay 2^12 = 4096 chips por símbolo: más largo alcance, menos velocidad; el pico concentra la energía y permite decodificar con poca señal.",
    notes: "CSS (chirp spread spectrum): con ancho de banda BW y factor de ensanchado SF hay N = 2^SF chips por símbolo. El decodificador del proyecto cubre SF7–SF12 con BW de 125 o 250 kHz. Fuente: docs/protocolos/lora.md." });

  // CRC
  lesson({ title: "Errores (1): comprobar con un CRC", imgs: [{ k: "e_acars", alt: "Bloque ACARS con BCS" }], iw: 6.2, ih: 2.3, cap: "Bloque ACARS: el BCS del final es un CRC-16 que protege todo el contenido.", side: "R",
    bullets: [{ b: "Analogía:", t: "la letra del DNI o el dígito de control del ISBN: se calcula a partir de los demás dígitos." }, { b: "CRC:", t: "el emisor divide el mensaje (como polinomio) por un polinomio fijo y añade el resto." }, { b: "El receptor repite la cuenta:", t: "si no coincide, hubo un error." }, { b: "En el proyecto:", t: "solo se emite un evento si el CRC es correcto." }],
    idea: "Un CRC detecta errores; no los corrige. Para corregir hace falta un código con más redundancia.", ojo: "Hay muchos CRC (CRC-16/X-25, KERMIT, CRC-24 de ADS-B): mismo método, distinto polinomio.",
    notes: "El proyecto incluye un módulo genérico con 55 presets de CRC (docs/INVESTIGACION y AGENTS.md §4.5b). ADS-B usa CRC-24 con polinomio 0xFFF409; AIS y APRS, CRC-16/X-25; ACARS, CRC-16/KERMIT. Regla de AGENTS.md §4.1: lo que no pase la comprobación de integridad no se emite." });

  // FEC
  lesson({ title: "Errores (2): corregir con Viterbi y Reed-Solomon", imgs: [{ k: "ber", alt: "BER con y sin Viterbi" }], iw: 6.3, ih: 3.9, cap: "BER con y sin Viterbi: código K = 7 de fec.c, BPSK sobre AWGN. Medida con el código real.", side: "L",
    bullets: [{ b: "Viterbi:", t: "ante una secuencia dudosa, elige el camino más probable de una máquina de estados. Usa decisiones blandas (con confianza)." }, { b: "Reed-Solomon:", t: "trabaja con bytes y corrige hasta t bytes erróneos; en DVB-T, RS(204,188) corrige 8." }, { b: "BCH y Hamming:", t: "corrigen unos pocos bits (POCSAG usa BCH(31,21))." }],
    idea: "A 4 dB de Eb/N0 el Viterbi blando baja el BER de 1,3·10⁻² (sin codificar) a unos 10⁻⁵.", ojo: "La corrección cuesta velocidad: se envían más bits de los que lleva el mensaje.", cs: 14,
    notes: "El proyecto tiene su propia biblioteca (fec.h): convolucionales NASA K=7, GSM K=5, DAB, LTE; Reed-Solomon CCSDS, RS41, VDL2, DVB y DAB; BCH, Golay y Hamming. Fuente: docs/arquitectura/DSP.md y AGENTS.md §4.5b." });

  wrapUp(["Un SDR digitaliza el espectro y el resto es software.", "IQ = un número complejo por muestra; fs fija el ancho visto.", "La FFT da el espectro; la cascada añade el tiempo.", "El DDC baja un canal a banda base: mezclar, filtrar, diezmar.", "Se modula la amplitud, frecuencia o fase; OFDM y LoRa usan muchas portadoras o chirps.", "CRC detecta errores; Viterbi y Reed-Solomon los corrigen."],
    ["# 1. genera una señal AIS sin antena", "./build/bladerf-lab --gen ais --gen-freq 161975000 \\", "  --gen-center 162000000 --gen-rate 1000000 \\", "  --out ais_prueba", "# 2. decodifícala", "./build/bladerf-lab --replay ais_prueba.sigmf-meta \\", "  --decode ais@161975000 --print"],
    "Genera una señal AIS con el modulador del proyecto y decodifícala sin antena. Verás un evento JSON por mensaje válido.");

  // ============================================================ MÓDULO 1
  section("Módulo 1. El hardware", 1);
  moduleStart(1, "El hardware: cuánto cuesta un SDR",
    ["Qué rango de precios tiene el mundo SDR y dónde está nuestra placa.", "Qué capacidades distinguen a unos SDR de otros (RX/TX, bits, ancho de banda).", "Por qué el bladeRF 2.0 micro cuesta lo que cuesta.", "Qué SDR necesitas para cada tarea del proyecto.", "Por qué el cable y el puerto USB pueden limitarte."],
    "Datos de precios verificados por el orquestador en noviembre de 2026. Son orientativos, nuevos y sin impuestos ni envío.");
  s = content("Precio orientativo de los SDR (USD)",
    "Precios orientativos en USD, nuevos, en tienda oficial o distribuidor, noviembre de 2026, sin impuestos ni envío. RTL-SDR Blog V4: 35 (rango 30–40). Airspy Mini 99. Airspy HF+ Discovery 169. SDRplay RSPdx-R2 210 (200–220). ADALM-Pluto 190 (150–230). HackRF Pro 400 (precio de lanzamiento de 2025; el HackRF One se ha retirado de SparkFun). LimeSDR Mini 2.0 399. bladeRF 2.0 micro xA4 540 y xA9 860 (Nuand). USRP B200mini 1500 y B210 2400 (Ettus). Nuestra placa es la xA9.");
  const sdr = [["RTL-SDR Blog V4", 35], ["Airspy Mini", 99], ["Airspy HF+ Discovery", 169], ["ADALM-Pluto", 190], ["SDRplay RSPdx-R2", 210], ["LimeSDR Mini 2.0", 399], ["HackRF Pro", 400], ["bladeRF 2.0 micro xA4", 540], ["bladeRF 2.0 micro xA9 (la nuestra)", 860], ["USRP B200mini", 1500], ["USRP B210", 2400]];
  s.addChart(pres.charts.BAR, [{ name: "Precio (USD)", labels: sdr.map((d) => d[0]), values: sdr.map((d) => d[1]) }], {
    x: MX, y: 1.4, w: CW, h: 5.0, barDir: "bar", chartColors: sdr.map((d) => (d[0].includes("xA9") ? HEX.red : HEX.violet)), showTitle: false,
    showValue: true, dataLabelPosition: "outEnd", dataLabelFontSize: 12, dataLabelColor: C.text1, dataLabelFontFace: "+mn-lt", dataLabelFormatCode: '"$"#,##0',
    catAxisLabelColor: C.text1, catAxisLabelFontSize: 13, catAxisLabelFontFace: "+mn-lt", valAxisLabelColor: C.accent6, valAxisLabelFontSize: 11, valAxisLabelFontFace: "+mn-lt",
    valGridLine: { color: HEX.line, size: 0.5 }, catGridLine: { style: "none" }, showLegend: false, valAxisMinVal: 0, catAxisOrientation: "maxMin", valAxisLabelFormatCode: '"$"#,##0', objectName: "grafica_precios" });
  cap(s, "Precios orientativos, nov. 2026, sin impuestos ni envío; cambian a menudo. HackRF Pro: precio de lanzamiento de 2025. En rojo, nuestra placa (bladeRF 2.0 micro xA9).", MX, 6.45, CW, 0.5, 12);

  s = content("Qué puede hacer cada SDR",
    "Solo se muestran las capacidades verificadas por el orquestador. RTL-SDR V4: solo recepción, 8 bits, unos 2,4 MHz de ancho útil. Airspy Mini: solo RX, 12 bits, 6 MHz, 24–1700 MHz. SDRplay RSPdx-R2: solo RX, ADC de 14 bits, hasta 10 MHz, 1 kHz–2 GHz. ADALM-Pluto: RX y TX a la vez, 1×1, AD9363, unos 20 MHz. HackRF: RX o TX pero no a la vez, 8 bits, 20 Msps. LimeSDR Mini 2.0: RX y TX, 1×1, 12 bits. bladeRF 2.0 micro: RX y TX a la vez, 2×2 MIMO, 12 bits, 47 MHz–6 GHz, hasta 56 MHz (61,44 Msps), USB 3, FPGA Cyclone V (xA4: 49 kLE; xA9: 301 kLE). USRP B210: 2×2, AD9361, 70 MHz–6 GHz.");
  table(s, [["SDR", "RX / TX", "Detalles"],
    ["RTL-SDR V4", "solo RX", "8 bits; ~2,4 MHz de ancho útil"],
    ["Airspy Mini", "solo RX", "12 bits; 6 MHz; 24–1700 MHz"],
    ["SDRplay RSPdx-R2", "solo RX", "ADC de 14 bits; hasta 10 MHz; 1 kHz–2 GHz"],
    ["ADALM-Pluto", "RX y TX a la vez", "1×1; AD9363; unos 20 MHz"],
    ["HackRF", "RX o TX (semidúplex)", "8 bits; 20 Msps"],
    ["LimeSDR Mini 2.0", "RX y TX", "1×1; 12 bits"],
    ["bladeRF 2.0 micro", "RX y TX a la vez", "2×2 MIMO; 12 bits; 47 MHz–6 GHz; hasta 56 MHz (61,44 Msps); USB 3; FPGA Cyclone V (xA4: 49 kLE; xA9: 301 kLE)"],
    ["USRP B210", "—", "2×2; AD9361; 70 MHz–6 GHz"]], { x: MX, y: 1.45, w: CW }, [2.6, 2.7, 6.83], 14, 0.58);

  s = content("¿Por qué el bladeRF vale lo que vale? (1)",
    "Desglose del precio. 1) El chip de radio AD9361 de Analog Devices, el mismo de los USRP B210, cuesta él solo unos 300–355 USD por unidad en DigiKey o Mouser: más de la mitad del precio de la xA4. 2) La FPGA Cyclone V: los 320 USD de diferencia entre xA4 (540) y xA9 (860) son básicamente la FPGA grande (49 kLE frente a 301 kLE). Con ella hemos podido meter nuestro propio motor de espectro. Fuente: datos verificados por el orquestador, nov. 2026.");
  const cards3 = [["300–355 USD", "El chip de radio AD9361", "Es de Analog Devices, el mismo chip del USRP B210. Solo él cuesta unos 300–355 USD por unidad en DigiKey o Mouser: más de la mitad del precio de la xA4."],
    ["320 USD", "La FPGA Cyclone V", "La diferencia entre la xA4 (540) y la xA9 (860) es básicamente la FPGA: 49 kLE frente a 301 kLE. Con la grande hemos metido nuestro propio motor de espectro."],
    ["USB 3 + RF", "Controlador, relojes y placa", "Controlador USB 3 (Cypress FX3), relojes de precisión, placa multicapa de RF y alimentación limpia."]];
  cards3.forEach((c, i) => {
    const x = MX + i * 4.1;
    card(s, { x, y: 1.5, w: 3.93, h: 4.2 }, "desglose");
    s.addText(c[0], { x: x + 0.25, y: 1.65, w: 3.43, h: 0.9, fontSize: 28, bold: true, color: C.accent1, margin: 0, valign: "middle", fit: "shrink", isTextBox: true });
    s.addText(c[1], { x: x + 0.25, y: 2.6, w: 3.43, h: 0.5, fontSize: 17, bold: true, color: C.text1, margin: 0, valign: "top", isTextBox: true });
    s.addText(c[2], { x: x + 0.25, y: 3.2, w: 3.43, h: 2.4, fontSize: 15, color: C.text1, margin: 0, valign: "top", isTextBox: true });
  });
  callout(s, "idea", "Más de la mitad del precio de la xA4 es el chip de radio; casi toda la diferencia con la xA9 es la FPGA.", { x: MX, y: 5.95, w: CW, h: 0.85 }, 15);

  s = content("¿Por qué el bladeRF vale lo que vale? (2)",
    "Pocas unidades: es un mercado de nicho; el diseño, las pruebas, la calibración y el soporte se reparten entre pocas placas. El RTL-SDR es barato porque usa el chip RTL2832U de los sintonizadores de TDT, fabricado por millones. Lo que compras: dúplex completo, 2×2, 12 bits, 56 MHz de ancho y hasta 6 GHz, con código abierto en la FPGA y en el host. Fuente: datos verificados por el orquestador, nov. 2026.");
  card(s, { x: MX, y: 1.5, w: 5.95, h: 5.2 }, "nicho");
  s.addText("Un mercado de nicho", { x: MX + 0.3, y: 1.65, w: 5.35, h: 0.5, fontSize: 20, bold: true, color: C.accent1, margin: 0, isTextBox: true });
  bullets(s, [{ b: "Pocas unidades:", t: "el diseño, las pruebas, la calibración y el soporte se reparten entre pocas placas." }, { b: "RTL-SDR:", t: "es barato porque usa el chip RTL2832U de los sintonizadores de TDT, fabricado por millones." }], { x: MX + 0.3, y: 2.3, w: 5.35, h: 4.2 }, 16, 12);
  card(s, { x: MX + 6.18, y: 1.5, w: 5.95, h: 5.2 }, "compras");
  s.addText("Lo que compras", { x: MX + 6.48, y: 1.65, w: 5.35, h: 0.5, fontSize: 20, bold: true, color: C.accent1, margin: 0, isTextBox: true });
  bullets(s, ["Dúplex completo: RX y TX a la vez.", "2×2 MIMO y 12 bits.", "Hasta 56 MHz de ancho y de 47 MHz a 6 GHz.", "Código abierto en la FPGA y en el host."], { x: MX + 6.48, y: 2.3, w: 5.35, h: 4.2 }, 16, 12);

  s = content("¿Qué SDR necesito para cada cosa?",
    "Con el proyecto como ejemplo: RDS, ADS-B, AIS, POCSAG o APRS funcionan con un RTL-SDR de 35 USD porque son señales estrechas. TDT (8 MHz), WiFi (20 MHz), LTE (hasta 20 MHz) o 5G NR necesitan mucho ancho de banda: bladeRF, Pluto (justo) o USRP. Para emitir hace falta TX: Pluto, HackRF, Lime, bladeRF o USRP. Cifras medidas por el proyecto (docs/arquitectura/RENDIMIENTO.md §4, repetición con la máquina libre): a 61,44 Msps, 3 desbordes (overruns) en 25 s y 0 drops; a 122,88 Msps, 22 desbordes por el límite del USB 3 y el FX3, no por el motor.");
  table(s, [["Quiero…", "Basta con…"],
    ["RDS, ADS-B, AIS, POCSAG, APRS (señales estrechas)", "un RTL-SDR de 35 USD"],
    ["TDT (8 MHz), WiFi (20 MHz), LTE (hasta 20 MHz), 5G NR", "mucho ancho de banda: bladeRF, Pluto (justo) o USRP"],
    ["Emitir señales", "TX: Pluto, HackRF, Lime, bladeRF o USRP"]], { x: MX, y: 1.5, w: 7.6 }, [3.8, 3.8], 14, 0.85);
  callout(s, "idea", "El precio lo marcan sobre todo el ancho de banda, la capacidad de transmitir y la FPGA.", { x: 8.5, y: 1.5, w: 4.23, h: 1.9 }, 15);
  card(s, { x: MX, y: 5.05, w: CW, h: 1.7 }, "medidas");
  s.addText([{ text: "Nuestras cifras medidas (solo recepción): ", options: { bold: true, color: C.accent1 } }, { text: "a 61,44 Msps el servidor sostiene la tasa con 3 desbordes y 0 descartes en 25 s; a 122,88 Msps hay 22 desbordes, por el límite del USB 3 y no del motor (docs/arquitectura/RENDIMIENTO.md §4)." }], { x: MX + 0.3, y: 5.15, w: CW - 0.6, h: 1.5, fontSize: 16, color: C.text1, margin: 0, valign: "middle", isTextBox: true });

  s = content("USB 2 frente a USB 3: comprueba tu cable",
    "Caso real: la placa estaba enumerada como USB 2.0 Hi-Speed (480 Mb/s). Con ella solo pasan unos 45 MB/s, es decir unos 11 millones de muestras por segundo (4 bytes por muestra SC16), tanto con la imagen de serie como con la lab, aunque se pidan 30,72 o 61,44 Msps. El USB 3 (5 Gb/s) es el que permite los 61,44 Msps del bladeRF. Datos verificados por el orquestador el 2026-11-16.");
  {
    const pipe = (y, h, color, label, sub) => {
      s.addShape(pres.ShapeType.roundRect, { x: 0.6, y: y - 0.45, w: 1.9, h: 0.9, rectRadius: 0.1, fill: { color: HEX.tint }, line: { color: HEX.violet, width: 1 }, objectName: "caja_bladerf" });
      s.addText("bladeRF", { x: 0.6, y: y - 0.45, w: 1.9, h: 0.9, fontSize: 15, bold: true, color: C.text1, align: "center", valign: "middle", margin: 0, isTextBox: true });
      s.addShape(pres.ShapeType.roundRect, { x: 2.7, y: y - h / 2, w: 7.0, h, rectRadius: Math.min(0.12, h / 2), fill: { color }, line: { color }, objectName: "tuberia" });
      s.addShape(pres.ShapeType.roundRect, { x: 9.9, y: y - 0.45, w: 1.9, h: 0.9, rectRadius: 0.1, fill: { color: HEX.tint }, line: { color: HEX.violet, width: 1 }, objectName: "caja_pc" });
      s.addText("Ordenador", { x: 9.9, y: y - 0.45, w: 1.9, h: 0.9, fontSize: 15, bold: true, color: C.text1, align: "center", valign: "middle", margin: 0, isTextBox: true });
      s.addText([{ text: label, options: { bold: true, color: C.text1, breakLine: true } }, { text: sub, options: { color: C.accent6 } }], { x: 2.7, y: y - h / 2 - 0.85, w: 7.0, h: 0.75, fontSize: 14, margin: 0, valign: "bottom", align: "center", isTextBox: true });
    };
    pipe(2.55, 0.14, HEX.amber, "USB 2.0 Hi-Speed · 480 Mb/s", "pasan ≈ 45 MB/s ≈ 11 millones de muestras por segundo");
    pipe(5.0, 1.0, HEX.teal, "USB 3 SuperSpeed · 5 Gb/s", "lo que permite los 61,44 Msps del bladeRF");
  }
  callout(s, "ojo", "Con USB 2 da igual pedir 30,72 o 61,44 Msps: solo llegan unos 11 Msps, con la imagen de serie y con la lab.", { x: MX, y: 6.0, w: CW, h: 0.8 }, 14);
  s = content("Cómo comprobar el USB de tu bladeRF",
    "Comprobación: bladeRF-cli -e info muestra la línea USB speed: SuperSpeed (bien) o Hi-Speed (USB 2). Hay que usar un puerto USB 3 (suele ser azul o llevar las letras SS) y un cable USB 3. Las señales estrechas (FM, AIS, ADS-B…) van sobradas con USB 2; las anchas (WiFi o LTE de 20 MHz, 5G) necesitan USB 3. Datos verificados por el orquestador el 2026-11-16. Solo es una consulta: no carga ni escribe nada en la placa.");
  code(s, ["bladeRF-cli -e info", "...", "USB speed: SuperSpeed  <- USB 3, bien", "USB speed: Hi-Speed    <- USB 2: revisa cable"], { x: MX, y: 1.5, w: 6.6, h: 2.0 }, 14);
  steps(s, [{ b: "Puerto:", t: "usa uno USB 3 (suele ser azul o llevar «SS»)." }, { b: "Cable:", t: "usa un cable USB 3; uno USB 2 limita aunque el puerto sea bueno." }, { b: "Vuelve a comprobar", t: "con bladeRF-cli -e info." }], { x: MX, y: 3.8, w: 6.6, h: 2.9 }, 15);
  card(s, { x: 7.5, y: 1.5, w: 5.23, h: 2.6 }, "va_sobrado");
  s.addText([{ text: "Con USB 2 va sobrado", options: { bold: true, color: C.accent2, breakLine: true } }, { text: "Señales estrechas: FM, RDS, AIS, ADS-B, POCSAG, APRS." }], { x: 7.75, y: 1.65, w: 4.73, h: 2.3, fontSize: 16, color: C.text1, margin: 0, valign: "top", isTextBox: true });
  card(s, { x: 7.5, y: 4.3, w: 5.23, h: 2.4 }, "necesita_usb3");
  s.addText([{ text: "Necesita USB 3", options: { bold: true, color: C.accent3, breakLine: true } }, { text: "Señales anchas: WiFi o LTE de 20 MHz y 5G." }], { x: 7.75, y: 4.45, w: 4.73, h: 2.1, fontSize: 16, color: C.text1, margin: 0, valign: "top", isTextBox: true });

  s = content("Fuentes de los datos del módulo",
    "Datos verificados por el orquestador en noviembre de 2026. Los precios cambian a menudo.");
  s.addText(["https://www.nuand.com/product/bladerf-xa4/", "https://www.nuand.com/product/bladerf-xa9/", "https://www.ettus.com/all-products/usrp-b200mini/", "https://www.ettus.com/allproducts/ub210-kit", "https://www.sparkfun.com/hackrf-pro.html", "https://www.digikey.es/en/products/detail/analog-devices-inc/AD9361BBCZ/4439647", "https://hamdeals.com/blog/sdr-dongles-ranked/", "https://www.sdrstore.eu/best-sdr-for-beginners-2026-rtl-sdr-hackrf-plutosdr-usrp-compared/", "https://www.alauda.ro/2026/06/the-complete-amateur-radio-sdr-buyers-guide-and-comparison-2025/"].map((u, i, a) => ({ text: u, options: { breakLine: i < a.length - 1, paraSpaceAfter: 6 } })),
    { x: MX, y: 1.5, w: CW, h: 4.6, fontSize: 14, color: C.text1, margin: 0, valign: "top", isTextBox: true, objectName: "fuentes" });
  cap(s, "Precios y especificaciones verificados en noviembre de 2026. Los precios son orientativos y cambian a menudo.", MX, 6.3, CW, 0.4, 12);

  wrapUp(["Los SDR van de 35 a 2400 USD.", "El precio depende del ancho de banda, de poder transmitir y de la FPGA.", "El bladeRF 2.0 micro xA9 (860 USD) es dúplex, 2×2, 12 bits, 47 MHz–6 GHz y hasta 56 MHz.", "Para señales estrechas basta un RTL-SDR; la TDT, el WiFi o el LTE piden ancho de banda.", "Comprueba que tu bladeRF enumera como USB 3 (SuperSpeed); con USB 2 solo pasan ≈ 11 Msps."],
    ["# ¿USB 3 o USB 2? (solo consulta)", "bladeRF-cli -e info", "# debe decir: USB speed: SuperSpeed", "./build/bladerf-lab", "# y abre http://127.0.0.1:8090"],
    "Arranca el servidor con tu bladeRF y abre la pestaña Hardware: verás la sintonía, la tasa y el ancho de banda que admite tu equipo.");

  // ============================================================ MÓDULO 2
  section("Módulo 2. Arquitectura", 2);
  moduleStart(2, "Arquitectura: hilos, cerrojos y WebSocket",
    ["Por qué el hilo DSP no puede esperar nunca y cómo se consigue.", "Cómo funciona el marco de decodificadores (un hilo por instancia).", "Qué recorrido sigue un bloque de IQ hasta el navegador.", "Qué mensajes intercambian el servidor y la interfaz web."]);
  lesson({ title: "Un hilo DSP que nunca espera", imgs: [{ k: "arq", alt: "Hilos y flujo de datos" }], iw: 7.8, ih: 5.0, cap: "Hilos y flujo de datos, del bladeRF o fichero al navegador. Diagrama del proyecto.",
    bullets: [{ b: "Problema:", t: "la radio llega sin parar; si el procesado se retrasa, se pierden muestras." }, { b: "Solución:", t: "el hilo DSP solo reparte punteros; cada decodificador trabaja en su propio hilo." }, { b: "Si uno va lento,", t: "pierde bloques (drops) y los demás no se enteran." }],
    idea: "dec_host_feed() copia el bloque una vez y encola un puntero con contador de referencias.", side: "L", cs: 14,
    notes: "Flujo general: el bladeRF o un fichero alimentan un hilo lector que escribe en un anillo de bloques; el hilo DSP calcula el espectro, el detector, el grabador y los canales de audio, y reparte punteros al host. Si la cola de una instancia se llena (16 bloques, unos 0,3 s) se descarta el bloque para esa instancia y se cuenta en drops. Solo el hilo DSP llama a las funciones de recepción de libbladeRF. Fuente: docs/arquitectura/ARQUITECTURA.md." });
  lesson({ title: "El marco de decodificadores", imgs: [{ k: "marco", alt: "Marco de decodificadores" }], iw: 7.8, ih: 5.0, cap: "Descriptor, host, DDC, cola de bloques refcontados, eventos y estado. Diagrama del proyecto.",
    bullets: [{ b: "Descriptor:", t: "cada protocolo declara su id, su tipo y sus funciones." }, { b: "Narrow:", t: "el host le construye un DDC a su tasa exacta." }, { b: "Wide:", t: "recibe muestras crudas a la tasa del hardware." }, { b: "Salida:", t: "un evento JSON por mensaje válido, máximo 200/s." }],
    ojo: "Una instancia fuera de la banda capturada queda en «waiting»: no es un error.", side: "L", cs: 14,
    notes: "Los eventos se guardan en un anillo de 100 que se reenvía con replay a los clientes nuevos. El estado vivo (aviones, barcos, celdas) se pide a cada instancia aproximadamente una vez por segundo con state_json. Fuente: docs/arquitectura/ARQUITECTURA.md." });
  s = content("Recorrido de un bloque de IQ",
    "Un bloque nace en el lector, pasa por el anillo, el hilo DSP lo consume y se difunde a los clientes WebSocket y a las colas de los decodificadores. La hora de cada bloque es la de fin de lectura. Fuente: docs/arquitectura/ARQUITECTURA.md, figura 3.");
  img(s, "secBloque", { x: MX, y: 1.4, w: CW, h: 4.6 }, "Secuencia del recorrido de un bloque");
  cap(s, "Un bloque de IQ desde el lector hasta el navegador. Diagrama del proyecto.", MX, 6.05, CW, 0.3);
  callout(s, "idea", "Cada bloque se lee una vez y se reparte a todos los consumidores; nada espera a nadie.", { x: MX, y: 6.0, w: CW, h: 0.8 }, 15);
  s = content("Hilos y cerrojos",
    "El orden de cerrojos es siempre h->lock antes que inst->m: un hilo de instancia nunca pide h->lock teniendo inst->m. El estado de cada decodificador lo calcula el propio hilo de la instancia, así que el decodificador no necesita cerrojos internos: process se llama siempre desde el mismo hilo. Fuente: docs/arquitectura/ARQUITECTURA.md, figura 4.");
  img(s, "hilos", { x: MX, y: 1.4, w: CW, h: 4.5 }, "Hilos, cerrojos y orden de adquisición");
  callout(s, "ojo", "Orden de cerrojos: primero h->lock y después inst->m, nunca al revés; así no hay interbloqueos.", { x: MX, y: 5.95, w: CW, h: 0.85 }, 15);
  lesson({ title: "WebSocket: mensajes binarios 1 a 6", imgs: [{ k: "msgBin", alt: "Mensajes binarios del WebSocket" }], iw: 7.6, ih: 4.8, cap: "Mensajes binarios 1 a 6 del protocolo GET /ws. Diagrama del proyecto.",
    bullets: [{ b: "1:", t: "espectro (un byte por bin: dBFS = dbmin + v·dbstep)." }, { b: "2 y 3:", t: "audio PCM a 48 kHz y solo nivel." }, { b: "4:", t: "barrido de banda ancha." }, { b: "5 y 6:", t: "vídeo H.264 y audio de la TV." }],
    idea: "Los datos pesados van en binario; las órdenes y los eventos, en JSON legible.", side: "L", cs: 14,
    notes: "Todo es little-endian. Las órdenes del cliente llevan el campo cmd (config, channel, profile, dec_add, tx...) y las respuestas el campo type. Fuente: docs/arquitectura/WEBSOCKET.md." });
  lesson({ title: "WebSocket: qué ocurre al conectar", imgs: [{ k: "secWs", alt: "Secuencia del WebSocket" }], iw: 5.3, ih: 5.2, cap: "Secuencia de mensajes del WebSocket. Diagrama del proyecto.",
    steps: ["El navegador abre GET /ws.", "El servidor envía el estado: status, signals, decoders, profiles, dec_list…", "Reenvía los últimos 100 eventos (replay).", "Empieza el flujo binario de espectro y audio.", "El cliente manda órdenes JSON (config, channel, dec_add…)."], side: "L", size: 15,
    idea: "El servidor es quien manda el estado: la interfaz solo lo dibuja.",
    notes: "Al conectar el servidor envía status, signals, recording, decoders, profiles, dec_list, tx_policy y los últimos 100 dec marcados replay. Fuente: docs/arquitectura/WEBSOCKET.md y AGENTS.md §4.6." });
  wrapUp(["El hilo DSP nunca se bloquea: reparte punteros.", "Cada decodificador es un hilo con su propio DDC.", "Los cerrojos tienen un orden fijo.", "El WebSocket combina binario (datos) y JSON (control)."],
    ["# arranca con un perfil y mira los eventos", "./build/bladerf-lab --replay grabacion.sigmf-meta \\", "  --decode ais@161975000 --print", "", "# comprueba el marco con sus pruebas", "make test T=host"],
    "Arranca el servidor sin hardware con un fichero SigMF y mira los eventos JSON que emite cada instancia por la consola. Después ejecuta la prueba del host.");

  // ============================================================ MÓDULO 3
  section("Módulo 3. Interfaz web", 3);
  moduleStart(3, "La interfaz web",
    ["Qué pestañas tiene la interfaz y para qué sirve cada una.", "Cómo se elige un perfil y se crean decodificadores.", "Qué es real y qué son datos de demostración en las capturas.", "Cómo probarla sin antena."],
    "La interfaz está en web/ y la sirve el propio binario (HTML, CSS y JavaScript sin dependencias).");
  const web = [
    ["receptor", "Receptor (1/2): sintonía y espectro", "Pestaña Receptor (FM generada con --gen rds). Captura de la interfaz con recuadros numerados.", ["Frecuencia de escucha: escribe MHz o pulsa F. Clic en el espectro también sintoniza; I abre «¿Qué es esta señal?».", "Modo de demodulación: WFM, NFM, AM, USB, LSB, CW.", "Escuchar: activa el audio de este navegador (cada cliente tiene su canal).", "S-meter y banderas: SQL squelch, ST estéreo, 19k piloto, FUERA fuera de banda.", "Espectro y cascada. El rectángulo violeta es el filtro del canal (arrastra sus bordes); las marcas amarillas son señales detectadas: clic para sintonizar o «Decodificar como…»."], 0],
    ["receptor", "Receptor (2/2): zoom, estado y RDS", "Pestaña Receptor (FM generada con --gen rds). Captura de la interfaz con recuadros numerados.", ["Zoom, rango de dB, máximo retenido y botón Barrido.", "Barra de estado: fuente, tasa, carga del hilo DSP, temperatura, clientes y overruns.", "Pestañas (teclas 1 a 8) y Ctrl+K para buscar cualquier orden.", "Anchura del filtro, paso de sintonía, squelch y deénfasis.", "RDS: PS, texto de radio, tipo de programa, hora y frecuencias alternativas."], 5],
    ["fuera-de-banda", "Qué pasa si sintonizas fuera de banda", "Sintonizar fuera de la banda capturada.", ["Frecuencia pedida (92,2 MHz) fuera de 100 MHz ± 1,2 MHz.", "FUERA: el canal queda fuera de la banda; el audio calla.", "Para verla, mueve el centro (pestaña Hardware) o sube la tasa."]],
    ["hardware", "Hardware: centro, tasa y ganancia", "Pestaña Hardware.", ["Centro, tasa, ancho RF y modo de ganancia (también AGC rápido, lento e híbrido).", "Tamaño de FFT, imágenes por segundo y umbral del detector.", "Latitud y longitud: las usan el mapa y ADS-B.", "Grabar (tecla R) guarda un SigMF en recordings/.", "Placa, versiones de firmware y FPGA, límites."]],
    ["protocolos", "Protocolos: un clic y el perfil lo configura", "Pestaña Protocolos.", ["Perfiles: un clic fija centro, tasa, audio y decodificadores.", "Agrupados por familia; cada botón muestra centro y tasa."]],
    ["decodificadores", "Decodificadores: instancias y tablas", "Pestaña Decodificadores (AIS y APRS generados).", ["Protocolo, frecuencia y opciones JSON.", "Instancias en marcha: estado, carga y descartes; Oír, Mover, Quitar.", "Tablas que mantienen los decodificadores (barcos, estaciones…).", "Flujo de eventos con filtro, búsqueda y pausa."]],
    ["decodificar-como", "«Decodificar como…» sobre una señal", "Menú «Decodificar como…» sobre una señal detectada.", ["Menú de la señal: «Escuchar aquí» o «Decodificar como…». ★ marca los protocolos cuyo ancho se parece al de la señal.", "Las marcas amarillas son las señales que detecta el servidor."]],
    ["mapa", "Mapa: barcos y estaciones", "Pestaña Mapa (barcos AIS y estación APRS generados).", ["Capas: una por protocolo; actívalas o desactívalas.", "Mapa: cada objeto con lat y lon; clic para ver sus campos.", "Todos los campos del objeto elegido.", "Tablas ordenables por capa."]],
    ["mapa-demo", "Mapa con datos de demostración", "Mapa con datos de DEMOSTRACIÓN (tests/web_mock.js, no es radio real).", ["Capas de demostración: AIS, APRS, Meshtastic…", "Posiciones inventadas por el servidor de pruebas."]],
    ["mensajes", "Mensajes: el flujo de eventos", "Pestaña Mensajes.", ["Filtrar por protocolo.", "Buscar texto.", "Pausar el flujo para leer.", "Mensajes decodificados con su hora, protocolo y frecuencia."]],
    ["barrido", "Barrido de 47 MHz a 6 GHz", "Barra de barrido (la fuente es un fichero: no se puede barrer).", ["Botón Barrido.", "Inicio, fin, FFT por salto, Iniciar y Parar. Con la placa recorre 47 MHz a 6 GHz."]],
    ["emision", "Emisión: modos y política vigente", "Pestaña Emisión (--enable-tx; no se emite nada).", ["Política vigente: aire, indicativo, ganancia máxima y bandas.", "Protocolo y modo: fichero SigMF (seguro), bucle interno o aire.", "Parámetros del mensaje en JSON.", "Emitir: en modo fichero escribe un SigMF en recordings/.", "Reproducir una grabación en modo fichero o bucle.", "Calibración IQ/LO por bucle interno."]],
    ["tv", "TV: ver un canal en el navegador", "Pestaña TV (TS de prueba en bucle, sin radio).", ["Imagen: H.264 decodificado por el navegador (WebCodecs).", "Ver / Detener el servicio elegido.", "Canal 21–48, Sintonizar y Explorar (con un fichero .ts no aplican).", "Servicios del múltiplex con códec y resolución.", "Enganche, MER/SNR, BER antes de Viterbi y de Reed-Solomon, tasa del TS."]],
  ];
  web.forEach((w) => {
    s = content(w[1], "Captura anotada de la interfaz (docs/img/guia/captura-" + w[0] + ".png): la pantalla lleva recuadros numerados y la leyenda se pasa aquí a texto para que se lea. " + w[2] + (w[0] === "mapa-demo" ? " Datos de demostración generados por el servidor de pruebas, no radio real." : " Las capturas salen de servidores propios con señales generadas con --gen y un TS de prueba.") + " Fuente: docs/uso/GUIA.md.");
    const r = img(s, "cs_" + w[0], { x: MX, y: 1.4, w: 7.4, h: 5.0 }, w[2], "left");
    cap(s, w[2] + (w[0] === "mapa-demo" ? " Datos de DEMOSTRACIÓN, no es radio real." : ""), MX, r.y + r.h + 0.05, r.w, 0.5, 11);
    const tx = MX + r.w + 0.3, tw = CW - r.w - 0.3;
    steps(s, w[3], { x: tx, y: 1.45, w: tw, h: 5.3 }, 15, w[4] || 0);
  });
  s = content("Antes y después de las pruebas de la interfaz",
    "La interfaz pasó una batería de pruebas (make webtest, tests/web_e2e.mjs) que encontró fallos y fricciones; docs/calidad/QA_WEB.md los recoge. El primero fue que al conectar el canal quedaba en 100,3 MHz fijo aunque la emisora estuviera en 100,4 (o 99,2 en la antena real); ahora sintoniza la señal más fuerte detectada. Resultado final: 266 de 266 comprobaciones correctas, también tras una prueba de 10 minutos sin fugas de memoria. Capturas en docs/img/qa/.");
  img(s, "qaAntes", { x: MX, y: 1.4, w: 5.95, h: 3.7 }, "Receptor antes de las pruebas");
  img(s, "qaDespues", { x: MX + 6.18, y: 1.4, w: 5.95, h: 3.7 }, "Receptor después de las pruebas");
  cap(s, "Antes: el canal queda en 100,3 MHz entre emisoras, las pestañas van en dos filas y hay un hueco bajo la cascada.", MX, 5.15, 5.95, 0.6, 12);
  cap(s, "Después: canal sobre 100,4 MHz, una sola fila de pestañas y el espectro a toda la altura.", MX + 6.18, 5.15, 5.95, 0.6, 12);
  callout(s, "idea", "Las pruebas automáticas (make webtest) comprueban 266 puntos, desde la sintonía hasta la accesibilidad y las fugas de memoria.", { x: MX, y: 5.85, w: CW, h: 0.9 }, 14);
  wrapUp(["La interfaz la sirve el propio binario; no hay que instalar nada.", "Un perfil ajusta centro, tasa, audio y decodificadores.", "El mapa dibuja cualquier tabla con lat y lon.", "La pestaña Emisión solo existe con --enable-tx."],
    ["./build/bladerf-lab --replay grabacion.sigmf-meta", "# abre http://127.0.0.1:8090", "", "# o arranca con un perfil", "./build/bladerf-lab --profile maritimo --print"],
    "Arranca el servidor con un fichero y abre la dirección en el navegador (Chrome o Edge para la TV). Elige un perfil en la pestaña Protocolos.");

  // ============================================================ MÓDULO 4
  section("Módulo 4. Procesado de señal propio", 4);
  moduleStart(4, "Procesado de señal propio",
    ["Cómo se estructura un canal de audio (de IQ a sonido).", "Cómo se diseñan el diezmador, el filtro de canal y el remuestreador.", "Por qué se usa la ventana Blackman-Harris y cómo se calibra el espectro.", "Cuánto mejora la corrección con Viterbi."],
    "Ninguna biblioteca DSP de terceros. Las gráficas de esta sección no son esquemas: salen de pequeños programas en C que enlazan con src/dsp y vuelcan los coeficientes y medidas reales (docs/arquitectura/DSP.md).");
  const dsp = [
    ["Canal de audio: de IQ a sonido", "canal", [{ b: "NCO:", t: "lleva la emisora elegida a 0 Hz." }, { b: "Diezmado y filtro de canal.", t: "" }, { b: "Nivel y squelch:", t: "silencia si no hay señal." }, { b: "Demodulador:", t: "WFM estéreo, NFM, AM, USB, LSB o CW; luego remuestreo a 48 kHz." }], "Cada navegador tiene su propio canal: escucha una frecuencia distinta de la misma captura.", "Cadena de un canal de audio (src/dsp/channel.h). Diagrama del proyecto.",
      "Cada navegador tiene su propio canal: mezcla con un NCO, diezma, aplica el filtro de canal, mide el nivel con squelch, demodula y remuestrea a 48 kHz. Fuente: docs/arquitectura/DSP.md."],
    ["Diezmador FIR de Kaiser", "firdec", [{ b: "Calcula 1 de cada D salidas.", t: "" }, { b: "Kaiser:", t: "atenuación de 65 dB o más." }, { b: "Alias:", t: "lo que cae sobre la banda útil queda bajo −68 dB." }], "El coste depende de la tasa de salida, no de la de entrada.", "Etapa ÷8 de un DDC con canal de 200 kHz, a 2,4 Msps (209 taps) y a 61,44 Msps (73 taps). Medida con el código real.",
      "Diseño por ventana de Kaiser: beta = 0,1102 (A − 8,7) y N ≈ (A − 8) / (2,285 · 2π · Δf/fs). La línea de retardo está duplicada para que las últimas N muestras sean contiguas y el producto escalar se vectorice. Fuente: docs/arquitectura/DSP.md."],
    ["Filtro de canal complejo", "cfir", [{ b: "Paso banda de coeficientes complejos", t: "a 48 kHz." }, { b: "USB y LSB:", t: "dejan pasar un solo lado (200–2700 Hz)." }, { b: "AM y NFM:", t: "simétricos." }], "Un filtro complejo permite bandas laterales asimétricas sin demodular antes.", "Respuesta de los filtros de canal de channel.c a 48 kHz. Medida con el código real.",
      "Nota técnica: cfir_process guarda los coeficientes en orden inverso. Fuente: docs/arquitectura/DSP.md."],
    ["Remuestreador polifásico", "resamp", [{ b: "256 fases;", t: "error temporal ≤ 1/512 de muestra." }, { b: "Tasa exacta:", t: "error < 10⁻¹² a largo plazo." }, { b: "Versión compleja", t: "(cresamp) para el DDC." }], "Sirve para llegar a tasas que no son divisores enteros, como 48 000 S/s desde 52 083.", "Prototipo del banco de 256 fases: WFM 240 → 48 kHz y NFM 52 083 → 48 kHz. Medida con el código real.",
      "Para obtener y(τ) entre muestras se evalúa una suma ponderada con un paso bajo continuo precalculado en 256 fases; el avance es fs_in/fs_out acumulado en doble precisión. Fuente: docs/arquitectura/DSP.md."],
    ["Ventana Blackman-Harris", "bh", [{ b: "Lóbulos laterales", t: "a −93 dB." }, { b: "Comparación:", t: "rectangular −13 dB; Hann −31 dB." }, { b: "Coste:", t: "lóbulo principal más ancho (ENBW = 2,006 bins)." }], "Una señal débil junto a otra fuerte solo se ve si los lóbulos laterales son bajos.", "window_blackman_harris() con N = 1024 y su espectro. Medida con el código real.",
      "Es la ventana del espectro y de la medida de ancho ocupado en la emisión. Fuente: docs/arquitectura/DSP.md."],
    ["Calibración del espectro", "calib", [{ b: "Un tono de amplitud conocida", t: "frente al pico medido." }, { b: "Error máximo de la GPU:", t: "0,09 dB." }, { b: "Festoneo:", t: "0,8 dB si el tono cae entre dos bins." }], "Por debajo de −70 dBFS el tono se acerca a la cuantización de 12 bits.", "Tono complejo de amplitud conocida frente al pico medido por spectrum.c (ruta CPU). Medida con el código real.",
      "0 dBFS equivale a un tono complejo de amplitud 1,0; el espectro se difunde con dbmin = −145 y dbstep = 0,6. Fuente: docs/arquitectura/DSP.md y docs/arquitectura/WEBSOCKET.md."],
    ["BER con y sin Viterbi", "ber", [{ b: "Código K = 7", t: "(0171, 0133), BPSK sobre AWGN." }, { b: "4 dB de Eb/N0:", t: "blando ≈ 10⁻⁵, duro ≈ 5·10⁻³, sin codificar 1,3·10⁻²." }, { b: "Int8 blando", t: "≈ coma flotante." }], "El Viterbi propio es la base de DAB, DVB-T y LTE.", "Código K = 7 de fec.c, BPSK sobre AWGN. La curva 3/4 es el punzonado de DVB. Medida con el código real.",
      "El Viterbi propio de fec.c es la base de DAB, DVB-T y LTE. Fuente: docs/arquitectura/DSP.md."],
  ];
  dsp.forEach((d) => lesson({ title: d[0], imgs: [{ k: d[1], alt: d[0] }], iw: 7.4, ih: d[1] === "canal" ? 3.0 : 3.4, cap: d[4], bullets: d[2], idea: d[3], side: "L", notes: d[5], cs: 14, boxH: 1.3 }));
  wrapUp(["El canal de audio encadena NCO, filtros, demodulador y remuestreo.", "Los filtros FIR se diseñan con Kaiser.", "El remuestreo polifásico da tasas exactas.", "La ventana Blackman-Harris deja los lóbulos a −93 dB.", "Viterbi blando mejora varios órdenes de magnitud el BER."],
    ["make test T=dsp", "make test T=fec"],
    "Ejecuta las pruebas de DSP y de FEC (fichero tests/test_dsp.c y tests/test_fec.c): comprueban las respuestas de los filtros y el BER con y sin Viterbi.");

  // ============================================================ MÓDULO 5
  section("Módulo 5. Protocolos", 5);
  moduleStart(5, "Protocolos: de la señal al dato",
    ["Qué cubre el catálogo de 21 protocolos y en qué bandas vive cada uno.", "Cómo es la señal de cada protocolo y cómo se ve en la cascada.", "Cómo lo decodifica el proyecto, paso a paso, con su diagrama de cadena.", "Qué está validado con señales reales y qué solo con señales sintéticas."],
    "Solo se decodifica información de difusión o de sistema: nada de voz digital, identidades de abonados ni tráfico cifrado (excepción: la clave pública por defecto de Meshtastic). Cada protocolo tiene cuatro diapositivas: qué es y la señal; cómo se decodifica (la cadena); la estructura del mensaje; y qué se ve en la interfaz web.");
  s = content("Mapa de bandas: de 47 MHz a 6 GHz",
    "El catálogo cubre radiodifusión, aviación, marítimo, radioaficionados, buscapersonas, ISM y telemetría, telefonía y redes y televisión. Fuente: README.md y docs/uso/PROTOCOLOS.md.");
  img(s, "bandas", { x: MX, y: 1.4, w: CW, h: 4.6 }, "Mapa de bandas");
  cap(s, "Mapa de bandas con cada protocolo del catálogo. Diagrama del proyecto.", MX, 6.05, CW, 0.3);
  callout(s, "idea", "Cada banda tiene su ancho y su modulación: por eso el proyecto necesita DDC estrecho para unos y captura ancha para otros.", { x: MX, y: 6.0, w: CW, h: 0.8 }, 14);
  s = content("Matriz de validación de los 21 decodificadores",
    "Cuatro estados: en el aire, datos reales, sintético y pendiente. En el aire: RDS (Radio Pamplona) y LTE (4 celdas con MIB). Datos reales (12): AIS, ACARS, FLEX 1600/6400, POCSAG, APRS, ISM, RS41, DAB, GSM (SCH), WiFi (DSSS), DVB-T (TPS) y VDL2 (38/41 tramas). Sintéticos (5): ADS-B y DSC (sin aire), LoRa y Meshtastic (referencias) y TDT (TS). Pendientes: NR, DMR, GSM BCCH/SI3, WiFi OFDM, Meshtastic en el aire y FLEX 3200. Fuente: README.md y docs/calidad/VALIDACION.md.");
  img(s, "matriz", { x: MX, y: 1.4, w: CW, h: 4.6 }, "Matriz de validación");
  cap(s, "Estado de validación de los 21 decodificadores. Diagrama del proyecto.", MX, 6.05, CW, 0.3);
  callout(s, "ojo", "Pasar la prueba sintética no basta: una constante equivocada pasaría los tests y fallaría con tráfico real. Cada ficha indica su nivel de validación.", { x: MX, y: 6.0, w: CW, h: 0.8 }, 14);

  PROTOS.forEach((p) => {
    const t = PTXT[p.id], nm = p.t.split(":")[0];
    const datos = p.f.map((x) => (x.b + " " + x.t).trim()).join(" ");
    // (a) qué es y para qué sirve, con la figura de la señal grande
    s = content(nm + " · qué es y para qué sirve", p.n);
    img(s, "s_" + p.id, { x: MX, y: 1.45, w: 8.0, h: 3.4 }, "Figura de la señal de " + p.id, "left");
    cap(s, "Así es la señal. " + p.sigCap, MX, 4.9, 8.0, 0.5, 11);
    chip(s, p.chip, CAT[p.cat], { x: MX, y: 5.65, w: 8.0, h: 0.7 }, 15);
    s.addText(p.meta, { x: MX, y: 6.4, w: 8.0, h: 0.4, fontSize: 14, color: C.accent6, margin: 0, valign: "top", isTextBox: true });
    s.addText([
      { text: "Qué es", options: { bold: true, color: C.accent1, breakLine: true } }, { text: t[0], options: { breakLine: true, paraSpaceAfter: 14 } },
      { text: "Para qué sirve", options: { bold: true, color: C.accent1, breakLine: true } }, { text: t[1] },
    ], { x: 8.95, y: 1.5, w: 3.78, h: 5.2, fontSize: 16, color: C.text1, margin: 0, valign: "top", isTextBox: true, objectName: "ficha_texto" });
    // (b) cómo se decodifica, con la cadena grande
    s = content(nm + " · cómo se decodifica", "Diagrama de cadena dibujado a partir del código de src/ y de la sección «Cadena DSP» del documento del protocolo. " + datos);
    img(s, "c_" + p.id, { x: MX, y: 1.35, w: CW, h: 4.95 }, "Cadena de procesado de " + p.id);
    s.addText([{ text: "Cómo se decodifica: ", options: { bold: true, color: C.accent1 } }, { text: t[2] }], { x: MX, y: 6.35, w: CW, h: 0.6, fontSize: 15, color: C.text1, margin: 0, valign: "top", isTextBox: true });
    // (c) cómo es la trama (estructura del mensaje)
    s = content(nm + " · la trama del mensaje", datos);
    img(s, "e_" + p.id, { x: MX, y: 1.45, w: 8.0, h: 3.6 }, "Estructura del mensaje de " + p.id, "left");
    cap(s, p.estCap + " Diagrama del proyecto.", MX, 5.1, 8.0, 0.4, 11);
    s.addText("Datos clave", { x: 8.95, y: 1.5, w: 3.78, h: 0.4, fontSize: 16, bold: true, color: C.accent1, margin: 0, isTextBox: true });
    bullets(s, p.f, { x: 8.95, y: 2.0, w: 3.78, h: 4.8 }, 14, 8);
    // (d) qué se ve en la web
    s = content(nm + " · qué se ve en la web", "Captura de la interfaz con un servidor --replay propio mostrando " + p.id + ". " + t[3]);
    img(s, "w_" + p.id, { x: MX, y: 1.4, w: 8.0, h: 5.0 }, "Captura de la interfaz mostrando " + nm, "left");
    cap(s, "Captura de la interfaz con un servidor --replay propio mostrando " + nm + "." + (p.id === "tdt" ? " El vídeo es un .ts de prueba reproducido con --tv-ts (carta de ajuste sintética), no una emisión recibida por radio." : ""), MX, 6.45, 8.0, 0.5, 11);
    s.addText([{ text: "Qué se ve", options: { bold: true, color: C.accent1, breakLine: true } }, { text: t[3] }], { x: 8.95, y: 1.5, w: 3.78, h: 3.4, fontSize: 16, color: C.text1, margin: 0, valign: "top", isTextBox: true });
    chip(s, p.chip, CAT[p.cat], { x: 8.95, y: 5.2, w: 3.78, h: 1.0 }, 13);
    s.addText(p.meta, { x: 8.95, y: 6.3, w: 3.78, h: 0.55, fontSize: 13, color: C.accent6, margin: 0, valign: "top", isTextBox: true });
  });
  wrapUp(["21 protocolos, de 47 MHz a 6 GHz, cada uno con su decodificador y su modulador.", "Narrow = DDC a su tasa; wide = muestras crudas.", "Solo se emite un evento si pasa el CRC o la comprobación equivalente.", "Cada ficha muestra su nivel de validación: aire, datos reales, sintético o pendiente."],
    ["./build/bladerf-lab --gen aprs --gen-freq 144800000 \\", "  --gen-center 145000000 --gen-rate 1000000 --out aprs_prueba", "./build/bladerf-lab --replay aprs_prueba.sigmf-meta \\", "  --decode aprs@144800000 --print", "make test T=aprs"],
    "Cada protocolo tiene su modulador: genera una señal APRS, decodifícala sin antena y ejecuta su prueba. Cambia aprs por otro id del catálogo para probar los demás.");

  // ============================================================ MÓDULO 6
  section("Módulo 6. TDT", 6);
  moduleStart(6, "TDT: ver y oír la televisión",
    ["Qué pasos separan una señal DVB-T de un vídeo en el navegador.", "Qué es un paquete de transporte (TS) y qué tablas lo describen.", "Cómo se sincronizan el vídeo y el audio.", "Qué límites tiene hoy y por qué la antena actual no basta."],
    "Receptor DVB-T propio hasta el TS, demux, audio MP2 propio y vídeo H.264 decodificado por el navegador. Excepción a la regla de no usar terceros: el vídeo lo decodifica el navegador (WebCodecs). Ningún contenido cifrado se descifra.");
  lesson({ title: "De la antena al navegador", imgs: [{ k: "tdt", alt: "Cadena completa de la TDT" }], iw: 7.8, ih: 5.0, cap: "Receptor OFDM 8k, demux y PSI, vídeo H.264 por WebCodecs y audio MP2 propio. Diagrama del proyecto.", side: "L",
    steps: ["dvbt_rx: OFDM → paquetes TS de 188 bytes.", "Demux: PAT, PMT, SDT y NIT dicen qué canales hay.", "PES: se reensamblan vídeo y audio con su PTS.", "Audio MP2 decodificado en el servidor; vídeo H.264 al navegador."], size: 14,
    idea: "Todo es propio salvo la decodificación del vídeo H.264, que hace el navegador.",
    notes: "dvbt_rx recibe IQ a 64/7 MHz y entrega paquetes TS. ts.c sincroniza con 0x47 repetido cuatro veces y comprueba continuidad; psi.c interpreta PAT, PMT, SDT y NIT con el número de canal lógico; pes.c reensambla PES con PTS y DTS; tv.c gestiona el servicio activo y emite los binarios 5 y 6. El audio MP2 lo decodifica el propio servidor (error rms < 0,1 LSB frente a ffmpeg, usado solo como referencia). Fuente: docs/protocolos/tdt.md." });
  lesson({ title: "Sincronía A/V y límites", imgs: [{ k: "e_tdt", alt: "Paquete TS y pila de tablas" }], iw: 6.8, ih: 3.0, cap: "Paquete TS y pila de tablas. Diagrama del proyecto.", side: "L",
    bullets: [{ b: "El audio es el reloj;", t: "el vídeo espera a que su PTS ≤ reloj." }, { b: "Vídeo:", t: "solo H.264 (Chrome o Edge, en localhost o HTTPS)." }, { b: "Audio:", t: "solo MP2; AAC, AC-3 y E-AC-3 no suenan." }],
    idea: "Sin radio puedes ver el reproductor: --replay con --tv-ts usa un TS de prueba.", ojo: "Con la antena actual la SNR es ≈ −9 dB y no hay TS en el aire; validado con señal sintética y con IQ de gr-dtv.", cs: 14, boxH: 1.4,
    notes: "El worklet del navegador devuelve el PTS de la muestra que suena, corrigiendo la latencia de salida, y el vídeo se presenta cuando su PTS es menor o igual. Sin audio se usa un reloj propio con 120 ms de retardo. Fuente: docs/protocolos/tdt.md y docs/protocolos/dvbt.md." });
  wrapUp(["DVB-T entrega un flujo TS de paquetes de 188 bytes.", "PAT/PMT/SDT/NIT describen los canales.", "El audio MP2 es el reloj de sincronía; el vídeo H.264 lo decodifica el navegador.", "Con la antena actual no hay TS en el aire: se prueba con un TS de prueba."],
    ["./build/bladerf-lab --replay grabacion.cs16 \\", "  --tv-ts fichero.ts", "# abre la pestaña TV en http://127.0.0.1:8090"],
    "Reproduce un TS de prueba (cualquier .cs16 como fuente de IQ y un .ts) y abre la pestaña TV en un navegador con soporte H.264.");

  // ============================================================ MÓDULO 7
  section("Módulo 7. Emisión y seguridad", 7);
  moduleStart(7, "Emisión y su política de seguridad",
    ["Para qué sirve el modulador gemelo de cada decodificador.", "Qué diferencia hay entre los modos file, loopback y air.", "Qué bandas están permitidas y por qué.", "Cómo se mide el ancho ocupado."],
    "Los moduladores sirven para probar decodificadores sin antena, generar ficheros SigMF y, solo si la política lo permite, emitir. La emisión está desactivada por defecto.");
  lesson({ title: "Tres modos y una lista blanca", imgs: [{ k: "politica", alt: "Diagrama de decisión de la emisión" }], iw: 7.4, ih: 5.0, cap: "Decisión de emisión: file, loopback y air con lista blanca, ciclo de trabajo y air_ok. Diagrama del proyecto.", side: "L",
    bullets: [{ b: "file:", t: "siempre; guarda un SigMF." }, { b: "loopback:", t: "--enable-tx; bucle interno, nada sale por la antena." }, { b: "air:", t: "--enable-tx-air, banda permitida, ≤ 5 s y ganancia ≤ 0 dB." }],
    ojo: "Nunca se emite en aviación, marítimo, GNSS, celular, radiodifusión ni 406 MHz. loopback y air no se han probado contra el hardware.", cs: 14, boxH: 1.6,
    notes: "air requiere --enable-tx-air y cumplir toda la política: modulador con air_ok, banda de la lista blanca, ganancia, duración ≤ 5 s, ciclo de trabajo ≤ 1 % por hora en 868,0–868,6 MHz y registro en el log. Fuente: docs/arquitectura/TX.md y AGENTS.md §6." });
  s = content("Bandas permitidas en modo air",
    "La señal entera, incluida la anchura ocupada, y el centro del hardware (donde sale la fuga del oscilador local) deben caer en la misma banda. En bandas de radioaficionado hace falta indicativo con --callsign. Fuente: docs/arquitectura/TX.md.");
  table(s, [["Banda", "Condición"], ["ISM 433,050–434,790 MHz", "ninguna adicional"], ["SRD 868,000–868,600 MHz", "ciclo de trabajo ≤ 1 %/hora (36 s/h)"], ["SRD 869,400–869,650 MHz", "ninguna adicional"], ["Radioaficionado 144–146 MHz", "--callsign"], ["Radioaficionado 430–440 MHz", "--callsign"]], { x: MX, y: 1.5, w: 7.2 }, [3.7, 3.5], 14, 0.55);
  callout(s, "ojo", "air_ok = false siempre: adsb, ais, dsc, acars, vdl2, rs41, dab, rds, lte, nr, gsm, wifi, dvbt, dmr, flex y pocsag solo emiten en modo file o loopback.", { x: 8.1, y: 1.5, w: 4.63, h: 3.0 }, 14);
  bullets(s, [{ b: "Además:", t: "ganancia ≤ --tx-max-gain y ≤ 5 s por orden." }, { b: "Registro:", t: "cada emisión queda en el log." }, { b: "Nunca se reproduce una grabación por antena.", t: "" }], { x: MX, y: 5.0, w: CW, h: 1.8 }, 15);
  lesson({ title: "Ancho ocupado y revisión de seguridad", imgs: [{ k: "ancho", alt: "Ancho ocupado de tres señales" }], iw: 7.6, ih: 3.2, cap: "Medida de measure_occupancy() sobre la señal por omisión de los moduladores aprs, ism y lora (señales generadas).", side: "L",
    bullets: [{ b: "Método:", t: "periodograma de Welch con ventana Blackman-Harris." }, { b: "Ancho ocupado:", t: "los bordes que dejan el 0,01 % de la potencia a cada lado (99,98 %)." }, { b: "También se vigilan", t: "la imagen IQ especular y la fuga del oscilador local." }],
    idea: "La señal entera, con su ancho, debe caber en la banda permitida.", cs: 14, boxH: 1.3,
    notes: "La revisión T1 cerró huecos como señales cortas con ancho 0, medidas parciales, ciclo de trabajo que no persistía o conversiones fuera de rango. La calibración de TX con bucle interno RFIC_BIST no pasa por el LO de TX; la real exige cable TX a RX con atenuador y permiso. Fuente: docs/arquitectura/TX.md." });
  wrapUp(["Cada decodificador tiene su modulador gemelo.", "file siempre; loopback con --enable-tx; air con --enable-tx-air y política estricta.", "Lista blanca de bandas, ancho ocupado medido y ciclo de trabajo.", "Aviación, marítimo y radiodifusión nunca se emiten."],
    ["# modo file: solo escribe un SigMF", "./build/bladerf-lab --gen adsb --gen-freq 1090M \\", "  --gen-center 1090M --gen-rate 2M --out adsb_prueba", "./build/bladerf-lab --replay adsb_prueba.sigmf-meta \\", "  --decode adsb --print --duration 3"],
    "Genera una señal ADS-B en modo file y decodifícala. No radia nada: solo escribe un fichero y lo lee.");

  // ============================================================ MÓDULO 8
  section("Módulo 8. Rendimiento y CUDA", 8);
  moduleStart(8, "Rendimiento y CUDA",
    ["Qué piezas se aceleran en la GPU.", "Cuánto baja la CPU al pasar el DDC a CUDA.", "Qué sostiene el motor con ficheros y con el bladeRF real.", "Dónde está el límite a 122,88 Msps."],
    "Las cifras salen de docs/arquitectura/RENDIMIENTO.md y docs/arquitectura/CUDA.md. Máquina de medida: RTX 4080 Laptop, CUDA 13.2.");
  lesson({ title: "Tres piezas en la GPU", imgs: [{ k: "gpu", alt: "Piezas CUDA" }], iw: 7.4, ih: 5.0, cap: "DDC por lotes, PFB y espectro asíncrono con las cifras medidas. Diagrama del proyecto.", side: "L",
    bullets: [{ b: "DDC por lotes:", t: "todos los decodificadores narrow desde un solo bloque." }, { b: "PFB:", t: "M canales uniformes; verificado, aún sin uso." }, { b: "Espectro asíncrono", t: "a 122,88 Msps." }],
    idea: "La GPU es opcional: sin ella (make CUDA=0) todo funciona en CPU.", cs: 14,
    notes: "El DDC por lotes calcula N canales de frecuencia arbitraria a la vez desde un solo bloque IQ en la GPU; el canalizador polifásico PFB produce M canales uniformes; y el espectro asíncrono libera al hilo DSP. Fuente: docs/arquitectura/CUDA.md." });
  s = content("DDC en GPU: de 5,2 a 0,32 núcleos",
    "Con 16 instancias a 20 Msps, el DDC en CPU consume el 524 % de un núcleo (5,2 núcleos); en GPU, el 32 % (0,32). La línea base sin decodificadores es 0,10 núcleos y una sola instancia en CPU cuesta 0,38. El banco sostiene 739 Msps con 16 canales a 20 Msps (carga a tiempo real del 2,7 %). Fuente: docs/arquitectura/CUDA.md §1.");
  s.addChart(pres.charts.BAR, [{ name: "Núcleos de CPU", labels: ["Línea base", "1 instancia (CPU)", "16 instancias, DDC en GPU", "16 instancias, DDC en CPU"], values: [0.10, 0.38, 0.32, 5.2] }], {
    x: MX, y: 1.45, w: 5.6, h: 4.2, barDir: "bar", chartColors: [HEX.violet], ...chartBase({ title: "Núcleos de CPU del proceso", fmt: "0.00" }), catAxisOrientation: "maxMin", objectName: "grafica_nucleos" });
  img(s, "cudaDdc", { x: 6.4, y: 1.5, w: 6.33, h: 2.9 }, "DDC por lotes en GPU");
  bullets(s, [{ b: "20 Msps × 16 canales:", t: "739 Msps sostenidos (2,7 % de carga)." }, { b: "61,44 Msps × 40 canales:", t: "427 Msps (14,4 %)." }], { x: 6.4, y: 4.6, w: 6.33, h: 1.4 }, 15);
  callout(s, "idea", "Con 16 decodificadores a 20 Msps, la CPU pasa de 5,2 a 0,3 núcleos. (Gráfica nativa con la tabla de docs/arquitectura/CUDA.md; a la derecha, la figura del proyecto.)", { x: MX, y: 5.85, w: CW, h: 0.9 }, 14);
  s = content("Espectro asíncrono, correlación NR y PFB",
    "A 122,88 Msps el espectro asíncrono baja el tiempo medio por bloque de 1,43 ms a 0,62 ms (bloque de 8,5 ms). La correlación de NR en GPU pasa de 15–24 ms a 1,1 ms de CPU por ventana de 25 ms. El PFB con M = 1024 alcanza 1205 Msps de cálculo y 348 Msps incluyendo la bajada a la CPU. Fuente: docs/arquitectura/CUDA.md §2, §6 y §7.");
  img(s, "cudaNr", { x: MX, y: 1.4, w: CW, h: 4.6 }, "Espectro asíncrono, NR y PFB");
  cap(s, "Espectro asíncrono frente a síncrono, correlación de NR en CPU y GPU, y PFB M = 1024. Medidas del proyecto.", MX, 6.05, CW, 0.3);
  callout(s, "idea", "Mover el espectro y la correlación a la GPU libera al hilo DSP, que es el que no puede esperar.", { x: MX, y: 6.0, w: CW, h: 0.8 }, 14);
  s = content("Motor DSP con ficheros: antes y después",
    "Medido con ficheros, espectro en GPU, FFT 4096, 20 fps y un cliente NFM escuchando. Con 61,44 Msps y un cliente la tasa sostenida pasó de 51,7 a 61,4 Msps y la CPU de 0,98 a 0,39 núcleos. Con 122,88 Msps (cs8) y un cliente, de 52,7 a 122,9 Msps; con cuatro clientes, de 16,1 a 122,7. Los cambios: hilo lector con anillo, S-meter a partir del espectro para los clientes que no escuchan, conversión solo si hay consumidores, mezclador y diezmador vectorizados. Cifras de docs/arquitectura/RENDIMIENTO.md §3 (la máquina tenía carga ajena en parte de las medidas).");
  const cats = ["61,44 Msps · 1 cliente", "122,88 Msps · 1 cliente", "122,88 Msps · 4 clientes"];
  const co = (title, fmt) => ({ barDir: "col", barGrouping: "clustered", chartColors: [HEX.grey, HEX.violet], ...chartBase({ title, fmt, legend: true }) });
  s.addChart(pres.charts.BAR, [{ name: "Antes", labels: cats, values: [51.7, 52.7, 16.1] }, { name: "Después", labels: cats, values: [61.4, 122.9, 122.7] }], { x: MX, y: 1.45, w: 5.95, h: 4.3, ...co("Tasa sostenida (Msps)", "0.0"), objectName: "grafica_tasa" });
  s.addChart(pres.charts.BAR, [{ name: "Antes", labels: cats, values: [0.98, 1.01, 0.98] }, { name: "Después", labels: cats, values: [0.39, 0.83, 0.74] }], { x: MX + 6.18, y: 1.45, w: 5.95, h: 4.3, ...co("CPU del proceso (núcleos)", "0.00"), objectName: "grafica_cpu" });
  cap(s, "Gráficas nativas con las tablas de docs/arquitectura/RENDIMIENTO.md §3 (fuente de fichero; «antes» = motor anterior).", MX, 5.8, CW, 0.3, 12);
  callout(s, "idea", "Un hilo lector con anillo y un S-meter calculado desde el espectro permiten sostener 122,88 Msps con ficheros.", { x: MX, y: 6.15, w: CW, h: 0.7 }, 14);
  lesson({ title: "Con el bladeRF real: el límite es el USB", imgs: [{ k: "rendBlade", alt: "Rendimiento con el bladeRF real" }], iw: 7.6, ih: 3.4, cap: "Rendimiento con el bladeRF real, solo recepción. Medidas del proyecto; los overruns a 122,88 Msps se midieron con la máquina cargada.", side: "L",
    bullets: [{ b: "61,44 Msps:", t: "0 drops; 3 overruns de hardware en 25 s." }, { b: "122,88 Msps:", t: "0 drops del motor; 22 overruns en 25 s." }, { b: "Causa:", t: "246 MB/s por USB 3: lo pierde el FX3, no el motor." }],
    idea: "Overrun = muestras perdidas por el hardware o el USB; drop = bloques que el DSP no llegó a consumir.", cs: 14, boxH: 1.3,
    notes: "Con el servidor y la máquina libre: 61,44 Msps, 3 overruns de hardware, 0 drops, DSP 0,20; 122,88 Msps, 22 overruns, 0 drops, DSP 0,26. Un bucle vacío de source_read, sin procesado, muestra overruns del mismo orden: el DSP no los provoca. Fuente: docs/arquitectura/RENDIMIENTO.md §4." });
  wrapUp(["La GPU baja 16 decodificadores de 5,2 a 0,32 núcleos.", "El motor sostiene 122,88 Msps con ficheros sin descartes.", "Con el bladeRF a 122,88 Msps el límite es el USB 3, no el motor.", "overruns ≠ drops: uno es hardware, el otro, DSP."],
    ["make test T=gpu", "make test T=engine_perf", "make CUDA=0   # compila sin GPU"],
    "Ejecuta las pruebas de la GPU y del rendimiento del motor (necesitan CUDA y ficheros de prueba). Con make CUDA=0 compilas el binario sin GPU.");

  // ============================================================ MÓDULO 9
  section("Módulo 9. FPGA", 9);
  moduleStart(9, "FPGA propia: la imagen «lab»",
    ["Qué es una FPGA y qué hace la del bladeRF.", "Qué añade la imagen «lab»: un banco de registros y un motor de espectro.", "Cómo se lee la trama de espectro que sale de la FPGA.", "En qué estado está: temporización cerrada y probada en la placa, cargada solo en RAM."],
    "Estado (verificado el 2026-11-16): la imagen «lab» se compila de forma reproducible con Quartus 20.1.1 (dos compilaciones dan el mismo SHA-256), se simula con Questa, el modelo en C coincide al bit, la temporización se cumple en los 4 modelos y la imagen se ha probado en la placa cargada solo en RAM. Nunca se escribe la flash SPI ni el firmware del FX3. Fuente: docs/hardware/FIRMWARE.md.");
  lesson({ title: "Imagen «lab»: banco Wishbone y motor de espectro", imgs: [{ k: "fpgaLab", alt: "Imagen lab de la FPGA" }], iw: 7.4, ih: 5.0, cap: "Resumen de la imagen lab. Diagrama del proyecto.", side: "L",
    bullets: [{ b: "Una FPGA", t: "es un chip cuyo circuito se programa: aquí corre la cadena de datos del bladeRF." }, { b: "FPGA 0.16.101:", t: "imagen de serie + banco Wishbone + motor de espectro." }, { b: "HDL propio,", t: "sin IP con licencia; sin parchear libbladeRF." }],
    ojo: "Probada en la placa, cargada solo en RAM (volátil).", cs: 14, boxH: 1.0,
    notes: "El banco de registros Wishbone se accede con bladerf_wishbone_master_read/write. El motor de espectro usa ventana Blackman-Harris, FFT SDF radix-2^2 de 1024 a 8192 puntos, |X|² y acumulación de K tramas. Con rx_mux distinto de 3 la ruta de datos es la de serie. Fuente: docs/hardware/FIRMWARE.md §9." });
  s = content("Motor de espectro en la FPGA",
    "Cadena: ventana, FFT SDF de N = 1024 a 8192, módulo al cuadrado y acumulación de K tramas con ciclo de trabajo del 100 %. Sustituye al flujo del ADC cuando rx_mux = 3 y entrega sus tramas por el flujo de muestras normal. Fuente: docs/hardware/FIRMWARE.md §10.");
  img(s, "fpgaMotor", { x: MX, y: 1.4, w: CW, h: 4.5 }, "Motor de espectro");
  cap(s, "Motor de espectro (simplificado: no muestra los registros de segmentación). Diagrama del proyecto.", MX, 5.95, CW, 0.3);
  callout(s, "idea", "Ventana → FFT → |X|² → acumular K tramas: el mismo cálculo del espectro del módulo 0, hecho en hardware.", { x: MX, y: 6.0, w: CW, h: 0.8 }, 14);
  s = content("Registros y formato de trama",
    "Registros: ID 0x4C414231 («LAB1»), VERSION, FEATURES, SCRATCH, SPEC_LOG2N (10 a 13), SPEC_K, SPEC_CTRL, contadores de muestras, desbordes, FFT y espectros, STATUS y CLEAR. lab_regs responde siempre, también a direcciones sin mapear (0xBAD0BAD0), así que no puede colgar el Nios. Trama: palabras de 32 bits; w0 = 0x4C415031 («LAP1»), w1 su complemento, N y K, contador, índice de la primera muestra, ocho palabras de cabecera, y luego las potencias en orden fftshift. Fuente: docs/hardware/FIRMWARE.md §10.2 y §11.");
  img(s, "fpgaReg", { x: MX, y: 1.4, w: 5.95, h: 3.7 }, "Mapa de registros Wishbone");
  img(s, "fpgaTrama", { x: MX + 6.18, y: 1.4, w: 5.95, h: 3.7 }, "Formato de trama del motor de espectro");
  cap(s, "Mapa de registros Wishbone (índice de registro = dirección / 4). Diagrama del proyecto.", MX, 5.15, 5.95, 0.4, 11);
  cap(s, "Formato de la trama de salida: cabecera de 8 palabras y N potencias. Diagrama del proyecto.", MX + 6.18, 5.15, 5.95, 0.4, 11);
  callout(s, "idea", "El host sincroniza la trama por sus dos primeras palabras (LAP1 y su complemento) y la valida con N, K y la longitud.", { x: MX, y: 5.75, w: CW, h: 1.0 }, 14);
  s = content("Recursos y temporización de la imagen «lab»",
    "Compilación completa con Quartus 20.1.1 en 11 min 45 s: 9518 ALM, 512 M10K (42 %) y 42 bloques DSP (12 %), frente a 6768, 274 y 8 de la imagen de serie. La temporización se cumple en los 4 modelos: setup ≥ +0,255 ns y hold ≥ +0,034 ns, sin ningún margen negativo (la imagen de serie tiene un hold de +0,065 ns). La compilación es determinista: dos compilaciones dan el mismo SHA-256 (0cd2913c…). Datos verificados por el orquestador el 2026-11-16. Fuente: docs/hardware/FIRMWARE.md §12.");
  const rl = ["ALM", "M10K (bloques)", "Bloques DSP"];
  s.addChart(pres.charts.BAR, [{ name: "Serie (hosted)", labels: rl, values: [6768, 274, 8] }, { name: "Lab", labels: rl, values: [9518, 512, 42] }], {
    x: MX, y: 1.45, w: 6.4, h: 4.6, barDir: "col", barGrouping: "clustered", chartColors: [HEX.grey, HEX.violet], ...chartBase({ title: "Recursos usados (Cyclone V 5CEBA9)", fmt: "#,##0", legend: true }), objectName: "grafica_recursos" });
  bullets(s, [{ b: "Temporización cumplida", t: "en los 4 modelos: setup ≥ +0,255 ns y hold ≥ +0,034 ns, sin márgenes negativos (serie: hold +0,065 ns)." }, { b: "Compilación determinista:", t: "dos compilaciones dan el mismo SHA-256 (0cd2913c…)." }, { b: "Recursos:", t: "9518 ALM, 512 M10K y 42 DSP." }], { x: 7.3, y: 1.5, w: 5.43, h: 3.6 }, 15);
  callout(s, "idea", "Una compilación reproducible y sin márgenes negativos es la condición para fiarse de la imagen antes de cargarla en la placa.", { x: 7.3, y: 5.2, w: 5.43, h: 1.3 }, 14);
  cap(s, "Gráfica nativa con la tabla de docs/hardware/FIRMWARE.md §12.", MX, 6.15, 6.4, 0.3, 11);
  s = content("Probada en la placa (cargada solo en RAM)",
    "Resultados verificados por el orquestador el 2026-11-16 con la imagen lab cargada únicamente en la RAM de la FPGA. La recepción normal (rx_mux distinto de 3) es igual que con la imagen de serie: -26,8 frente a -26,0 dBFS. El motor de espectro ve las emisoras de FM reales en unos 99,2 MHz y 100,4 MHz (Radio Pamplona), con el suelo de ruido en unos -74 dBFS. El índice de muestra avanza exactamente K·N por trama y no se pierde ninguna trama hasta unos 3 millones de palabras por segundo. Fuente: docs/hardware/FIRMWARE.md.");
  const pr = [["−26,8 / −26,0", "dBFS", "Recepción normal con la imagen lab frente a la de serie: igual."], ["99,2 y 100,4", "MHz", "Emisoras de FM reales vistas por el motor de espectro (Radio Pamplona en 100,4)."], ["≈ −74", "dBFS", "Suelo de ruido del espectro de la FPGA."], ["K · N", "por trama", "El índice de muestra avanza exactamente K·N en cada trama."], ["0", "tramas perdidas", "Hasta unos 3 millones de palabras por segundo."]];
  pr.forEach((c, i) => {
    const x = MX + (i % 3) * 4.1, y = 1.5 + Math.floor(i / 3) * 2.75;
    card(s, { x, y, w: 3.93, h: 2.55 }, "prueba");
    s.addText(c[0], { x: x + 0.2, y: y + 0.12, w: 3.53, h: 0.8, fontSize: 30, bold: true, color: C.accent1, margin: 0, valign: "middle", fit: "shrink", isTextBox: true });
    s.addText(c[1], { x: x + 0.2, y: y + 0.9, w: 3.53, h: 0.35, fontSize: 14, bold: true, color: C.accent6, margin: 0, isTextBox: true });
    s.addText(c[2], { x: x + 0.2, y: y + 1.3, w: 3.53, h: 1.15, fontSize: 15, color: C.text1, margin: 0, valign: "top", isTextBox: true });
  });
  callout(s, "idea", "Con rx_mux distinto de 3 la ruta de datos es la de serie: la imagen lab puede usarse como imagen normal.", { x: MX + 8.2, y: 4.25, w: 3.93, h: 2.55 }, 15);
  lesson({ title: "Cómo se prueba una FPGA sin riesgo", side: "L", iw: -0.35,
    steps: [{ b: "Cargar solo en RAM:", t: "la imagen es volátil y se pierde al desconectar. Nunca se escribe la flash SPI ni el firmware del FX3." }, { b: "Detección segura:", t: "antes de tocar registros se lee una marca de solo lectura (bits 27:24 de config_gpio = 0xA) para saber que es la imagen lab." }, { b: "Vuelta atrás:", t: "si algo sale mal, se desenchufa la placa y vuelve a arrancar con su imagen de serie." }, { b: "Registros que no cuelgan:", t: "lab_regs responde siempre, también a direcciones sin mapear (0xBAD0BAD0)." }],
    idea: "Cargar en RAM es como probar un programa sin instalarlo: al reiniciar todo vuelve a como estaba.", size: 16, boxH: 1.2,
    notes: "Procedimiento seguro de prueba: nunca flash SPI, nunca firmware del FX3; la carga solo la hace el orquestador con permiso explícito del usuario. Fuente: docs/hardware/FIRMWARE.md §7, §11.1 y §15." });
  wrapUp(["La FPGA ejecuta la cadena de datos del bladeRF.", "La imagen «lab» añade registros Wishbone y un motor de espectro.", "La trama de espectro lleva una cabecera de 8 palabras y N potencias.", "Estado: simulada, compilada y probada en la placa (solo RAM)."],
    ["# compilar y simular (necesita Quartus 20.1 y Questa)", "source firmware/env.sh", "firmware/build_lab.sh", "firmware/sim/run_lab_spec.sh", "# NO cargues nada en la placa sin permiso explícito"],
    "Puedes reproducir la compilación y la simulación en tu equipo. Cargar la imagen en la RAM de la FPGA solo se hace con permiso explícito del usuario; nunca se escribe la flash SPI.");

  // ============================================================ MÓDULO 10
  section("Módulo 10. Validación y estado", 10);
  moduleStart(10, "Validación: qué está probado y cómo",
    ["Por qué una prueba sintética no basta.", "Qué se contrastó con grabaciones reales y con herramientas de referencia.", "Qué decodificadores siguen pendientes.", "Cuál es el estado del proyecto y los próximos pasos."],
    "Una constante equivocada (tabla, polaridad, CRC, orden de bits) pasa los tests contra el propio modulador y falla con tráfico real. Por eso se contrastó con grabaciones públicas y con decodificadores de referencia (acarsdec, dumpvdl2, multimon-ng, redsea, dump1090-fa, direwolf, rtl_433, gr-dtv) usados como cajas negras. Fuente: docs/calidad/VALIDACION.md.");
  lesson({ title: "Grupo A: avión, mar, aficionados y buscas", imgs: [{ k: "valA", alt: "Evidencia del grupo A" }], iw: 7.4, ih: 4.2, cap: "Lo que se contrastó con grabaciones públicas y decodificadores de referencia. Figura del proyecto.", side: "L",
    bullets: [{ b: "Igual que la referencia:", t: "ACARS, FLEX 1600/6400, POCSAG, APRS y VDL2 (38/41)." }, { b: "Corregidos con datos reales:", t: "AIS (orden de bits), ACARS (diferencial) y VDL2." }, { b: "Pendiente:", t: "FLEX 3200; ADS-B y DSC con aire." }],
    idea: "RDS real: Radio Pamplona, 100,4 MHz, PI E239 y PS «SER».", cs: 14, boxH: 1.1,
    notes: "AIS tipo 1 con MMSI 367003560 tras corregir el orden de bits; ACARS 11/11 igual que acarsdec tras descubrir la codificación diferencial; FLEX 1600 con 20 mensajes y 6400/4 con 24 iguales a multimon-ng; POCSAG 3/3; APRS idéntico a multimon-ng y Dire Wolf; ADS-B igual que dump1090-fa sobre señal generada; VDL2 38 de 41 tramas de dumpvdl2; DSC validado con la norma. Fuente: docs/calidad/VALIDACION.md." });
  lesson({ title: "Grupo B: ISM, LoRa, celular, WiFi y DAB", imgs: [{ k: "valB", alt: "Evidencia del grupo B" }], iw: 7.4, ih: 4.2, cap: "Grupo B: decodificados frente a referencia donde hay recuento. Figura del proyecto.", side: "L",
    bullets: [{ b: "Igual que la referencia:", t: "ISM (rtl_433), RS41, LoRa y DAB." }, { b: "Con aire o grabación:", t: "LTE (PCI y MIB), GSM (SCH) y WiFi (DSSS)." }, { b: "Sin validar:", t: "NR, DMR, WiFi OFDM y descifrado de Meshtastic." }],
    idea: "LTE en Pamplona: 4 celdas con MIB.", cs: 14, boxH: 1.1,
    notes: "ISM igual que rtl_433 en todos los modelos reales; RS41 con 38/38 tramas por grabación; LoRa contrastado con loratx/lorarx; DAB con ensemble 5001 y 36/36 FIB; LTE con PCI 65 y 44 y MIB; GSM con 211/211 SCH y BSIC 23 pero sin BCCH/SI3; WiFi con 8 BSSID DSSS. Fuente: docs/calidad/VALIDACION.md." });
  s = content("Estado actual: lo hecho y lo que falta",
    "Verificado: captura de ficheros, espectro CPU y GPU, audio por cliente, detector, grabación SigMF, servidor web, marco de decodificadores, DDC en GPU, 22 perfiles y los 21 decodificadores con prueba sintética (make check). Validados con el aire: RDS y LTE en Pamplona. Pendientes: NR, DMR, GSM BCCH/SI3, WiFi OFDM, Meshtastic en el aire y FLEX 3200. Hardware sin probar: loopback, air y 122,88 Msps; la imagen FPGA lab se probó en la placa (solo RAM) y su espectro es fuente alternativa en placa. TDT: con la antena actual no hay TS. Fuente: README.md y docs/calidad/VALIDACION.md.");
  const cols = [
    ["Verificado", HEX.teal, ["Espectro CPU/GPU, audio por cliente y detector OS-CFAR sin bordes de banda", "Servidor web, grabación SigMF con etiquetas en tres niveles y 22 perfiles", "21 decodificadores con prueba sintética de extremo a extremo", "DDC en GPU: de 5,2 a 0,32 núcleos", "Imagen FPGA lab y espectro en la placa (solo RAM)"]],
    ["Con señal real", HEX.blue, ["En el aire: RDS y LTE (Pamplona)", "Datos reales: AIS, ACARS, FLEX 1600/6400, POCSAG, APRS, ISM, RS41, DAB, GSM (SCH), WiFi (DSSS), DVB-T y VDL2 (38/41)", "Sintéticos: ADS-B, DSC, LoRa, Meshtastic y TDT"]],
    ["Pendiente", HEX.amber, ["NR, DMR, GSM BCCH/SI3, WiFi OFDM, Meshtastic en el aire, FLEX 3200", "Hardware: loopback, air y 122,88 Msps", "TDT: SNR de ≈ −9 dB con la antena actual"]],
  ];
  cols.forEach((c, i) => {
    const x = MX + i * 4.1;
    card(s, { x, y: 1.5, w: 3.93, h: 5.25 }, "columna");
    chip(s, c[0], c[1], { x: x + 0.2, y: 1.7, w: 3.53, h: 0.55 }, 16);
    bullets(s, c[2], { x: x + 0.25, y: 2.5, w: 3.45, h: 4.1 }, 14, 8);
  });
  s = content("Próximos pasos",
    "Pendientes recogidos en AGENTS.md §8b y docs/calidad/VALIDACION.md. Se citan tal como figuran en el plan; algunos pueden haberse resuelto desde entonces. Para la calibración real del TX hace falta cable de TX a RX con atenuador de al menos 30 dB y permiso del usuario.");
  const stp = [["1", "Validar con señales reales", "grabaciones públicas y capturas con el bladeRF; en especial ADS-B, DSC, NR, DMR, GSM BCCH/SI3 y WiFi OFDM"],
    ["2", "Probar en la placa", "modo de 122,88 Msps, loopback y air"],
    ["3", "Espectro a 122,88 Msps", "usar la GPU o el motor de espectro de la FPGA e integrarlo en el motor"],
    ["4", "Calibración real del TX", "cable TX → RX con atenuador ≥ 30 dB y permiso del usuario"],
    ["5", "Cabos sueltos de integración", "perfiles auto, --no-gpu en el host, documentar mensajes nuevos del protocolo y AIS PLVA"]];
  stp.forEach((t, i) => {
    const y = 1.5 + i * 1.03;
    card(s, { x: MX, y, w: CW, h: 0.92 }, "paso");
    s.addShape(pres.ShapeType.ellipse, { x: MX + 0.2, y: y + 0.16, w: 0.6, h: 0.6, fill: { color: C.accent1 }, line: { color: C.accent1 }, objectName: "numero" });
    s.addText(t[0], { x: MX + 0.2, y: y + 0.16, w: 0.6, h: 0.6, fontSize: 18, bold: true, color: C.background1, align: "center", valign: "middle", margin: 0, isTextBox: true });
    s.addText([{ text: t[1] + ": ", options: { bold: true, color: C.accent1 } }, { text: t[2] }], { x: MX + 1.05, y: y + 0.05, w: CW - 1.3, h: 0.82, fontSize: 16, color: C.text1, margin: 0, valign: "middle", isTextBox: true });
  });
  wrapUp(["Una prueba sintética no basta: hay que contrastar con aire y con referencias.", "En el aire: RDS y LTE. Con datos reales: doce más.", "Pendientes: NR, DMR, GSM BCCH/SI3, WiFi OFDM, Meshtastic y FLEX 3200.", "Hardware aún sin probar: loopback, air y 122,88 Msps (la FPGA lab sí se probó, en RAM)."],
    ["make check   # -Werror + todas las pruebas", "make test T=rds", "make SAN=1 test   # con sanitizers"],
    "Ejecuta todas las pruebas del proyecto con compilación estricta, o solo la de un protocolo. Así comprobarás con tus propios ojos qué pasa y qué no.");

  // ============================================================ ANEXOS
  section("Anexos", "A");
  const gl = [
    ["IQ", "Par de números (I, Q) por muestra: una muestra compleja de la señal."], ["dBFS", "Decibelios respecto al fondo de escala; 0 dBFS = tono complejo de amplitud 1,0."],
    ["SNR", "Relación señal/ruido: cuánto más fuerte es la señal que el ruido (en dB)."], ["FFT", "Algoritmo rápido que pasa de tiempo a frecuencia."],
    ["Bin", "Cada «cajón» de frecuencia de una FFT; ancho = fs / N."], ["DDC", "Digital Down Converter: baja un canal a banda base (NCO, filtro, diezmado)."],
    ["CFO", "Carrier Frequency Offset: desvío entre la frecuencia emisor y receptor."],
    ["Símbolo", "Lo que se transmite de una vez; lleva uno o varios bits."], ["Baudio", "Símbolos por segundo."], ["OFDM", "Muchas portadoras lentas ortogonales en paralelo (DAB, DVB-T, WiFi, LTE)."],
    ["CSS", "Chirp Spread Spectrum, la modulación de LoRa."], ["CRC", "Comprobación cíclica: detecta errores en un mensaje."], ["FEC", "Corrección de errores hacia delante, con redundancia añadida."],
    ["Viterbi", "Decodificador de códigos convolucionales por camino más probable."], ["Reed-Solomon", "Código corrector que trabaja con bytes (RS(204,188) en DVB-T)."],
    ["PCI", "Physical Cell ID: identificador físico de una celda LTE o 5G."], ["MMSI", "Identificador de 9 cifras de un barco (AIS, DSC)."],
    ["SigMF", "Formato abierto de grabación IQ: datos más un fichero de metadatos."], ["PTS", "Marca de tiempo de presentación del vídeo y el audio (reloj de 90 kHz)."],
    ["TS", "Transport Stream de MPEG-2: paquetes de 188 bytes de la TDT."], ["Overrun / drop", "Muestras perdidas por el hardware o USB / bloques que el DSP no llegó a consumir."]];
  for (let g = 0; g < 3; g++) {
    const part = gl.slice(g * 7, g * 7 + 7);
    s = content(`Glosario (${g + 1} de 3)`, "Glosario de términos usados en el curso. Para más detalle, consulta el módulo correspondiente y los documentos de docs/.");
    table(s, [["Término", "Significado"], ...part], { x: MX, y: 1.5, w: CW }, [2.6, 9.53], 15, 0.62);
  }
  s = content("Mapa de lectura: qué leer después",
    "Documentos de docs/ para profundizar en cada tema. AGENTS.md recoge los contratos entre módulos y el plan de trabajo.");
  table(s, [["Si quieres saber más sobre…", "Lee…"],
    ["Cómo usar el programa, paso a paso", "docs/uso/GUIA.md"], ["Hilos y decodificadores; protocolo WebSocket", "docs/arquitectura/ARQUITECTURA.md y docs/arquitectura/WEBSOCKET.md"],
    ["Filtros, DDC y canal de audio", "docs/arquitectura/DSP.md"], ["Cada protocolo", "docs/uso/PROTOCOLOS.md y docs/protocolos/<id>.md (ADS-B: docs/protocolos/adsb.md)"],
    ["TDT y DVB-T", "docs/protocolos/tdt.md y docs/protocolos/dvbt.md"], ["Emisión y seguridad", "docs/arquitectura/TX.md y AGENTS.md §6"],
    ["Rendimiento y GPU", "docs/arquitectura/RENDIMIENTO.md y docs/arquitectura/CUDA.md"], ["Hardware y FPGA", "docs/hardware/HARDWARE.md y docs/hardware/FIRMWARE.md"],
    ["Validación y papers aplicados", "docs/calidad/VALIDACION.md y docs/investigacion/INVESTIGACION.md"]], { x: MX, y: 1.45, w: CW }, [5.0, 7.13], 14, 0.5);

  await pres.writeFile({ fileName: OUT });
  if (applyTheme) await applyTheme(OUT, THEME);
  else console.warn("AVISO: PPTX_TEMA_JS no está definida; el pptx se ha escrito sin aplicar el tema.");
  console.log("OK", OUT);
})().catch((e) => { console.error(e); process.exit(1); });
