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
#     SIZES="S0 S1 S2" sbatch scripts/run_perf_metricsV5.sbatch
#     SIZES="S3"       sbatch scripts/run_perf_metricsV5.sbatch
#     SIZES="S4"       sbatch scripts/run_perf_metricsV5.sbatch
#     SIZES="S5"       sbatch scripts/run_perf_metricsV5.sbatch
# (SIZES se propaga por el entorno, igual que el resto de perillas.)
#
# Se lanza desde la raiz del repositorio o desde scripts/; los resultados van a
# data/perf_out_v5 (o al OUTDIR que se exporte) y el .out/.err de SLURM queda en el
# directorio desde el que se hizo sbatch.
#
# Igual que el resto de .sbatch del proyecto, NO carga modulos: depende de que SLURM
# propague el entorno del shell que lo lanza (--export=ALL por defecto), asi que hay
# que preparar el toolchain (clang + hwloc) ANTES de hacer sbatch.
#
# Comprobar antes de lanzar:
#     cat /proc/sys/kernel/perf_event_paranoid     # debe ser <= 2
#     perf list | grep -i fills                    # validar las mascaras de PMCx044

set -euo pipefail

# SLURM ejecuta una COPIA de este fichero desde su directorio de spool, asi que
# BASH_SOURCE no sirve para encontrar el repositorio: se parte del directorio desde
# el que se hizo sbatch y se sube hasta dar con la raiz.
RAIZ="${SLURM_SUBMIT_DIR:-$PWD}"
until [[ -f "$RAIZ/scripts/run_perf_metricsV5.sh" ]]; do
  [[ "$RAIZ" != "/" ]] || { echo "ERROR: hay que lanzar sbatch desde dentro del repositorio" >&2; exit 1; }
  RAIZ="$(dirname "$RAIZ")"
done
cd "$RAIZ"

# ── LIMITE DE DESCRIPTORES ────────────────────────────────────────────────────
# OBLIGATORIO, no opcional. Cada hilo OpenMP abre varios fd de perf_event_open, y
# con el limite por defecto de 1024 de exadell la cuenta revienta a partir de 128
# hilos: el proceso ya no puede ni abrir su CSV de salida, sale con codigo 0 y deja
# la fila entera vacia. Asi se perdieron 144 ejecuciones (el 25 %) en la campana
# 29355, y en silencio, porque el stderr iba a /dev/null.
#
#   fds por hilo ~ 2 (ciclos+instr) + 2 (par 0xFF44/0xD044) + hasta 6 del desglose
#   256 hilos x 10 = 2560, mas stdio, el runtime de OpenMP y los ficheros de salida
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
echo "=== limite de descriptores: $(ulimit -n) (hard: $(ulimit -Hn)) ==="
if [ "$(ulimit -n)" -lt 4096 ]; then
  echo "AVISO: menos de 4096 descriptores; los puntos de 128 y 256 hilos pueden fallar" >&2
fi

# ── Contadores libres ─────────────────────────────────────────────────────────
# Zen 4 tiene 6 contadores programables, pero el nmi_watchdog ocupa uno de forma
# permanente y deja solo CINCO. Un grupo de 6 eventos se abre pero NUNCA se
# planifica (perf lo reporta como "<not counted> (0.00%)"). El codigo lo sondea en
# tiempo de ejecucion y ajusta el tamano del desglose; ratio_rm no se ve afectado
# porque sale de un par que siempre cabe. Se deja registrado el estado:
echo "=== nmi_watchdog: $(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo '?') ==="
echo "=== perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid) ==="

srun ./scripts/run_perf_metricsV5.sh
