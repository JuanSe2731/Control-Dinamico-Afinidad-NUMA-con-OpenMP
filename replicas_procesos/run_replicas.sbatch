#!/bin/bash
#SBATCH --job-name=replicas_v5
#SBATCH --partition=amd
#SBATCH --nodelist=exadell
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=250000
#SBATCH --time=10:00:00
#SBATCH --output=replicas.%j.out
#SBATCH --error=replicas.%j.err
#SBATCH --mail-type=ALL
#SBATCH --mail-user=jsotero12555@gmail.com

# Campaña de réplicas de proceso: 10 réplicas x 180 ejecuciones (S3-S5, 5
# configuraciones, 6 recuentos de hilos, 2 kernels). Ver LEEME.md.
#
# DURACIÓN. El bucle medido de una réplica suma ~18 min (tiempos de la campaña
# 29390: 1062 s para estas 180 ejecuciones) y la preparación de cada proceso
# (generar la matriz de S5, inicializar, validar) añade unos 8; en total ~25 min
# por réplica y ~4,5 h las diez. Las 10 h son colchón. Y el colchón importa poco:
# el trabajo es REANUDABLE y una réplica completa se salta entera, así que si SLURM
# lo corta se reenvía el mismo sbatch hasta que el resumen diga "PENDIENTES: 0".
#
# Mismos recursos que run_perf_metricsV5.sbatch (256 CPU = el nodo entero, 250 GB),
# a propósito: las réplicas deben correr en las mismas condiciones que la campaña
# 29390 a la que complementan.
#
# SE LANZA DESDE ESTE DIRECTORIO:
#     cd replicas_procesos && sbatch run_replicas.sbatch
# SLURM ejecuta una copia de este fichero desde su spool, así que el único ancla
# fiable es el directorio de envío. Lanzado desde la raíz, aquí se ejecutaría la
# suite de la raíz y los datos irían a perf_out_v5; por eso se comprueba y se aborta.
#
# Igual que el resto de .sbatch del proyecto, NO carga módulos: SLURM propaga el
# entorno del shell que hace el sbatch (--export=ALL por defecto), así que clang y
# hwloc se cargan ANTES:
#     module load clang/17.0.6 hwloc
#
# Las perillas de run_replicas.sh se pasan igual, por el entorno:
#     N_REPLICAS=5 sbatch run_replicas.sbatch
#     REPLICAS="r11 r12" sbatch run_replicas.sbatch
#     N_REPLICAS=1 SIZES="S5" THREAD_LIST="8" RESULTADOS=prueba_humo \
#         sbatch --time=00:30:00 run_replicas.sbatch       # prueba de humo, ~7 min

set -euo pipefail
cd "$SLURM_SUBMIT_DIR"

if [[ ! -f run_replicas.sh || ! -f analisis_replicas.py ]]; then
  echo "ERROR: este sbatch se lanza DESDE replicas_procesos/ (se lanzo desde $PWD)." >&2
  echo "       cd replicas_procesos && sbatch run_replicas.sbatch" >&2
  exit 1
fi

# ── LIMITE DE DESCRIPTORES ────────────────────────────────────────────────────
# OBLIGATORIO, no opcional. Cada hilo OpenMP abre varios fd de perf_event_open, y
# con el limite por defecto de 1024 de exadell la cuenta revienta a partir de 128
# hilos: el proceso ya no puede ni abrir su CSV de salida, sale con codigo 0 y deja
# la fila entera vacia. Asi se perdieron 144 ejecuciones en la campana 29355.
ulimit -n 65536 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
echo "=== limite de descriptores: $(ulimit -n) (hard: $(ulimit -Hn)) ==="
if [ "$(ulimit -n)" -lt 4096 ]; then
  echo "AVISO: menos de 4096 descriptores; los puntos de 128 y 256 hilos pueden fallar" >&2
fi

echo "=== nmi_watchdog: $(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo '?') ==="
echo "=== perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid) ==="
echo "=== numa_balancing: $(cat /proc/sys/kernel/numa_balancing 2>/dev/null || echo '?') ==="

srun ./run_replicas.sh
