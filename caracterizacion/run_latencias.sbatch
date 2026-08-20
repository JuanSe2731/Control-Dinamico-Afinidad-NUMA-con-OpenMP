#!/bin/bash
#SBATCH --job-name=latencias
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --exclusive
#SBATCH --mem=250000
#SBATCH --time=02:00:00
#SBATCH --output=latencias.%j.out
#SBATCH --error=latencias.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

# Caracterizacion de latencias de la jerarquia de memoria de exadell.
#
# --exclusive es OBLIGATORIO aqui, no una comodidad: una medida de latencia con
# vecinos en el nodo mide el ruido de los vecinos. Es la unica parte del proyecto
# que exige el nodo entero para si.
#
# Se ejecuta UNA sola vez, como caracterizacion de la plataforma, ANTES de la
# campana de la Fase 2. Nunca en paralelo con los kernels: se contaminan.
#
# Igual que el resto de .sbatch del proyecto, NO carga modulos: depende de que
# SLURM propague el entorno del shell que lo lanza (--export=ALL por defecto), asi
# que hay que preparar el toolchain (clang + hwloc) ANTES de hacer sbatch.

set -euo pipefail
export LC_ALL=C

cd "$SLURM_SUBMIT_DIR"

CXX="${CXX:-clang++}"
HWLOC_INCLUDE="${HWLOC_INCLUDE:-/opt/ohpc/pub/libs/hwloc/include}"
HWLOC_LIB="${HWLOC_LIB:-/opt/ohpc/pub/libs/hwloc/lib}"
OUT_CSV="${OUT_CSV:-caracterizacion/latencias_exadell.csv}"

echo "=== Compilando latencias_numa ==="
"$CXX" -std=c++17 -O2 -pthread \
    caracterizacion/latencias_numa.cpp -o latencias_numa \
    -I"$HWLOC_INCLUDE" -L"$HWLOC_LIB" -lhwloc

# El CSV se reescribe desde cero: es una caracterizacion, no una serie temporal.
rm -f "$OUT_CSV"

echo
echo "=== Estado de las paginas enormes del sistema ==="
# Que politica de THP tiene la maquina determina en cual de las dos curvas del
# modo A caen los kernels de la campana. Hay que dejarlo registrado.
cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "(no legible)"

echo
echo "=== Modos A (curva, con y sin THP), B (pares) y C (victima) ==="
./latencias_numa --curva --c2c --victima --reps 7 --csv "$OUT_CSV"

echo
echo "=== Modo B sobre la matriz completa 128x128 (mapa de calor) ==="
# Es lo que hace visible la estructura de chiplets y, de paso, valida la hipotesis
# de que el nucleo fisico c vive en el CCD c/8. Si los bloques de 8x8 no aparecen,
# el diseno de tamanos de la Fase 2 esta construido sobre una topologia falsa.
./latencias_numa --matriz --reps 1 --csv "$OUT_CSV"

echo
echo "=== Terminado. Resultados en $OUT_CSV ==="
wc -l "$OUT_CSV"
