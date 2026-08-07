#!/usr/bin/env bash
set -euo pipefail

# Forzar locale C: garantiza separador decimal "." en awk/printf y en los CSV,
# independiente del locale de la maquina (p.ej. es_ES usa "," y corrompe el CSV,
# ademas de truncar a 0 cualquier decimal en las comparaciones numericas de awk).
export LC_ALL=C

########################
# run_perf_metricsV4.sh
#
# Campana experimental: dinamico (scheduler OMPT) vs estatico (OMP_PROC_BIND),
# con controles que permiten DESCOMPONER la diferencia en sus tres causas.
#
# Seis configuraciones por (kernel, hilos). Las tres marcadas "control" NO son
# competidoras: existen solo para poder atribuir la diferencia observada.
#
#   id         | PROC_BIND | tool | migracion | perf stat | rol
#   -----------+-----------+------+-----------+-----------+---------------------
#   spread     | spread    | no   |    -      | agregado  | BASELINE estatico
#   close      | close     | no   |    -      | agregado  | baseline secundario
#   nobind     | (sin fijar)| no  |    -      | agregado  | control: coste de no fijar
#   obs        | (sin fijar)| si  |   OFF     |    no     | control: coste de monitorizar
#   ovh        | spread    | si   |   OFF     |    no     | control: overhead del tool
#   scheduler  | (sin fijar)| si  |   ON      |    no     | LA PROPUESTA
#
# Descomposicion (con tiempos, menor = mejor):
#   D_binding = T_nobind / T_spread - 1     coste de no fijar afinidad
#   D_instr   = T_obs    / T_nobind - 1     coste de monitorizar
#   D_migr    = T_sched  / T_obs    - 1     efecto de migrar
#   Total     = T_sched  / T_spread - 1
#
# Salidas (SOLO CSV; no se generan archivos .txt intermedios):
#   kernel_metrics.csv      una fila por corrida
#   ompt_window_metrics.csv por ventana de 100ms y por hilo (solo runs con tool)
#   ompt_summary.csv        por hilo (solo runs con tool)
#   ompt_overhead.csv       overhead intrinseco del tool (solo runs con tool)
#   metrics/<tag>.csv       CSV propio del kernel
#   metrics/<tag>_times.csv las REPS repeticiones individuales -> Welch/ANOVA
########################

# Parametros de la campana. Se pueden sobreescribir por entorno para hacer una
# campana recortada de verificacion sin tocar el script:
#     THREAD_LIST="8 128" OUTDIR=perf_out_humo ./run_perf_metricsV4.sh
#
# CUIDADO al bajar REPS: WARMUP_WINDOWS(5) x MONITOR_MS(100) = 500 ms es el tiempo
# minimo antes de que pueda producirse cualquier migracion. Si la region medida
# dura menos que eso, el scheduler NUNCA migrara y la corrida no prueba nada.
STENCIL_N="${STENCIL_N:-23500}"
SPMV_MTX="${SPMV_MTX:-stokes.mtx}"
REPS="${REPS:-150}"
read -r -a THREAD_LIST <<< "${THREAD_LIST:-8 16 32 64 128}"

OUTDIR="${OUTDIR:-perf_out_v4}"
METRICSDIR="${OUTDIR}/metrics"
OMPT_LOGDIR="${OUTDIR}/ompt_logs"

KERNEL_CSV="${OUTDIR}/kernel_metrics.csv"
OMPT_SUMMARY_CSV_FILE="${OUTDIR}/ompt_summary.csv"
OMPT_WINDOW_CSV_FILE="${OUTDIR}/ompt_window_metrics.csv"
OMPT_OVERHEAD_CSV_FILE="${OUTDIR}/ompt_overhead.csv"

CXX=clang++
CXXFLAGS=(-std=c++17 -O3 -fopenmp -ffast-math)

OMPT_TOOL_SRC="sched_NUMA_optC_leaky.cpp"
OMPT_TOOL_BIN="numa_sched_optC.so"
OMPT_TOOL_FLAGS=(-std=c++17 -fPIC -shared -fopenmp -pthread -O2)
HWLOC_INCLUDE="${HWLOC_INCLUDE:-/opt/ohpc/pub/libs/hwloc/include}"
HWLOC_LIB="${HWLOC_LIB:-/opt/ohpc/pub/libs/hwloc/lib}"

REMOTE_EVENT="rD044"
ALL_FILLS_EVENT="rFF44"

declare -a SERIAL_KERNELS=(
  "stencil_serial:Stencil_serial.cpp:stencil_serial:STENCIL"
  "spmv_serial:spmv_serial.cpp:spmv_serial:SPMV"
)

declare -a PAR_KERNELS=(
  "stencil:Stencil.cpp:stencil:STENCIL"
  "spmv_static:spmv_staticSeed.cpp:spmv_static:SPMV"
)

# id : OMP_PLACES : OMP_PROC_BIND : usa_tool : migracion : usa_perf_stat
#   "-" en places/bind = dejar SIN FIJAR (el scheduler decide la afinidad)
declare -a CONFIGS=(
  "spread:cores:spread:0:0:1"
  "close:cores:close:0:0:1"
  "nobind:-:-:0:0:1"
  "obs:-:-:1:0:0"
  "ovh:cores:spread:1:0:0"
  "scheduler:-:-:1:1:0"
)

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: Falta comando requerido: $1" >&2
    exit 1
  }
}

mkdirs() { mkdir -p "${OUTDIR}" "${METRICSDIR}" "${OMPT_LOGDIR}"; }

compile_kernel() {
  local label="$1" src="$2" bin="$3"
  if [[ ! -f "$src" ]]; then
    echo "[!] ERROR: source $src not found for $label" >&2
    return 1
  fi
  echo "[*] Compiling $label: $src -> $bin"
  "${CXX}" "${CXXFLAGS[@]}" "$src" -o "$bin"
  [[ -x "$bin" ]] || { echo "[!] ERROR: compilation failed: $bin" >&2; return 1; }
}

compile_ompt_tool() {
  [[ -f "$OMPT_TOOL_SRC" ]] || { echo "[!] ERROR: no existe $OMPT_TOOL_SRC" >&2; exit 1; }
  echo "[*] Compiling OMPT tool: $OMPT_TOOL_SRC -> $OMPT_TOOL_BIN"
  "${CXX}" "${OMPT_TOOL_FLAGS[@]}" "$OMPT_TOOL_SRC" -o "$OMPT_TOOL_BIN" \
    -I"${HWLOC_INCLUDE}" -L"${HWLOC_LIB}" -lhwloc
  [[ -f "$OMPT_TOOL_BIN" ]] || { echo "[!] ERROR: fallo compilando el tool" >&2; exit 1; }
}

compile_all() {
  compile_ompt_tool
  local list=("${PAR_KERNELS[@]}")
  [[ "${RUN_SERIAL:-0}" == "1" ]] && list+=("${SERIAL_KERNELS[@]}")
  for kernel_spec in "${list[@]}"; do
    IFS=':' read -r label src bin _ktype <<< "$kernel_spec"
    compile_kernel "$label" "$src" "$bin" || { echo "[!] FATAL: $label" >&2; exit 1; }
  done
}

build_cmd() {
  # Construye el arreglo global 'cmd' de forma SEGURA para rutas con espacios.
  # NO usar una cadena + read -a: el word-splitting parte csv_prefix en el
  # espacio, el kernel escribe su CSV en otro sitio y todas las columnas de
  # rendimiento salen vacias (fue el fallo del job 29168).
  local ktype="$1" bin="$2" threads="$3" csv_prefix="$4"
  if [[ "$ktype" == "STENCIL" ]]; then
    cmd=("./${bin}" "${STENCIL_N}" "${threads}" "${REPS}" "${csv_prefix}")
  else
    cmd=("./${bin}" "${SPMV_MTX}" "${threads}" "${REPS}" "${csv_prefix}")
  fi
}

# perf stat AGREGADO (sin -I): una sola linea por evento al final de la corrida.
# Antes se muestreaba cada 100 ms y se volcaba a un .txt que luego se parseaba,
# generando ~26k filas por campana que no entraban en ningun resultado. Aqui se
# parsea por tuberia, sin archivo intermedio, y se emite "ipc ratio_rm".
# Los kernels acotan la region contada con prctl(PR_TASK_PERF_EVENTS_*), asi que
# el agregado corresponde SOLO al bucle medido.
# Emite tres campos: "ipc ratio_rm rc". El codigo de salida se DEVUELVE en la
# salida en vez de contarse aqui dentro, porque esta funcion se invoca dentro de
# $(...) — un subshell — y cualquier variable que incrementase aqui se perderia
# al volver al padre.
run_with_perf_aggregate() {
  local out rc=0
  out="$(perf stat -x, -e "${REMOTE_EVENT},${ALL_FILLS_EVENT},instructions,cycles" \
          -- "$@" 2>&1 >/dev/null)" || rc=$?
  printf '%s\n' "$out" | awk -F',' -v rc="$rc" '
    # Formato de "perf stat -x," SIN -I: $1=cuenta, $3=nombre del evento.
    # (Con -I el nombre cae en $4 porque $1 es la marca de tiempo.)
    $1 ~ /^[0-9]+$/ {
      if      ($3 == "'"${REMOTE_EVENT}"'")    rm  = $1;
      else if ($3 == "'"${ALL_FILLS_EVENT}"'") all = $1;
      else if ($3 == "instructions")           ins = $1;
      else if ($3 == "cycles")                 cyc = $1;
    }
    END {
      ipc   = (cyc > 0) ? ins / cyc : -1;
      ratio = (all > 0) ? rm  / all : -1;
      printf "%.6f %.6f %d\n", ipc, ratio, rc;
    }'
}

# Lee el CSV del kernel por NOMBRE de columna (no por posicion): stencil y spmv
# tienen esquemas distintos y spmv no emite mlups.
extract_kernel_metrics() {
  local csv="$1"
  if [[ ! -f "$csv" ]]; then
    echo "NA NA NA NA NA NA NA"
    return
  fi
  awk -F',' '
    NR==1 { for (i=1;i<=NF;i++) idx[$i]=i; next }
    NR==2 {
      printf "%s %s %s %s %s %s %s\n",
        get("min_ms"), get("avg_ms"), get("max_ms"), get("stddev_ms"),
        get("bw_gibs"), get("mlups_min"), get("mlups_avg");
      found = 1;
      exit
    }
    # Si el kernel murio tras crear el CSV, este END evita que la fila salga con
    # columnas VACIAS, indistinguibles de una medida buena.
    END { if (!found) print "NA NA NA NA NA NA NA" }
    function get(name) { return (name in idx) ? $(idx[name]) : "NA" }
  ' "$csv"
}

# Agrega por tag desde ompt_summary.csv: migraciones totales, IPC y ratio_rm.
# El IPC agregado es sum(instr)/sum(cyc) — NO el promedio de la columna ipc,
# que seria una media sin ponderar.
ompt_aggregate_for_tag() {
  local tag="$1"
  if [[ ! -f "$OMPT_SUMMARY_CSV_FILE" ]]; then
    echo "0 NA NA"
    return
  fi
  awk -F',' -v tag="$tag" '
    NR==1 { for (i=1;i<=NF;i++) idx[$i]=i; next }
    $(idx["tag"])==tag {
      mig += $(idx["migrations"]);
      ins += $(idx["instructions"]);
      cyc += $(idx["cycles"]);
      rm  += $(idx["remote_fills"]);
      all += $(idx["all_fills"]);
      n++;
    }
    END {
      if (n == 0) { print "0 NA NA"; exit }
      printf "%d %s %s\n", mig,
        (cyc > 0 ? sprintf("%.6f", ins/cyc) : "NA"),
        (all > 0 ? sprintf("%.6f", rm/all)  : "NA");
    }' "$OMPT_SUMMARY_CSV_FILE"
}

append_kernel_summary() {
  local config="$1" threads="$2" binding="$3" tool="$4" migration="$5"
  local migrations="$6" ipc="$7" ratio="$8" kernel_csv="$9"
  local min_ms avg_ms max_ms stdev_ms bw_gibs mlups_min mlups_avg
  read -r min_ms avg_ms max_ms stdev_ms bw_gibs mlups_min mlups_avg \
    <<< "$(extract_kernel_metrics "$kernel_csv")"
  echo "${config},${threads},${binding},${tool},${migration},${min_ms},${avg_ms},${max_ms},${stdev_ms},${migrations},${bw_gibs},${mlups_min},${mlups_avg},${ipc},${ratio}" \
    >> "$KERNEL_CSV"
}

need_cmd perf
need_cmd "${CXX}"
need_cmd numactl

mkdirs
compile_all

rm -f "$KERNEL_CSV" "$OMPT_SUMMARY_CSV_FILE" "$OMPT_WINDOW_CSV_FILE" "$OMPT_OVERHEAD_CSV_FILE"
echo "config,threads,binding,tool,migration,min_ms,avg_ms,max_ms,stdev_ms,migrations,bw_gibs,mlups_min,mlups_avg,ipc,ratio_rm" > "$KERNEL_CSV"

unset OMP_TOOL OMP_TOOL_LIBRARIES OMPT_LOG_FILE OMPT_TAG OMPT_DISABLE_MIGRATION
RUN_FAILED=0
CURRENT_TAG=""

# ── Etapa opcional: seriales ────────────────────────────────────────────────
# Ya NO son la linea base del proyecto: la comparacion es dinamico vs estatico.
# Se conservan tras RUN_SERIAL=1 por si hacen falta puntualmente.
if [[ "${RUN_SERIAL:-0}" == "1" ]]; then
  echo "[*] Etapa opcional: seriales"
  for threads in "${THREAD_LIST[@]}"; do
    for kernel_spec in "${SERIAL_KERNELS[@]}"; do
      IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
      tag="${label}_t${threads}_serial"
      csv_prefix="${METRICSDIR}/${tag}"
      export OMP_NUM_THREADS="${threads}" OMP_DYNAMIC="FALSE"
      unset OMP_PLACES OMP_PROC_BIND
      build_cmd "$ktype" "$bin" "$threads" "$csv_prefix"
      read -r ipc ratio rc <<< "$(run_with_perf_aggregate "${cmd[@]}")"
      [[ "$rc" -eq 0 ]] || { echo "[!] FALLO: ${tag} codigo ${rc}" >&2; RUN_FAILED=$(( RUN_FAILED + 1 )); }
      append_kernel_summary "${label}_serial" "$threads" "none" "no" "no" \
        "0" "$ipc" "$ratio" "${csv_prefix}.csv"
    done
  done
  echo ""
fi

# ── Campana principal: 6 configuraciones x 2 kernels x |THREAD_LIST| ────────
total=$(( ${#CONFIGS[@]} * ${#PAR_KERNELS[@]} * ${#THREAD_LIST[@]} ))
i=0
for threads in "${THREAD_LIST[@]}"; do
  for cfg in "${CONFIGS[@]}"; do
    IFS=':' read -r cfg_id places bind use_tool migrate use_perf <<< "$cfg"
    for kernel_spec in "${PAR_KERNELS[@]}"; do
      IFS=':' read -r label _src bin ktype <<< "$kernel_spec"
      i=$(( i + 1 ))
      tag="${label}_t${threads}_${cfg_id}"
      csv_prefix="${METRICSDIR}/${tag}"
      echo "[${i}/${total}] ${tag}"
      CURRENT_TAG="$tag"

      export OMP_NUM_THREADS="${threads}" OMP_DYNAMIC="FALSE"
      if [[ "$places" == "-" ]]; then
        unset OMP_PLACES OMP_PROC_BIND
      else
        export OMP_PLACES="$places" OMP_PROC_BIND="$bind"
      fi

      if [[ "$use_tool" == "1" ]]; then
        export OMP_TOOL="enabled"
        export OMP_TOOL_LIBRARIES="$(realpath "$OMPT_TOOL_BIN")"
        export OMPT_WINDOW_CSV="${OMPT_WINDOW_CSV_FILE}"
        export OMPT_SUMMARY_CSV="${OMPT_SUMMARY_CSV_FILE}"
        export OMPT_OVERHEAD_CSV="${OMPT_OVERHEAD_CSV_FILE}"
        export OMPT_LOG_FILE="${OMPT_LOGDIR}/${tag}.log"
        export OMPT_TAG="${tag}"
        if [[ "$migrate" == "1" ]]; then
          unset OMPT_DISABLE_MIGRATION
        else
          export OMPT_DISABLE_MIGRATION=1
        fi
      else
        unset OMP_TOOL OMP_TOOL_LIBRARIES OMPT_WINDOW_CSV OMPT_SUMMARY_CSV \
              OMPT_OVERHEAD_CSV OMPT_LOG_FILE OMPT_TAG OMPT_DISABLE_MIGRATION
      fi

      build_cmd "$ktype" "$bin" "$threads" "$csv_prefix"

      ipc="NA"; ratio="NA"; migrations="0"
      if [[ "$use_perf" == "1" ]]; then
        # Sin tool: los contadores agregados de perf son la unica fuente de IPC
        # y ratio_rm para las configuraciones estaticas.
        read -r ipc ratio rc <<< "$(run_with_perf_aggregate numactl --interleave=all -- "${cmd[@]}")"
        [[ "$rc" -eq 0 ]] || { echo "[!] FALLO: ${tag} codigo ${rc}" >&2; RUN_FAILED=$(( RUN_FAILED + 1 )); }
      else
        # Con tool: NO se usa perf stat, para no competir por los registros de la
        # PMU con los contadores por hilo que abre el propio scheduler. El IPC y
        # el ratio salen agregados de ompt_summary.csv.
        rc=0
        numactl --interleave=all -- "${cmd[@]}" >/dev/null 2>/dev/null || rc=$?
        if [[ "$rc" -ne 0 ]]; then
          echo "[!] FALLO: ${tag} devolvio codigo ${rc} (revisar ${OMPT_LOGDIR}/${tag}.log)" >&2
          RUN_FAILED=$(( RUN_FAILED + 1 ))
        fi
        read -r migrations ipc ratio <<< "$(ompt_aggregate_for_tag "$tag")"
      fi

      binding_label="$([[ "$places" == "-" ]] && echo "none" || echo "$bind")"
      tool_label="$([[ "$use_tool" == "1" ]] && echo "ompt" || echo "no")"
      mig_label="$([[ "$use_tool" != "1" ]] && echo "-" || { [[ "$migrate" == "1" ]] && echo "on" || echo "off"; })"

      append_kernel_summary "${label}_${cfg_id}" "$threads" "$binding_label" \
        "$tool_label" "$mig_label" "$migrations" "$ipc" "$ratio" "${csv_prefix}.csv"
    done
  done
done

echo ""
if [[ "$RUN_FAILED" -gt 0 ]]; then
  echo "[!] ATENCION: ${RUN_FAILED} de ${total} corridas fallaron; sus filas llevan NA."
fi
echo "[OK] Campana completada: ${total} corridas (${RUN_FAILED} fallidas)."
echo "     Resumen por corrida : ${KERNEL_CSV}"
echo "     Ventanas OMPT       : ${OMPT_WINDOW_CSV_FILE}"
echo "     Resumen por hilo    : ${OMPT_SUMMARY_CSV_FILE}"
echo "     Overhead del tool   : ${OMPT_OVERHEAD_CSV_FILE}"
echo "     Tiempos por rep     : ${METRICSDIR}/<tag>_times.csv"
