#!/usr/bin/env bash
# Prueba del banco de registros Wishbone con el wishbone_master real del Nios.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FPGA="$(cd "$HERE/../bladeRF/hdl/fpga" && pwd)"
LAB="$FPGA/platforms/bladerf-micro/vhdl/lab"
export PATH="$HOME/altera_lite/25.1std/questa_fse/bin:$PATH"
export SALT_LICENSE_SERVER="$HOME/.altera.quartus/questa_lic.dat"
W="$HERE/work_lab_regs"; rm -rf "$W"; mkdir -p "$W"; cd "$W"
vlib rtl_work >/dev/null
cat > run.do <<DO
onerror {quit -f -code 1}
source $FPGA/ip/nuand/nuand.do
compile_nuand $FPGA/ip/nuand bladerf-micro
vcom -work nuand -2008 $FPGA/ip/nuand/wishbone_master/vhdl/wishbone_master.vhd
vcom -work nuand -2008 $LAB/lab_cdc_bus.vhd
vcom -work nuand -2008 $LAB/lab_regs.vhd
vcom -work nuand -2008 $HERE/tb_lab_regs.vhd
vsim -c -t 1ps nuand.tb_lab_regs
run 400 us
quit -f
DO
timeout 600 vsim -c -do run.do > "$HERE/lab_regs.log" 2>&1 < /dev/null || echo "vsim rc=$?"
grep -E "\*\* (Note|Error|Failure)" "$HERE/lab_regs.log" | grep -v "vcom-\|vsim-" | tail -30
# El dcfifo de wishbone_master (IP original) avisa "Illegal INTENDED_DEVICE_FAMILY" en simulación; es benigno.
grep -q "TB_LAB_REGS: OK" "$HERE/lab_regs.log" && ! grep "\*\* Error" "$HERE/lab_regs.log" | grep -qv "INTENDED_DEVICE_FAMILY" && echo "RESULTADO: OK" || { echo "RESULTADO: FALLO"; exit 1; }
