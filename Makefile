# Makefile de BladeRF Lab (AGENTS.md §7)
#
#   make                  compila $(BUILD)/bladerf-lab (CUDA automático si hay nvcc)
#   make CUDA=0           sin GPU (usa src/gpu/*_stub.c)
#   make BUILD=build/a1   directorio de compilación propio (por agente)
#   make test             compila y ejecuta tests/test_*.c   (T=ais → solo tests/test_ais.c)
#   make SAN=1 test       con AddressSanitizer + UBSan
#   make check            todo con -Werror (en $(BUILD)/check) + todos los tests
#   make run ARGS="..."   compila y ejecuta
#   make clean            borra $(BUILD)
#
# Las fuentes se buscan recursivamente en src/: basta con crear el fichero .c.
# Cada tests/test_*.c se enlaza con todos los objetos de src/ menos main.o.

BUILD ?= build
comma := ,
CC    := gcc

# --- CUDA: automático -----------------------------------------------------
# nvcc puede estar en el PATH o en /usr/local/cuda/bin. CUDA=0 lo desactiva.
NVCC ?= $(shell command -v nvcc 2>/dev/null || ([ -x /usr/local/cuda/bin/nvcc ] && echo /usr/local/cuda/bin/nvcc))
ifeq ($(CUDA),0)
  USE_CUDA :=
else ifneq ($(NVCC),)
  USE_CUDA := 1
endif
CUDA_HOME ?= $(abspath $(dir $(NVCC))/..)
CUDA_ARCH ?= sm_89

# --- Banderas --------------------------------------------------------------
OPT      := -O2
SANFLAGS :=
ifeq ($(SAN),1)
  OPT      := -O1
  SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif
WARN     := -Wall -Wextra
ifeq ($(WERROR),1)
  WARN += -Werror
endif
# libbladeRF: pkg-config da la 2.6.1 de /usr/local (la de /usr/lib es anterior); rpath para cargarla en ejecución.
BLADERF_CFLAGS := $(shell pkg-config --cflags libbladeRF 2>/dev/null)
BLADERF_LIBS   := $(shell pkg-config --libs libbladeRF 2>/dev/null)
BLADERF_RPATH  := $(patsubst -L%,-Wl$(comma)-rpath$(comma)%,$(filter -L%,$(BLADERF_LIBS)))
ifeq ($(strip $(BLADERF_LIBS)),)
  BLADERF_LIBS := -lbladeRF
endif
CFLAGS   := -std=c11 -D_GNU_SOURCE $(OPT) -g $(WARN) -march=native -Isrc $(BLADERF_CFLAGS) $(SANFLAGS) -MMD -MP
LDLIBS   := $(BLADERF_LIBS) $(BLADERF_RPATH) -lm -lpthread
NVFLAGS  := -O2 -arch=$(CUDA_ARCH) -Isrc
ifeq ($(USE_CUDA),1)
  LDLIBS += -L$(CUDA_HOME)/lib64 -Wl,-rpath,$(CUDA_HOME)/lib64 -lcudart -lstdc++
endif
LDFLAGS  := $(SANFLAGS)

# --- Fuentes ---------------------------------------------------------------
SRC_C  := $(shell find src -name '*.c' | sort)
SRC_CU := $(shell find src -name '*.cu' | sort)
ifeq ($(USE_CUDA),1)
  SRC_C := $(filter-out %_stub.c,$(SRC_C))
else
  SRC_CU :=
endif
OBJS      := $(patsubst %.c,$(BUILD)/obj/%.o,$(SRC_C)) $(patsubst %.cu,$(BUILD)/obj/%.o,$(SRC_CU))
MAIN_OBJ  := $(BUILD)/obj/src/main.o
LIB_OBJS  := $(filter-out $(MAIN_OBJ),$(OBJS))
BIN       := $(BUILD)/bladerf-lab

# --- Tests -----------------------------------------------------------------
ifdef T
  TESTS := test_$(T)
else
  TESTS := $(notdir $(basename $(wildcard tests/test_*.c)))
endif
TEST_BINS := $(addprefix $(BUILD)/tests/,$(TESTS))

.PHONY: all test check run clean FORCE
all: $(BIN)

# Las banderas se guardan en un sello: si cambian (SAN, CUDA, WERROR…) se recompila todo.
$(BUILD)/.flags: FORCE
	@mkdir -p $(BUILD)
	@echo '$(CC) $(CFLAGS) $(NVFLAGS) $(USE_CUDA) $(LDLIBS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(NVFLAGS) $(USE_CUDA) $(LDLIBS)' > $@

$(BIN): $(OBJS)
	@echo "  ENLAZA $@  (CUDA: $(if $(USE_CUDA),sí,no))"
	@$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

$(BUILD)/obj/%.o: %.c $(BUILD)/.flags
	@mkdir -p $(dir $@)
	@echo "  CC     $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.cu $(BUILD)/.flags
	@mkdir -p $(dir $@)
	@echo "  NVCC   $<"
	@$(NVCC) $(NVFLAGS) -MMD -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) $(wildcard tests/*.h)
	@mkdir -p $(dir $@)
	@echo "  TEST   $<"
	@$(CC) $(CFLAGS) -Itests $< $(LIB_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_BINS)
	@tests/run_tests.sh $(BUILD)/tests $(TESTS)

check:
	@$(MAKE) --no-print-directory BUILD=$(BUILD)/check WERROR=1 all test

run: $(BIN)
	$(BIN) $(ARGS)

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)
