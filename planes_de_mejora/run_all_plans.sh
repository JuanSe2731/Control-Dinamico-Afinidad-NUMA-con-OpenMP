#!/usr/bin/env bash
# =============================================================================
# run_all_plans.sh
#
# Orquesta TODAS las pruebas de los planes de mejora con UN solo envio.
# Por cada plan: entra a su carpeta, compila su scheduler/kernels propios y
# corre el benchmark completo (run_perf_metricsV4.sh) escribiendo a
# 'results/<PLAN>/'. NO toca el directorio principal del proyecto.
#
# Es REANUDABLE: si el job se corta por tiempo, vuelve a enviarlo y saltara
# los planes ya terminados (marca 'results/<PLAN>/.done'). Para re-correr un
# plan, borra su .done (o toda su carpeta en results/).
#
# Uso:
#   ./run_all_plans.sh                # todos los planes, en orden
#   PLANS="B1 C1" ./run_all_plans.sh  # solo algunos
#   FORCE=1 ./run_all_plans.sh        # ignora los .done y re-corre todo
# =============================================================================
set -uo pipefail
export LC_ALL=C

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS="${HERE}/results"
mkdir -p "${RESULTS}"

# Orden planteado: referencia primero, luego lo barato/aislado, luego lo estructural.
DEFAULT_PLANS="BASE B1 C1 B3 C3 B2 C2 A1 A2 D1"
PLANS="${PLANS:-$DEFAULT_PLANS}"
FORCE="${FORCE:-0}"
RUN_SERIAL="${RUN_SERIAL:-0}"   # 0 = excluir etapa 1 (serial) en las pruebas

echo "============================================================"
echo " Orquestador de planes de mejora"
echo " Planes: ${PLANS}"
echo " Resultados en: ${RESULTS}"
echo " Inicio: $(date)"
echo "============================================================"

overall_start=$(date +%s)
declare -a OK_PLANS=()
declare -a FAIL_PLANS=()

for plan in ${PLANS}; do
  pdir="${HERE}/${plan}"
  outdir="${RESULTS}/${plan}"

  if [[ ! -d "${pdir}" ]]; then
    echo "[!] Plan ${plan}: carpeta no existe (${pdir}) — se omite"
    FAIL_PLANS+=("${plan}(sin-carpeta)")
    continue
  fi

  if [[ "${FORCE}" != "1" && -f "${outdir}/.done" ]]; then
    echo "[=] Plan ${plan}: ya completado (.done) — se salta"
    OK_PLANS+=("${plan}")
    continue
  fi

  echo ""
  echo "------------------------------------------------------------"
  echo "[*] Plan ${plan}: iniciando  ($(date))"
  echo "------------------------------------------------------------"
  mkdir -p "${outdir}"
  t0=$(date +%s)

  # Cada plan corre su PROPIO run_perf_metricsV4.sh dentro de su carpeta,
  # con OUTDIR apuntando a results/<PLAN> (ruta absoluta).
  if ( cd "${pdir}" && OUTDIR="${outdir}" RUN_SERIAL="${RUN_SERIAL}" ./run_perf_metricsV4.sh ); then
    t1=$(date +%s)
    touch "${outdir}/.done"
    echo "[OK] Plan ${plan}: terminado en $(( t1 - t0 )) s"
    OK_PLANS+=("${plan}")
  else
    echo "[!] Plan ${plan}: FALLO (continuo con el resto)"
    FAIL_PLANS+=("${plan}")
  fi
done

overall_end=$(date +%s)
echo ""
echo "============================================================"
echo " Resumen"
echo "   OK   : ${OK_PLANS[*]:-(ninguno)}"
echo "   FALLO: ${FAIL_PLANS[*]:-(ninguno)}"
echo "   Tiempo total: $(( overall_end - overall_start )) s"
echo "   Fin: $(date)"
echo "============================================================"
echo ""
echo "Siguiente paso — analizar y comparar:"
echo "   python3 \"${HERE}/analyze_plans.py\" --results \"${RESULTS}\""
