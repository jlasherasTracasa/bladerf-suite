#!/usr/bin/env bash
# Compila la imagen "lab" (hosted + banco Wishbone + motor de espectro) para bladeRF micro xA9.
# Uso: firmware/build_lab.sh [-l synth]   (genera firmware/out/hosted-xA9-lab-<versión>.rbf)
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/env.sh"
VER="0.16.101"
cd "$HERE/bladeRF/hdl/quartus"
start=$(date +%s)
./build_bladerf.sh -b bladeRF-micro -r lab -s A9 -D "$@" 2>&1 | tee "$HERE/out/logs/build_lab_xA9.log"
rc=${PIPESTATUS[0]}
echo "RC=$rc TIEMPO_S=$(( $(date +%s) - start ))" | tee -a "$HERE/out/logs/build_lab_xA9.log"
if [ "$rc" = 0 ] && [ -f labxA9/labxA9.rbf ]; then
    cp labxA9/labxA9.rbf "$HERE/out/hosted-xA9-lab-$VER.rbf"
    (cd "$HERE/out" && sha256sum "hosted-xA9-lab-$VER.rbf" > "hosted-xA9-lab-$VER.rbf.sha256")
    mkdir -p "$HERE/out/reports_lab" && cp labxA9/*.summary labxA9/*.rpt labxA9/git_info.txt "$HERE/out/reports_lab/"
fi
exit $rc
