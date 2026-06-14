#!/usr/bin/env python3
import argparse
import csv
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams.update({
    "font.family":      "sans-serif",
    "font.sans-serif":  ["Segoe UI", "Arial", "Helvetica", "DejaVu Sans"],
    "font.size":        11,
    "axes.titlesize":   14,
    "axes.titleweight": "bold",
    "axes.labelsize":   12,
    "axes.grid":        True,
    "grid.alpha":       0.3,
    "grid.linestyle":   "--",
    "legend.fontsize":  10,
    "figure.dpi":       150,
    "savefig.dpi":      200,
    "savefig.bbox":     "tight",
})

COLOR_NONE  = "#2196F3"   # blue  — without scheduler
COLOR_OMPT  = "#F44336"   # red   — with NUMA scheduler
MARKER_NONE = "o"
MARKER_OMPT = "s"
THREAD_LIST = [8, 16, 32, 64, 128]

PALETTE_NONE = ["#1565C0", "#1976D2", "#42A5F5"]
PALETTE_OMPT = ["#C62828", "#E53935", "#EF9A9A"]
LINE_STYLES  = ["-", "--", ":"]


def read_csv_rows(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def group_by(rows, key_fn):
    groups = {}
    for row in rows:
        key = key_fn(row)
        groups.setdefault(key, []).append(row)
    return groups


def safe_float(val):
    try:
        return float(val)
    except (TypeError, ValueError):
        return 0.0


def save_fig(fig, stem):
    fig.savefig(stem + ".png")
    fig.savefig(stem + ".eps", format="eps")
    plt.close(fig)


def annotate_points(ax, xs, ys, color, fmt="{:.1f}", offset_y=14):
    for x, y in zip(xs, ys):
        ax.annotate(
            fmt.format(y), (x, y),
            textcoords="offset points", xytext=(0, offset_y),
            ha="center", fontsize=9, color=color, fontweight="bold",
            bbox=dict(facecolor="white", edgecolor="none", alpha=0.8, pad=1.5),
        )


def plot_metric(rows, kernel_prefix, metric, ylabel, title, fig_stem):
    filtered = [r for r in rows if r["config"].startswith(kernel_prefix)]
    if not filtered:
        return

    groups = group_by(filtered, lambda r: (r["config"], r["scheduler"]))
    none_keys = sorted(k for k in groups if k[1] == "none")
    ompt_keys = sorted(k for k in groups if k[1] == "ompt")

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for i, (config, scheduler) in enumerate(none_keys):
        points = sorted(
            [(int(r["threads"]), safe_float(r[metric])) for r in groups[(config, scheduler)] if r[metric]],
            key=lambda x: x[0],
        )
        if not points:
            continue
        xs, ys = zip(*points)
        ax.plot(xs, ys, color=PALETTE_NONE[i % len(PALETTE_NONE)], marker=MARKER_NONE,
                linewidth=2.2, markersize=7, linestyle=LINE_STYLES[i % len(LINE_STYLES)],
                label=f"{config} (none)")

    for i, (config, scheduler) in enumerate(ompt_keys):
        points = sorted(
            [(int(r["threads"]), safe_float(r[metric])) for r in groups[(config, scheduler)] if r[metric]],
            key=lambda x: x[0],
        )
        if not points:
            continue
        xs, ys = zip(*points)
        ax.plot(xs, ys, color=PALETTE_OMPT[i % len(PALETTE_OMPT)], marker=MARKER_OMPT,
                linewidth=2.2, markersize=7, linestyle=LINE_STYLES[i % len(LINE_STYLES)],
                label=f"{config} (ompt)")

    ax.set_xlabel("Number of Threads")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(THREAD_LIST)
    ax.set_xticklabels([str(t) for t in THREAD_LIST])
    ax.legend(loc="upper left", framealpha=0.9)
    ax.set_xlim(4, 144)
    fig.tight_layout()
    save_fig(fig, fig_stem)


def plot_metric_scheduler_mean(rows, kernel_prefix, metric, ylabel, title, fig_stem):
    filtered = [
        r for r in rows
        if r["config"].startswith(kernel_prefix) and "serial" not in r["config"]
    ]
    if not filtered:
        return

    groups = group_by(filtered, lambda r: (r["threads"], r["scheduler"]))
    series = {}
    for (threads, scheduler), items in groups.items():
        valid = [safe_float(r[metric]) for r in items if r[metric]]
        if not valid:
            continue
        series.setdefault(scheduler, []).append((int(threads), sum(valid) / len(valid)))

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for scheduler, points in sorted(series.items()):
        points = sorted(points, key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        if scheduler == "none":
            color, marker, label, offset = COLOR_NONE, MARKER_NONE, "Without Scheduler", 14
        else:
            color, marker, label, offset = COLOR_OMPT, MARKER_OMPT, "With NUMA Scheduler", -18
        ax.plot(xs, ys, color=color, marker=marker, linewidth=2.2, markersize=8,
                label=label, zorder=3)
        annotate_points(ax, xs, ys, color, offset_y=offset)

    ax.set_xlabel("Number of Threads")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(THREAD_LIST)
    ax.set_xticklabels([str(t) for t in THREAD_LIST])
    ax.legend(loc="upper left", framealpha=0.9)
    ax.set_xlim(4, 144)
    fig.tight_layout()
    save_fig(fig, fig_stem)


def plot_scheduler_ratio(rows, kernel_prefix, metric, ylabel, title, fig_stem):
    filtered = [
        r for r in rows
        if r["config"].startswith(kernel_prefix) and "serial" not in r["config"]
    ]
    if not filtered:
        return

    groups = group_by(filtered, lambda r: (r["config"], r["threads"]))
    series = {}
    for (config, threads), items in groups.items():
        vals = {r["scheduler"]: safe_float(r[metric]) for r in items if r[metric]}
        if "ompt" not in vals or "none" not in vals or vals["none"] == 0:
            continue
        series.setdefault(config, []).append((int(threads), vals["ompt"] / vals["none"]))

    if not series:
        return

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for i, (config, points) in enumerate(sorted(series.items())):
        points = sorted(points, key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        color = PALETTE_NONE[i % len(PALETTE_NONE)]
        ax.plot(xs, ys, color=color, marker=MARKER_NONE, linewidth=2.2, markersize=8,
                label=config, zorder=3)
        annotate_points(ax, xs, ys, color, fmt="{:.2f}", offset_y=10)

    ax.axhline(y=1.0, color="#555555", linewidth=1.0, linestyle="-", zorder=2)
    ax.set_xlabel("Number of Threads")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(THREAD_LIST)
    ax.set_xticklabels([str(t) for t in THREAD_LIST])
    ax.legend(loc="upper left", framealpha=0.9)
    ax.set_xlim(4, 144)
    fig.tight_layout()
    save_fig(fig, fig_stem)


def parse_tag(tag):
    m = re.match(r"^(?P<kernel>.+?)_t(?P<threads>\d+)_(?P<config>.+)$", tag)
    if not m:
        return None
    kernel = m.group("kernel")
    threads = int(m.group("threads"))
    config = m.group("config")
    scheduler = "none"
    if config.endswith("_ompt"):
        scheduler = "ompt"
        config = config[: -len("_ompt")]
    return kernel, threads, config, scheduler


def plot_ratio_rm(window_rows, kernel_prefix, title, fig_stem):
    parsed = []
    for row in window_rows:
        result = parse_tag(row["tag"])
        if not result:
            continue
        kernel, threads, config, scheduler = result
        if not kernel.startswith(kernel_prefix):
            continue
        ratio_rm = safe_float(row.get("ratio_rm", ""))
        if ratio_rm == 0.0:
            continue
        parsed.append({"threads": threads, "config": config,
                        "scheduler": scheduler, "ratio_rm": ratio_rm})

    if not parsed:
        return

    groups = group_by(parsed, lambda r: (r["config"], r["threads"], r["scheduler"]))
    series = {}
    for (config, threads, scheduler), items in groups.items():
        avg = sum(r["ratio_rm"] for r in items) / max(1, len(items))
        series.setdefault((config, scheduler), []).append((threads, avg))

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for (config, scheduler), points in sorted(series.items()):
        points = sorted(points, key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        color  = COLOR_OMPT  if scheduler == "ompt" else COLOR_NONE
        marker = MARKER_OMPT if scheduler == "ompt" else MARKER_NONE
        ax.plot(xs, ys, color=color, marker=marker, linewidth=2.2, markersize=8,
                label=f"{config} ({scheduler})", zorder=3)

    ax.set_xlabel("Number of Threads")
    ax.set_ylabel("Average ratio_rm")
    ax.set_title(title)
    ax.set_xticks(THREAD_LIST)
    ax.set_xticklabels([str(t) for t in THREAD_LIST])
    ax.legend(loc="upper left", framealpha=0.9)
    ax.set_xlim(4, 144)
    fig.tight_layout()
    save_fig(fig, fig_stem)


def plot_window_metric_time_compare(window_rows, kernel_prefix, threads, metric, ylabel, title, fig_stem):
    parsed = []
    for row in window_rows:
        result = parse_tag(row["tag"])
        if not result:
            continue
        kernel, tcount, config, scheduler = result
        if not kernel.startswith(kernel_prefix) or tcount != threads:
            continue
        val = safe_float(row.get(metric, ""))
        t   = safe_float(row.get("window_ms", ""))
        if val == 0.0 or t == 0.0:
            continue
        parsed.append({"config": config, "scheduler": scheduler, "time": t, "value": val})

    if not parsed:
        return

    groups = group_by(parsed, lambda r: (r["config"], r["scheduler"]))
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for i, ((config, scheduler), items) in enumerate(sorted(groups.items())):
        points = sorted([(r["time"], r["value"]) for r in items], key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        color = COLOR_OMPT if scheduler == "ompt" else COLOR_NONE
        ax.plot(xs, ys, color=color, linewidth=1.8,
                linestyle=LINE_STYLES[i % len(LINE_STYLES)],
                label=f"{config} ({scheduler})", alpha=0.85)

    ax.set_xlabel("Window Time (ms)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.legend(loc="upper left", framealpha=0.9, fontsize=9)
    fig.tight_layout()
    save_fig(fig, fig_stem)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default="perf_out_v4")
    parser.add_argument("--figdir", default="plots/figures")
    args = parser.parse_args()

    os.makedirs(args.figdir, exist_ok=True)

    kernel_rows = read_csv_rows(os.path.join(args.outdir, "kernel_metrics.csv"))
    window_rows = (
        read_csv_rows(os.path.join(args.outdir, "window_metrics.csv")) +
        read_csv_rows(os.path.join(args.outdir, "ompt_window_metrics.csv"))
    )

    def fig(name):
        return os.path.join(args.figdir, name)

    # ── Stencil ────────────────────────────────────────────────────────────────
    plot_metric(kernel_rows, "stencil", "gflops", "GFlops",
                "Stencil2D Performance — GFlops", fig("stencil_gflops"))
    plot_metric(kernel_rows, "stencil", "bw_gibs", "Bandwidth (GiB/s)",
                "Stencil2D Performance — Bandwidth", fig("stencil_bw"))
    plot_metric(kernel_rows, "stencil", "stdev_ms", "Stddev (ms)",
                "Stencil2D Time Variability", fig("stencil_stddev"))
    plot_metric_scheduler_mean(kernel_rows, "stencil", "gflops", "GFlops",
                               "Stencil2D — GFlops (mean across configs)",
                               fig("stencil_gflops_sched_mean"))
    plot_metric_scheduler_mean(kernel_rows, "stencil", "bw_gibs", "Bandwidth (GiB/s)",
                               "Stencil2D — Bandwidth (mean across configs)",
                               fig("stencil_bw_sched_mean"))
    plot_scheduler_ratio(kernel_rows, "stencil", "gflops", "GFlops (OMPT / Baseline)",
                         "Stencil2D — Scheduler Impact on GFlops",
                         fig("stencil_gflops_ratio"))
    plot_scheduler_ratio(kernel_rows, "stencil", "bw_gibs", "Bandwidth ratio (OMPT / Baseline)",
                         "Stencil2D — Scheduler Impact on Bandwidth",
                         fig("stencil_bw_ratio"))
    plot_ratio_rm(window_rows, "stencil",
                  "Stencil2D — Remote Fill Ratio vs Threads",
                  fig("stencil_ratio_rm"))
    plot_window_metric_time_compare(window_rows, "stencil", 128, "ratio_rm",
                                    "ratio_rm", "Stencil2D — ratio_rm over time (128 threads)",
                                    fig("stencil_ratio_rm_time_t128"))
    plot_window_metric_time_compare(window_rows, "stencil", 128, "ipc",
                                    "IPC", "Stencil2D — IPC over time (128 threads)",
                                    fig("stencil_ipc_time_t128"))

    # ── SpMV ───────────────────────────────────────────────────────────────────
    plot_metric(kernel_rows, "spmv_static", "gflops", "GFlops",
                "SpMV Performance — GFlops", fig("spmv_gflops"))
    plot_metric(kernel_rows, "spmv_static", "bw_gibs", "Bandwidth (GiB/s)",
                "SpMV Performance — Bandwidth", fig("spmv_bw"))
    plot_metric(kernel_rows, "spmv_static", "migrations", "Migrations",
                "SpMV — Thread Migrations by NUMA Scheduler", fig("spmv_migrations"))
    plot_metric_scheduler_mean(kernel_rows, "spmv_static", "gflops", "GFlops",
                               "SpMV — GFlops (mean across configs)",
                               fig("spmv_gflops_sched_mean"))
    plot_metric_scheduler_mean(kernel_rows, "spmv_static", "bw_gibs", "Bandwidth (GiB/s)",
                               "SpMV — Bandwidth (mean across configs)",
                               fig("spmv_bw_sched_mean"))
    plot_scheduler_ratio(kernel_rows, "spmv_static", "gflops", "GFlops (OMPT / Baseline)",
                         "SpMV — Scheduler Impact on GFlops",
                         fig("spmv_gflops_ratio"))
    plot_scheduler_ratio(kernel_rows, "spmv_static", "bw_gibs", "Bandwidth ratio (OMPT / Baseline)",
                         "SpMV — Scheduler Impact on Bandwidth",
                         fig("spmv_bw_ratio"))
    plot_ratio_rm(window_rows, "spmv_static",
                  "SpMV — Remote Fill Ratio vs Threads",
                  fig("spmv_ratio_rm"))
    plot_window_metric_time_compare(window_rows, "spmv_static", 128, "ratio_rm",
                                    "ratio_rm", "SpMV — ratio_rm over time (128 threads)",
                                    fig("spmv_ratio_rm_time_t128"))
    plot_window_metric_time_compare(window_rows, "spmv_static", 128, "ipc",
                                    "IPC", "SpMV — IPC over time (128 threads)",
                                    fig("spmv_ipc_time_t128"))


if __name__ == "__main__":
    main()
