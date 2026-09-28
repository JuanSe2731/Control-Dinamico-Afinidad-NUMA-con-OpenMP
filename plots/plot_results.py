#!/usr/bin/env python3
"""Figuras de la campaña V5 — seaborn, solo PNG, todo en plots/figuras.

QUÉ CAMBIA RESPECTO A LA VERSIÓN ANTERIOR
-----------------------------------------
1. TODO EN MILISEGUNDOS ABSOLUTOS. Las figuras de speedup (T_ref/T_nuevo) y de
   sobrecoste ((T/T_ref - 1)*100) se han eliminado: son de donde salían el "<1" y
   el "sobrecoste negativo" que nadie entendía. Un sobrecoste negativo es una
   ganancia, y llamarlo así hace que el signo apunte al lado bueno.

2. LOS TÉRMINOS SUMAN, NO MULTIPLICAN. En milisegundos:
       C_monitor = T_obs  - T_base      coste de instrumentar
       G_migrar  = T_obs  - T_sched     ganancia por migrar
       G_neta    = T_base - T_sched     ganancia neta = G_migrar - C_monitor
   La descomposición anterior era un producto de razones y necesitaba una sección
   entera de la guía advirtiendo "se multiplican, no se suman".

3. SOLO QUEDA UNA MAGNITUD EN GiB/s, Y ES TRAFICO REAL.
       bw_remoto_gibs = rellenos_remotos * 64 B / t -> MENOS es mejor
   `caudal_util_gibs` se retira en V6: era bytes_del_modelo / tiempo con los bytes
   fijados por el tamano, o sea 1/t reescalado, y se confundia sistematicamente con
   lo anterior. Retirado el numerador constante, no queda ambiguedad posible.

4. Se eliminan, por indicación del director: el speedup relativo por hilos, el
   volumen de tráfico remoto y la ventana de la primera migración.

DISEÑO DE COLOR
---------------
Paleta de referencia de la skill dataviz, sin modificar. Como las figuras son
paneles múltiples (facetas por tamaño), aplica la regla de "todos los pares", y
solo los TRES primeros slots la superan en ambos modos. Por eso las figuras de
línea llevan como mucho tres series de identidad (base / obs / scheduler) y las
demás comparaciones se resuelven con mapas de calor de rampa secuencial en vez de
con más colores.

El desglose de tráfico por origen NO es categórico: es una magnitud ordenada por
distancia al núcleo (L2 -> L3 propia -> CCD vecino -> DRAM local -> otro socket),
así que usa una rampa secuencial de un solo tono, cerca=claro, lejos=oscuro.

Dependencias: seaborn + pandas + matplotlib. El análisis es LOCAL; el cluster solo
produce los CSV.
"""

import argparse
import os
import sys

import textwrap

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

# ─────────────────────────────────────────────────────────────────────────────
# Paleta (skill dataviz, instancia de referencia, modo claro)
# ─────────────────────────────────────────────────────────────────────────────

# Slots categóricos 1-3: los únicos que superan la comprobación de todos los pares
# en ambos modos, que es la que aplica a los paneles múltiples.
AZUL, NARANJA, AGUA = "#2a78d6", "#eb6834", "#1baf7a"

# Tinta y cromo. El texto NUNCA lleva el color de la serie: la identidad la carga
# la marca de color que tiene al lado.
TINTA        = "#0b0b0b"
TINTA_2      = "#52514e"
TINTA_MUTE   = "#898781"
REJILLA      = "#e1e0d9"
EJE          = "#c3c2b7"
SUPERFICIE   = "#fcfcfb"

# Rampa secuencial azul, tramos ordinales (el más claro no baja del tramo 250 para
# no perderse contra la superficie). Ordenada por DISTANCIA al núcleo.
RAMPA_DISTANCIA = ["#86b6ef", "#2a78d6", "#184f95", "#0d366b"]

# Rampa DIVERGENTE para la comparacion contra el caso base: dos tonos opuestos y
# un gris neutro en el centro (la regla es "dos hues + gris", nunca un tono en el
# medio). El brazo rojo NO se eligio a ojo: cada paso tiene la MISMA L y la MISMA
# croma en OKLCH que el paso azul que le corresponde, asi que los dos brazos pesan
# igual visualmente y ninguno de los dos lados grita mas que el otro.
#   azul  #184f95 L=0.433 C=0.128   <->  rojo #892b2a
#   azul  #2a78d6 L=0.575 C=0.163   <->  rojo #c74845
#   azul  #86b6ef L=0.764 C=0.097   <->  rojo #ea9a93
# En una rampa divergente lo que se comprueba es la monotonia de luminosidad por
# brazo, no el contraste entre pares: pasarle el validador categorico da FAIL por
# diseno.
RAMPA_DIVERGENTE = ["#184f95", "#2a78d6", "#86b6ef",
                    "#f0efec",
                    "#ea9a93", "#c74845", "#892b2a"]

# Solo se miden las TRES que cruzan una interconexion. Con 5 contadores libres (el
# nmi_watchdog ocupa uno de los 6) y 2 gastados en el par exacto de ratio_rm, quedan
# 3. La banda "local" NO se mide: se DERIVA como all - (las tres lejanas), y sale
# exacta porque all viene del par.
ORIGENES = [
    ("fills_locales",      "local (mismo chiplet)"),   # derivada
    ("fills_ccd_vecino",   "L3 de otro CCD"),
    ("fills_far_cache",    "cache del otro nodo"),
    ("fills_far_dram",     "DRAM remota"),
]

# Las que sí salen de un contador propio.
ORIGENES_MEDIDOS = ["fills_ccd_vecino", "fills_far_cache", "fills_far_dram"]

# Las tres configuraciones que forman el argumento central. El color sigue a la
# ENTIDAD: si una faceta se queda sin alguna, las demás no cambian de color.
COLOR_CFG = {"base": AZUL, "obs": NARANJA, "scheduler": AGUA}
ETIQUETA_CFG = {
    "base":      "caso base (OpenMP puro)",
    "obs":       "base + monitorizar",
    "scheduler": "scheduler (migra)",
}
CFG_NUCLEO = ["base", "obs", "scheduler"]

# Las seis estáticas, en el orden del diseño factorial afinidad x politica.
CFG_ESTATICAS = ["base", "bind_close", "bind_spread",
                 "interleave", "bind_close_il", "bind_spread_il"]

# LAS OCHO, en el orden del diseno: la referencia, las cinco estaticas, el control
# de monitorizacion y la propuesta. Es el unico sitio donde los dos grupos que el
# resto de figuras separa (CFG_NUCLEO y CFG_ESTATICAS) se ven juntos.
CFG_TODAS = ["base", "bind_close", "bind_spread", "interleave",
             "bind_close_il", "bind_spread_il", "obs", "scheduler"]

ETIQUETA_TODAS = {
    "base":           "base (OpenMP puro)",
    "bind_close":     "close",
    "bind_spread":    "spread",
    "interleave":     "interleave",
    "bind_close_il":  "close + interleave",
    "bind_spread_il": "spread + interleave",
    "obs":            "obs (solo monitoriza)",
    "scheduler":      "scheduler (migra)",
}

ORDEN_TAMANOS = ["S0", "S1", "S2", "S3", "S4", "S5"]
TITULO_TAMANO = {
    "S0": "S0 · L1d de un CCD",
    "S1": "S1 · L2 de un CCD",
    "S2": "S2 · L3 de UN CCD",
    "S3": "S3 · L3 de un nodo",
    "S4": "S4 · sobre la L3 total",
    "S5": "S5 · DRAM / NUMA",
}

KERNELS = {
    "stencil":     ("Stencil 2D", "mlups", "MLUPS"),
    "spmv_static": ("SpMV CSR",   "gflops", "GFLOPS"),
}


def estilo():
    sns.set_theme(style="whitegrid", context="notebook")
    plt.rcParams.update({
        "figure.facecolor":  SUPERFICIE,
        "axes.facecolor":    SUPERFICIE,
        "savefig.facecolor": SUPERFICIE,
        "savefig.dpi":       200,
        "savefig.bbox":      "tight",
        "axes.edgecolor":    EJE,
        "axes.labelcolor":   TINTA_2,
        "axes.titlecolor":   TINTA,
        "axes.titlesize":    11,
        "axes.titleweight":  "semibold",
        "grid.color":        REJILLA,
        "grid.linewidth":    0.8,
        "text.color":        TINTA,
        "xtick.color":       TINTA_MUTE,
        "ytick.color":       TINTA_MUTE,
        "legend.frameon":    False,
        "lines.linewidth":   2.0,       # marcas finas
        "lines.markersize":  7,         # >= 8 px en pantalla a 200 dpi
        "font.size":         10,
    })


def guardar(fig, figdir, nombre):
    """Solo PNG. El .eps se ha retirado: ya no hace falta para la memoria, y con
    él se va el apaño de mezclar-con-blanco que existía solo porque PostScript no
    admite transparencia."""
    ruta = os.path.join(figdir, f"{nombre}.png")
    fig.savefig(ruta)
    plt.close(fig)
    return ruta


def nota(fig, texto):
    """Las notas al pie se ajustan al ancho de la figura.

    Sin envolver, una nota larga se dibuja como una unica linea y, con
    bbox_inches="tight" al guardar, ENSANCHA el lienzo hasta donde llegue el
    texto: la figura del trafico paso de 4747 a 7511 px de ancho al alargar su
    nota, dejando los paneles diminutos."""
    ancho_pulgadas = fig.get_size_inches()[0]
    # ~11 caracteres por pulgada a 8 pt es lo que cabe sin desbordar.
    columnas = max(60, int(ancho_pulgadas * 11))
    envuelto = "\n".join(textwrap.wrap(texto, width=columnas))
    fig.text(0.005, -0.02, envuelto, fontsize=8, color=TINTA_MUTE,
             ha="left", va="top")


# ─────────────────────────────────────────────────────────────────────────────
# Carga
# ─────────────────────────────────────────────────────────────────────────────

def cargar(outdir):
    ruta = os.path.join(outdir, "kernel_metrics.csv")
    if not os.path.exists(ruta):
        sys.exit(f"ERROR: no existe {ruta}")
    df = pd.read_csv(ruta)

    numericas = ["threads", "min_ms", "avg_ms", "max_ms", "stdev_ms", "ws_bytes",
                 "mlups", "gflops", "ratio_rm",
                 "bw_remoto_gibs", "gib_remotos_total", "migrations", "n_or_rows",
                 "fills_l2", "fills_l3_ccd", "fills_dram_local"] + ORIGENES_MEDIDOS
    for c in numericas:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")

    # Banda "local" derivada: todo lo que NO cruzo una interconexion. Se obtiene de
    # all_fills (exacto, del par) menos las tres lejanas medidas. Si ratio_rm existe,
    # all = remoto/ratio_rm; si no, se aproxima con lo que haya.
    import numpy as np
    if all(c in df.columns for c in ORIGENES_MEDIDOS) and "ratio_rm" in df.columns:
        lejanas = df[ORIGENES_MEDIDOS].fillna(0).sum(axis=1)
        remoto = df[["fills_far_cache", "fills_far_dram"]].fillna(0).sum(axis=1)
        # total = remoto / ratio_rm. Un ratio de 0 o NA deja el total indefinido, y
        # eso es correcto: significa que no hubo medida, no que fuera cero.
        ratio = pd.to_numeric(df["ratio_rm"], errors="coerce")
        total = remoto.divide(ratio.where(ratio > 0))
        df["fills_locales"] = (total - lejanas).replace(
            [np.inf, -np.inf], np.nan).clip(lower=0)
    else:
        df["fills_locales"] = np.nan

    # ratio_rm = -1 es el centinela de "no medible" (sin eventos NUMA), que NO es
    # lo mismo que 0: un 0 genuino significa "todos los rellenos fueron locales".
    if "ratio_rm" in df.columns:
        df.loc[df["ratio_rm"] < 0, "ratio_rm"] = pd.NA

    df["size_tag"] = pd.Categorical(df["size_tag"], categories=ORDEN_TAMANOS, ordered=True)
    return df


def tamanos_presentes(df):
    return [s for s in ORDEN_TAMANOS if (df["size_tag"] == s).any()]


def cargar_repeticiones(outdir, kernel, configs=None):
    """Long DataFrame (size_tag, threads, config, ms) desde los *_times.csv.

    Es la MUESTRA real: 150 filas por ejecucion, en orden de ejecucion. El
    kernel_metrics.csv agregado solo guarda min/avg/max/stdev, asi que cualquier
    figura que necesite cuantiles o la forma de la distribucion tiene que venir
    por aqui."""
    import glob
    import re
    patron = os.path.join(outdir, "metrics", f"{kernel}_*_times.csv")
    rx = re.compile(rf"^{re.escape(kernel)}_(S\d)_t(\d+)_([a-z_]+)_times\.csv$")
    permitidas = CFG_NUCLEO if configs is None else configs

    filas = []
    for ruta in sorted(glob.glob(patron)):
        m = rx.match(os.path.basename(ruta))
        if not m:
            continue
        tam, hilos, cfg = m.group(1), int(m.group(2)), m.group(3)
        if cfg not in permitidas:
            continue
        try:
            t = pd.read_csv(ruta)
        except Exception:
            continue
        if "time_ms" not in t.columns:
            continue
        for v in t["time_ms"]:
            filas.append({"size_tag": tam, "threads": hilos, "config": cfg, "ms": v})
    if not filas:
        return None
    g = pd.DataFrame(filas)
    g["size_tag"] = pd.Categorical(g["size_tag"], categories=ORDEN_TAMANOS,
                                   ordered=True)
    return g


def rejilla(df, ncols=3):
    """Devuelve (fig, dict tamaño->eje) con un panel por tamaño presente."""
    tams = tamanos_presentes(df)
    n = len(tams)
    ncols = min(ncols, n)
    nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows, ncols, figsize=(4.6 * ncols, 3.5 * nrows),
                             squeeze=False)
    ejes = {}
    for i, t in enumerate(tams):
        ejes[t] = axes[i // ncols][i % ncols]
    for j in range(n, nrows * ncols):
        axes[j // ncols][j % ncols].axis("off")
    return fig, ejes


def eje_hilos(ax, hilos):
    ax.set_xscale("log", base=2)
    ax.set_xticks(hilos)
    ax.set_xticklabels([str(h) for h in hilos])
    ax.set_xlabel("hilos")


def etiquetar_series(ax, entradas, sep_min=0.075):
    """Etiquetas directas en el último punto de cada serie, SIN solaparse.

    Las series que acaban en valores parecidos escribían su texto una encima de
    otra y salía ilegible ('obas' donde debía leerse obs y base). Aquí se pasan
    las posiciones a fracción del eje, se ordenan y se separa cada etiqueta al
    menos `sep_min`, de modo que la marca sigue apuntando a su punto pero el texto
    queda escalonado.

    entradas: lista de (x, y, texto). Debe llamarse con los límites del eje ya
    fijados, o sea al final del panel.
    """
    if not entradas:
        return
    inv = ax.transAxes.inverted()
    puntos = []
    for x, y, texto in entradas:
        px, py = ax.transData.transform((x, y))
        fx, fy = inv.transform((px, py))
        puntos.append([fx, fy, texto])

    puntos.sort(key=lambda p: p[1])
    for i in range(1, len(puntos)):
        if puntos[i][1] - puntos[i - 1][1] < sep_min:
            puntos[i][1] = puntos[i - 1][1] + sep_min

    for fx, fy, texto in puntos:
        ax.annotate(texto, xy=(fx, fy), xycoords="axes fraction",
                    xytext=(7, 0), textcoords="offset points",
                    fontsize=8.5, color=TINTA_2, va="center", ha="left",
                    annotation_clip=False)


# ─────────────────────────────────────────────────────────────────────────────
# 01 — tiempo absoluto
# ─────────────────────────────────────────────────────────────────────────────

def fig_tiempo(df, kernel, figdir):
    d = df[df["kernel"] == kernel]
    if d.empty:
        return None
    fig, ejes = rejilla(d)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        marcas = []
        for cfg in CFG_NUCLEO:
            s = sub[sub["config"] == cfg].sort_values("threads")
            if s.empty:
                continue
            ax.plot(s["threads"], s["avg_ms"], marker="o",
                    color=COLOR_CFG[cfg], label=ETIQUETA_CFG[cfg])
            # Banda de +-1 desviacion: la dispersion es parte del resultado, sobre
            # todo en las configuraciones que no fijan afinidad.
            ax.fill_between(s["threads"], s["avg_ms"] - s["stdev_ms"],
                            s["avg_ms"] + s["stdev_ms"],
                            color=COLOR_CFG[cfg], alpha=0.15, linewidth=0)
            marcas.append((s["threads"].iloc[-1], s["avg_ms"].iloc[-1], cfg))
        eje_hilos(ax, hilos)
        ax.set_yscale("log")
        etiquetar_series(ax, marcas)
        ax.set_ylabel("tiempo medio por repeticion (ms)")
        ax.set_title(TITULO_TAMANO.get(tam, tam))

    manejadores = [plt.Line2D([], [], color=COLOR_CFG[c], marker="o", lw=2,
                              label=ETIQUETA_CFG[c]) for c in CFG_NUCLEO]
    fig.legend(handles=manejadores, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.04))
    fig.suptitle(f"{KERNELS[kernel][0]} — tiempo de ejecucion  ·  MENOS ES MEJOR",
                 y=1.09, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Media de las repeticiones (no el minimo). Banda = ±1 desviacion tipica. "
              "Eje Y logaritmico: los tamanos abarcan cuatro ordenes de magnitud.")
    return guardar(fig, figdir, f"{kernel}_01_tiempo")


# ─────────────────────────────────────────────────────────────────────────────
# 01b — tiempo con banda de CUANTILES (candidata a sustituir a la 01)
# ─────────────────────────────────────────────────────────────────────────────

def fig_tiempo_cuantiles(df, kernel, outdir, figdir):
    """La misma lectura que la 01, con dos cambios y sin tocar la 01.

    1. LA BANDA ES p10-p90, NO media +- 1 sigma. La banda de sigma supone una
       distribucion simetrica que estas muestras no tienen: tienen un suelo (el
       tiempo minimo alcanzable) y una cola larga hacia arriba. En 9 de las 216
       celdas sigma es MAYOR que la media, asi que el borde inferior sale
       negativo y el eje logaritmico no puede dibujarlo: la banda se derrama
       hasta el suelo del panel y se lee como "el tiempo podria ser casi cero",
       que es falso. La p10-p90 no puede salir negativa y dice algo exacto: el
       80 % central de las repeticiones cayo aqui dentro.

    2. LA LINEA ES LA MEDIA, la misma que la 01 y la misma que reporta
       kernel_metrics.csv, de modo que la figura no introduce una tercera cifra
       que nadie mas usa. Lo unico que cambia respecto a la 01 es COMO se dibuja
       la dispersion a su alrededor.

    CONSECUENCIA QUE HAY QUE CONOCER: al ser la media el centro y los cuantiles
    la banda, en 21 de las 216 celdas la LINEA CAE FUERA DE SU PROPIA BANDA,
    siempre por arriba. No es un fallo de dibujo: es la definicion de una
    distribucion sesgada a la derecha. Si mas del 10 % de las repeticiones son
    muy lentas, la media se va por encima del p90 aunque el 80 % central este
    apretado. El caso de libro es SpMV S5 con 8 hilos y scheduler: p10-p90 =
    [266,4 , 267,0] y media 273,2, porque las 5 primeras repeticiones corren a
    414 ms, antes de que la migracion llegue a saltar. Donde la linea se sale de
    la banda hay una cola, y la cola es el resultado.
    """
    g = cargar_repeticiones(outdir, kernel)
    if g is None:
        return None

    resumen = (g.groupby(["size_tag", "threads", "config"], observed=True)["ms"]
                 .agg(p10=lambda x: x.quantile(0.10),
                      p50="median",
                      p90=lambda x: x.quantile(0.90),
                      media="mean")
                 .reset_index())

    fig, ejes = rejilla(resumen)
    for tam, ax in ejes.items():
        sub = resumen[resumen["size_tag"] == tam]
        if sub.empty:
            continue
        hilos = sorted(sub["threads"].unique())
        marcas = []
        for cfg in CFG_NUCLEO:
            r = sub[sub["config"] == cfg].sort_values("threads")
            if r.empty:
                continue
            ax.plot(r["threads"], r["media"], marker="o", color=COLOR_CFG[cfg],
                    label=ETIQUETA_CFG[cfg], zorder=3)
            ax.fill_between(r["threads"], r["p10"], r["p90"],
                            color=COLOR_CFG[cfg], alpha=0.15, linewidth=0)
            marcas.append((r["threads"].iloc[-1], r["media"].iloc[-1], cfg))
        eje_hilos(ax, hilos)
        # ESCALA LINEAL, como la figura 02, y a diferencia de la 01. En un eje
        # lineal los milisegundos se RESTAN: la distancia entre dos lineas es
        # directamente la diferencia en ms, y el grosor de la banda es
        # directamente la dispersion en ms. En el eje logaritmico de la 01 una
        # misma distancia visual significa una misma RAZON, no una misma
        # diferencia, y por eso alli no se puede medir nada a ojo.
        #
        # Lo que se paga: dentro de un panel los valores llegan a diferir 300x
        # (en S0, de 0,0063 ms a 2,03 ms), asi que las series mas rapidas quedan
        # aplastadas contra el suelo en los recuentos de hilos bajos. Es el mismo
        # efecto que ya tiene la 02 en sus paneles S0 y S1. Para leer los valores
        # pequenos esta la 01, que para eso es logaritmica.
        etiquetar_series(ax, marcas)
        ax.set_ylabel("ms por repeticion")
        ax.set_title(TITULO_TAMANO.get(tam, tam))

    manejadores = [plt.Line2D([], [], color=COLOR_CFG[c], marker="o", lw=2,
                              label=ETIQUETA_CFG[c]) for c in CFG_NUCLEO]
    manejadores += [
        plt.Line2D([], [], color=TINTA_2, lw=2, marker="o",
                   label="media + banda p10-p90 (80 % central)"),
    ]
    fig.legend(handles=manejadores, loc="upper center", ncol=4,
               bbox_to_anchor=(0.5, 1.05), fontsize=9)
    fig.suptitle(f"{KERNELS[kernel][0]} — tiempo por repeticion, escala lineal"
                 "  ·  MENOS ES MEJOR",
                 y=1.11, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "VARIANTE de la figura 01, con DOS cambios. (1) Escala LINEAL, como la 02: "
              "las distancias en vertical son milisegundos y se pueden restar; a cambio, "
              "las series rapidas quedan aplastadas contra el suelo en S0-S2, y para "
              "leerlas hay que ir a la 01, que es logaritmica. (2) La banda es la franja "
              "p10-p90 (el 80 % central de las 150 repeticiones) en vez de media +- 1 "
              "desviacion tipica: no puede salir negativa y no supone simetria. La linea "
              "sigue siendo la MEDIA, la misma que la 01 y la de las tablas. En 21 de las "
              "216 celdas la linea queda POR ENCIMA de su propia banda: no es un fallo, es "
              "una cola de repeticiones lentas. En SpMV S5 con 8 hilos son las 5 previas a "
              "que salte la migracion.")
    return guardar(fig, figdir, f"{kernel}_01b_tiempo_cuantiles")


# ─────────────────────────────────────────────────────────────────────────────
# 02 — descomposición en milisegundos (la figura del boceto)
# ─────────────────────────────────────────────────────────────────────────────

def fig_descomposicion(df, kernel, figdir):
    d = df[df["kernel"] == kernel]
    if d.empty:
        return None
    fig, ejes = rejilla(d)
    hay_datos = False

    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        piv = sub.pivot_table(index="threads", columns="config",
                              values="avg_ms", aggfunc="mean")
        if not {"base", "obs", "scheduler"}.issubset(piv.columns):
            ax.set_title(TITULO_TAMANO.get(tam, tam))
            ax.text(0.5, 0.5, "faltan configuraciones", ha="center", va="center",
                    transform=ax.transAxes, color=TINTA_MUTE, fontsize=9)
            continue
        hay_datos = True
        hilos = list(piv.index)

        ax.plot(hilos, piv["base"], marker="o", color=AZUL, label="T base")
        ax.plot(hilos, piv["obs"], marker="s", color=NARANJA, label="T base + monitor")
        ax.plot(hilos, piv["scheduler"], marker="D", color=AGUA, label="T con scheduler")

        # El overhead de la PROPUESTA COMPLETA: monitorizar y migrar, frente a
        # OpenMP puro. Es −G_neta, o sea la distancia vertical entre la linea azul
        # y la de agua, dibujada explicitamente. NEGATIVO = la propuesta gana.
        #
        # Antes aqui iba C_monitor = T_obs − T_base (solo instrumentar). Sigue
        # siendo legible en la figura sin dibujarla: es el hueco entre la azul y la
        # naranja. Con las tres lineas absolutas puestas, los tres terminos de la
        # descomposicion son huecos verticales, y el que se dibuja aparte es el que
        # decide si el trabajo aporta algo.
        #
        # Va en tinta neutra, no en el cuarto slot categorico: ese slot es amarillo
        # y quedaria junto al naranja, que es el par que la propia paleta declara
        # que no separa.
        ovh_propuesta = piv["scheduler"] - piv["base"]
        ax.plot(hilos, ovh_propuesta, marker="^", color=TINTA_MUTE, linestyle="--",
                label="overhead de la propuesta")

        ax.axhline(0.0, color=EJE, linewidth=1)
        eje_hilos(ax, hilos)
        ax.set_ylabel("ms por repeticion (y diferencias)")
        ax.set_title(TITULO_TAMANO.get(tam, tam))

    if not hay_datos:
        plt.close(fig)
        return None

    manejadores = [
        plt.Line2D([], [], color=AZUL,       marker="o", lw=2, label="T base (OpenMP puro)"),
        plt.Line2D([], [], color=NARANJA,    marker="s", lw=2, label="T base + monitorizar"),
        plt.Line2D([], [], color=AGUA,       marker="D", lw=2, label="T con scheduler"),
        plt.Line2D([], [], color=TINTA_MUTE, marker="^", lw=2, ls="--",
                   label="overhead de la PROPUESTA = T_sched − T_base  (< 0 = gana)"),
    ]
    fig.legend(handles=manejadores, loc="upper center", ncol=2,
               bbox_to_anchor=(0.5, 1.07))
    fig.suptitle(f"{KERNELS[kernel][0]} — descomposicion del tiempo, en milisegundos",
                 y=1.13, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Todo en ms absolutos: los terminos SUMAN. "
              "G_neta = (T_obs − T_sched) − (T_obs − T_base). "
              "Sin razones ni porcentajes, asi que no hay 'sobrecoste negativo'. "
              "La discontinua gris es el overhead de la PROPUESTA COMPLETA (monitorizar "
              "y migrar) frente a OpenMP puro: es la distancia entre la linea azul y la "
              "de agua, y POR DEBAJO DE CERO significa que la propuesta gana. El "
              "overhead de solo monitorizar no se dibuja pero se lee igual: es el hueco "
              "entre la azul y la naranja.")
    return guardar(fig, figdir, f"{kernel}_02_descomposicion_ms")


# ─────────────────────────────────────────────────────────────────────────────
# 03 — ganancia en milisegundos (positivo = gana)
# ─────────────────────────────────────────────────────────────────────────────

def fig_ganancia(df, kernel, figdir):
    d = df[df["kernel"] == kernel]
    if d.empty:
        return None

    filas = []
    for tam in tamanos_presentes(d):
        sub = d[d["size_tag"] == tam]
        piv = sub.pivot_table(index="threads", columns="config",
                              values="avg_ms", aggfunc="mean")
        if not {"base", "obs", "scheduler"}.issubset(piv.columns):
            continue
        for h in piv.index:
            filas.append({"size_tag": tam, "threads": h,
                          "magnitud": "ganancia por migrar",
                          "ms": piv.loc[h, "obs"] - piv.loc[h, "scheduler"]})
            filas.append({"size_tag": tam, "threads": h,
                          "magnitud": "ganancia neta",
                          "ms": piv.loc[h, "base"] - piv.loc[h, "scheduler"]})
    if not filas:
        return None
    g = pd.DataFrame(filas)

    fig, ejes = rejilla(g)
    for tam, ax in ejes.items():
        sub = g[g["size_tag"] == tam]
        sns.barplot(data=sub, x="threads", y="ms", hue="magnitud", ax=ax,
                    palette={"ganancia por migrar": AGUA, "ganancia neta": AZUL},
                    edgecolor=SUPERFICIE, linewidth=2)   # separador de 2 px
        ax.axhline(0.0, color=EJE, linewidth=1.2)
        ax.set_title(TITULO_TAMANO.get(tam, tam))
        ax.set_xlabel("hilos")
        ax.set_ylabel("milisegundos ganados")
        if ax.get_legend():
            ax.get_legend().remove()

    manejadores = [
        plt.Line2D([], [], color=AGUA, lw=8, label="ganancia por migrar = T_obs − T_sched"),
        plt.Line2D([], [], color=AZUL, lw=8, label="ganancia neta = T_base − T_sched"),
    ]
    fig.legend(handles=manejadores, loc="upper center", ncol=2,
               bbox_to_anchor=(0.5, 1.05))
    fig.suptitle(f"{KERNELS[kernel][0]} — ganancia en tiempo  ·  POSITIVO ES MEJOR",
                 y=1.10, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Positivo = el scheduler ahorra tiempo; negativo = lo cuesta. "
              "Se llama ganancia, no sobrecoste, para que el signo apunte al lado bueno.")
    return guardar(fig, figdir, f"{kernel}_03_ganancia_ms")


# ─────────────────────────────────────────────────────────────────────────────
# 04 — rendimiento (MLUPS / GFLOPS)
# ─────────────────────────────────────────────────────────────────────────────

def fig_rendimiento(df, kernel, figdir):
    d = df[df["kernel"] == kernel]
    if d.empty:
        return None
    _, campo, unidad = KERNELS[kernel]
    if campo not in d.columns or d[campo].isna().all():
        return None

    fig, ejes = rejilla(d)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        marcas = []
        for cfg in CFG_NUCLEO:
            s = sub[sub["config"] == cfg].sort_values("threads")
            if s.empty or s[campo].isna().all():
                continue
            ax.plot(s["threads"], s[campo], marker="o", color=COLOR_CFG[cfg])
            marcas.append((s["threads"].iloc[-1], s[campo].iloc[-1], cfg))
        eje_hilos(ax, hilos)
        etiquetar_series(ax, marcas)
        ax.set_ylabel(unidad)
        ax.set_title(TITULO_TAMANO.get(tam, tam))

    manejadores = [plt.Line2D([], [], color=COLOR_CFG[c], marker="o", lw=2,
                              label=ETIQUETA_CFG[c]) for c in CFG_NUCLEO]
    fig.legend(handles=manejadores, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.04))
    fig.suptitle(f"{KERNELS[kernel][0]} — rendimiento  ·  MAS ES MEJOR",
                 y=1.09, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Derivado del tiempo MEDIO. En el stencil GFLOPS = MLUPS/200 exactamente: "
              "no es evidencia independiente.")
    return guardar(fig, figdir, f"{kernel}_04_rendimiento")


# ─────────────────────────────────────────────────────────────────────────────
# 05 — tráfico real de memoria por origen  (la métrica del director)
# ─────────────────────────────────────────────────────────────────────────────

def fig_trafico(df, kernel, figdir):
    d = df[df["kernel"] == kernel]
    if d.empty:
        return None
    cols = [c for c, _ in ORIGENES if c in d.columns]
    if not cols or d[cols].fillna(0).to_numpy().sum() == 0:
        return None

    fig, ejes = rejilla(d)
    for tam, ax in ejes.items():
        sub = d[(d["size_tag"] == tam) & (d["config"].isin(CFG_NUCLEO))]
        if sub.empty:
            continue
        piv = sub.pivot_table(index=["threads", "config"], values=cols, aggfunc="mean")
        piv = piv.reset_index()
        # Etiqueta corta y rotada: con 6 recuentos x 3 configuraciones son 18 barras
        # por panel, y el texto horizontal se solapaba hasta ser ilegible.
        corta = {"base": "base", "obs": "obs", "scheduler": "sched"}
        piv["etiqueta"] = (piv["threads"].astype(str) + " "
                           + piv["config"].map(corta).fillna(piv["config"]))
        piv = piv.sort_values(["threads", "config"])

        # GiB reales = rellenos * 64 B (tamano de linea de cache).
        abajo = None
        for (col, nombre), color in zip(ORIGENES, RAMPA_DISTANCIA):
            if col not in piv.columns:
                continue
            gib = piv[col].fillna(0) * 64.0 / (1024 ** 3)
            ax.bar(piv["etiqueta"], gib, bottom=abajo, color=color,
                   edgecolor=SUPERFICIE, linewidth=2, label=nombre)
            abajo = gib if abajo is None else abajo + gib

        # Una configuracion cuyo desglose es todo cero NO tiene trafico cero: no
        # se midio. Dejar el hueco en blanco invita a leer "el scheduler eliminio
        # toda la memoria", que es justo lo contrario de la verdad. Se marca.
        vacias = piv[cols].fillna(0).sum(axis=1) == 0
        if vacias.any():
            tope = ax.get_ylim()[1] or 1.0
            for i, es_vacia in enumerate(vacias.to_numpy()):
                if es_vacia:
                    ax.text(i, tope * 0.02, "sin dato", rotation=90, fontsize=5.5,
                            ha="center", va="bottom", color=TINTA_MUTE)

        ax.set_title(TITULO_TAMANO.get(tam, tam))
        ax.set_ylabel("GiB traidos a cache")
        ax.tick_params(axis="x", labelsize=6.5)
        ax.set_xticks(range(len(piv)))
        ax.set_xticklabels(piv["etiqueta"], rotation=90, ha="center")
        ax.set_xlabel("hilos y configuracion")

    manejadores = [plt.Line2D([], [], color=c, lw=8, label=n)
                   for (_, n), c in zip(ORIGENES, RAMPA_DISTANCIA)]
    fig.legend(handles=manejadores, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.07))
    fig.suptitle(f"{KERNELS[kernel][0]} — trafico REAL de memoria por origen"
                 "  ·  MENOS ES MEJOR",
                 y=1.13, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Rellenos de cache x 64 B, separados por procedencia de la linea. "
              "Rampa ordenada por DISTANCIA al nucleo: claro = cerca, oscuro = lejos. "
              "Solo se MIDEN las tres que cruzan una interconexion (otro CCD, cache "
              "remota, DRAM remota); la banda local se DERIVA restandolas del total, "
              "que es exacto. Esta es la magnitud que hay que bajar; no confundir con "
              "el tiempo invertido. Las columnas marcadas 'sin dato' NO son trafico cero: en la campana 29390 el desglose por origen no se leyo para obs ni scheduler (bug corregido, ver ANALISIS_RESULTADOS_29390.md 6.1); solo el par exacto que sostiene ratio_rm es valido ahi.")
    return guardar(fig, figdir, f"{kernel}_05_trafico_memoria")


# ─────────────────────────────────────────────────────────────────────────────
# 06 — localidad (proporción de rellenos remotos)
# ─────────────────────────────────────────────────────────────────────────────

def fig_localidad(df, kernel, figdir):
    d = df[df["kernel"] == kernel]
    if d.empty or "ratio_rm" not in d.columns or d["ratio_rm"].isna().all():
        return None

    fig, ejes = rejilla(d)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        marcas = []
        for cfg in CFG_NUCLEO:
            s = sub[(sub["config"] == cfg)].sort_values("threads").dropna(subset=["ratio_rm"])
            if s.empty:
                continue
            ax.plot(s["threads"], s["ratio_rm"], marker="o", color=COLOR_CFG[cfg])
            marcas.append((s["threads"].iloc[-1], s["ratio_rm"].iloc[-1], cfg))
        eje_hilos(ax, hilos)
        etiquetar_series(ax, marcas)
        ax.set_ylabel("rellenos remotos / rellenos totales")
        ax.set_title(TITULO_TAMANO.get(tam, tam))

    manejadores = [plt.Line2D([], [], color=COLOR_CFG[c], marker="o", lw=2,
                              label=ETIQUETA_CFG[c]) for c in CFG_NUCLEO]
    fig.legend(handles=manejadores, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.04))
    fig.suptitle(f"{KERNELS[kernel][0]} — localidad  ·  MENOS ES MEJOR",
                 y=1.09, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Proporcion de EVENTOS, no de paginas: el denominador incluye los "
              "rellenos servidos por la cache local. -1 (no medible) se descarta.")
    return guardar(fig, figdir, f"{kernel}_06_localidad")


# ─────────────────────────────────────────────────────────────────────────────
# 07 — migraciones
# ─────────────────────────────────────────────────────────────────────────────

def fig_migraciones(df, kernel, figdir):
    d = df[(df["kernel"] == kernel) & (df["config"] == "scheduler")]
    if d.empty or d["migrations"].fillna(0).sum() == 0:
        return None

    fig, ejes = rejilla(d)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam].sort_values("threads")
        if sub.empty:
            continue
        ax.bar(sub["threads"].astype(str), sub["migrations"].fillna(0),
               color=AGUA, edgecolor=SUPERFICIE, linewidth=2)
        ax.set_title(TITULO_TAMANO.get(tam, tam))
        ax.set_xlabel("hilos")
        ax.set_ylabel("hilos migrados")

    fig.suptitle(f"{KERNELS[kernel][0]} — actividad del scheduler",
                 y=1.04, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "MAX_MIGRATIONS = 1, asi que el conteo es tambien el numero de hilos "
              "distintos que llegaron a migrar.")
    return guardar(fig, figdir, f"{kernel}_07_migraciones")


# ─────────────────────────────────────────────────────────────────────────────
# 08 — distribución de las repeticiones
# ─────────────────────────────────────────────────────────────────────────────

def fig_boxplot(df, kernel, outdir, figdir):
    g = cargar_repeticiones(outdir, kernel)
    if g is None:
        return None

    fig, ejes = rejilla(g)
    for tam, ax in ejes.items():
        sub = g[g["size_tag"] == tam]
        if sub.empty:
            continue
        sns.boxplot(data=sub, x="threads", y="ms", hue="config", ax=ax,
                    hue_order=CFG_NUCLEO, palette=COLOR_CFG, showfliers=False,
                    linewidth=1.0, linecolor=TINTA_2)
        ax.set_title(TITULO_TAMANO.get(tam, tam))
        ax.set_xlabel("hilos")
        ax.set_ylabel("ms por repeticion")
        if ax.get_legend():
            ax.get_legend().remove()

    manejadores = [plt.Line2D([], [], color=COLOR_CFG[c], lw=8, label=ETIQUETA_CFG[c])
                   for c in CFG_NUCLEO]
    fig.legend(handles=manejadores, loc="upper center", ncol=3,
               bbox_to_anchor=(0.5, 1.05))
    fig.suptitle(f"{KERNELS[kernel][0]} — distribucion de las repeticiones",
                 y=1.10, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Caja mas estrecha = resultado mas predecible, aunque este a la misma "
              "altura. Sin valores atipicos dibujados.")
    return guardar(fig, figdir, f"{kernel}_08_boxplot")


# ─────────────────────────────────────────────────────────────────────────────
# 09 — optimizaciones estáticas (mapa de calor, rampa secuencial)
# ─────────────────────────────────────────────────────────────────────────────

def fig_estaticas(df, kernel, figdir):
    """Seis configuraciones no caben como seis colores: solo los tres primeros
    slots de la paleta superan la comprobacion de todos los pares. Como lo que se
    compara es MAGNITUD (tiempo), un mapa de calor de rampa secuencial es la forma
    correcta y ademas hace legible el diseno factorial afinidad x politica."""
    d = df[(df["kernel"] == kernel) & (df["config"].isin(CFG_ESTATICAS))]
    if d.empty:
        return None

    fig, ejes = rejilla(d)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        piv = sub.pivot_table(index="config", columns="threads",
                              values="avg_ms", aggfunc="mean")
        piv = piv.reindex([c for c in CFG_ESTATICAS if c in piv.index])
        if piv.empty:
            continue
        # fmt=".3g" y no ".2f": los tamanos van de microsegundos a segundos, y con
        # dos decimales fijos el panel de S0 salia entero a "0.00".
        # El color del texto lo elige seaborn segun la celda: fijarlo a tinta oscura
        # lo hacia ilegible sobre los azules mas oscuros de la rampa.
        sns.heatmap(piv, ax=ax, cmap="Blues", annot=True, fmt=".3g",
                    annot_kws={"fontsize": 7},
                    cbar_kws={"label": "ms"}, linewidths=2, linecolor=SUPERFICIE)
        ax.set_title(TITULO_TAMANO.get(tam, tam))
        ax.set_xlabel("hilos")
        ax.set_ylabel("")
        ax.tick_params(axis="y", labelsize=8, rotation=0)

    fig.suptitle(f"{KERNELS[kernel][0]} — optimizaciones estaticas frente al caso base"
                 "  ·  MENOS ES MEJOR",
                 y=1.04, fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Tiempo medio en ms. Diseno factorial: afinidad {ninguna, close, spread} "
              "x politica de memoria {first-touch, interleave}. Mas oscuro = mas lento. "
              "OJO: la escala de color es POR PANEL, porque los tamanos difieren en "
              "ordenes de magnitud; no se comparan celdas entre paneles.")
    return guardar(fig, figdir, f"{kernel}_09_estaticas")


# ─────────────────────────────────────────────────────────────────────────────
# 10 — IPC: RETIRADA (campana V6)
# ─────────────────────────────────────────────────────────────────────────────
# La figura del IPC se elimina junto con el propio contador. No discriminaba entre
# configuraciones: un IPC alto puede significar trabajo util o espera productiva en
# un bucle de sincronizacion, y aqui significaba lo segundo tan a menudo como lo
# primero, asi que la figura no permitia concluir nada. Los dos contadores del PMU
# que ocupaba (ciclos e instrucciones) pasan al desglose de rellenos por origen,
# que sube de 3 mascaras a 5. Las columnas `ipc` de las campanas anteriores siguen
# en sus CSV; simplemente ya no se dibujan.

# ─────────────────────────────────────────────────────────────────────────────
# 12 — barrido de tamaños
# ─────────────────────────────────────────────────────────────────────────────

MARCAS_CACHE = [
    (32 * 1024**2,  "L3 de un CCD\n32 MiB"),
    (256 * 1024**2, "L3 de un nodo\n256 MiB"),
    (512 * 1024**2, "L3 total\n512 MiB"),
]


def fig_barrido(df, kernel, figdir, hilos_fijos=None):
    d = df[df["kernel"] == kernel].dropna(subset=["ws_bytes"])
    if d.empty:
        return None
    disponibles = sorted(d["threads"].unique())
    if hilos_fijos is None or hilos_fijos not in disponibles:
        hilos_fijos = disponibles[-1]
    d = d[d["threads"] == hilos_fijos]
    if d.empty:
        return None

    fig, ax = plt.subplots(figsize=(8.4, 5.0))
    marcas = []
    for cfg in CFG_NUCLEO:
        s = d[d["config"] == cfg].sort_values("ws_bytes")
        if s.empty:
            continue
        ax.plot(s["ws_bytes"], s["avg_ms"], marker="o", color=COLOR_CFG[cfg],
                label=ETIQUETA_CFG[cfg])
        marcas.append((s["ws_bytes"].iloc[-1], s["avg_ms"].iloc[-1], cfg))

    for x, texto in MARCAS_CACHE:
        if d["ws_bytes"].min() <= x <= d["ws_bytes"].max() * 4:
            ax.axvline(x, color=EJE, linestyle=":", linewidth=1.2)
            ax.annotate(texto, xy=(x, 1.0), xycoords=("data", "axes fraction"),
                        xytext=(3, -12), textcoords="offset points",
                        fontsize=7.5, color=TINTA_MUTE, ha="left", va="top")

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("working set (bytes)")
    ax.set_ylabel("tiempo medio por repeticion (ms)")
    etiquetar_series(ax, marcas)
    ax.legend(loc="upper left")
    ax.set_title(f"{KERNELS[kernel][0]} — barrido de tamanos a {hilos_fijos} hilos"
                 "  ·  MENOS ES MEJOR", color=TINTA)
    fig.tight_layout()
    nota(fig, "Las lineas verticales son los limites reales de exadell. El tramo entre "
              "32 MiB y 256 MiB es donde el trabajo deja de caber en la L3 de un chiplet "
              "y empieza a cruzar el Infinity Fabric.")
    return guardar(fig, figdir, f"{kernel}_12_barrido_tamano")


# ─────────────────────────────────────────────────────────────────────────────
# 13 — LA COMPARACION COMPLETA: las ocho configuraciones a la vez
# ─────────────────────────────────────────────────────────────────────────────

def fig_comparacion_completa(df, kernel, figdir):
    """Las ocho configuraciones en un solo panel por tamano.

    POR QUE EXISTE: el resto de la campana esta partido en dos mitades que nunca
    se cruzaban. Las figuras 01-08, 10 y 12 dibujan solo CFG_NUCLEO (base, obs,
    scheduler) porque solo los tres primeros slots categoricos superan la
    comprobacion de todos los pares; la 09 dibuja solo las seis estaticas. Ninguna
    ponia la propuesta al lado de las optimizaciones estaticas, que es exactamente
    la comparacion que decide si el trabajo aporta algo.

    COMO SE LEE: el numero de cada celda es el tiempo medio en ms (la magnitud
    real). El COLOR es la razon contra el caso base de esa misma celda, en escala
    log2 y divergente: azul = mas rapido que base, rojo = mas lento que base, gris
    = igual que base. La fila 'base' es gris entera por construccion, y hace de
    linea de flotacion.

    A diferencia de la figura 09, aqui el color SI es comparable entre paneles: la
    escala es una razon adimensional, no un tiempo.
    """
    import numpy as np
    from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm

    d = df[(df["kernel"] == kernel) & (df["config"].isin(CFG_TODAS))]
    if d.empty:
        return None

    cmap = LinearSegmentedColormap.from_list("base_div", RAMPA_DIVERGENTE)
    # Recorte a +-3 en log2, o sea 8x en cada sentido. Sin recortar, el 177x del
    # scheduler en S0 se come toda la escala y las diferencias del rango util
    # (donde se juega el argumento, entre 0.5x y 2x) quedan en un gris uniforme.
    TOPE = 3.0
    norma = TwoSlopeNorm(vmin=-TOPE, vcenter=0.0, vmax=TOPE)

    fig, ejes = rejilla(d)
    # Un 25 % mas ancho que la rejilla estandar: estos paneles llevan ocho etiquetas
    # de fila largas a la izquierda, y con el ancho por defecto el rotulo de un panel
    # queda pegado a las celdas del panel anterior.
    ancho, alto = fig.get_size_inches()
    fig.set_size_inches(ancho * 1.25, alto)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        ms = sub.pivot_table(index="config", columns="threads",
                             values="avg_ms", aggfunc="mean")
        ms = ms.reindex([c for c in CFG_TODAS if c in ms.index])
        if ms.empty or "base" not in ms.index:
            continue
        razon = np.log2(ms.div(ms.loc["base"], axis=1)).clip(-TOPE, TOPE)

        # cbar=False: la escala es LA MISMA en los seis paneles, asi que se dibuja
        # una sola vez al margen. Repetirla seis veces es cromo redundante y ademas
        # roba a cada panel el ancho que necesitan las etiquetas de fila.
        sns.heatmap(razon, ax=ax, cmap=cmap, norm=norma, cbar=False,
                    annot=ms.to_numpy(), fmt=".3g", annot_kws={"fontsize": 6.5},
                    linewidths=2, linecolor=SUPERFICIE)
        ax.set_title(TITULO_TAMANO.get(tam, tam))
        ax.set_xlabel("hilos")
        ax.set_ylabel("")
        ax.set_yticklabels([ETIQUETA_TODAS.get(c.get_text(), c.get_text())
                            for c in ax.get_yticklabels()],
                           fontsize=7.5, rotation=0)

    fig.suptitle(f"{KERNELS[kernel][0]} — LAS OCHO CONFIGURACIONES"
                 "  ·  numero = ms (menos es mejor)  ·  color = frente al caso base",
                 y=1.04, fontsize=13, color=TINTA)
    fig.tight_layout()

    # Una unica barra de color para toda la figura.
    fig.subplots_adjust(right=0.91)
    cax = fig.add_axes([0.935, 0.18, 0.012, 0.64])
    barra = fig.colorbar(plt.cm.ScalarMappable(norm=norma, cmap=cmap), cax=cax,
                         ticks=[-3, -2, -1, 0, 1, 2, 3])
    barra.set_label("log2(tiempo / tiempo del caso base)", color=TINTA_2, fontsize=9)
    barra.ax.set_yticklabels(["8x mas rapido", "4x", "2x", "igual que base",
                              "2x", "4x", "8x mas lento"], fontsize=8)
    barra.outline.set_visible(False)
    nota(fig, "AZUL = mas rapido que el caso base de esa misma celda; ROJO = mas lento; "
              "GRIS = igual. La fila 'base' es gris por definicion y hace de linea de "
              "flotacion. Escala recortada a 8x en cada sentido: el numero de la celda "
              "sigue siendo el valor real. Aqui el color SI se compara entre paneles, "
              "porque es una razon, no un tiempo.")
    return guardar(fig, figdir, f"{kernel}_13_comparacion_completa")


# ─────────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--outdir", default="perf_out_v5")
    ap.add_argument("--figdir", default="plots/figuras")
    ap.add_argument("--hilos-barrido", type=int, default=None)
    args = ap.parse_args()

    estilo()
    os.makedirs(args.figdir, exist_ok=True)
    df = cargar(args.outdir)

    generadas = []
    for kernel in KERNELS:
        if (df["kernel"] == kernel).sum() == 0:
            continue
        for f in (fig_tiempo, fig_descomposicion, fig_ganancia, fig_rendimiento,
                  fig_trafico, fig_localidad, fig_migraciones, fig_estaticas,
                  fig_comparacion_completa):
            r = f(df, kernel, args.figdir)
            if r:
                generadas.append(r)
        for f in (fig_boxplot, fig_tiempo_cuantiles):
            r = f(df, kernel, args.outdir, args.figdir)
            if r:
                generadas.append(r)
        r = fig_barrido(df, kernel, args.figdir, args.hilos_barrido)
        if r:
            generadas.append(r)

    print(f"{len(generadas)} figuras en {args.figdir}")
    for r in generadas:
        print("  " + os.path.basename(r))
    if not generadas:
        sys.exit("ERROR: no se genero ninguna figura")


if __name__ == "__main__":
    main()
