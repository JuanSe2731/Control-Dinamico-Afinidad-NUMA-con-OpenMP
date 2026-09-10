#!/usr/bin/env python3
"""Figuras de la caracterizacion de latencias (Fase 0) -> plots/figuras.

Lee caracterizacion/latencias_exadell.csv, producido por latencias_numa.cpp, y
genera las tres figuras que sostienen todo el marco teorico del trabajo:

  lat_curva    modo A: latencia frente al tamano del working set, memoria local
               frente a remota, con y sin paginas enormes. Los escalones marcan
               L1d, L2 y la L3 del propio chiplet.

  lat_heatmap  modo B: mapa de calor nucleo a nucleo. Es la figura que hace
               VISIBLE la estructura de chiplets, y ademas es el control que
               valida (o refuta) la hipotesis "el nucleo fisico c vive en el CCD
               c/8" sobre la que se apoya el diseno de tamanos de la Fase 2.

  lat_clases   modos B y C: latencia por clase topologica. El modo B mide
               transferencia de propiedad de linea (coherencia, escritura); el
               modo C mide LECTURA desde la cache ajena, que es lo que hacen de
               verdad los kernels. Son magnitudes distintas del mismo enlace y
               por eso se dibujan juntas pero separadas.

Las latencias de aqui se reutilizan fuera de las figuras: multiplicadas por los
rellenos por origen dan el tiempo muerto de espera estimado, que es la forma de
convertir "el scheduler quita espera" en un numero con unidades.
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

AZUL, NARANJA, AGUA = "#2a78d6", "#eb6834", "#1baf7a"
TINTA, TINTA_2, TINTA_MUTE = "#0b0b0b", "#52514e", "#898781"
REJILLA, EJE, SUPERFICIE = "#e1e0d9", "#c3c2b7", "#fcfcfb"

# Rampa secuencial azul: magnitud continua (latencia), un solo tono, claro->oscuro.
RAMPA = "Blues"

ORDEN_CLASES = ["smt", "mismo_ccd", "otro_ccd", "otro_ccd_lejano", "otro_nodo"]
ETIQUETA_CLASE = {
    "smt":             "mismo nucleo\n(SMT)",
    "mismo_ccd":       "mismo CCD\n(L3 compartida)",
    "otro_ccd":        "otro CCD\n(Infinity Fabric)",
    "otro_ccd_lejano": "otro CCD lejano",
    "otro_nodo":       "otro nodo\n(xGMI)",
}

# Limites reales de exadell (AMD EPYC 9554, Zen 4).
MARCAS_NIVEL = [
    (32 * 1024,        "L1d 32 KiB"),
    (1024 * 1024,      "L2 1 MiB"),
    (32 * 1024**2,     "L3 del CCD\n32 MiB"),
    (256 * 1024**2,    "L3 del nodo\n256 MiB"),
]


def estilo():
    sns.set_theme(style="whitegrid", context="notebook")
    plt.rcParams.update({
        "figure.facecolor": SUPERFICIE, "axes.facecolor": SUPERFICIE,
        "savefig.facecolor": SUPERFICIE, "savefig.dpi": 200,
        "savefig.bbox": "tight", "axes.edgecolor": EJE,
        "axes.labelcolor": TINTA_2, "axes.titlecolor": TINTA,
        "grid.color": REJILLA, "grid.linewidth": 0.8, "text.color": TINTA,
        "xtick.color": TINTA_MUTE, "ytick.color": TINTA_MUTE,
        "legend.frameon": False, "lines.linewidth": 2.0,
        "lines.markersize": 7, "font.size": 10,
    })


def guardar(fig, figdir, nombre):
    ruta = os.path.join(figdir, f"{nombre}.png")
    fig.savefig(ruta)
    plt.close(fig)
    return ruta


def nota(fig, texto):
    fig.text(0.005, -0.02, texto, fontsize=8, color=TINTA_MUTE, ha="left", va="top")


def fig_curva(df, figdir):
    d = df[df["modo"] == "A"].copy()
    if d.empty:
        return None

    variantes = sorted(d["thp"].unique(), reverse=True)
    fig, axes = plt.subplots(1, len(variantes), squeeze=False,
                             figsize=(6.2 * len(variantes), 4.6))
    for i, thp in enumerate(variantes):
        ax = axes[0][i]
        sub = d[d["thp"] == thp]
        for clase, color in (("local", AZUL), ("remoto", NARANJA)):
            s = sub[sub["clase"] == clase].sort_values("bytes")
            if s.empty:
                continue
            ax.plot(s["bytes"], s["ns_min"], marker="o", color=color,
                    label=f"memoria {clase}")
        for x, texto in MARCAS_NIVEL:
            if sub["bytes"].min() <= x <= sub["bytes"].max():
                ax.axvline(x, color=EJE, linestyle=":", linewidth=1.2)
                ax.annotate(texto, xy=(x, 1.0), xycoords=("data", "axes fraction"),
                            xytext=(3, -12), textcoords="offset points",
                            fontsize=7.5, color=TINTA_MUTE, ha="left", va="top")
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xlabel("tamano del working set (bytes)")
        ax.set_ylabel("latencia por acceso (ns)")
        ax.set_title("paginas enormes (THP)" if thp == 1 else "paginas de 4 KiB")
        ax.legend(loc="upper left")

    fig.suptitle("exadell — latencia frente al tamano  ·  MENOS ES MEJOR",
                 y=1.03, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Persecucion de punteros de un hilo, minimo de N repeticiones. La "
              "diferencia entre los dos paneles es el coste de los fallos de TLB: sin "
              "paginas enormes, un recorrido aleatorio de unos pocos MiB toca cientos "
              "de paginas y el escalon de la L3 queda enterrado bajo recorridos de "
              "tablas. Este modo NO alcanza la L3 de otro chiplet: los datos de un "
              "hilo solo se cachean en su propio CCD (ver lat_clases).")
    return guardar(fig, figdir, "lat_curva")


def fig_heatmap(df, figdir):
    d = df[(df["modo"] == "B") & (df["cpu_calienta"] >= 0)].copy()
    # Solo tiene sentido como mapa si se corrio --matriz (miles de pares).
    if len(d) < 100:
        return None

    piv = d.pivot_table(index="cpu_mide", columns="cpu_calienta",
                        values="ns_min", aggfunc="mean")
    # El recorrido solo hace a<b: se refleja para que el mapa salga completo.
    # Y se reordenan los ejes: al reflejar, la CPU 0 quedaba al final y rompia la
    # lectura por bloques, que es justo lo que esta figura tiene que mostrar.
    piv = piv.combine_first(piv.T)
    orden = sorted(set(piv.index) | set(piv.columns))
    piv = piv.reindex(index=orden, columns=orden)

    fig, ax = plt.subplots(figsize=(8.6, 7.4))
    sns.heatmap(piv, ax=ax, cmap=RAMPA, cbar_kws={"label": "latencia (ns)"},
                square=True, xticklabels=8, yticklabels=8)
    ax.set_xlabel("CPU")
    ax.set_ylabel("CPU")
    ax.set_title("exadell — latencia nucleo a nucleo", color=TINTA)
    fig.tight_layout()
    nota(fig, "Ping-pong sobre una linea compartida. Los bloques de 8x8 son los CCD y "
              "el contraste entre cuadrantes son los dos sockets. SI ESOS BLOQUES NO "
              "APARECEN, la hipotesis de que el nucleo fisico c vive en el CCD c/8 es "
              "falsa y hay que rehacer el diseno de tamanos de la Fase 2 antes de nada.")
    return guardar(fig, figdir, "lat_heatmap")


def fig_clases(df, figdir):
    d = df[df["modo"].isin(["B", "C"])].copy()
    d = d[d["clase"].isin(ORDEN_CLASES)]
    if d.empty:
        return None
    # Del modo B se descarta la matriz completa: aqui interesan los pares
    # representativos, no las 8128 combinaciones.
    d = d.groupby(["modo", "clase"], as_index=False)["ns_min"].min()

    d["orden"] = d["clase"].map({c: i for i, c in enumerate(ORDEN_CLASES)})
    d = d.sort_values("orden")
    d["etiqueta"] = d["clase"].map(ETIQUETA_CLASE)
    d["medida"] = d["modo"].map({
        "B": "transferencia de linea (ping-pong)",
        "C": "lectura desde la cache ajena",
    })

    fig, ax = plt.subplots(figsize=(8.8, 4.8))
    sns.barplot(data=d, x="etiqueta", y="ns_min", hue="medida", ax=ax,
                palette={"transferencia de linea (ping-pong)": AZUL,
                         "lectura desde la cache ajena": AGUA},
                edgecolor=SUPERFICIE, linewidth=2)
    ax.set_xlabel("")
    ax.set_ylabel("latencia (ns)")
    ax.set_title("exadell — latencia por distancia topologica  ·  MENOS ES MEJOR",
                 color=TINTA)
    ax.legend(loc="upper left")
    fig.tight_layout()
    nota(fig, "Dos magnitudes del mismo enlace, no dos medidas de lo mismo: el "
              "ping-pong mide transferencia de PROPIEDAD de la linea (coherencia, "
              "escritura) y la victima mide LECTURA desde la cache ajena, que es lo que "
              "hacen los kernels. El salto de 'mismo CCD' a 'otro CCD' es el coste del "
              "Infinity Fabric intra-socket; el de 'otro nodo' es el del xGMI.")
    return guardar(fig, figdir, "lat_clases")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", default="caracterizacion/latencias_exadell.csv")
    ap.add_argument("--figdir", default="plots/figuras")
    args = ap.parse_args()

    if not os.path.exists(args.csv):
        sys.exit(f"ERROR: no existe {args.csv}\n"
                 f"       Ejecuta antes: sbatch caracterizacion/run_latencias.sbatch")

    estilo()
    os.makedirs(args.figdir, exist_ok=True)
    df = pd.read_csv(args.csv)
    for c in ("bytes", "cpu_mide", "cpu_calienta", "nodo_mem", "thp",
              "ns_min", "ns_media", "ns_p95"):
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")

    generadas = [r for r in (fig_curva(df, args.figdir),
                             fig_heatmap(df, args.figdir),
                             fig_clases(df, args.figdir)) if r]
    print(f"{len(generadas)} figuras en {args.figdir}")
    for r in generadas:
        print("  " + os.path.basename(r))


if __name__ == "__main__":
    main()
