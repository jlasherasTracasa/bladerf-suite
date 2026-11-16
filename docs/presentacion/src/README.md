# Presentación-curso de BladeRF Lab

Generador reproducible de `docs/presentacion/BladeRF_Lab.pptx` y `BladeRF_Lab.pdf`.
Usa las imágenes de `docs/img/` (incluidas `screens/`, `guia/` y `qa/`): al cambiar una captura,
basta con volver a generar.

## Requisitos

- Node.js, `pptxgenjs` y `sharp` en `build/ppt1/node_modules`.
- Python 3 y LibreOffice (`soffice`) y `pdftoppm` para el PDF y la revisión visual.
- El tema se aplica con un `apply_theme.js` externo indicado en la variable de entorno `PPTX_TEMA_JS`.
  Si no está definida, el generador escribe el pptx sin aplicar el tema y avisa por consola.
- Opcional para validar y convertir: `validate.py` y `soffice.py` de la herramienta de PPTX.

## Instalar las bibliotecas

```sh
mkdir -p build/ppt1 && cd build/ppt1
npm init -y && npm install pptxgenjs sharp
cd ../..
```

## Regenerar el .pptx

```sh
PPTX_TEMA_JS=<ruta a apply_theme.js> NODE_PATH=build/ppt1/node_modules node docs/presentacion/src/generar_presentacion.js
```

Las imágenes reducidas (≤ 1600 px) quedan en `build/ppt1/img/`.

## Regenerar el PDF y revisar

```sh
python3 <skill>/scripts/office/validate.py docs/presentacion/BladeRF_Lab.pptx
cd build/ppt1 && mkdir -p render && cp ../../docs/presentacion/BladeRF_Lab.pptx render/ && cd render
python3 <skill>/scripts/office/soffice.py --headless --convert-to pdf BladeRF_Lab.pptx
cp BladeRF_Lab.pdf ../../../docs/presentacion/
pdftoppm -jpeg -r 50 BladeRF_Lab.pdf s    # miniaturas para la revisión visual
```
