#!/bin/bash
#SBATCH --job-name=planes_mejora
#SBATCH --partition=amd
#SBATCH --nodelist=smexa
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=250000
#SBATCH --time=24:00:00
#SBATCH --output=planes_mejora.%j.out
#SBATCH --error=planes_mejora.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

# =============================================================================
# UN SOLO ENVIO corre todos los planes de mejora.
#
# IMPORTANTE: el orquestador es REANUDABLE. Las 10 corridas (etapa serial excluida,
# pero con Stencil ahora a N=23500 ~4.4GB) pueden pasarse de 24h; si el job se corta
# por tiempo simplemente vuelve a enviar este mismo sbatch: saltara los planes ya
# terminados y seguira con los que falten (marca results/<PLAN>/.done). 2-3 reenvios.
#
# Si tu particion permite mas tiempo, sube --time (p.ej. 72:00:00).
# =============================================================================

set -euo pipefail

# --- Compilador: cargar clang++ (en smexa/OpenHPC viene por modulos Lmod) ---
# smexa NO trae clang++ en el PATH por defecto -> el job moria en 1s (need_cmd falla).
# Inicializa el sistema de modulos y carga el toolchain de LLVM/clang.
for _f in /etc/profile.d/lmod.sh /etc/profile.d/z00_lmod.sh /etc/profile.d/modules.sh /usr/share/lmod/lmod/init/bash; do
  if [ -r "$_f" ]; then source "$_f"; break; fi
done
CLANG_MODULE="${CLANG_MODULE:-llvm}"   # si el nombre es otro: CLANG_MODULE=<nombre> sbatch ...
if command -v module >/dev/null 2>&1; then
  module load "${CLANG_MODULE}" 2>/dev/null || module load clang 2>/dev/null || true
fi
if ! command -v clang++ >/dev/null 2>&1; then
  echo "ERROR: clang++ no esta en el PATH en $(hostname)." >&2
  echo "  1) Busca el modulo:  module avail 2>&1 | grep -iE 'clang|llvm'" >&2
  echo "  2) Reenvia con el nombre exacto:  CLANG_MODULE=<nombre> sbatch planes_de_mejora/run_all_plans.sbatch" >&2
  exit 1
fi
echo "[i] Nodo $(hostname): clang++ = $(command -v clang++)  |  $(clang++ --version | head -1)"

# Raiz del repo en el cluster (igual que run_perf_metricsV4.sbatch)
# TEMPORAL: /scratch no disponible actualmente, se usa el directorio principal del proyecto
# cd "/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP"
cd "$SLURM_SUBMIT_DIR"

srun bash "planes_de_mejora/run_all_plans.sh"

# Al terminar, generar las graficas de comparacion (si hay python+matplotlib):
srun python3 "planes_de_mejora/analyze_plans.py" --results "planes_de_mejora/results" || \
  echo "[i] Analisis diferido: corre analyze_plans.py en un nodo con matplotlib."
