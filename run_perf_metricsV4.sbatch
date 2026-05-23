#!/bin/bash
#SBATCH --job-name=perf_v4
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --reservation=CARLA26
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

cd "/scratch/CAGE/jsoterov01/Control-Dinamico-Afinidad-NUMA-con-OpenMP"

srun ./run_perf_metricsV4.sh
