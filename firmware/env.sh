#!/usr/bin/env bash
# Entorno de compilación del HDL de la bladeRF 2.0 micro (BladeRF Lab, agente F1).
#
# Uso:  source firmware/env.sh
#
# Reproduce lo que haría nios2_command_shell.sh de Quartus 20.1 (PATH de Quartus,
# Qsys y toolchain Nios II) sin abrir un subshell, y añade los ajustes que pide
# build_bladerf.sh. NO toca el sistema ni el hardware.

QUARTUS_HOME="${QUARTUS_HOME:-$HOME/altera_lite/20.1}"

if [ ! -d "$QUARTUS_HOME/quartus/bin" ]; then
    echo "env.sh: no existe $QUARTUS_HOME/quartus/bin" >&2
    return 1 2>/dev/null || exit 1
fi

export QUARTUS_ROOTDIR="$QUARTUS_HOME/quartus"
export SOPC_KIT_NIOS2="$QUARTUS_HOME/nios2eds"

# Quita duplicados al re-sourcear
_f1_path_prepend() { case ":$PATH:" in *":$1:"*) ;; *) PATH="$1:$PATH";; esac; }
_f1_path_prepend "$SOPC_KIT_NIOS2/bin/gnu/H-x86_64-pc-linux-gnu/bin"  # nios2-elf-gcc
_f1_path_prepend "$SOPC_KIT_NIOS2/sdk2/bin"                            # nios2-bsp-*
_f1_path_prepend "$SOPC_KIT_NIOS2/bin"                                 # elf2hex, etc.
_f1_path_prepend "$QUARTUS_ROOTDIR/sopc_builder/bin"                   # qsys-generate/script
_f1_path_prepend "$QUARTUS_ROOTDIR/bin"                                # quartus_sh, quartus_map...
unset -f _f1_path_prepend
export PATH

# Quartus 20.1 en glibc modernas: pila ejecutable (lo exporta también build_bladerf.sh)
export GLIBC_TUNABLES="${GLIBC_TUNABLES:+${GLIBC_TUNABLES}:}glibc.rtld.execstack=2"

# Los scripts de Quartus ponen mal el locale con otros idiomas
export LC_ALL=C

# Simulador (Questa FSE 25.1) solo si se pide expresamente: no se añade al PATH
# para no mezclar bibliotecas de 25.1 con las de 20.1.
export QUESTA_BIN="$HOME/altera_lite/25.1std/questa_fse/bin"
