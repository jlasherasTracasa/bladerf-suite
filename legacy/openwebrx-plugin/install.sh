#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SETTINGS_SRC="$APP_DIR/settings.bladerf.json"
SETTINGS_DST="/var/lib/openwebrx/settings.json"
HTDOCS="/usr/lib/python3/dist-packages/htdocs"
PLUGIN_DST="$HTDOCS/plugins/receiver"

if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi

echo "[1/5] Repositorio OpenWebRX+"
if [ ! -f /etc/apt/sources.list.d/openwebrx-plus.list ]; then
  curl -s https://luarvique.github.io/ppa/openwebrx-plus.gpg | $SUDO gpg --yes --dearmor -o /etc/apt/trusted.gpg.d/openwebrx-plus.gpg
  echo "deb [signed-by=/etc/apt/trusted.gpg.d/openwebrx-plus.gpg] https://luarvique.github.io/ppa/noble ./" | $SUDO tee /etc/apt/sources.list.d/openwebrx-plus.list >/dev/null
  $SUDO apt-get update
fi

echo "[2/5] Paquete OpenWebRX+"
if ! dpkg -s openwebrx >/dev/null 2>&1; then
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y openwebrx
fi

echo "[3/5] Configuracion bladeRF"
TMP_SETTINGS="$(mktemp)"
python3 - "$SETTINGS_SRC" "$SETTINGS_DST" >"$TMP_SETTINGS" <<'PY'
import json
import sys

src = json.load(open(sys.argv[1], encoding="utf-8"))
try:
    dst = json.load(open(sys.argv[2], encoding="utf-8"))
except Exception:
    dst = {}

dst["version"] = max(int(dst.get("version", 8)), 8)
dst["receiver_name"] = src.get("receiver_name", "BladeRF Lab")
dst["receiver_location"] = src.get("receiver_location", "Local")
dst["receiver_admin"] = src.get("receiver_admin", "local@example.com")
dst.setdefault("sdrs", {})
dst["sdrs"]["bladerf"] = src["sdrs"]["bladerf"]

print(json.dumps(dst, indent=2, ensure_ascii=False))
PY
$SUDO cp "$TMP_SETTINGS" "$SETTINGS_DST"
$SUDO chown openwebrx:openwebrx "$SETTINGS_DST"
$SUDO chmod 644 "$SETTINGS_DST"
rm -f "$TMP_SETTINGS"

echo "[4/5] Interfaz Material"
$SUDO mkdir -p "$PLUGIN_DST"
$SUDO rm -rf "$PLUGIN_DST/material"
$SUDO cp -r "$APP_DIR/plugins/receiver/material" "$PLUGIN_DST/material"
$SUDO cp "$APP_DIR/plugins/receiver/init.js" "$PLUGIN_DST/init.js"
$SUDO chmod -R a+rX "$PLUGIN_DST/material" "$PLUGIN_DST/init.js"

echo "[5/5] Servicio"
$SUDO systemctl enable openwebrx >/dev/null 2>&1 || true
$SUDO systemctl restart openwebrx

for _ in $(seq 1 40); do
  if curl -sf http://localhost:8073/status.json >/dev/null 2>&1; then
    echo "Listo: http://localhost:8073/#protocols"
    exit 0
  fi
  sleep 0.5
done

echo "OpenWebRX+ no respondio. Revisa: sudo journalctl -u openwebrx -n 80"
exit 1
