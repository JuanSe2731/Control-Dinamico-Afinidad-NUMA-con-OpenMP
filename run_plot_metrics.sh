#!/usr/bin/env bash
#
# Genera TODAS las figuras del proyecto en un unico directorio por campana:
#   perf_out_v6 -> plots/figuras_v6      (por defecto)
#   perf_out_v5 -> plots/figuras         (los resultados anteriores, intactos)
#
# El directorio de figuras se DERIVA del de resultados, asi que las dos campanas
# nunca se pisan y no hace falta acordarse de pasar FIGDIR a mano.
#
# Antes este script escribia en plots/figures mientras el resto del proyecto decia
# plots/figuras, asi que en disco convivian dos generaciones distintas de figuras y
# era facil analizar las viejas sin darse cuenta. Ahora hay un solo destino.
#
# El analisis es LOCAL: los CSV se traen de exadell y se procesan aqui. El cluster
# no necesita seaborn ni matplotlib.
#
# Uso:
#   ./run_plot_metrics.sh                 # perf_out_v6 -> plots/figuras_v6
#   ./run_plot_metrics.sh perf_out_v5     # perf_out_v5 -> plots/figuras
#   FIGDIR=/otro/sitio ./run_plot_metrics.sh   # destino explicito

set -euo pipefail

OUTDIR="${1:-perf_out_v6}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Destino derivado del origen: perf_out_v5 -> figuras, perf_out_vN -> figuras_vN.
# Antes esto era una constante y las figuras de una campana sobrescribian las de la
# otra en silencio, que es la misma clase de fallo que el CSV en modo append.
case "$(basename "$OUTDIR")" in
  perf_out_v5) FIG_POR_DEFECTO="${ROOT_DIR}/plots/figuras" ;;
  perf_out_v*) FIG_POR_DEFECTO="${ROOT_DIR}/plots/figuras_${OUTDIR##*_}" ;;
  *)           FIG_POR_DEFECTO="${ROOT_DIR}/plots/figuras_$(basename "$OUTDIR")" ;;
esac
FIGDIR="${FIGDIR:-${FIG_POR_DEFECTO}}"
LAT_CSV="${LAT_CSV:-${ROOT_DIR}/caracterizacion/latencias_exadell.csv}"

# Comprobacion de dependencias por adelantado: sin esto, un seaborn ausente
# reventaba a mitad del recorrido y dejaba el directorio de figuras a medias.
if ! python3 -c '
import importlib.util, sys
faltan = [m for m in ("seaborn", "pandas", "matplotlib")
          if importlib.util.find_spec(m) is None]
if faltan:
    print("faltan: " + ", ".join(faltan), file=sys.stderr)
    sys.exit(1)
'; then
  echo "ERROR: faltan dependencias de Python. Instala con:" >&2
  echo "    pip install --user seaborn pandas scipy" >&2
  exit 1
fi

mkdir -p "$FIGDIR"

echo "=== Figuras de la campana (${OUTDIR}) ==="
python3 "${ROOT_DIR}/plots/plot_results.py" --outdir "${OUTDIR}" --figdir "${FIGDIR}"

# La caracterizacion de latencias se mide una sola vez y es independiente de la
# campana, asi que puede no estar todavia. No es un error.
if [[ -f "$LAT_CSV" ]]; then
  echo
  echo "=== Figuras de latencias (${LAT_CSV}) ==="
  python3 "${ROOT_DIR}/plots/plot_latencias.py" --csv "$LAT_CSV" --figdir "${FIGDIR}"
else
  echo
  echo "(sin ${LAT_CSV}: ejecuta 'sbatch caracterizacion/run_latencias.sbatch' para"
  echo " generar las figuras de latencia)"
fi

echo
echo "Todas las figuras en: ${FIGDIR}"
