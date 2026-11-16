#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL="http://localhost:8073/"

PLUGIN="/usr/lib/python3/dist-packages/htdocs/plugins/receiver/material/material.js"

if ! command -v openwebrx >/dev/null 2>&1 \
  || ! grep -q '"bladerf"' /var/lib/openwebrx/settings.json 2>/dev/null \
  || [ ! -f "$PLUGIN" ]; then
  "$APP_DIR/install.sh"
fi

if ! systemctl is-active --quiet openwebrx; then
  if [ "$(id -u)" -eq 0 ]; then
    systemctl start openwebrx
  else
    sudo systemctl start openwebrx
  fi
fi

for _ in $(seq 1 40); do
  if curl -sf "${URL}status.json" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

echo "BladeRF Lab: ${URL}#protocols"
if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "${URL}#protocols" >/dev/null 2>&1 &
fi
