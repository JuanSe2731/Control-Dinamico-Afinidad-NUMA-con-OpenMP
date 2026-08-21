#!/bin/bash
#SBATCH --job-name=perf_v5
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=250000
#SBATCH --time=48:00:00
#SBATCH --output=perf_v5.%j.out
#SBATCH --error=perf_v5.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

# Campaña V5: 576 ejecuciones (6 tamaños x 6 recuentos de hilos x 8 configuraciones
# x 2 kernels).
#
# ESTIMACION DE DURACION. La referencia es el job 29223: 600 ejecuciones en 17 472 s,
# o sea ~29 s por ejecucion, y todas al tamaño grande cargando stokes.mtx desde
# disco. Las nuestras son mayoritariamente de tamaños menores (S0-S4 son de
# milisegundos) y las de S5 generan la matriz en memoria en vez de parsear 4 GB de
# texto. Estimacion: entre 5 y 10 horas para el barrido completo.
#
# El --time de 48 h es un COLCHON, no una necesidad. Si la particion no admite tanto
# (`scontrol show partition amd | grep MaxTime`), bajarlo a 24:00:00 no cambia nada
# en la practica.
#
# Y el colchon importa poco, porque el script es REANUDABLE: cada (tamaño, config,
# hilos, kernel) que termina bien deja un marcador en $OUTDIR/.done y se salta en la
# siguiente tanda. Que SLURM mate el trabajo por pared de tiempo no cuesta datos,
# solo un reenvio. La forma de trabajar es lanzarlo hasta que diga "ejecutadas: 0".
#
# LO RECOMENDABLE ES TROCEAR POR TAMAÑO, no alargar el --time: el backfill del
# planificador mete antes los trabajos cortos en los huecos, asi que cuatro trabajos
# troceados suelen terminar antes que uno largo esperando una ventana enorme.
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
