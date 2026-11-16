#!/usr/bin/env bash
# Demuestra que la ruta de datos de la imagen lab con rx_mux /= 3 es la de serie:
# el testbench oficial rx_timestamp_tb se recompila sustituyendo la entidad rx por
# rx_lab (sin tocar el original) y debe dar el mismo resultado que con rx.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FPGA="$(cd "$HERE/../bladeRF/hdl/fpga" && pwd)"
LAB="$FPGA/platforms/bladerf-micro/vhdl/lab"
export PATH="$HOME/altera_lite/25.1std/questa_fse/bin:$PATH"
export SALT_LICENSE_SERVER="$HOME/.altera.quartus/questa_lic.dat"
W="$HERE/work_rx_timestamp_lab"; rm -rf "$W"; mkdir -p "$W"; cd "$W"
vlib rtl_work >/dev/null
sed -e 's/entity rx_timestamp_tb/entity rx_timestamp_lab_tb/' \
    -e 's/architecture arch of rx_timestamp_tb/architecture arch of rx_timestamp_lab_tb/' \
    -e 's/U_rx : entity work.rx$/U_rx : entity work.rx_lab/' \
    -e 's/U_rx : entity work.rx\b/U_rx : entity work.rx_lab/' \
    "$FPGA/platforms/bladerf-micro/vhdl/tb/rx_timestamp_tb.vhd" > rx_timestamp_lab_tb.vhd
grep -q "entity work.rx_lab" rx_timestamp_lab_tb.vhd
cat > run.do <<DO
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
vcom -work nuand -2008 $LAB/rx_lab.vhd
vcom -work nuand -2008 \$P/vhdl/tx.vhd
vcom -work nuand -2008 $FPGA/platforms/common/bladerf/vhdl/fx3_gpif.vhd
vcom -work nuand -2008 \$P/vhdl/tb/fx3_pll.vhd
vcom -work nuand -2008 \$P/vhdl/tb/fx3_model.vhd
vcom -work nuand -2008 rx_timestamp_lab_tb.vhd
vsim -c -t 1ps nuand.rx_timestamp_lab_tb
run 12 ms
quit -f
DO
timeout "${SIM_TIMEOUT:-900}" vsim -c -do run.do > "$HERE/rx_timestamp_lab.log" 2>&1 < /dev/null || echo "vsim rc=$?"
grep -v "^# //" "$HERE/rx_timestamp_lab.log" | tail -6
if grep -q "Unexpected Timestamp Delta\|Failure" "$HERE/rx_timestamp_lab.log"; then echo "RESULTADO: FALLO"; exit 1; fi
if grep -q "Done with RX sample stream" "$HERE/rx_timestamp_lab.log"; then
    echo "RESULTADO: OK (rx_lab con rx_mux /= 3: mismos deltas de timestamp que rx)"
    # mismo conjunto de líneas "Timestamp Delta" que la simulación de serie
    diff <(grep -o "Timestamp Delta: [0-9]*" "$HERE/rx_timestamp.log") <(grep -o "Timestamp Delta: [0-9]*" "$HERE/rx_timestamp_lab.log") && echo "Deltas idénticos a los de rx.vhd"
else echo "RESULTADO: INCOMPLETO"; exit 2; fi
