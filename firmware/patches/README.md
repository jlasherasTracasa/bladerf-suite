# Parches del HDL de la imagen "lab"

`firmware/bladeRF` (clon del árbol oficial con la rama `lab`) no se versiona: pesa ~1,8 GB con los productos de
compilación. Los cambios de HDL se exportan aquí con `git format-patch` sobre el commit oficial `35174c3`
("hdl: clean Quartus work directories by default", libbladeRF 2.6.1).

## Reconstruir el árbol y la imagen

```bash
# 1. Árbol oficial en el commit exacto (desde cualquier copia del repositorio oficial de Nuand)
git clone https://github.com/Nuand/bladeRF firmware/bladeRF
git -C firmware/bladeRF checkout -b lab 35174c3

# 2. Aplicar los parches (conserva el mensaje de commit)
git -C firmware/bladeRF am ../patches/*.patch
# 2b. Parche 0003 (lab 1.2, FPGA 0.16.102): es un `git diff` sin commit; se aplica con
git -C firmware/bladeRF apply ../patches/0003-*.diff
# 2c. Parche 0004 (lab 1.3, FPGA 0.16.103, F11: GAIN8/CNT_SAT8): también `git diff`, sobre el resultado de 0003
git -C firmware/bladeRF apply ../patches/0004-*.diff

# 3. Compilar (Quartus Prime Lite 20.1.1 en ~/altera_lite/20.1; ver docs/hardware/FIRMWARE.md)
firmware/build_lab.sh          # firmware/out/hosted-xA9-lab-0.16.103.rbf + .sha256 + reports_lab/
```

`firmware/model/` y `firmware/sim/` ya están en el repositorio del proyecto. `lab_rom_p.vhd` (ventana y twiddles) viene
dentro del parche; se regenera con `make -C firmware/model rom` si se cambia el modelo.

## Regenerar los parches tras cambiar el HDL

```bash
git -C firmware/bladeRF add -A hdl && git -C firmware/bladeRF commit -m "..."
rm firmware/patches/*.patch
git -C firmware/bladeRF format-patch 35174c3..lab -o ../patches/
```
