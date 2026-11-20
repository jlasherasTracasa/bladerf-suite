#!/usr/bin/env bash
# render_decoders.sh — convierte docs/img/src/decoders/*.html en PNG claro/oscuro
# (docs/img/decoders/<nombre>-{light,dark}.png), mismo método que render.sh (2x, < 400 KB).
# Uso: docs/img/src/render_decoders.sh [nombre ...]
set -euo pipefail
cd "$(dirname "$0")/.."
CHROME="${CHROME:-$(ls "$HOME"/.cache/ms-playwright/chromium_headless_shell-1234/*/chrome-headless-shell 2>/dev/null | head -1)}"
LIMIT=$((400*1024))
names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  for f in src/decoders/*.html; do names+=("$(basename "$f" .html)"); done
fi
mkdir -p decoders
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
for n in "${names[@]}"; do
  f="src/decoders/$n.html"
  W=$(grep -o '<svg[^>]*' "$f" | head -1 | sed -n 's/.* width="\([0-9]*\)".*/\1/p')
  H=$(grep -o '<svg[^>]*' "$f" | head -1 | sed -n 's/.* height="\([0-9]*\)".*/\1/p')
  for theme in light dark; do
    q=""; [ "$theme" = dark ] && q="?dark"
    out="decoders/$n-$theme.png"
    timeout 60 "$CHROME" --no-sandbox --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
      --window-size="$W,$H" --screenshot="$tmp/raw.png" "file://$PWD/$f$q" >/dev/null 2>&1
    convert "$tmp/raw.png" -crop "$((W*2))x$((H*2))+0+0" +repage -strip -define png:compression-level=9 "$out"
    sz=$(stat -c %s "$out")
    for c in 256 160 112 80; do
      [ "$sz" -le "$LIMIT" ] && break
      convert "$tmp/raw.png" -crop "$((W*2))x$((H*2))+0+0" +repage -strip +dither -colors "$c" -define png:compression-level=9 "PNG8:$out"
      sz=$(stat -c %s "$out")
    done
    printf '%-34s %5dx%-5d %7d B\n' "$out" "$((W*2))" "$((H*2))" "$sz"
  done
done
