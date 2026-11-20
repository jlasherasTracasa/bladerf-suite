#!/usr/bin/env bash
# Prueba de humo en Questa FSE: testbench rx_timestamp_tb del micro.
# Ejercita la ruta de RX del micro (rx.vhd + FIFOs + fx3_gpif + metadatos con
# marca de tiempo) y comprueba que el delta de timestamps entre paquetes es el
# esperado. Uso: firmware/sim/run_rx_timestamp.sh
# (El .do original de Nuand compila rx.vhd antes que sus paquetes; aqui se
# reordena: biblioteca nuand -> bladerf_p -> rx/tx -> tb.)
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FPGA="$(cd "$HERE/../bladeRF/hdl/fpga" && pwd)"
export PATH="$HOME/altera_lite/25.1std/questa_fse/bin:$PATH"
# Licencia gratuita (Altera Starter FSE) ligada a la MAC de la NIC; ver docs/hardware/FIRMWARE.md
export SALT_LICENSE_SERVER="$HOME/.altera.quartus/questa_lic.dat"
W="$HERE/work_rx_timestamp"; rm -rf "$W"; mkdir -p "$W"; cd "$W"
vlib rtl_work >/dev/null
cat > run.do <<DO
onerror {quit -f -code 1}
source $FPGA/ip/nuand/nuand.do
set P $FPGA/platforms/bladerf-micro
compile_nuand $FPGA/ip/nuand bladerf-micro
vcom -work nuand -2008 \$P/vhdl/bladerf_p.vhd
vcom -work nuand -2008 \$P/vhdl/rx.vhd
vcom -work nuand -2008 \$P/vhdl/tx.vhd
vcom -work nuand -2008 $FPGA/platforms/common/bladerf/vhdl/fx3_gpif.vhd
vcom -work nuand -2008 \$P/vhdl/tb/fx3_pll.vhd
vcom -work nuand -2008 \$P/vhdl/tb/fx3_model.vhd
vcom -work nuand -2008 \$P/vhdl/tb/rx_timestamp_tb.vhd
vsim -c -t 1ps nuand.rx_timestamp_tb
run 12 ms
quit -f
DO
timeout "${SIM_TIMEOUT:-900}" vsim -c -do run.do > "$HERE/rx_timestamp.log" 2>&1 < /dev/null || echo "vsim rc=$?"
grep -v "^# //" "$HERE/rx_timestamp.log" | tail -${TAILN:-40}
# Veredicto: el tb usa "assert ... severity failure" si el delta de timestamps es incorrecto
if grep -q "Unexpected Timestamp Delta\|Failure" "$HERE/rx_timestamp.log"; then echo "RESULTADO: FALLO"; exit 1; fi
if grep -q "Done with RX sample stream" "$HERE/rx_timestamp.log"; then echo "RESULTADO: OK (deltas de timestamp correctos)"; else echo "RESULTADO: INCOMPLETO"; exit 2; fi
