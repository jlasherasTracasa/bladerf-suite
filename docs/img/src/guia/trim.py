#!/usr/bin/env python3
"""Recorta el fondo sobrante bajo la leyenda de docs/img/guia/captura-*.png."""
import glob, os
from PIL import Image
for p in sorted(glob.glob("docs/img/guia/captura-*.png")):
    im = Image.open(p).convert("RGB")
    w, h = im.size
    y = h - 1
    while y > 0 and all(im.getpixel((x, y)) == (20, 18, 24) for x in range(2, 60, 6)):
        y -= 1
    im.crop((0, 0, w, min(h, y + 1))).save(p, optimize=True)
    if os.path.getsize(p) > 380 * 1024:
        os.system(f"convert '{p}' +dither -colors 160 -define png:compression-level=9 'PNG8:{p}'")
    print(p, Image.open(p).size, os.path.getsize(p) // 1024, "KB")
