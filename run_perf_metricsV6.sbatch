#!/bin/bash
#SBATCH --job-name=perf_v6
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=250000
#SBATCH --time=48:00:00
#SBATCH --output=perf_v6.%j.out
#SBATCH --error=perf_v6.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

# Campaña V6: 576 ejecuciones (6 tamaños x 6 recuentos de hilos x 8 configuraciones
# x 2 kernels) — las MISMAS que V5. Lo único que cambia es que `scheduler` y `obs`
# usan el umbral colgado del mínimo alcanzable (OMPT_REFERENCIA=min).
#
# NO se toca perf_out_v5. V6 escribe en perf_out_v6, con sus propios marcadores
# .done, para que las dos campañas se puedan comparar y ninguna pise a la otra.
#
# UN SOLO TRABAJO. El job 29390 hizo las 576 ejecuciones de V5 de una tirada
# ("ejecutadas: 576, saltadas: 0") dentro de este mismo --time, y V6 tiene las
# mismas 576. Lanzar:
#     sbatch run_perf_metricsV6.sbatch
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
# Si aun asi SLURM lo mata por tiempo, se relanza EXACTAMENTE el mismo comando y
# continua donde se quedo. Trocear por tamaño es solo una alternativa, por si la
# cola tarda en darle una ventana tan larga (el backfill mete antes los cortos):
#     SIZES="S0 S1 S2" sbatch run_perf_metricsV6.sbatch
#     SIZES="S3"       sbatch run_perf_metricsV6.sbatch
#     SIZES="S4"       sbatch run_perf_metricsV6.sbatch
#     SIZES="S5"       sbatch run_perf_metricsV6.sbatch
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

srun ./run_perf_metricsV6.sh
