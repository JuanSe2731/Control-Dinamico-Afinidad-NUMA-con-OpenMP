#!/usr/bin/env bash
set -euo pipefail

OUTDIR="${1:-perf_out_v4}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 "${ROOT_DIR}/plots/plot_results.py" \
  --outdir "${OUTDIR}" \
  --figdir "${ROOT_DIR}/plots/figures"
