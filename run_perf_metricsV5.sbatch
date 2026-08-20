#!/bin/bash
#SBATCH --job-name=perf_v5
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=250000
#SBATCH --time=24:00:00
#SBATCH --output=perf_v5.%j.out
#SBATCH --error=perf_v5.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

# Campaña V5. La matriz completa son 576 ejecuciones (6 tamaños x 6 recuentos de
# hilos x 8 configuraciones x 2 kernels) y NO cabe en una ventana de 24 h.
#
# Por eso el script es reanudable: cada (tamaño, config, hilos, kernel) que termina
# bien deja un marcador en $OUTDIR/.done y se salta en la siguiente tanda. La forma
# de trabajar es lanzar esto varias veces hasta que reporte "ejecutadas: 0".
#
# Para trocear por tamaños y que cada trabajo entre holgado:
#     SIZES="S0 S1 S2" sbatch run_perf_metricsV5.sbatch
#     SIZES="S3"       sbatch run_perf_metricsV5.sbatch
#     SIZES="S4"       sbatch run_perf_metricsV5.sbatch
#     SIZES="S5"       sbatch run_perf_metricsV5.sbatch
# (SIZES se propaga por el entorno, igual que el resto de perillas.)
#
# Igual que el resto de .sbatch del proyecto, NO carga modulos: depende de que SLURM
# propague el entorno del shell que lo lanza (--export=ALL por defecto), asi que hay
# que preparar el toolchain (clang + hwloc) ANTES de hacer sbatch.
#
# Comprobar antes de lanzar:
#     cat /proc/sys/kernel/perf_event_paranoid     # debe ser <= 2
#     perf list | grep -i fills                    # validar las mascaras de PMCx044

set -euo pipefail
cd "$SLURM_SUBMIT_DIR"

srun ./run_perf_metricsV5.sh
