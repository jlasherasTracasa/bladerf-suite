#!/usr/bin/env bash
# Simulación del motor de espectro de la imagen "lab" (a través de rx_lab.vhd) y
# comparación exacta al bit con el modelo en C (firmware/model).
#
#   firmware/sim/run_lab_spec.sh [log2n k grupos escenario pad]   (una configuración)
#   firmware/sim/run_lab_spec.sh all                                (matriz de pruebas)
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FPGA="$(cd "$HERE/../bladeRF/hdl/fpga" && pwd)"
MODEL="$HERE/../model"
export PATH="$HOME/altera_lite/25.1std/questa_fse/bin:$PATH"
export SALT_LICENSE_SERVER="$HOME/.altera.quartus/questa_lic.dat"
W="$HERE/work_lab"
LAB="$FPGA/platforms/bladerf-micro/vhdl/lab"

make -s -C "$MODEL" spec_tool

compile() {
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
vcom -work nuand -2008 $HERE/tb_rx_lab.vhd
quit -f
DO
    vsim -c -do compile.do > compile.log 2>&1 < /dev/null || { grep -B1 -A3 "\*\* Error" compile.log | head -60; echo "COMPILACION FALLIDA"; exit 1; }
}

run_one() {
    local l=$1 k=$2 g=$3 sc=$4 pad=$5
    local n=$((1<<l)) tag="n${l}_k${k}_g${g}_s${sc}_p${pad}"
    local d="$W/$tag"; mkdir -p "$d"
    "$MODEL/spec_tool" vec "$l" "$k" "$g" "$sc" 7 "$pad" "$d/in.txt" "$d/exp.txt"
    local ns=$((g*k*n)) ex=$((2*n+256))
    cd "$W"
    timeout "${SIM_TIMEOUT:-3000}" vsim -c -t 1ps -GLOG2N=$l -GK=$k -GPAD=$pad -GNSAMP=$ns -GEXTRA=$ex \
        -GINFILE="$d/in.txt" -GOUTFILE="$d/out.txt" nuand.tb_rx_lab -do "run -all; quit -f" \
        > "$d/sim.log" 2>&1 < /dev/null || echo "vsim rc=$?"
    grep -E "FIN:|Error|Failure" "$d/sim.log" | head -5
    "$MODEL/spec_tool" check "$l" "$k" "$pad" "$d/exp.txt" "$d/out.txt" && echo "[$tag] OK" || { echo "[$tag] FALLO"; return 1; }
}

# Identidad con rx_mux = 0: rx_lab y rx de serie dan las mismas palabras, iguales a las muestras
run_identity() {
    local l=10 n=1024 d="$W/ident"; mkdir -p "$d"
    "$MODEL/spec_tool" vec "$l" 1 2 1 5 1 "$d/in.txt" "$d/exp.txt"
    cd "$W"
    for st in 0 1; do
        timeout 900 vsim -c -t 1ps -GLOG2N=$l -GK=1 -GPAD=1 -GNSAMP=2048 -GEXTRA=64 -GMUX=0 -GSTOCK=$st \
            -GINFILE="$d/in.txt" -GOUTFILE="$d/out_$st.txt" nuand.tb_rx_lab -do "run -all; quit -f" \
            > "$d/sim_$st.log" 2>&1 < /dev/null || true
    done
    python3 - "$d" <<'PY'
import sys
d=sys.argv[1]
ins=[tuple(map(int,l.split())) for l in open(d+"/in.txt")]
exp=["%08X"%(((q&0xFFFF)<<16)|(i&0xFFFF)) for i,q in ins]
o0=[l.strip() for l in open(d+"/out_0.txt")][:len(exp)]
o1=[l.strip() for l in open(d+"/out_1.txt")][:len(exp)]
ok=(o0==o1==exp)
print("identidad rx_mux=0: rx_lab == rx de serie == muestras de entrada (%d palabras): %s"%(len(exp),"OK" if ok else "FALLO"))
sys.exit(0 if ok else 1)
PY
}

[ -d "$W/nuand" ] && [ -z "${RECOMPILE:-}" ] || compile
rc=0
if [ "${1:-}" = "ident" ]; then
    run_identity || rc=1
elif [ "${1:-}" = "all" ]; then
    run_one 10 3 3 0 1 || rc=1
    run_one 11 2 2 0 1 || rc=1
    run_one 12 2 2 0 0 || rc=1
    run_one 13 2 2 0 1 || rc=1
    run_one 10 1 4 1 1 || rc=1
    run_one 12 1 2 2 1 || rc=1
    run_identity || rc=1
else
    run_one "${1:-10}" "${2:-2}" "${3:-2}" "${4:-0}" "${5:-1}" || rc=1
fi
exit $rc
