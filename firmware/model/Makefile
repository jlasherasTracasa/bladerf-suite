CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra
all: spec_tool pack_tool
spec_tool: spec_tool.c spec_model.c spec_model.h
	$(CC) $(CFLAGS) -o $@ spec_tool.c spec_model.c -lm
pack_tool: pack_tool.c
	$(CC) $(CFLAGS) -o $@ pack_tool.c
rom: spec_tool
	./spec_tool genrom ../bladeRF/hdl/fpga/platforms/bladerf-micro/vhdl/lab/lab_rom_p.vhd
test: spec_tool
	./spec_tool selftest
clean:
	rm -f spec_tool pack_tool
