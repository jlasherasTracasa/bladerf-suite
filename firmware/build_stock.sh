#!/usr/bin/env bash
# Compila la imagen hosted de serie (sin cambios) para bladeRF micro xA9.
# Uso: firmware/build_stock.sh   (genera firmware/out/hosted-xA9-stock.rbf y log)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/env.sh"
cd "$HERE/bladeRF/hdl/quartus"
start=$(date +%s)
./build_bladerf.sh -b bladeRF-micro -r hosted -s A9 -D 2>&1 | tee "$HERE/out/logs/build_stock_hosted_xA9.log"
rc=${PIPESTATUS[0]}
echo "RC=$rc TIEMPO_S=$(( $(date +%s) - start ))" | tee -a "$HERE/out/logs/build_stock_hosted_xA9.log"
