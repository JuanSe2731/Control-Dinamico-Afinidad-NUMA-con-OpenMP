#!/usr/bin/env python3
import argparse
import csv
import os
import re

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


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


def plot_metric(rows, kernel_prefix, metric, ylabel, fig_path):
    filtered = [r for r in rows if r["config"].startswith(kernel_prefix)]
    if not filtered:
        return

    groups = group_by(filtered, lambda r: (r["config"], r["scheduler"]))
    plt.figure(figsize=(6, 4))
    for (config, scheduler), items in groups.items():
        points = sorted(
            [(int(r["threads"]), float(r[metric])) for r in items],
            key=lambda x: x[0],
        )
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        label = f"{config} ({scheduler})"
        plt.plot(xs, ys, marker="o", label=label)

    plt.xlabel("Threads")
    plt.ylabel(ylabel)
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend(fontsize=7)
    plt.tight_layout()
    plt.savefig(fig_path, format="eps")
    plt.close()


def plot_metric_scheduler_mean(rows, kernel_prefix, metric, ylabel, fig_path):
    filtered = [
        r for r in rows
        if r["config"].startswith(kernel_prefix) and "serial" not in r["config"]
    ]
    if not filtered:
        return

    groups = group_by(filtered, lambda r: (r["threads"], r["scheduler"]))
    series = {}
    for (threads, scheduler), items in groups.items():
        avg = sum(safe_float(r[metric]) for r in items) / max(1, len(items))
        series.setdefault(scheduler, []).append((int(threads), avg))

    plt.figure(figsize=(6, 4))
    for scheduler, points in series.items():
        points = sorted(points, key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        plt.plot(xs, ys, marker="o", label=scheduler)

    plt.xlabel("Threads")
    plt.ylabel(ylabel)
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend(fontsize=7)
    plt.tight_layout()
    plt.savefig(fig_path, format="eps")
    plt.close()


def plot_scheduler_ratio(rows, kernel_prefix, metric, ylabel, fig_path):
    filtered = [
        r for r in rows
        if r["config"].startswith(kernel_prefix) and "serial" not in r["config"]
    ]
    if not filtered:
        return

    groups = group_by(filtered, lambda r: (r["config"], r["threads"]))
    series = {}
    for (config, threads), items in groups.items():
        vals = {r["scheduler"]: safe_float(r[metric]) for r in items}
        if "ompt" not in vals or "none" not in vals:
            continue
        ratio = vals["ompt"] / vals["none"] if vals["none"] > 0 else 0.0
        series.setdefault(config, []).append((int(threads), ratio))

    if not series:
        return

    plt.figure(figsize=(6, 4))
    for config, points in series.items():
        points = sorted(points, key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        plt.plot(xs, ys, marker="o", label=config)

    plt.xlabel("Threads")
    plt.ylabel(ylabel)
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend(fontsize=7)
    plt.tight_layout()
    plt.savefig(fig_path, format="eps")
    plt.close()


def parse_tag(tag):
    m = re.match(r"^(?P<kernel>.+?)_t(?P<threads>\\d+)_(?P<config>.+)$", tag)
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


def plot_ratio_rm(window_rows, kernel_prefix, fig_path):
    parsed = []
    for row in window_rows:
        parsed_tag = parse_tag(row["tag"])
        if not parsed_tag:
            continue
        kernel, threads, config, scheduler = parsed_tag
        if not kernel.startswith(kernel_prefix):
            continue
        parsed.append({
            "kernel": kernel,
            "threads": threads,
            "config": config,
            "scheduler": scheduler,
            "ratio_rm": float(row["ratio_rm"]),
        })

    if not parsed:
        return

    # promedio por tag (config+threads+scheduler)
    groups = group_by(parsed, lambda r: (r["config"], r["threads"], r["scheduler"]))
    series = {}
    for (config, threads, scheduler), items in groups.items():
        avg = sum(r["ratio_rm"] for r in items) / max(1, len(items))
        key = (config, scheduler)
        series.setdefault(key, []).append((threads, avg))

    plt.figure(figsize=(6, 4))
    for (config, scheduler), points in series.items():
        points = sorted(points, key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        label = f"{config} ({scheduler})"
        plt.plot(xs, ys, marker="o", label=label)

    plt.xlabel("Threads")
    plt.ylabel("ratio_rm (avg)")
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend(fontsize=7)
    plt.tight_layout()
    plt.savefig(fig_path, format="eps")
    plt.close()


def plot_window_metric_time_compare(window_rows, kernel_prefix, threads, metric, ylabel, fig_path):
    parsed = []
    for row in window_rows:
        parsed_tag = parse_tag(row["tag"])
        if not parsed_tag:
            continue
        kernel, tcount, config, scheduler = parsed_tag
        if not kernel.startswith(kernel_prefix) or tcount != threads:
            continue
        parsed.append({
            "config": config,
            "scheduler": scheduler,
            "time": safe_float(row["window_ms"]),
            "value": safe_float(row.get(metric)),
        })

    if not parsed:
        return

    groups = group_by(parsed, lambda r: (r["config"], r["scheduler"]))
    plt.figure(figsize=(6, 4))
    for (config, scheduler), items in groups.items():
        points = sorted([(r["time"], r["value"]) for r in items], key=lambda x: x[0])
        xs = [p[0] for p in points]
        ys = [p[1] for p in points]
        label = f"{config} ({scheduler})"
        plt.plot(xs, ys, label=label)

    plt.xlabel("Window time (ms)")
    plt.ylabel(ylabel)
    plt.grid(True, linestyle="--", alpha=0.4)
    plt.legend(fontsize=7)
    plt.tight_layout()
    plt.savefig(fig_path, format="eps")
    plt.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", default="perf_out_v4")
    parser.add_argument("--figdir", default="plots/figures")
    args = parser.parse_args()

    os.makedirs(args.figdir, exist_ok=True)

    kernel_csv = os.path.join(args.outdir, "kernel_metrics.csv")
    window_csv = os.path.join(args.outdir, "window_metrics.csv")

    kernel_rows = read_csv_rows(kernel_csv)
    window_rows = read_csv_rows(window_csv)

    # Stencil
    plot_metric(kernel_rows, "stencil", "gflops", "GFlops", os.path.join(args.figdir, "stencil_gflops.eps"))
    plot_metric(kernel_rows, "stencil", "bw_gibs", "GiB/s", os.path.join(args.figdir, "stencil_bw.eps"))
    plot_metric(kernel_rows, "stencil", "stdev_ms", "Stddev (ms)", os.path.join(args.figdir, "stencil_stddev.eps"))
    plot_ratio_rm(window_rows, "stencil", os.path.join(args.figdir, "stencil_ratio_rm.eps"))
    plot_metric_scheduler_mean(kernel_rows, "stencil", "gflops", "GFlops (mean by scheduler)",
                               os.path.join(args.figdir, "stencil_gflops_sched_mean.eps"))
    plot_metric_scheduler_mean(kernel_rows, "stencil", "bw_gibs", "GiB/s (mean by scheduler)",
                               os.path.join(args.figdir, "stencil_bw_sched_mean.eps"))
    plot_scheduler_ratio(kernel_rows, "stencil", "gflops", "GFlops (ompt/none)",
                         os.path.join(args.figdir, "stencil_gflops_ratio.eps"))
    plot_scheduler_ratio(kernel_rows, "stencil", "bw_gibs", "GiB/s (ompt/none)",
                         os.path.join(args.figdir, "stencil_bw_ratio.eps"))
    plot_window_metric_time_compare(window_rows, "stencil", 128, "ratio_rm",
                                    "ratio_rm (window)", os.path.join(args.figdir, "stencil_ratio_rm_time_t128.eps"))
    plot_window_metric_time_compare(window_rows, "stencil", 128, "ipc",
                                    "IPC (window)", os.path.join(args.figdir, "stencil_ipc_time_t128.eps"))

    # SpMV
    plot_metric(kernel_rows, "spmv_static", "gflops", "GFlops", os.path.join(args.figdir, "spmv_gflops.eps"))
    plot_metric(kernel_rows, "spmv_static", "bw_gibs", "GiB/s", os.path.join(args.figdir, "spmv_bw.eps"))
    plot_metric(kernel_rows, "spmv_static", "migrations", "Migrations", os.path.join(args.figdir, "spmv_migrations.eps"))
    plot_ratio_rm(window_rows, "spmv_static", os.path.join(args.figdir, "spmv_ratio_rm.eps"))
    plot_metric_scheduler_mean(kernel_rows, "spmv_static", "gflops", "GFlops (mean by scheduler)",
                               os.path.join(args.figdir, "spmv_gflops_sched_mean.eps"))
    plot_metric_scheduler_mean(kernel_rows, "spmv_static", "bw_gibs", "GiB/s (mean by scheduler)",
                               os.path.join(args.figdir, "spmv_bw_sched_mean.eps"))
    plot_scheduler_ratio(kernel_rows, "spmv_static", "gflops", "GFlops (ompt/none)",
                         os.path.join(args.figdir, "spmv_gflops_ratio.eps"))
    plot_scheduler_ratio(kernel_rows, "spmv_static", "bw_gibs", "GiB/s (ompt/none)",
                         os.path.join(args.figdir, "spmv_bw_ratio.eps"))
    plot_window_metric_time_compare(window_rows, "spmv_static", 128, "ratio_rm",
                                    "ratio_rm (window)", os.path.join(args.figdir, "spmv_ratio_rm_time_t128.eps"))
    plot_window_metric_time_compare(window_rows, "spmv_static", 128, "ipc",
                                    "IPC (window)", os.path.join(args.figdir, "spmv_ipc_time_t128.eps"))


if __name__ == "__main__":
    main()
