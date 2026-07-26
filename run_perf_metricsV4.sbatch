#!/bin/bash
#SBATCH --job-name=perf_v4
#SBATCH --partition=amd
#SBATCH --nodelist=smexa
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=250000
#SBATCH --time=24:00:00
#SBATCH --output=perf_v4.%j.out
#SBATCH --error=perf_v4.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

set -euo pipefail

# --- Compilador: cargar clang++ (en smexa/OpenHPC viene por modulos Lmod) ---
for _f in /etc/profile.d/lmod.sh /etc/profile.d/z00_lmod.sh /etc/profile.d/modules.sh /usr/share/lmod/lmod/init/bash; do
  if [ -r "$_f" ]; then source "$_f"; break; fi
done
CLANG_MODULE="${CLANG_MODULE:-clang/17.0.6}"   # modulo del cluster; override: CLANG_MODULE=<nombre> sbatch ...
if command -v module >/dev/null 2>&1; then
  module load "${CLANG_MODULE}" 2>/dev/null || module load clang 2>/dev/null || true
fi
if ! command -v clang++ >/dev/null 2>&1; then
  echo "ERROR: clang++ no esta en el PATH en $(hostname)." >&2
  echo "  Busca el modulo:  module avail 2>&1 | grep -iE 'clang|llvm'" >&2
  echo "  y reenvia:        CLANG_MODULE=<nombre> sbatch run_perf_metricsV4.sbatch" >&2
  exit 1
fi
echo "[i] Nodo $(hostname): clang++ = $(command -v clang++)  |  $(clang++ --version | head -1)"

# TEMPORAL: /scratch no disponible actualmente, se usa el directorio principal del proyecto
# cd "/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP"
cd "$SLURM_SUBMIT_DIR"

srun ./run_perf_metricsV4.sh
