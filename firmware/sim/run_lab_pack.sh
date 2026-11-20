#!/usr/bin/env bash
# Simulación del empaquetado de RX de la imagen lab 1.3 (F11) contra el modelo en C
# (firmware/model/pack_tool): SC8 con ganancia digital 0..3 y SC16_Q11_PACKED (12 bit).
#   firmware/sim/run_lab_pack.sh [semilla [tramas]]     (por defecto 20260101, 1000 tramas de 64 muestras)
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FPGA="$(cd "$HERE/../bladeRF/hdl/fpga" && pwd)"
MODEL="$HERE/../model"
export PATH="$HOME/altera_lite/25.1std/questa_fse/bin:$PATH"
export SALT_LICENSE_SERVER="$HOME/.altera.quartus/questa_lic.dat"
W="$HERE/work_lab_pack"
LAB="$FPGA/platforms/bladerf-micro/vhdl/lab"
SEED="${1:-20260101}"; FR="${2:-1000}"
make -s -C "$MODEL" pack_tool
rm -rf "$W"; mkdir -p "$W"; cd "$W"
vlib rtl_work >/dev/null
cat > compile.do <<DO
onerror {quit -f -code 1}
source $FPGA/ip/nuand/nuand.do
set P $FPGA/platforms/bladerf-micro
compile_nuand $FPGA/ip/nuand bladerf-micro
vcom -work nuand -2008 \$P/vhdl/bladerf_p.vhd
vcom -work nuand -2008 $LAB/lab_rom_p.vhd
vcom -work nuand -2008 $LAB/sdf_stage.vhd
vcom -work nuand -2008 $LAB/sdf_mult.vhd
vcom -work nuand -2008 $LAB/spec_fft.vhd
vcom -work nuand -2008 $LAB/spec_acc_ram.vhd
vcom -work nuand -2008 $LAB/spec_engine.vhd
vcom -work nuand -2008 \$P/vhdl/rx.vhd
vcom -work nuand -2008 $LAB/rx_lab.vhd
vcom -work nuand -2008 $HERE/tb_rx_pack.vhd
quit -f
DO
vsim -c -do compile.do > compile.log 2>&1 < /dev/null || { grep -B1 -A3 "\*\* Error" compile.log | head -60; echo "COMPILACION FALLIDA"; exit 1; }

rc=0
run_one() {   # modo shift
    local mode=$1 sh=$2 tag="m${1}_s${2}" ns=$((FR*64))
    local d="$W/$tag"; mkdir -p "$d"
    "$MODEL/pack_tool" vec "$SEED" "$FR" "$mode" "$sh" "$d/in.txt" "$d/exp.txt" "$d/sat.txt"
    cd "$W"
    timeout 1800 vsim -c -t 1ps -GMODE=$mode -GSHIFT=$sh -GNSAMP=$ns -GEXTRA=64 -GINFILE="$d/in.txt" -GOUTFILE="$d/out.txt" \
        nuand.tb_rx_pack -do "run -all; quit -f" > "$d/sim.log" 2>&1 < /dev/null || echo "vsim rc=$?"
    local fin; fin=$(grep "FIN:" "$d/sim.log" | sed 's/.*FIN: //')
    local words
    if [ "$mode" = 8 ]; then words=$((FR*64/2)); else words=$((FR*64*3/4)); fi
    local exp_sat; exp_sat=$(cat "$d/sat.txt")
    local ok=1
    "$MODEL/pack_tool" check "$d/exp.txt" "$d/out.txt" "$words" || ok=0
    if [ "$mode" = 8 ]; then
        case "$fin" in *"sat8=$exp_sat") ;; *) echo "sat8 distinto: esperado $exp_sat, sim: $fin"; ok=0;; esac
    fi
    [ $ok = 1 ] && echo "[$tag] OK ($FR tramas, semilla $SEED; $fin)" || { echo "[$tag] FALLO"; rc=1; }
}
for s in 0 1 2 3; do run_one 8 $s; done
run_one 12 0
exit $rc
