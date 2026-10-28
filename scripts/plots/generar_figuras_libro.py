#!/usr/bin/env python3
"""Las 16 figuras del Capítulo 3 (Resultados) del libro, Figuras 3.1 a 3.16.

Reutiliza scripts/plots/plot_presentacion.py y scripts/plots/plot_results.py sin
modificarlos y aplica, al guardar cada figura:
  - sin título interno (el pie de figura del documento lo cubre);
  - leyendas con los nombres de la tabla de configuraciones:
    Base, Close, Spread, Interleave, Control, Scheduler;
  - rótulos de panel neutros (S3 · 191 MiB, S4 · 1,0 GiB, S5 · 4,1 GiB);
  - tildes, fondo blanco.
Genera además la figura de ganancia neta con intervalos de confianza entre
procesos (Figura 3.16; lee data/analisis/contrastes_scheduler.csv, que escribe
scripts/analisis/analisis_entre_procesos.py).

Los nombres de archivo son los que usa el libro (p01_curva_latencia.png, ...).
La correspondencia con el número de figura está en docs/thesis-map.md.

Uso:  python3 scripts/plots/generar_figuras_libro.py        (desde cualquier directorio)
Salida: data/figuras/libro/*.png
"""

import os
import re
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))      # este fichero vive en scripts/plots/
DESTINO = os.path.join(RAIZ, "data", "figuras", "libro")
CONTRASTES = os.path.join(RAIZ, "data", "analisis", "contrastes_scheduler.csv")

os.environ["CAMPANA"] = os.path.join(RAIZ, "data", "perf_out_v5")
os.environ["FIGDIR"] = DESTINO
os.environ["IDIOMA"] = "es"
sys.path.insert(0, AQUI)

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.text import Text
from matplotlib.ticker import FuncFormatter, ScalarFormatter

import plot_results as pr
import plot_presentacion as pp

pp.NOMBRE.update({"base": "Base", "obs": "Control", "scheduler": "Scheduler",
                  "bind_close": "Close", "bind_spread": "Spread", "interleave": "Interleave",
                  "bind_close_il": "Close+il", "bind_spread_il": "Spread+il"})
pp.KERNELS.update({"stencil": "Stencil 2D", "spmv_static": "SpMV"})
pp.TITULO_TAM.clear()
pp.TITULO_TAM.update({"S3": "S3 · 191 MiB", "S4": "S4 · 1,0 GiB", "S5": "S5 · 4,1 GiB"})
pp.TITULO_TAM_CORTO.clear()
pp.TITULO_TAM_CORTO.update(pp.TITULO_TAM)

# Textos fijos de las funciones de plot_presentacion que no pasan por los
# diccionarios anteriores.
REEMPLAZO_EXACTO = {
    "memoria en el nodo LOCAL": "Memoria local",
    "memoria en el nodo REMOTO": "Memoria remota",
    "L3 chiplet\n32 MiB": "L3 CCD\n32 MiB",
    "(a) No hay nada que corregir en S0-S2": "(a) Accesos remotos en la configuración Base",
    "(b) No hay tiempo para actuar en S0-S2": "(b) Ventanas de monitorización observadas",
    "menos del 1 %\nde accesos remotos": "menos del 1 %\nde accesos remotos",
    "5 ventanas — lo que el mecanismo debe\nobservar antes de poder migrar":
        "Mínimo de 5 ventanas\npara autorizar una migración",
    "accesos servidos desde el OTRO nodo (%)": "accesos servidos desde el otro nodo (%)",
    "la propuesta GANA tiempo": "Scheduler más rápido que Base",
    "la propuesta CUESTA tiempo": "Scheduler más lento que Base",
    "frente a OpenMP sin optimizar (%)": "diferencia frente a Base (%)",
    "OpenMP sin optimizar (referencia)": "Base",
    "tiempo final con la propuesta": "Scheduler",
    "lo que el usuario se ahorra": "Ahorro neto",
    "lo que la propuesta anade": "Tiempo añadido",
    "control: medir sin migrar": "Control",
}
REEMPLAZO_REGEX = [
    (r"^Fase 0 .*", ""),                      # título interno de la curva de latencia
    (r"^ahorra\n", "ahorro\n"),
    (r"^anade\n", "añadido\n"),
]
TILDES = [
    (r"\bnumero\b", "número"), (r"\btamanos\b", "tamaños"), (r"\btamano\b", "tamaño"),
    (r"\brepeticion\b", "repetición"), (r"\bcache\b", "caché"), (r"\bmas\b", "más"),
    (r"\brapido\b", "rápido"), (r"\blatencia de acceso\b", "latencia de acceso"),
    (r"\bpenalizacion\b", "penalización"), (r"\bmonitorizacion\b", "monitorización"),
]


# Coma decimal, como en el texto del documento ("1,86x" -> "1,86×").
DECIMAL = re.compile(r"(?<=\d)\.(?=\d)")
FACTOR = re.compile(r"(?<=\d)x\b")


def numero_es(v, pos=None):
    if abs(v - round(v)) < 1e-9:
        n = int(round(v))
        s = f"{n:,}".replace(",", " ") if abs(n) >= 10000 else str(n)
    else:
        s = DECIMAL.sub(",", f"{v:g}")
    return s.replace("-", "−")


def coma_decimal(fig):
    """Marcas de los ejes lineales con coma decimal; los logarítmicos no cambian."""
    for ax in fig.axes:
        for eje, escala in ((ax.xaxis, ax.get_xscale()), (ax.yaxis, ax.get_yscale())):
            if escala == "linear" and isinstance(eje.get_major_formatter(), ScalarFormatter):
                eje.set_major_formatter(FuncFormatter(numero_es))


def limpiar(fig):
    if getattr(fig, "_suptitle", None) is not None:
        fig._suptitle.set_text("")
    coma_decimal(fig)
    for t in fig.findobj(Text):
        s = t.get_text()
        if not s:
            continue
        nuevo = REEMPLAZO_EXACTO.get(s, s)
        for patron, rep in REEMPLAZO_REGEX + TILDES:
            nuevo = re.sub(patron, rep, nuevo)
        nuevo = FACTOR.sub("×", DECIMAL.sub(",", nuevo))
        if nuevo != s:
            t.set_text(nuevo)


def guardar(fig, figdir, nombre):
    limpiar(fig)
    ruta = os.path.join(DESTINO, f"{nombre}.png")
    fig.savefig(ruta, facecolor="white")
    plt.close(fig)
    return ruta


def sin_nota(fig, texto):
    return None


for modulo in (pr, pp):
    modulo.guardar = guardar
    modulo.nota = sin_nota


def fmt(v, dec=1):
    return f"{v:.{dec}f}".replace(".", ",")


def _etiqueta(ax, x, y, arriba, color):
    ax.annotate(f"{y:.3g}", xy=(x, y),
                xytext=(0, 7 if arriba else -7), textcoords="offset points",
                ha="center", va="bottom" if arriba else "top", fontsize=7.5, color=color,
                zorder=7, clip_on=False,
                bbox=dict(boxstyle="round,pad=0.12", fc="white", ec="none", alpha=0.85))


def f08_estaticas_lineas(df, kernel, figdir):
    """Versión de scripts/plots/plot_presentacion.f08_estaticas_lineas con etiquetas que no
    se superponen (observación P8 de la segunda revisión): en cada recuento de
    hilos, la etiqueta del mayor de los dos valores rotulados (Base y Scheduler)
    va encima de su punto y la del menor debajo, con fondo blanco."""
    d = pp.solo_tamanos(df[(df["kernel"] == kernel) & df["config"].isin(pp.CFG_COMPARACION)])
    if d.empty:
        return None
    fig, ejes = pp.rejilla_tam(alto=4.3, ancho=5.9)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        for cfg in pp.CFG_COMPARACION:
            r = sub[sub["config"] == cfg].sort_values("threads")
            if r.empty:
                continue
            propuesta = (cfg == "scheduler")
            ax.plot(r["threads"], r["avg_ms"], marker="o" if propuesta else "s",
                    ms=6 if propuesta else 4.5, lw=2.6 if propuesta else 1.7,
                    color=pp.COLOR[cfg], label=pp.NOMBRE[cfg], zorder=5 if propuesta else 3)
        b = sub[sub["config"] == "base"].set_index("threads")["avg_ms"]
        s = sub[sub["config"] == "scheduler"].set_index("threads")["avg_ms"]
        for h in hilos:
            if h in b.index and h in s.index:
                sched_arriba = s[h] >= b[h]
                _etiqueta(ax, h, b[h], not sched_arriba, pp.COLOR["base"])
                _etiqueta(ax, h, s[h], sched_arriba, pp.COLOR["scheduler"])
        pp.eje_hilos(ax, hilos)
        ax.set_yscale("log")
        ax.set_ylabel("tiempo por repetición (ms, escala log)")
        ax.set_title(pp.TITULO_TAM.get(tam, tam), fontsize=11.5, color=pp.TINTA)
        ax.margins(y=0.22)
    ejes[pp.TAMANOS[0]].legend(frameon=False, fontsize=8.5, loc="best")
    fig.tight_layout()
    return guardar(fig, figdir, f"p08_{kernel}_estaticas")


def figura_intervalos():
    """Ganancia neta del scheduler frente a Base (%), con IC del 95 % entre procesos."""
    if not os.path.exists(CONTRASTES):
        print("  (sin contrastes_scheduler.csv: ejecutar scripts/analisis/analisis_entre_procesos.py)")
        return None
    c = pd.read_csv(CONTRASTES)
    c = c[c.size_tag.isin(["S4", "S5"])]
    fig, ejes = plt.subplots(1, 2, figsize=(13.5, 4.6), sharey=False)
    serie = {"S4": (pr.AZUL, "o", -0.14, "S4 · 1,0 GiB"), "S5": (pr.NARANJA, "s", 0.14, "S5 · 4,1 GiB")}
    for ax, (kernel, titulo) in zip(ejes, [("spmv_static", "SpMV"), ("stencil", "Stencil 2D")]):
        sub = c[c.kernel == kernel]
        hilos = sorted(sub.threads.unique())
        x = np.arange(len(hilos))
        for tam, (color, marcador, dx, etiqueta) in serie.items():
            s = sub[sub.size_tag == tam].set_index("threads").reindex(hilos)
            y = 100 * s.G_neta / s.T_base
            lo = 100 * s.G_neta_lo / s.T_base
            hi = 100 * s.G_neta_hi / s.T_base
            ax.errorbar(x + dx, y, yerr=[y - lo, hi - y], fmt=marcador, color=color,
                        ms=8, lw=2, capsize=4, capthick=2, label=etiqueta,
                        markeredgecolor="white", markeredgewidth=1.2, zorder=4)
            izquierda = dx < 0
            for xi, yi, pi in zip(x + dx, y, s.G_neta_p):
                if np.isfinite(yi) and pi < 0.05 and yi >= 5:
                    ax.annotate(fmt(yi) + " %", xy=(xi, yi), xytext=(-9 if izquierda else 9, 0),
                                textcoords="offset points", ha="right" if izquierda else "left",
                                va="center", fontsize=8, color=pr.TINTA_2,
                                bbox=dict(boxstyle="round,pad=0.1", fc="white", ec="none", alpha=0.85))
        ax.axhline(0, color=pr.TINTA, lw=1)
        ax.set_xticks(x)
        ax.set_xticklabels([str(h) for h in hilos])
        ax.set_xlabel("número de hilos")
        ax.set_ylabel("ganancia neta frente a Base (%)")
        ax.set_title(titulo, fontsize=11.5, color=pr.TINTA)
        ax.margins(x=0.08, y=0.15)
    ejes[0].legend(frameon=False, fontsize=9, loc="upper right")
    fig.tight_layout()
    return guardar(fig, DESTINO, "p13_ganancia_ic_procesos")


def main():
    pr.estilo()
    plt.rcParams.update({"figure.facecolor": "white", "axes.facecolor": "white",
                         "savefig.facecolor": "white"})
    os.makedirs(DESTINO, exist_ok=True)
    df = pp.cargar(pp.CAMPANA)
    hechas = [pp.f01_curva_latencia(DESTINO), pp.f03_por_que_no_s0s2(df, DESTINO),
              pp.f04_migraciones(df, DESTINO)]
    for kernel in ("spmv_static", "stencil"):
        for f in (pp.f05_tiempo, pp.f06_ganancia_pct, pp.f07_descomposicion_apilada,
                  f08_estaticas_lineas):
            hechas.append(f(df, kernel, DESTINO))
        metricas = ("gflops", "gibs") if kernel == "spmv_static" else ("mlups", "gflops")
        for metrica in metricas:
            hechas.append(pp.f10_throughput(df, kernel, metrica, DESTINO))
    hechas.append(figura_intervalos())
    hechas = [h for h in hechas if h]
    print(f"{len(hechas)} figuras en {DESTINO}")
    for h in hechas:
        print("  " + os.path.basename(h))


if __name__ == "__main__":
    main()
