#!/usr/bin/env bash
#
# Campaña V5 — caso base = OpenMP puro, barrido de tamaños por nivel de caché.
#
# QUÉ CAMBIA RESPECTO A V4
# ------------------------
# 1. EL CASO BASE ES OpenMP PURO: sin OMP_PLACES, sin OMP_PROC_BIND, sin numactl.
#    En V4 las SEIS configuraciones corrían bajo `numactl --interleave=all`, así que
#    no había ninguna medida del comportamiento por defecto de OpenMP. La afinidad y
#    la política de memoria pasan a ser optimizaciones estáticas con su propia línea,
#    y el scheduler se compara contra el base igual que ellas.
#
# 2. EL SCHEDULER TAMPOCO LLEVA numactl. Corre con first-touch natural, como el base.
#    Bajo interleave las páginas ya están repartidas y no hay desequilibrio que
#    corregir: se le estaba pidiendo que arreglara algo ya arreglado.
#
# 3. `perf stat` DESAPARECE. Contaba el proceso entero, incluida la generación de la
#    matriz, y por eso el IPC de las configuraciones sin tool estaba declarado
#    inutilizable (2.648 medido frente a 1.277 real). Ahora todas las configuraciones
#    se miden con el MISMO instrumento, acotado a la región: perf_region.hpp cuando no
#    hay tool, el propio tool OMPT cuando lo hay. Nunca los dos a la vez.
#
# 4. EL TAMAÑO ES UN PARÁMETRO BARRIDO (S0..S5), escalonado sobre la jerarquía real de
#    exadell: L1d de un CCD, L2 de un CCD, L3 de UN CCD (32 MiB), L3 de un nodo
#    (256 MiB), por encima de la L3 de la máquina (512 MiB), y DRAM. El par S2/S3 es
#    el experimento que aísla el efecto de los chiplets.
#
# 5. 256 HILOS. 128 = todos los núcleos físicos; 256 = SMT completo (dos hilos por
#    núcleo compartiendo L1 y L2), que no es "el doble de máquina".
#
# 6. REANUDABLE. Un marcador .done por (tamaño, config, hilos, kernel): 576 ejecuciones
#    no caben en una ventana de 24 h, así que el trabajo se relanza sin repetir nada.
#
# 7. stokes.mtx SE DESCARTA. Todos los peldaños salen del generador con semilla fija.
#
# Uso:
#   ./run_perf_metricsV5.sh
#   SIZES="S2 S3" THREAD_LIST="8 128" ./run_perf_metricsV5.sh
#   FORCE=1 ./run_perf_metricsV5.sh          # ignora los marcadores .done
#   OUTDIR=/otro/sitio ./run_perf_metricsV5.sh

set -euo pipefail

# LC_ALL=C para que awk y printf emitan SIEMPRE punto decimal. Con una configuración
# regional de coma (es_ES.UTF-8) los CSV salen corruptos y los scripts de figuras
# revientan. El cluster está en C, pero la ruta de ejecución local lo necesita.
export LC_ALL=C

# ── Parámetros de la campaña ──────────────────────────────────────────────────
REPS="${REPS:-150}"
read -r -a THREAD_LIST <<< "${THREAD_LIST:-8 16 32 64 128 256}"
read -r -a SIZE_LIST   <<< "${SIZES:-S0 S1 S2 S3 S4 S5}"
export WARMUP_ITERS="${WARMUP_ITERS:-5}"
export SPMV_SEED="${SPMV_SEED:-42}"
export SPMV_AVG_NNZ="${SPMV_AVG_NNZ:-32}"

# Familia de eventos de relleno. Son DOS magnitudes distintas y cada una pide la
# suya, y no caben a la vez (6 mascaras x 2 familias = 12 eventos sobre 6 PMC):
#
#   any   ls_any_fills_from_sys  (0x44)  demanda + prefetch hw + prefetch sw
#         -> es la del TRAFICO: un prefetch consume enlace igual que una demanda.
#            Es tambien la variable de decision del scheduler, asi que dejarla en
#            `any` mantiene la comparabilidad con las campanas 29223 y 29311.
#
#   dmnd  ls_dmnd_fills_from_sys (0x43)  SOLO demanda
#         -> es la del TIEMPO MUERTO: un prefetch que llega a tiempo no para al
#            nucleo, asi que multiplicar rellenos `any` por la latencia remota
#            sobreestimaria la espera.
#
# La campana principal corre con `any`. Para la figura de tiempo muerto basta una
# segunda tanda corta:
#     PERF_FAMILIA=dmnd SIZES="S3 S5" ./run_perf_metricsV5.sh
# El sufijo del tag evita que las dos tandas se pisen los marcadores .done.
export PERF_FAMILIA="${PERF_FAMILIA:-any}"
if [[ "$PERF_FAMILIA" == "dmnd" ]]; then
  SUFIJO_FAMILIA="_dmnd"
else
  PERF_FAMILIA="any"
  SUFIJO_FAMILIA=""
fi

# ── Escalera de tamaños ───────────────────────────────────────────────────────
# tag : N del Stencil : filas del SpMV
#
# Working set del Stencil = 2*N^2*4 B; del SpMV = filas*(12*avg_nnz + 20) B con
# avg_nnz=32, o sea ~404 B por fila. Los dos kernels quedan emparejados peldaño a
# peldaño para que las figuras sean comparables.
#
#   S0  L1d agregada de un CCD (256 KiB)      253 KiB
#   S1  L2 agregada de un CCD (8 MiB)        3.74 MiB
#   S2  L3 de UN CCD (32 MiB)                24.7 MiB   <-- cabe en un chiplet
#   S3  L3 de un nodo NUMA (256 MiB)        190.7 MiB   <-- obliga a cruzar chiplets
#   S4  por encima de la L3 de la maquina   1008 MiB
#   S5  DRAM / NUMA                         4.11 GiB
#
# Aviso declarado, no escondido: en S0 con 128 o 256 hilos hay 2-5 filas por hilo y el
# punto lo domina el coste de abrir la region paralela. Es el dato que muestra donde
# deja de compensar paralelizar, no un fallo de medida.
declare -a SIZES_DEF=(
  "S0:180:640"
  "S1:700:9700"
  "S2:1800:64000"
  "S3:5000:495000"
  "S4:11500:2616000"
  "S5:23500:10923000"
)

# ── Configuraciones ───────────────────────────────────────────────────────────
# id : OMP_PLACES : OMP_PROC_BIND : numactl_interleave : usa_tool : migracion
#   "-" en places/bind = dejar SIN FIJAR
declare -a CONFIGS=(
  "base:-:-:0:0:0"
  "bind_close:cores:close:0:0:0"
  "bind_spread:cores:spread:0:0:0"
  "interleave:-:-:1:0:0"
  "bind_close_il:cores:close:1:0:0"
  "bind_spread_il:cores:spread:1:0:0"
  "obs:-:-:0:1:0"
  "scheduler:-:-:0:1:1"
)

# ── Rutas de salida ───────────────────────────────────────────────────────────
OUTDIR="${OUTDIR:-perf_out_v5}"
METRICSDIR="${OUTDIR}/metrics"
LOGDIR="${OUTDIR}/ompt_logs"
DONEDIR="${OUTDIR}/.done"
KERNEL_CSV="${OUTDIR}/kernel_metrics.csv"
OMPT_WINDOW_CSV_PATH="${OUTDIR}/ompt_window_metrics.csv"
OMPT_SUMMARY_CSV_PATH="${OUTDIR}/ompt_summary.csv"
OMPT_OVERHEAD_CSV_PATH="${OUTDIR}/ompt_overhead.csv"

# ── Toolchain ─────────────────────────────────────────────────────────────────
CXX="${CXX:-clang++}"
CXXFLAGS_KERNEL="-std=c++17 -O3 -fopenmp -ffast-math"
CXXFLAGS_TOOL="-std=c++17 -fPIC -shared -fopenmp -pthread -O2"
TOOL_SRC="sched_NUMA_optC_leaky.cpp"
TOOL_SO="numa_sched_optC.so"
HWLOC_INCLUDE="${HWLOC_INCLUDE:-/opt/ohpc/pub/libs/hwloc/include}"
HWLOC_LIB="${HWLOC_LIB:-/opt/ohpc/pub/libs/hwloc/lib}"

declare -a PAR_KERNELS=(
  "stencil:Stencil.cpp:stencil:STENCIL"
  "spmv_static:spmv_staticSeed.cpp:spmv_static:SPMV"
)

# ── Utilidades ────────────────────────────────────────────────────────────────
need_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "ERROR: falta '$1'"; exit 1; }; }

compile_all() {
  echo "=== Compilando ==="
  $CXX $CXXFLAGS_TOOL "$TOOL_SRC" -o "$TOOL_SO" \
      -I"$HWLOC_INCLUDE" -L"$HWLOC_LIB" -lhwloc
  for spec in "${PAR_KERNELS[@]}"; do
    IFS=':' read -r _label src bin _ktype <<< "$spec"
    $CXX $CXXFLAGS_KERNEL "$src" -o "$bin"
  done
  echo "    ok"
}

# Construye el arreglo global 'cmd'. NO usar una cadena + read -a: el
# word-splitting parte csv_prefix en el espacio, el kernel escribe su CSV en otro
# sitio y todas las columnas de rendimiento salen vacias (fue el fallo del job
# 29168). El primer argumento es un NUMERO en los dos kernels: N para el stencil,
# filas para el SpMV, que lo distingue solo de una ruta .mtx.
build_cmd() {
  local ktype="$1" bin="$2" tam="$3" threads="$4" csv_prefix="$5"
  cmd=("./${bin}" "${tam}" "${threads}" "${REPS}" "${csv_prefix}")
  [[ "$ktype" == "STENCIL" || "$ktype" == "SPMV" ]] || { echo "ktype desconocido"; exit 1; }
}

# Lee una columna por NOMBRE de la fila 2 de un CSV de una sola fila de datos.
csv_get() {
  local file="$1" col="$2"
  [[ -f "$file" ]] || { echo "NA"; return; }
  awk -F, -v c="$col" '
    NR==1 { for (i=1;i<=NF;i++) if ($i==c) k=i; next }
    NR==2 { print (k>0 && $k!="") ? $k : "NA"; found=1; exit }
    END   { if (!found) print "NA" }
  ' "$file"
}

# Agrega los contadores por hilo y emite:
#   ipc  ratio_rm  fill_l2 fill_l3_ccd fill_ccd_vecino fill_dram_local
#   fill_far_cache fill_far_dram  migraciones
#
# Un solo agregador para los DOS caminos de medida, porque perf_region.hpp y el tool
# OMPT emiten los mismos nombres de columna a proposito. Suma por columna y calcula
# los cocientes sobre las SUMAS (sum(ins)/sum(cyc)), no la media de los cocientes por
# hilo: un hilo casi inactivo no debe pesar igual que uno cargado.
aggregate_counters() {
  local file="$1" tag_filter="${2:-}"
  if [[ ! -f "$file" ]]; then
    echo "NA NA 0 0 0 0 0 0 0"; return
  fi
  awk -F, -v tf="$tag_filter" '
    # Devuelve la columna por NOMBRE, o 0 si no existe. Sin esta guarda, un nombre
    # ausente daria idx=0 y $0 seria la LINEA ENTERA, que awk convertiria a numero
    # en silencio y envenenaria la suma.
    function col(nombre,   i) { i = idx[nombre]; return (i > 0) ? $i + 0 : 0 }
    function txt(nombre,    i) { i = idx[nombre]; return (i > 0) ? $i : "" }

    NR==1 { for (i=1;i<=NF;i++) idx[$i]=i; next }
    {
      if (tf != "" && txt("tag") != tf) next
      ins += col("instructions")
      cyc += col("cycles")
      l2  += col("fill_l2")
      l3  += col("fill_l3_ccd")
      cv  += col("fill_ccd_vecino")
      dl  += col("fill_dram_local")
      fc  += col("fill_far_cache")
      fd  += col("fill_far_dram")
      mig += col("migrations")
      n++
    }
    END {
      if (n == 0) { print "NA NA 0 0 0 0 0 0 0"; exit }
      all = l2 + l3 + cv + dl + fc + fd
      rem = fc + fd
      printf "%s %s %.0f %.0f %.0f %.0f %.0f %.0f %d\n",
        (cyc > 0 ? sprintf("%.6f", ins/cyc) : "NA"),
        (all > 0 ? sprintf("%.6f", rem/all) : "NA"),
        l2, l3, cv, dl, fc, fd, mig
    }
  ' "$file"
}

# ── Preflight ─────────────────────────────────────────────────────────────────
need_cmd "$CXX"
need_cmd numactl
need_cmd awk

mkdir -p "$OUTDIR" "$METRICSDIR" "$LOGDIR" "$DONEDIR"
compile_all

# Cabecera del CSV agregado. Solo se escribe si el fichero no existe: la campaña es
# reanudable y un `rm -f` aqui borraria el trabajo de las tandas anteriores.
if [[ ! -s "$KERNEL_CSV" ]]; then
  echo "kernel,size_tag,n_or_rows,ws_bytes,threads,config,binding,memory_policy,tool,migration,familia,warmup,reps,min_ms,avg_ms,max_ms,stdev_ms,migrations,caudal_util_gibs,mlups,gflops,ipc,fills_l2,fills_l3_ccd,fills_ccd_vecino,fills_dram_local,fills_far_cache,fills_far_dram,bw_remoto_gibs,gib_remotos_total,ratio_rm" > "$KERNEL_CSV"
fi

TOOL_ABS="$(realpath "$TOOL_SO")"

total=$(( ${#CONFIGS[@]} * ${#PAR_KERNELS[@]} * ${#THREAD_LIST[@]} * ${#SIZE_LIST[@]} ))
hecho=0
saltados=0
fallidos=0

echo
echo "=== Campana V5: ${total} ejecuciones ==="
echo "    tamanos : ${SIZE_LIST[*]}"
echo "    hilos   : ${THREAD_LIST[*]}"
echo "    configs : ${#CONFIGS[@]}"
echo "    reps=${REPS}  warmup=${WARMUP_ITERS}  semilla=${SPMV_SEED}  avg_nnz=${SPMV_AVG_NNZ}"
echo "    familia de rellenos: ${PERF_FAMILIA}  (any=trafico, dmnd=tiempo muerto)"
echo

for size_tag in "${SIZE_LIST[@]}"; do
  # Localiza la definición del peldaño
  stencil_n=""; spmv_rows=""
  for sd in "${SIZES_DEF[@]}"; do
    IFS=':' read -r st sn sr <<< "$sd"
    if [[ "$st" == "$size_tag" ]]; then stencil_n="$sn"; spmv_rows="$sr"; fi
  done
  if [[ -z "$stencil_n" ]]; then
    echo "AVISO: tamano '$size_tag' desconocido, se salta"; continue
  fi

  for threads in "${THREAD_LIST[@]}"; do
    for cfg in "${CONFIGS[@]}"; do
      IFS=':' read -r cfg_id places bind use_il use_tool migrate <<< "$cfg"

      for kernel_spec in "${PAR_KERNELS[@]}"; do
        IFS=':' read -r label src bin ktype <<< "$kernel_spec"
        (( ++hecho ))

        tag="${label}_${size_tag}_t${threads}_${cfg_id}${SUFIJO_FAMILIA}"
        marca="${DONEDIR}/${tag}"
        if [[ -f "$marca" && "${FORCE:-0}" != "1" ]]; then
          (( ++saltados ))
          continue
        fi

        csv_prefix="${METRICSDIR}/${tag}"
        if [[ "$ktype" == "STENCIL" ]]; then tam="$stencil_n"; else tam="$spmv_rows"; fi

        echo "[${hecho}/${total}] ${tag}"

        # ── Entorno de la configuración ───────────────────────────────────────
        export OMP_NUM_THREADS="${threads}" OMP_DYNAMIC="FALSE"
        if [[ "$places" == "-" ]]; then
          unset OMP_PLACES OMP_PROC_BIND
        else
          export OMP_PLACES="$places" OMP_PROC_BIND="$bind"
        fi

        if [[ "$use_tool" == "1" ]]; then
          export OMP_TOOL=enabled
          export OMP_TOOL_LIBRARIES="$TOOL_ABS"
          export OMPT_TAG="$tag"
          export OMPT_WINDOW_CSV="$OMPT_WINDOW_CSV_PATH"
          export OMPT_SUMMARY_CSV="$OMPT_SUMMARY_CSV_PATH"
          export OMPT_OVERHEAD_CSV="$OMPT_OVERHEAD_CSV_PATH"
          export OMPT_LOG_FILE="${LOGDIR}/${tag}.log"
          if [[ "$migrate" == "1" ]]; then
            unset OMPT_DISABLE_MIGRATION
          else
            export OMPT_DISABLE_MIGRATION=1
          fi
        else
          unset OMP_TOOL OMP_TOOL_LIBRARIES OMPT_TAG OMPT_WINDOW_CSV \
                OMPT_SUMMARY_CSV OMPT_OVERHEAD_CSV OMPT_LOG_FILE \
                OMPT_DISABLE_MIGRATION
        fi

        # ── Ejecución ─────────────────────────────────────────────────────────
        build_cmd "$ktype" "$bin" "$tam" "$threads" "$csv_prefix"
        rc=0
        if [[ "$use_il" == "1" ]]; then
          numactl --interleave=all -- "${cmd[@]}" >/dev/null 2>/dev/null || rc=$?
        else
          "${cmd[@]}" >/dev/null 2>/dev/null || rc=$?
        fi
        if [[ $rc -ne 0 ]]; then
          echo "    FALLO rc=$rc"
          (( ++fallidos ))
        fi

        # ── Recolección ───────────────────────────────────────────────────────
        kcsv="${csv_prefix}.csv"
        min_ms=$(csv_get "$kcsv" min_ms)
        avg_ms=$(csv_get "$kcsv" avg_ms)
        max_ms=$(csv_get "$kcsv" max_ms)
        stdev_ms=$(csv_get "$kcsv" stddev_ms)
        ws_bytes=$(csv_get "$kcsv" ws_bytes)
        caudal=$(csv_get "$kcsv" caudal_util_gibs)
        gflops=$(csv_get "$kcsv" gflops)
        mlups=$(csv_get "$kcsv" mlups)          # NA en SpMV, por diseño
        warm=$(csv_get "$kcsv" warmup)

        # Contadores: del tool si estaba cargado, del propio kernel si no. Nunca de
        # los dos: un solo propietario por hilo.
        if [[ "$use_tool" == "1" ]]; then
          read -r ipc ratio l2 l3 cv dl fc fd migraciones \
            <<< "$(aggregate_counters "$OMPT_SUMMARY_CSV_PATH" "$tag")"
        else
          read -r ipc ratio l2 l3 cv dl fc fd migraciones \
            <<< "$(aggregate_counters "${csv_prefix}_counters.csv" "")"
          migraciones=0
        fi

        # Tráfico REAL hacia memoria: rellenos * 64 B (tamaño de línea). Esta es la
        # magnitud del director, y aquí MENOS ES MEJOR: menos tráfico remoto es menos
        # tiempo muerto esperando datos. No confundir con caudal_util_gibs, que es
        # 1/tiempo con unidades de ancho de banda.
        read -r bw_remoto gib_remotos <<< "$(awk -v fc="$fc" -v fd="$fd" -v ms="$avg_ms" '
          BEGIN {
            rem = fc + fd
            gib = rem * 64.0 / 1073741824.0
            if (ms ~ /^[0-9.]+$/ && ms > 0) printf "%.6f %.6f\n", gib / (ms/1000.0), gib
            else                            printf "NA %.6f\n", gib
          }')"

        binding_label="$([[ "$places" == "-" ]] && echo "none" || echo "$bind")"
        mempol_label="$([[ "$use_il" == "1" ]] && echo "interleave" || echo "first-touch")"
        tool_label="$([[ "$use_tool" == "1" ]] && echo "ompt" || echo "no")"
        mig_label="$([[ "$use_tool" != "1" ]] && echo "-" || { [[ "$migrate" == "1" ]] && echo "on" || echo "off"; })"

        echo "${label},${size_tag},${tam},${ws_bytes},${threads},${cfg_id},${binding_label},${mempol_label},${tool_label},${mig_label},${PERF_FAMILIA},${warm},${REPS},${min_ms},${avg_ms},${max_ms},${stdev_ms},${migraciones},${caudal},${mlups},${gflops},${ipc},${l2},${l3},${cv},${dl},${fc},${fd},${bw_remoto},${gib_remotos},${ratio}" \
          >> "$KERNEL_CSV"

        [[ $rc -eq 0 ]] && touch "$marca"
      done
    done
  done
done

echo
echo "=== Terminado ==="
echo "    ejecutadas : $(( hecho - saltados ))"
echo "    saltadas   : ${saltados}  (marcador .done; FORCE=1 para rehacerlas)"
echo "    fallidas   : ${fallidos}"
echo "    resultados : ${KERNEL_CSV}"
