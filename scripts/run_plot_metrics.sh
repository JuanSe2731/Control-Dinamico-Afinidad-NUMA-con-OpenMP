#!/usr/bin/env bash
#
# Genera TODAS las figuras de la campana en un unico directorio: data/figuras.
#
# Antes este script escribia en plots/figures mientras el resto del proyecto decia
# plots/figuras, asi que en disco convivian dos generaciones distintas de figuras y
# era facil analizar las viejas sin darse cuenta. Ahora hay un solo destino.
#
# El analisis es LOCAL: los CSV se traen de exadell y se procesan aqui. El cluster
# no necesita seaborn ni matplotlib.
#
# Uso (desde la raiz del repositorio; las rutas relativas se leen desde ella):
#   ./scripts/run_plot_metrics.sh                      # data/perf_out_v5 -> data/figuras
#   ./scripts/run_plot_metrics.sh data/perf_out_v5     # directorio de resultados explicito

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)"
cd "$ROOT_DIR"

OUTDIR="${1:-data/perf_out_v5}"
FIGDIR="${FIGDIR:-${ROOT_DIR}/data/figuras}"
LAT_CSV="${LAT_CSV:-${ROOT_DIR}/data/caracterizacion/latencias_exadell.csv}"

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
python3 "${ROOT_DIR}/scripts/plots/plot_results.py" --outdir "${OUTDIR}" --figdir "${FIGDIR}"

# La caracterizacion de latencias se mide una sola vez y es independiente de la
# campana, asi que puede no estar todavia. No es un error.
if [[ -f "$LAT_CSV" ]]; then
  echo
  echo "=== Figuras de latencias (${LAT_CSV}) ==="
  python3 "${ROOT_DIR}/scripts/plots/plot_latencias.py" --csv "$LAT_CSV" --figdir "${FIGDIR}"
else
  echo
  echo "(sin ${LAT_CSV}: ejecuta 'sbatch scripts/run_latencias.sbatch' para"
  echo " generar las figuras de latencia)"
fi

echo
echo "Todas las figuras en: ${FIGDIR}"
