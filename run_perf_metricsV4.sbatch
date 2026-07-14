#!/bin/bash
#SBATCH --job-name=perf_v4
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
# Nodo COMPLETO en exclusiva: se reservan los 256 CPUs de los 2 sockets
# (= 2 dominios NUMA reales) y TODA la RAM del nodo (~385 GB, necesaria para el
# working set de ~4.4 GB del Stencil/SpMV). Sin --exclusive, en un nodo compartido
# SLURM puede empaquetar el job en un solo socket (= 1 NUMA efectivo) y la memoria
# libre no alcanza. Con exadell en estado MIXED, el job quedara en cola hasta que
# el nodo se libere por completo.
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --time=24:00:00
#SBATCH --output=perf_v4.%j.out
#SBATCH --error=perf_v4.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

set -euo pipefail

cd "/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP"

srun ./run_perf_metricsV4.sh
