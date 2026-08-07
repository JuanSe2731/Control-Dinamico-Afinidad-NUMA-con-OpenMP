#!/usr/bin/env python3
"""
Figuras de la campana experimental.

Todas las figuras responden a la misma pregunta desde angulos distintos: que le
pasa al scheduler dinamico segun el GRADO DE PARALELISMO, y por que.

  1  throughput      rendimiento vs hilos (metrica primaria) con IC95
  2  speedup         T_spread / T_scheduler vs hilos, con significancia
  3  overhead        coste puro del tool a igual afinidad
  4  descomposicion  en que se descompone la diferencia total  <-- la figura clave
  5  escalado        escalado fuerte relativo a 8 hilos
  6  localidad       ratio_rm y remote_fills vs hilos
  7  migraciones     cuantas, y en que ventana ocurre la primera
  8  boxplot         distribucion de las REPS repeticiones
  9  mecanismo       balde vs umbral en el tiempo (por que decide migrar)
 10  ipc             instrucciones por ciclo vs hilos

Entradas (ver run_perf_metricsV4.sh):
  <outdir>/kernel_metrics.csv, <outdir>/metrics/*_times.csv,
  <outdir>/ompt_window_metrics.csv, <outdir>/ompt_summary.csv

Dependencias: matplotlib + biblioteca estandar. Sin pandas ni numpy.
"""

import argparse
import csv
import glob
import math
import os
import re
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

# ─────────────────────────────────────────────────────────────────────────────
# Paleta. Los tres primeros slots estan validados para TODAS las parejas; el
# resto solo para parejas adyacentes (barras/boxplots), que es como se usan aqui.
# El color sigue a la ENTIDAD: una configuracion tiene siempre el mismo color en
# todas las figuras, aunque cambie el conjunto que se dibuja.
# Marcador y trazo son codificacion SECUNDARIA: la identidad nunca depende solo
# del color (imprescindible para imprimir la memoria en escala de grises).
# ─────────────────────────────────────────────────────────────────────────────
ESTILO = {
    "spread":    dict(color="#2a78d6", marker="o", ls="-",   label="estatico spread"),
    "close":     dict(color="#eb6834", marker="s", ls="--",  label="estatico close"),
    "scheduler": dict(color="#1baf7a", marker="D", ls="-",   label="scheduler (dinamico)"),
    "nobind":    dict(color="#eda100", marker="^", ls=":",   label="sin fijar (control)"),
    "obs":       dict(color="#e87ba4", marker="v", ls=":",   label="tool sin migrar (control)"),
    "ovh":       dict(color="#4a3aa7", marker="P", ls="-.",  label="tool + spread (control)"),
}
# Componentes de la descomposicion (slots 1-3, validados all-pairs)
COMP = [
    ("binding", "#2a78d6", "coste de no fijar afinidad"),
    ("instr",   "#eb6834", "coste de monitorizar"),
    ("migr",    "#1baf7a", "efecto de migrar"),
]

TINTA        = "#0b0b0b"
TINTA_SUAVE  = "#52514e"
TINTA_TENUE  = "#898781"
REJILLA      = "#e1e0d9"
EJE          = "#c3c2b7"

plt.rcParams.update({
    "figure.dpi": 110, "savefig.dpi": 200, "savefig.bbox": "tight",
    "font.size": 11, "axes.titlesize": 12, "axes.labelsize": 11,
    "legend.fontsize": 9.5, "xtick.labelsize": 10, "ytick.labelsize": 10,
    "axes.edgecolor": EJE, "axes.labelcolor": TINTA, "text.color": TINTA,
    "xtick.color": TINTA_TENUE, "ytick.color": TINTA_TENUE,
    "axes.linewidth": 0.8, "axes.grid": True, "grid.color": REJILLA,
    "grid.linewidth": 0.8, "grid.linestyle": "-",       # solida, nunca discontinua
    "axes.axisbelow": True, "axes.spines.top": False, "axes.spines.right": False,
    "legend.frameon": False,
})

KERNELS = [("stencil", "Stencil 2D", "mlups_min", "MLUPS"),
           ("spmv_static", "SpMV CSR", "bw_gibs", "Ancho de banda (GiB/s)")]
TAG_RE = re.compile(r"^(?P<kernel>.+)_t(?P<threads>\d+)_(?P<config>[a-z]+)$")


# ─────────────────────────────────────────────────────────────────────────────
# Carga
# ─────────────────────────────────────────────────────────────────────────────
def num(v):
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


def leer_csv(ruta):
    if not os.path.exists(ruta):
        return []
    with open(ruta, newline="") as f:
        return list(csv.DictReader(f))


def cargar(outdir):
    d = {}
    # kernel_metrics: {(kernel, threads, config): fila}
    d["agg"] = {}
    for r in leer_csv(os.path.join(outdir, "kernel_metrics.csv")):
        cfg_full = r.get("config", "")
        th = num(r.get("threads"))
        if th is None:
            continue
        for k, _, _, _ in KERNELS:
            if cfg_full.startswith(k + "_"):
                d["agg"][(k, int(th), cfg_full[len(k) + 1:])] = r
                break
    # tiempos por repeticion
    d["times"] = {}
    for ruta in sorted(glob.glob(os.path.join(outdir, "metrics", "*_times.csv"))):
        m = TAG_RE.match(os.path.basename(ruta)[:-len("_times.csv")])
        if not m:
            continue
        xs = [num(r["time_ms"]) for r in leer_csv(ruta)]
        xs = [x for x in xs if x is not None]
        if len(xs) > 1:
            d["times"][(m.group("kernel"), int(m.group("threads")), m.group("config"))] = xs
    # ventanas y resumen OMPT, indexados por tag
    d["win"] = defaultdict(list)
    for r in leer_csv(os.path.join(outdir, "ompt_window_metrics.csv")):
        d["win"][r["tag"]].append(r)
    d["sum"] = defaultdict(list)
    for r in leer_csv(os.path.join(outdir, "ompt_summary.csv")):
        d["sum"][r["tag"]].append(r)
    return d


def hilos_de(d, kernel):
    return sorted({t for (k, t, _) in d["agg"] if k == kernel})


def media_ic(xs):
    """media y semiancho del IC95 (n>=100 -> la normal basta)."""
    n = len(xs)
    m = sum(xs) / n
    if n < 2:
        return m, 0.0
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))
    return m, 1.96 * sd / math.sqrt(n)


def aclarar(hexcol, f=0.55):
    """Mezcla el color con blanco. Se usa en vez de alpha porque el backend
    PostScript (los .eps que van a la memoria) NO soporta transparencia y los
    renderiza opacos, cambiando el color respecto al .png."""
    r, g, b = (int(hexcol[i:i + 2], 16) for i in (1, 3, 5))
    m = lambda c: int(round(c + (255 - c) * f))
    return f"#{m(r):02x}{m(g):02x}{m(b):02x}"


def guardar(fig, base):
    fig.savefig(base + ".png")
    fig.savefig(base + ".eps", format="eps")
    plt.close(fig)
    print(f"  [fig] {os.path.basename(base)}")


def eje_hilos(ax, hilos):
    ax.set_xscale("log", base=2)
    ax.set_xticks(hilos)
    ax.set_xticklabels([str(h) for h in hilos])
    ax.set_xlabel("Numero de hilos")
    ax.minorticks_off()


def nota(fig, texto):
    """Pie de figura: donde va lo que el lector necesita para no malinterpretar."""
    fig.text(0.5, -0.045, texto, ha="center", va="top",
             fontsize=8.5, color=TINTA_SUAVE, wrap=True)


# ─────────────────────────────────────────────────────────────────────────────
# 1. Throughput vs hilos
# ─────────────────────────────────────────────────────────────────────────────
def fig_throughput(d, kernel, titulo, campo, etiqueta, base):
    hilos = hilos_de(d, kernel)
    if not hilos:
        return
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    hay = False
    for cfg in ("spread", "close", "scheduler"):
        xs, ys = [], []
        for h in hilos:
            r = d["agg"].get((kernel, h, cfg))
            v = num(r.get(campo)) if r else None
            if v is not None:
                xs.append(h); ys.append(v)
        if not xs:
            continue
        hay = True
        e = ESTILO[cfg]
        ax.plot(xs, ys, color=e["color"], marker=e["marker"], linestyle=e["ls"],
                linewidth=2, markersize=7, label=e["label"],
                markeredgecolor="white", markeredgewidth=1.2)
    if not hay:
        plt.close(fig); return
    eje_hilos(ax, hilos)
    ax.set_ylabel(etiqueta)
    ax.set_title(f"{titulo} — rendimiento segun el grado de paralelismo")
    ax.legend(loc="upper left")
    nota(fig, "Metrica derivada del tiempo minimo de las repeticiones (convencion HPC).")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 2. Speedup del scheduler frente al baseline estatico
# ─────────────────────────────────────────────────────────────────────────────
def fig_speedup(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    xs, ys, err, marca = [], [], [], []
    for h in hilos:
        a = d["times"].get((kernel, h, "scheduler"))
        b = d["times"].get((kernel, h, "spread"))
        if not a or not b:
            continue
        ma, ea = media_ic(a)
        mb, eb = media_ic(b)
        if ma <= 0:
            continue
        s = mb / ma                       # >1 => el scheduler es mas rapido
        # propagacion de la incertidumbre del cociente
        rel = math.sqrt((eb / mb) ** 2 + (ea / ma) ** 2) if mb > 0 else 0.0
        xs.append(h); ys.append(s); err.append(s * rel)
        marca.append(abs(s - 1.0) > s * rel)   # el IC no cruza 1
    if not xs:
        return
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    e = ESTILO["scheduler"]
    ax.errorbar(xs, ys, yerr=err, color=e["color"], marker=e["marker"],
                linewidth=2, markersize=7, capsize=3, elinewidth=1,
                markeredgecolor="white", markeredgewidth=1.2)
    ax.axhline(1.0, color=EJE, linewidth=1.2, zorder=1)
    ax.text(xs[0], 1.0, " paridad con spread", va="bottom", ha="left",
            fontsize=8.5, color=TINTA_TENUE)
    # Etiqueta selectiva: solo los extremos, nunca un numero en cada punto.
    for i in (0, len(xs) - 1):
        ax.annotate(f"{ys[i]:.3f}x", (xs[i], ys[i]), textcoords="offset points",
                    xytext=(0, 11), ha="center", fontsize=9, color=TINTA_SUAVE)
    for x, y, m in zip(xs, ys, marca):
        if m:
            ax.annotate("*", (x, y), textcoords="offset points", xytext=(0, -16),
                        ha="center", fontsize=13, color=TINTA_SUAVE)
    eje_hilos(ax, hilos)
    ax.set_ylabel("Speedup  (T spread / T scheduler)")
    ax.set_title(f"{titulo} — speedup del scheduler frente al estatico")
    nota(fig, "Barras: IC95 sobre la media de las repeticiones.  "
              "*: el IC95 no cruza la paridad.  Por encima de 1 el scheduler gana.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 3. Overhead del tool a igual afinidad
# ─────────────────────────────────────────────────────────────────────────────
def fig_overhead(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    series = [("ovh", "spread", "tool cargado, misma afinidad (spread)"),
              ("obs", "nobind", "tool cargado, sin fijar afinidad")]
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    hay = False
    for cfg, ref, etiq in series:
        xs, ys, err = [], [], []
        for h in hilos:
            a = d["times"].get((kernel, h, cfg))
            b = d["times"].get((kernel, h, ref))
            if not a or not b:
                continue
            ma, ea = media_ic(a); mb, eb = media_ic(b)
            if mb <= 0:
                continue
            xs.append(h); ys.append((ma / mb - 1) * 100)
            ys_rel = math.sqrt((ea / ma) ** 2 + (eb / mb) ** 2)
            err.append(abs(ma / mb) * ys_rel * 100)
        if not xs:
            continue
        hay = True
        e = ESTILO[cfg]
        ax.errorbar(xs, ys, yerr=err, color=e["color"], marker=e["marker"],
                    linestyle=e["ls"], linewidth=2, markersize=7, capsize=3,
                    elinewidth=1, label=etiq, markeredgecolor="white",
                    markeredgewidth=1.2)
    if not hay:
        plt.close(fig); return
    ax.axhline(0.0, color=EJE, linewidth=1.2, zorder=1)
    eje_hilos(ax, hilos)
    ax.set_ylabel("Sobrecoste en tiempo (%)")
    ax.set_title(f"{titulo} — overhead introducido por el tool OMPT")
    ax.legend(loc="upper left")
    nota(fig, "Migracion DESACTIVADA en ambas series: mide solo el coste de instrumentar "
              "(monitor, callbacks y lectura de contadores).")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 4. Descomposicion — la figura que explica la diferencia total
# ─────────────────────────────────────────────────────────────────────────────
def fig_descomposicion(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    filas = []
    for h in hilos:
        t = {c: d["times"].get((kernel, h, c)) for c in
             ("spread", "nobind", "obs", "scheduler")}
        if not all(t.values()):
            continue
        m = {c: sum(v) / len(v) for c, v in t.items()}
        filas.append((h,
                      (m["nobind"] / m["spread"] - 1) * 100,
                      (m["obs"] / m["nobind"] - 1) * 100,
                      (m["scheduler"] / m["obs"] - 1) * 100,
                      (m["scheduler"] / m["spread"] - 1) * 100))
    if not filas:
        return
    fig, ax = plt.subplots(figsize=(7.6, 5.0))
    x = list(range(len(filas)))
    ancho = min(0.5, 1.6 / max(1, len(filas)))   # con pocas categorias, barras finas
    pos = [0.0] * len(filas)
    neg = [0.0] * len(filas)
    for j, (nombre, color, etiq) in enumerate(COMP):
        vals = [f[j + 1] for f in filas]
        abajo = [pos[i] if vals[i] >= 0 else neg[i] for i in range(len(vals))]
        ax.bar(x, vals, bottom=abajo, width=ancho, color=color, label=etiq,
               edgecolor="white", linewidth=1.5)   # separacion entre segmentos
        for i, v in enumerate(vals):
            if v >= 0:
                pos[i] += v
            else:
                neg[i] += v
    total = [f[4] for f in filas]
    ax.plot(x, total, color=TINTA, marker="o", markersize=7, linewidth=0,
            markeredgecolor="white", markeredgewidth=1.5, label="total observado",
            zorder=5)
    for i, v in enumerate(total):
        ax.annotate(f"{v:+.1f}%", (x[i], v), textcoords="offset points",
                    xytext=(0, 13 if v >= 0 else -20), ha="center",
                    fontsize=9, color=TINTA, zorder=6,
                    bbox=dict(boxstyle="round,pad=0.18", fc="white",
                              ec="none", alpha=0.85))
    ax.axhline(0.0, color=EJE, linewidth=1.2, zorder=1)
    ax.set_xticks(x)
    ax.set_xticklabels([str(f[0]) for f in filas])
    ax.set_xlabel("Numero de hilos")
    ax.set_ylabel("Contribucion al sobrecoste en tiempo (%)")
    ax.set_title(f"{titulo} — de que se compone la diferencia frente al estatico",
                 pad=42)
    # Leyenda FUERA del area de trazado: dentro chocaba con las barras.
    ax.legend(loc="lower left", bbox_to_anchor=(0, 1.01), ncol=2, borderaxespad=0)
    ax.set_xlim(-0.7, len(filas) - 0.3)
    lo = min(min(neg), min(total), 0.0)
    hi = max(max(pos), max(total), 0.0)
    margen = max((hi - lo) * 0.18, 0.5)
    ax.set_ylim(lo - margen, hi + margen)
    nota(fig, "Positivo = mas lento. Los tres bloques se multiplican para dar el total; "
              "el punto negro es el total medido directamente (spread vs scheduler).")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 5. Escalado fuerte relativo
# ─────────────────────────────────────────────────────────────────────────────
def fig_escalado(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    if len(hilos) < 2:
        return
    h0 = hilos[0]
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    ax.plot(hilos, [h / h0 for h in hilos], color=TINTA_TENUE, linewidth=1.2,
            linestyle=(0, (6, 4)), label=f"ideal (lineal desde {h0})", zorder=1)
    hay = False
    for cfg in ("spread", "scheduler"):
        ref = d["times"].get((kernel, h0, cfg))
        if not ref:
            continue
        t0 = sum(ref) / len(ref)
        xs, ys = [], []
        for h in hilos:
            v = d["times"].get((kernel, h, cfg))
            if v:
                xs.append(h); ys.append(t0 / (sum(v) / len(v)))
        if not xs:
            continue
        hay = True
        e = ESTILO[cfg]
        ax.plot(xs, ys, color=e["color"], marker=e["marker"], linestyle=e["ls"],
                linewidth=2, markersize=7, label=e["label"],
                markeredgecolor="white", markeredgewidth=1.2)
    if not hay:
        plt.close(fig); return
    eje_hilos(ax, hilos)
    ax.set_ylabel(f"Speedup relativo a {h0} hilos")
    ax.set_title(f"{titulo} — escalado fuerte")
    ax.legend(loc="upper left")
    nota(fig, f"Referencia = {h0} hilos, no la version serial: la comparacion del "
              "proyecto es dinamico vs estatico.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 6. Localidad — dos paneles, NUNCA dos ejes en uno
# ─────────────────────────────────────────────────────────────────────────────
def fig_localidad(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11.2, 4.4))
    hay = False
    for cfg in ("spread", "close", "scheduler"):
        xr, yr, xf, yf = [], [], [], []
        for h in hilos:
            r = d["agg"].get((kernel, h, cfg))
            if r:
                v = num(r.get("ratio_rm"))
                if v is not None and v >= 0:      # -1 = no medible
                    xr.append(h); yr.append(v)
            filas = d["sum"].get(f"{kernel}_t{h}_{cfg}", [])
            tot = sum(num(f.get("remote_fills")) or 0 for f in filas)
            if filas and tot > 0:
                xf.append(h); yf.append(tot)
        e = ESTILO[cfg]
        if xr:
            hay = True
            a1.plot(xr, yr, color=e["color"], marker=e["marker"], linestyle=e["ls"],
                    linewidth=2, markersize=7, label=e["label"],
                    markeredgecolor="white", markeredgewidth=1.2)
        if xf:
            hay = True
            a2.plot(xf, yf, color=e["color"], marker=e["marker"], linestyle=e["ls"],
                    linewidth=2, markersize=7, label=e["label"],
                    markeredgecolor="white", markeredgewidth=1.2)
    if not hay:
        plt.close(fig); return
    for ax, ylab, tit in ((a1, "ratio_rm  (fills remotos / totales)", "Proporcion de accesos remotos"),
                          (a2, "remote_fills acumulados", "Volumen de trafico remoto")):
        eje_hilos(ax, hilos)
        ax.set_ylabel(ylab)
        ax.set_title(tit, fontsize=11)
    a2.set_yscale("log")
    a1.legend(loc="upper left")
    fig.suptitle(f"{titulo} — localidad de datos segun el grado de paralelismo", y=1.02)
    nota(fig, "Izquierda: proporcion (perf agregado para las estaticas, contadores por hilo "
              "para el scheduler). Derecha: volumen absoluto, escala logaritmica; solo hay "
              "dato donde el tool estuvo cargado.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 7. Migraciones
# ─────────────────────────────────────────────────────────────────────────────
def fig_migraciones(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    xs, mig, frac, prim = [], [], [], []
    for h in hilos:
        filas = d["sum"].get(f"{kernel}_t{h}_scheduler", [])
        if not filas:
            continue
        m = sum(int(num(f.get("migrations")) or 0) for f in filas)
        ventanas = [num(f.get("first_migration_win")) for f in filas]
        ventanas = [v for v in ventanas if v is not None and v >= 0]
        xs.append(h); mig.append(m)
        frac.append(100.0 * m / len(filas))
        prim.append(sum(ventanas) / len(ventanas) if ventanas else None)
    if not xs:
        return
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11.2, 4.4))
    x = list(range(len(xs)))
    a1.bar(x, mig, width=0.55, color=ESTILO["scheduler"]["color"],
           edgecolor="white", linewidth=1.5)
    for i, (m, f) in enumerate(zip(mig, frac)):
        a1.annotate(f"{m}\n({f:.0f}%)", (x[i], m), textcoords="offset points",
                    xytext=(0, 6), ha="center", fontsize=9, color=TINTA_SUAVE)
    a1.set_xticks(x); a1.set_xticklabels([str(v) for v in xs])
    a1.set_xlabel("Numero de hilos"); a1.set_ylabel("Hilos migrados")
    a1.set_title("Cuantos hilos migran", fontsize=11)
    a1.set_ylim(0, max(mig) * 1.3 if max(mig) else 1)

    xv = [x[i] for i, p in enumerate(prim) if p is not None]
    yv = [p for p in prim if p is not None]
    if xv:
        a2.bar(xv, yv, width=0.55, color=ESTILO["obs"]["color"],
               edgecolor="white", linewidth=1.5)
    a2.set_xticks(x); a2.set_xticklabels([str(v) for v in xs])
    a2.set_xlabel("Numero de hilos")
    a2.set_ylabel("Ventana media de la 1a migracion")
    a2.set_title("Cuando migran (1 ventana = 100 ms)", fontsize=11)
    fig.suptitle(f"{titulo} — actividad del scheduler", y=1.02)
    nota(fig, "El porcentaje es sobre el total de hilos. Cada hilo migra como maximo "
              "una vez (MAX_MIGRATIONS=1); el warmup impide migrar antes de la ventana 5.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 8. Boxplots de las repeticiones
# ─────────────────────────────────────────────────────────────────────────────
def fig_boxplot(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    cfgs = [c for c in ("spread", "close", "scheduler")
            if any((kernel, h, c) in d["times"] for h in hilos)]
    if not cfgs or not hilos:
        return
    fig, axes = plt.subplots(1, len(hilos), figsize=(2.6 * len(hilos) + 1.4, 4.4),
                             sharey=False)
    if len(hilos) == 1:
        axes = [axes]
    for ax, h in zip(axes, hilos):
        datos, colores, etiquetas = [], [], []
        for c in cfgs:
            v = d["times"].get((kernel, h, c))
            if v:
                datos.append(v); colores.append(ESTILO[c]["color"]); etiquetas.append(c)
        if not datos:
            ax.set_visible(False); continue
        bp = ax.boxplot(datos, patch_artist=True, widths=0.55, showfliers=False,
                        medianprops=dict(color=TINTA, linewidth=1.6),
                        whiskerprops=dict(color=EJE, linewidth=1),
                        capprops=dict(color=EJE, linewidth=1))
        for caja, col in zip(bp["boxes"], colores):
            caja.set_facecolor(aclarar(col, 0.62))
            caja.set_edgecolor(col); caja.set_linewidth(1.5)
        ax.set_xticks(range(1, len(etiquetas) + 1))
        ax.set_xticklabels(etiquetas, rotation=30, ha="right")
        ax.set_title(f"{h} hilos", fontsize=11)
        ax.grid(axis="x", visible=False)
    axes[0].set_ylabel("Tiempo por repeticion (ms)")
    fig.suptitle(f"{titulo} — distribucion de las repeticiones", y=1.02)
    nota(fig, "Sin valores atipicos dibujados. Cada panel tiene su propia escala: "
              "interesa la FORMA de la distribucion, no comparar entre paneles.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 9. Mecanismo en el tiempo
# ─────────────────────────────────────────────────────────────────────────────
def fig_mecanismo(d, kernel, titulo, base, hilos_obj=None):
    hilos = hilos_de(d, kernel)
    h = (hilos_obj if hilos_obj in hilos else (hilos[-1] if hilos else None))
    if h is None:
        return
    filas = d["win"].get(f"{kernel}_t{h}_scheduler", [])
    if not filas:
        return
    por_hilo = defaultdict(list)
    for r in filas:
        t = num(r.get("t_ms")); b = num(r.get("bucket")); ra = num(r.get("ratio_rm"))
        # ratio_rm == -1 es el CENTINELA de "no medible" (sin eventos NUMA o
        # ventana sin fills). Dibujarlo como si fuera un dato produciria una
        # linea plana en -1 que se leeria como una medida real.
        if t is None or b is None or ra is None or ra < 0:
            continue
        por_hilo[r["tid"]].append((t, b, ra, num(r.get("thr_T")),
                                   r.get("flagged") == "1"))
    por_hilo = {k: v for k, v in por_hilo.items() if len(v) >= 3}
    if not por_hilo:
        print("  [--] mecanismo: sin ratio_rm medible (¿PMU sin los eventos NUMA?)")
        return
    for v in por_hilo.values():
        v.sort()
    # El hilo que mas llena el balde es el que cuenta la historia.
    protagonista = max(por_hilo, key=lambda k: max(p[1] for p in por_hilo[k]))

    fig, (a1, a2) = plt.subplots(2, 1, figsize=(8.4, 6.2), sharex=True)
    for tid, v in por_hilo.items():
        if tid == protagonista:
            continue
        a1.plot([p[0] for p in v], [p[1] for p in v], color=TINTA_TENUE,
                linewidth=0.6, alpha=0.28, zorder=1)
    v = por_hilo[protagonista]
    a1.plot([p[0] for p in v], [p[1] for p in v], color=ESTILO["scheduler"]["color"],
            linewidth=2, zorder=3, label=f"hilo {protagonista} (el que mas se llena)")
    disparos = [p[0] for p in v if p[4]]
    if disparos:
        a1.scatter(disparos, [p[1] for p in v if p[4]], s=70, marker="*",
                   color=ESTILO["close"]["color"], zorder=5, label="disparo de migracion")
    a1.plot([], [], color=TINTA_TENUE, linewidth=1, alpha=0.5,
            label=f"otros {len(por_hilo)-1} hilos")
    a1.set_ylabel("Nivel del balde")
    a1.set_title(f"{titulo} — mecanismo de decision con {h} hilos", fontsize=12)
    a1.legend(loc="upper left")

    a2.plot([p[0] for p in v], [p[2] for p in v], color=ESTILO["scheduler"]["color"],
            linewidth=1.6, label="ratio_rm del hilo")
    a2.plot([p[0] for p in v], [p[3] for p in v], color=ESTILO["spread"]["color"],
            linewidth=1.6, linestyle="--", label="umbral T = g_ref + MAD")
    a2.set_ylabel("Proporcion de accesos remotos")
    a2.set_xlabel("Tiempo desde el inicio del monitor (ms)")
    a2.legend(loc="upper left")
    nota(fig, "El balde se llena por el EXCESO de ratio_rm sobre el umbral y se vacia "
              "lentamente si no lo supera; exige elevacion sostenida, no un pico aislado.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
# 10. IPC
# ─────────────────────────────────────────────────────────────────────────────
def fig_ipc(d, kernel, titulo, base):
    hilos = hilos_de(d, kernel)
    fig, ax = plt.subplots(figsize=(7.2, 4.6))
    hay = False
    for cfg in ("spread", "close", "scheduler"):
        xs, ys = [], []
        for h in hilos:
            r = d["agg"].get((kernel, h, cfg))
            v = num(r.get("ipc")) if r else None
            if v is not None and v >= 0:
                xs.append(h); ys.append(v)
        if not xs:
            continue
        hay = True
        e = ESTILO[cfg]
        ax.plot(xs, ys, color=e["color"], marker=e["marker"], linestyle=e["ls"],
                linewidth=2, markersize=7, label=e["label"],
                markeredgecolor="white", markeredgewidth=1.2)
    if not hay:
        plt.close(fig); return
    eje_hilos(ax, hilos)
    ax.set_ylabel("IPC (instrucciones por ciclo)")
    ax.set_title(f"{titulo} — IPC segun el grado de paralelismo")
    ax.legend(loc="upper right")
    nota(fig, "ALCANCES DISTINTOS: en las estaticas es perf agregado de todo el proceso; "
              "en el scheduler es la suma de contadores por hilo (exclude_kernel=1), "
              "acotada por omp_control_tool. Comparables con esa salvedad.")
    guardar(fig, base)


# ─────────────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="perf_out_v4")
    ap.add_argument("--figdir", default="plots/figuras")
    ap.add_argument("--hilos-mecanismo", type=int, default=128)
    args = ap.parse_args()

    d = cargar(args.outdir)
    if not d["agg"]:
        print(f"[!] No hay kernel_metrics.csv utilizable en {args.outdir}")
        return 1
    os.makedirs(args.figdir, exist_ok=True)

    for kernel, titulo, campo, etiqueta in KERNELS:
        if not hilos_de(d, kernel):
            continue
        print(f"[{kernel}]")
        p = lambda n: os.path.join(args.figdir, f"{kernel}_{n}")
        fig_throughput(d, kernel, titulo, campo, etiqueta, p("01_throughput"))
        fig_speedup(d, kernel, titulo, p("02_speedup"))
        fig_overhead(d, kernel, titulo, p("03_overhead"))
        fig_descomposicion(d, kernel, titulo, p("04_descomposicion"))
        fig_escalado(d, kernel, titulo, p("05_escalado"))
        fig_localidad(d, kernel, titulo, p("06_localidad"))
        fig_migraciones(d, kernel, titulo, p("07_migraciones"))
        fig_boxplot(d, kernel, titulo, p("08_boxplot"))
        fig_mecanismo(d, kernel, titulo, p("09_mecanismo"), args.hilos_mecanismo)
        fig_ipc(d, kernel, titulo, p("10_ipc"))
    print(f"\n[ok] Figuras en {args.figdir}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
