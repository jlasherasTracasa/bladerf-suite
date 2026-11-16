#!/usr/bin/env bash
# render.sh — convierte cada docs/img/src/*.html (SVG en línea) en dos PNG
# (<nombre>-light.png y <nombre>-dark.png) con Chromium sin cabeza a 2x.
#
# Uso:  docs/img/render.sh [nombre ...]      (sin argumentos: todos)
#       (antes, si has tocado los diagramas: python3 docs/img/src/gen.py)
#
# El tamaño del lienzo sale de <svg width= height=>. Límite: < 400 KB por PNG:
# si se pasa, se cuantiza a paleta (convert -colors N) sin perder legibilidad.
set -euo pipefail
cd "$(dirname "$0")"
# Chromium sin cabeza. El binario completo (chrome-linux64) se queda esperando a
# la red en este equipo; el "headless shell" de la misma versión no.
CHROME="${CHROME:-$(ls "$HOME"/.cache/ms-playwright/chromium_headless_shell-1234/*/chrome-headless-shell 2>/dev/null | head -1)}"
[ -x "$CHROME" ] || CHROME="$HOME/.cache/ms-playwright/chromium-1234/chrome-linux64/chrome"
LIMIT=$((400*1024))
names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  for f in src/*.html; do names+=("$(basename "$f" .html)"); done
fi
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
for n in "${names[@]}"; do
  f="src/$n.html"
  W=$(grep -o '<svg[^>]*' "$f" | head -1 | sed -n 's/.* width="\([0-9]*\)".*/\1/p')
  H=$(grep -o '<svg[^>]*' "$f" | head -1 | sed -n 's/.* height="\([0-9]*\)".*/\1/p')
  for theme in light dark; do
    q=""; [ "$theme" = dark ] && q="?dark"
    out="$n-$theme.png"
    timeout 60 "$CHROME" --no-sandbox --disable-gpu --hide-scrollbars \
      --force-device-scale-factor=2 --window-size="$W,$H" \
      --screenshot="$tmp/raw.png" "file://$PWD/$f$q" >/dev/null 2>&1
    convert "$tmp/raw.png" -crop "$((W*2))x$((H*2))+0+0" +repage -strip -define png:compression-level=9 "$out"
    sz=$(stat -c %s "$out")
    for c in 256 160 112 80; do
      [ "$sz" -le "$LIMIT" ] && break
      convert "$tmp/raw.png" -crop "$((W*2))x$((H*2))+0+0" +repage -strip +dither -colors "$c" \
        -define png:compression-level=9 "PNG8:$out"
      sz=$(stat -c %s "$out")
    done
    printf '%-28s %5dx%-5d %7d B\n' "$out" "$((W*2))" "$((H*2))" "$sz"
  done
done
