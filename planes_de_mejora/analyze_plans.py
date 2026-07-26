#!/usr/bin/env python3
# =============================================================================
# analyze_plans.py
#
# Lee results/<PLAN>/kernel_metrics.csv de cada plan y genera:
#   - combined_kernel_metrics.csv : todo junto, con columna 'plan'
#   - ranking.csv                 : mejor plan por (kernel, hilos, metrica)
#   - comparison_figures/*.png/.eps :
#       * ABS: metrica del scheduler de cada plan vs hilos (+ baseline spread de BASE)
#       * REL: scheduler / (spread propio del plan)  -> >1.0 = supera al baseline estatico
#
# No usa pandas (solo csv + matplotlib) para minimizar dependencias.
#
# Uso:
#   python3 analyze_plans.py --results "planes_de_mejora/results" [--figdir ...]
# =============================================================================
import argparse, csv, os, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

METRICS = ["mlups", "gflops", "bw_gibs", "stdev_ms"]
HIGHER_BETTER = {"mlups": True, "gflops": True, "bw_gibs": True, "stdev_ms": False}
# Metrica "principal" por kernel para el ranking global.
# Stencil: MLUPS (millones de actualizaciones de malla/s) — metrica de dominio del stencil.
PRIMARY = {"stencil": "mlups", "spmv_static": "bw_gibs"}


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def load(results_dir):
    """Devuelve:
       sched[plan][kernel][threads] = {metric: val, 'migrations': m}
       spread[plan][kernel][threads] = {metric: val}   (baseline cores_spread_interleave)
       plans encontrados (en orden de aparicion)
    """
    sched = defaultdict(lambda: defaultdict(dict))
    spread = defaultdict(lambda: defaultdict(dict))
    plans = []
    if not os.path.isdir(results_dir):
        sys.exit(f"[ERROR] no existe {results_dir}")
    for plan in sorted(os.listdir(results_dir)):
        csvf = os.path.join(results_dir, plan, "kernel_metrics.csv")
        if not os.path.isfile(csvf):
            continue
        plans.append(plan)
        with open(csvf, newline="") as fh:
            for row in csv.DictReader(fh):
                cfg = row.get("config", "")
                thr = fnum(row.get("threads"))
                if thr is None:
                    continue
                thr = int(thr)
                sc = row.get("scheduler", "")
                vals = {m: fnum(row.get(m)) for m in METRICS}
                if sc == "ompt" and cfg.endswith("_scheduler"):
                    kernel = cfg[:-len("_scheduler")]
                    vals["migrations"] = fnum(row.get("migrations")) or 0
                    sched[plan][kernel][thr] = vals
                elif sc == "none" and cfg.endswith("cores_spread_interleave"):
                    # kernel = prefijo antes de "_cores_spread_interleave"
                    kernel = cfg[:-len("_cores_spread_interleave")]
                    spread[plan][kernel][thr] = vals
    return sched, spread, plans


def all_kernels(sched):
    ks = set()
    for plan in sched:
        ks.update(sched[plan].keys())
    return sorted(ks)


def write_combined(results_dir, sched, spread, outdir):
    path = os.path.join(outdir, "combined_kernel_metrics.csv")
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["plan", "run", "kernel", "threads", "mlups", "gflops", "bw_gibs", "stdev_ms", "migrations"])
        for plan in sched:
            for kernel in sched[plan]:
                for thr, v in sorted(sched[plan][kernel].items()):
                    w.writerow([plan, "scheduler", kernel, thr, v.get("mlups"), v.get("gflops"),
                                v.get("bw_gibs"), v.get("stdev_ms"), v.get("migrations")])
        for plan in spread:
            for kernel in spread[plan]:
                for thr, v in sorted(spread[plan][kernel].items()):
                    w.writerow([plan, "spread_baseline", kernel, thr, v.get("mlups"), v.get("gflops"),
                                v.get("bw_gibs"), v.get("stdev_ms"), ""])
    print(f"[OK] {path}")


def savefig(fig, figdir, name):
    for ext in ("png", "eps"):
        p = os.path.join(figdir, f"{name}.{ext}")
        try:
            fig.savefig(p, bbox_inches="tight", dpi=130)
        except Exception as e:
            print(f"[i] no se pudo guardar {p}: {e}")
    plt.close(fig)


def plot_absolute(sched, spread, kernel, metric, plans, figdir):
    fig, ax = plt.subplots(figsize=(9, 5.5))
    plotted = False
    for plan in plans:
        d = sched.get(plan, {}).get(kernel, {})
        if not d:
            continue
        xs = sorted(d.keys())
        ys = [d[t].get(metric) for t in xs]
        if any(y is not None for y in ys):
            ax.plot(xs, ys, marker="o", linewidth=1.6, label=plan)
            plotted = True
    # baseline spread de BASE (objetivo a superar)
    base = spread.get("BASE", {}).get(kernel, {})
    if base:
        xs = sorted(base.keys())
        ys = [base[t].get(metric) for t in xs]
        ax.plot(xs, ys, "k--", linewidth=2.4, label="baseline spread (BASE)")
    if not plotted:
        plt.close(fig)
        return
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted({t for p in plans for t in sched.get(p, {}).get(kernel, {})}))
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.set_xlabel("Hilos")
    ax.set_ylabel(metric)
    ax.set_title(f"{kernel} — {metric} (scheduler de cada plan) vs baseline spread")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, ncol=2)
    savefig(fig, figdir, f"cmp_{kernel}_{metric}_abs")


def plot_relative(sched, spread, kernel, metric, plans, figdir):
    """scheduler / spread-propio-del-plan. >1 (o <1 en stdev) = mejor que su baseline."""
    fig, ax = plt.subplots(figsize=(9, 5.5))
    plotted = False
    for plan in plans:
        d = sched.get(plan, {}).get(kernel, {})
        b = spread.get(plan, {}).get(kernel, {})
        if not b:  # si el plan no tiene su propio spread, usar el de BASE
            b = spread.get("BASE", {}).get(kernel, {})
        if not d or not b:
            continue
        xs = sorted(set(d.keys()) & set(b.keys()))
        ys = []
        for t in xs:
            num = d[t].get(metric)
            den = b[t].get(metric)
            ys.append(num / den if (num and den) else None)
        if any(y is not None for y in ys):
            ax.plot(xs, ys, marker="o", linewidth=1.6, label=plan)
            plotted = True
    if not plotted:
        plt.close(fig)
        return
    ax.axhline(1.0, color="k", linestyle="--", linewidth=1.5, label="baseline (=1.0)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted({t for p in plans for t in sched.get(p, {}).get(kernel, {})}))
    ax.get_xaxis().set_major_formatter(matplotlib.ticker.ScalarFormatter())
    ax.set_xlabel("Hilos")
    better = "mayor=mejor" if HIGHER_BETTER[metric] else "menor=mejor"
    ax.set_ylabel(f"scheduler / spread-propio ({better})")
    ax.set_title(f"{kernel} — {metric}: ¿supera el scheduler a SU baseline spread?")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, ncol=2)
    savefig(fig, figdir, f"cmp_{kernel}_{metric}_rel")


def write_ranking(sched, spread, plans, outdir):
    path = os.path.join(outdir, "ranking.csv")
    win_count = defaultdict(int)   # plan -> veces que gana la metrica principal
    beat_base = defaultdict(int)   # plan -> veces que supera su baseline spread
    total_pts = defaultdict(int)
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["kernel", "threads", "metric", "best_plan", "best_value",
                    "base_spread", "best_vs_base"])
        for kernel in all_kernels(sched):
            metric = PRIMARY.get(kernel, "gflops")
            thrs = sorted({t for p in plans for t in sched.get(p, {}).get(kernel, {})})
            for thr in thrs:
                best_plan, best_val = None, None
                for plan in plans:
                    v = sched.get(plan, {}).get(kernel, {}).get(thr, {}).get(metric)
                    if v is None:
                        continue
                    total_pts[plan] += 1
                    b = (spread.get(plan, {}).get(kernel, {}).get(thr, {})
                         or spread.get("BASE", {}).get(kernel, {}).get(thr, {})).get(metric)
                    if b:
                        ratio = v / b
                        if (HIGHER_BETTER[metric] and ratio > 1.0) or \
                           (not HIGHER_BETTER[metric] and ratio < 1.0):
                            beat_base[plan] += 1
                    if best_val is None or \
                       (HIGHER_BETTER[metric] and v > best_val) or \
                       (not HIGHER_BETTER[metric] and v < best_val):
                        best_val, best_plan = v, plan
                if best_plan is not None:
                    win_count[best_plan] += 1
                    base = spread.get("BASE", {}).get(kernel, {}).get(thr, {}).get(metric)
                    ratio = (best_val / base) if base else ""
                    w.writerow([kernel, thr, metric, best_plan, f"{best_val:.4f}",
                                f"{base:.4f}" if base else "", f"{ratio:.3f}" if ratio else ""])
    print(f"[OK] {path}")
    print("\n=================  RESUMEN  =================")
    print("Veces que cada plan es el MEJOR (metrica principal por kernel):")
    for plan in sorted(win_count, key=lambda p: -win_count[p]):
        print(f"   {plan:6s}: gana {win_count[plan]:2d}  |  supera su baseline {beat_base.get(plan,0):2d}/{total_pts.get(plan,0)}")
    if win_count:
        champ = max(win_count, key=lambda p: win_count[p])
        print(f"\n>>> Plan que gana mas veces: {champ}")
    print("=============================================\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="planes_de_mejora/results")
    ap.add_argument("--figdir", default=None)
    args = ap.parse_args()
    figdir = args.figdir or os.path.join(args.results, "..", "comparison_figures")
    figdir = os.path.abspath(figdir)
    os.makedirs(figdir, exist_ok=True)

    sched, spread, plans = load(args.results)
    if not plans:
        sys.exit(f"[ERROR] no se encontraron kernel_metrics.csv en {args.results}")
    print(f"[i] Planes con resultados: {', '.join(plans)}")

    write_combined(args.results, sched, spread, figdir)
    for kernel in all_kernels(sched):
        for metric in METRICS:
            plot_absolute(sched, spread, kernel, metric, plans, figdir)
            plot_relative(sched, spread, kernel, metric, plans, figdir)
    write_ranking(sched, spread, plans, figdir)
    print(f"[OK] Figuras y tablas en: {figdir}")


if __name__ == "__main__":
    main()
