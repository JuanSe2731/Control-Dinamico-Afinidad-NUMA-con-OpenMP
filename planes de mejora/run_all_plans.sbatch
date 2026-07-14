#!/bin/bash
#SBATCH --job-name=planes_mejora
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
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

# Raiz del repo en el cluster (igual que run_perf_metricsV4.sbatch)
cd "/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP"

srun bash "planes de mejora/run_all_plans.sh"

# Al terminar, generar las graficas de comparacion (si hay python+matplotlib):
srun python3 "planes de mejora/analyze_plans.py" --results "planes de mejora/results" || \
  echo "[i] Analisis diferido: corre analyze_plans.py en un nodo con matplotlib."
