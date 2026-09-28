#!/usr/bin/env python3
"""Figuras del MAZO DE PRESENTACION. Salida: presentacion/figures/.

NO sustituye a plots/plot_results.py. Aquel es la referencia tecnica de la
campana y se queda como esta; este produce un juego distinto, pensado para
proyectar ante un publico que no conoce el proyecto. Las diferencias son
deliberadas:

1. SOLO S3-S5. En S0-S2 el scheduler no migra ni una vez, y no por casualidad:
   la Fase 0 midio que la penalizacion NUMA en L1d y L2 es 1,00x-1,01x, o sea
   inexistente. Ensenar esos peldanos en las figuras de resultado invita a
   comparar configuraciones en un regimen donde la propuesta no puede actuar
   por construccion. Se explican con su propia figura (04) y se retiran del
   resto.

2. NOMBRES EXPLICITOS. Nada de `base`, `obs`, `bind_spread_il`. El publico no
   tiene ese diccionario y cada etiqueta que hay que traducir en voz alta es
   tiempo perdido.

3. VALORES SOBRE LOS PUNTOS. Son figuras para senalar mientras se habla, no
   para leer en papel.

4. SIN IPC. El GiB/s SI se dibuja, junto con MLUPS y GFLOPS: son las metricas de
   throughput que fija el plan de trabajo. Las tres salen del tiempo medio, asi que
   no son evidencia independiente, y cada figura lo dice en su nota. Como V6 quito
   caudal_util_gibs de los kernels, el GiB/s se reconstruye del tiempo cuando el
   CSV no lo trae (ver gibs_del_plan).

Uso:  python3 plots/plot_presentacion.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns

from plot_results import (AZUL, NARANJA, AGUA, TINTA, TINTA_2, TINTA_MUTE,
                          REJILLA, EJE, SUPERFICIE, ORDEN_TAMANOS,
                          cargar, cargar_repeticiones, nota, guardar, estilo)

# Slots categoricos 4 y 5 de la paleta de referencia, que la figura de las
# estaticas necesita para llegar a cinco series. El 4 (amarillo) queda por
# debajo de 3:1 sobre la superficie clara: aplica la regla de relieve, y por eso
# esa figura lleva etiqueta directa en cada serie ademas de la leyenda.
AMARILLO, MAGENTA = "#eda100", "#e87ba4"

# Campana y destino, parametrizables por entorno para que V5 y V6 no se pisen:
#   CAMPANA=perf_out_v4 python3 plots/plot_presentacion.py  -> presentacion/figures_v4
CAMPANA = os.environ.get("CAMPANA", "perf_out_v5")
_sufijo = CAMPANA.split("_")[-1] if CAMPANA.startswith("perf_out_") else CAMPANA
FIGDIR = os.environ.get(
    "FIGDIR",
    "presentacion/figures" if _sufijo == "v5" else f"presentacion/figures_{_sufijo}")

# ── Idioma de los TEXTOS de la figura ────────────────────────────────────────
# IDIOMA=en genera el mismo juego con los rotulos en ingles y el sufijo _en en el
# nombre, para que convivan con los de espanol. Se envuelve CADA texto visible en
# tx(es, en); con el valor por defecto "es" devuelve la cadena de siempre, asi que
# las figuras en espanol salen identicas byte a byte.
IDIOMA = os.environ.get("IDIOMA", "es").lower()
if IDIOMA not in ("es", "en"):
    sys.exit(f"ERROR: IDIOMA={IDIOMA} no reconocido (es | en)")


def tx(es, en):
    """Texto visible en la figura, en el idioma pedido. NO se usa para nombres de
    archivo ni para claves de datos: solo para lo que lee el publico."""
    return en if IDIOMA == "en" else es


# Los peldanos donde el mecanismo puede actuar. Ver el docstring.
TAMANOS = ["S3", "S4", "S5"]

# ── Subconjunto de tamanos: las figuras "solo S5" ────────────────────────────
# TAMANOS_FIG=S5 redibuja las mismas figuras con UNA sola faceta y anade el sufijo
# _S5 al nombre, de modo que conviven con las de tres paneles en vez de pisarlas.
# Un panel unico necesita mas ancho que un tercio de la rejilla, o el titulo de la
# figura no cabe: de ahi PANEL_UNICO.
#
#   CAMPANA=perf_out_v5 TAMANOS_FIG=S5 KERNELS_FIG=spmv_static \
#   FIGURAS=05,06,07,08,11,12 python3 plots/plot_presentacion.py
_pedidos = os.environ.get("TAMANOS_FIG", "").replace(",", " ").split()
if _pedidos:
    TAMANOS = [t for t in TAMANOS if t in _pedidos]
    if not TAMANOS:
        sys.exit(f"ERROR: TAMANOS_FIG={_pedidos} no contiene ninguno de S3, S4, S5")
SUFIJO = (("_" + "".join(TAMANOS)) if _pedidos else "") + ("_en" if IDIOMA == "en" else "")
PANEL_UNICO = (9.6, 5.2) if len(TAMANOS) == 1 else None
TITULO_TAM = {
    "S3": tx("S3 · 191 MiB — cabe en la L3 del nodo",
             "S3 · 191 MiB — fits in the node's L3"),
    "S4": tx("S4 · 1,0 GiB — no cabe en ninguna cache",
             "S4 · 1.0 GiB — fits in no cache"),
    "S5": tx("S5 · 4,1 GiB — plenamente en DRAM",
             "S5 · 4.1 GiB — entirely in DRAM"),
}
TITULO_TAM_CORTO = {"S3": "S3 · 191 MiB",
                    "S4": tx("S4 · 1,0 GiB", "S4 · 1.0 GiB"),
                    "S5": tx("S5 · 4,1 GiB", "S5 · 4.1 GiB")}

KERNELS = {"stencil": "Stencil 2D",
           "spmv_static": tx("SpMV (matriz dispersa)", "SpMV (sparse matrix)")}

# Nombres para proyectar. La regla es que cada uno se entienda sin glosario.
NOMBRE = {
    "base":           tx("OpenMP sin optimizar", "plain OpenMP (baseline)"),
    "obs":            tx("solo medir, sin migrar (control)",
                         "measure only, no migration (control)"),
    "scheduler":      tx("scheduler dinamico (propuesta)", "dynamic scheduler (proposal)"),
    "bind_close":     tx("afinidad fija: hilos juntos", "static affinity: threads packed"),
    "bind_spread":    tx("afinidad fija: hilos repartidos", "static affinity: threads spread"),
    "interleave":     tx("datos repartidos entre nodos", "data interleaved across nodes"),
    "bind_close_il":  tx("hilos juntos + datos repartidos",
                         "threads packed + data interleaved"),
    "bind_spread_il": tx("hilos repartidos + datos repartidos",
                         "threads spread + data interleaved"),
}

COLOR = {
    "base":         AZUL,
    "obs":          TINTA_MUTE,
    "scheduler":    AGUA,
    "bind_close":   NARANJA,
    "bind_spread":  AMARILLO,
    "interleave":   MAGENTA,
}

VERDE_GANA  = AGUA
ROJO_CUESTA = "#c74845"     # mismo brazo rojo calibrado que la figura 13


def rejilla_tam(ncols=None, alto=3.6, ancho=5.4):
    """Una faceta por tamano de TAMANOS. Con un solo tamano manda PANEL_UNICO: el
    ancho de un tercio de rejilla no da para el titulo de la figura."""
    ncols = len(TAMANOS) if ncols is None else ncols
    if PANEL_UNICO and len(TAMANOS) == 1:
        ancho, alto = PANEL_UNICO
    fig, axes = plt.subplots(1, ncols, figsize=(ancho * ncols, alto), squeeze=False)
    return fig, {t: axes[0][i] for i, t in enumerate(TAMANOS[:ncols])}


def eje_hilos(ax, hilos):
    ax.set_xscale("log", base=2)
    ax.set_xticks(hilos)
    ax.set_xticklabels([str(h) for h in hilos])
    ax.set_xlabel(tx("numero de hilos", "number of threads"))


def etiqueta_puntos(ax, xs, ys, fmt="{:.3g}", dy=9, color=None, fontsize=7.5,
                    saltar=None):
    """Valor sobre cada punto. Es lo que permite senalar un dato concreto
    mientras se habla, sin tener que leerlo del eje."""
    for i, (x, y) in enumerate(zip(xs, ys)):
        if saltar and i in saltar:
            continue
        if y is None or (isinstance(y, float) and not np.isfinite(y)):
            continue
        ax.annotate(fmt.format(y), xy=(x, y), xytext=(0, dy),
                    textcoords="offset points", ha="center", va="bottom",
                    fontsize=fontsize, color=color or TINTA_2,
                    zorder=6, clip_on=False)


def solo_tamanos(df):
    return df[df["size_tag"].isin(TAMANOS)]


# ═════════════════════════════════════════════════════════════════════════════
# BLOQUE 1 — Fase 0: la caracterizacion que decidio el diseno del experimento
# ═════════════════════════════════════════════════════════════════════════════

def f01_curva_latencia(figdir):
    """La figura que justifica TODO el diseno posterior.

    Mide la latencia de un recorrido dependiente sobre working sets crecientes,
    con la memoria fijada en el nodo local y en el remoto.

    DOS PANELES APILADOS, NO UN EJE DOBLE. Son dos magnitudes de escala distinta
    (ns y un cociente adimensional) y meterlas en un mismo marco con dos ejes Y
    invita a leer cruces que no significan nada. Comparten el eje X, que es lo
    unico que de verdad comparten.

    El resultado que decide el experimento: en L1d y L2 la penalizacion es 1,00.
    Un scheduler NUMA no tiene NADA que corregir ahi, y eso se sabe ANTES de
    lanzar la campana. Solo aparece cuando el dato se sale de la cache."""
    ruta = "caracterizacion/latencias_exadell.csv"
    if not os.path.exists(ruta):
        return None
    d = pd.read_csv(ruta)
    d = d[(d.modo == "A") & (d.thp == 1)]
    if d.empty:
        return None
    piv = d.pivot_table(index=["bytes", "nivel_esperado"], columns="clase",
                        values="ns_media").dropna().reset_index()
    piv["razon"] = piv["remoto"] / piv["local"]

    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(12.5, 7.4), sharex=True,
                                  gridspec_kw={"height_ratios": [1.55, 1]})

    ax.plot(piv["bytes"], piv["local"], marker="o", ms=4, color=AZUL, lw=2,
            label="memoria en el nodo LOCAL")
    ax.plot(piv["bytes"], piv["remoto"], marker="s", ms=4, color=NARANJA, lw=2,
            label="memoria en el nodo REMOTO")
    ax.set_yscale("log")
    ax.set_ylabel("latencia de acceso (ns)")
    ax.legend(loc="upper left", frameon=False)
    ax.set_title("Fase 0 — la penalizacion NUMA solo existe cuando el dato NO cabe en cache",
                 fontsize=13, color=TINTA)

    ax2.plot(piv["bytes"], piv["razon"], color=TINTA_2, lw=2)
    ax2.axhline(1.0, color=EJE, lw=1.2)
    ax2.set_ylabel("penalizacion NUMA\n(remoto / local)")
    ax2.set_xlabel("tamano del conjunto de trabajo (bytes)")
    ax2.set_xscale("log", base=2)
    ax2.set_ylim(0.92, 2.15)

    # Fronteras reales de exadell, en los dos paneles.
    for a in (ax, ax2):
        for x in (32 * 1024, 1024 * 1024, 32 * 1024 * 1024, 256 * 1024 * 1024):
            a.axvline(x, color=EJE, lw=1, ls=":")
    # Etiquetas de frontera FUERA del marco, en horizontal: dentro y en vertical
    # se cruzaban con la leyenda y con las propias curvas. Van sobre el panel de
    # ABAJO: encima del de arriba quedaban detras del titulo y no se leian enteras.
    for x, txt in [(32 * 1024, "L1d\n32 KiB"), (1024 * 1024, "L2\n1 MiB"),
                   (32 * 1024 * 1024, "L3 chiplet\n32 MiB"),
                   (256 * 1024 * 1024, "L3 nodo\n256 MiB")]:
        ax2.annotate(txt, xy=(x, 1.0), xycoords=("data", "axes fraction"),
                    xytext=(0, 6), textcoords="offset points", fontsize=8,
                    color=TINTA_2, ha="center", va="bottom", linespacing=1.15,
                    annotation_clip=False)

    # Un valor por nivel de cache: la media de la razon dentro del nivel,
    # colocada en el centro geometrico de su tramo.
    for nivel, etq in [("L1d", "L1d"), ("L2", "L2"), ("L3_propia", "L3"),
                       ("DRAM", "DRAM")]:
        s = piv[piv["nivel_esperado"] == nivel]
        if s.empty:
            continue
        r = s["razon"].mean()
        xc = float(np.exp(np.log(s["bytes"].astype(float)).mean()))
        marca = VERDE_GANA if r < 1.05 else ROJO_CUESTA
        ax2.plot([xc], [r], marker="o", ms=11, color=marca, zorder=6)
        ax2.annotate(f"{etq}\n{r:.2f}x", xy=(xc, r), xytext=(0, 14),
                     textcoords="offset points", ha="center", fontsize=10.5,
                     color=TINTA, fontweight="bold")

    fig.tight_layout()
    nota(fig, "Panel de abajo: cuantas veces mas lento es leer del otro nodo. En L1d y L2 vale "
              "1,00: la memoria remota NO se nota, porque el dato nunca sale del chiplet. En la "
              "L3 la penalizacion es una rampa (de 1,03 a 1,80 segun el conjunto se acerca al "
              "limite) y en DRAM se estabiliza en 1,86. Esta figura es ANTERIOR a la campana y "
              "es la que fija los seis tamanos de carga: sin ella, medir en L1 o L2 habria "
              "parecido un experimento razonable.")
    return guardar(fig, figdir, "p01_curva_latencia")


def f02_fronteras(figdir):
    """Las dos fronteras de la maquina, medidas. El titular es que la que se
    suele ignorar (el chiplet) cuesta casi tres veces mas que la que todo el
    mundo mira (el socket)."""
    ruta = "caracterizacion/latencias_exadell.csv"
    if not os.path.exists(ruta):
        return None
    d = pd.read_csv(ruta)
    c = d[d.modo == "C"].groupby("clase")["ns_media"].mean()
    orden = [("mismo_ccd", "mismo chiplet"), ("otro_ccd", "otro chiplet\n(mismo socket)"),
             ("otro_nodo", "otro socket")]
    orden = [(k, v) for k, v in orden if k in c.index]
    if len(orden) < 2:
        return None
    vals = [c[k] for k, _ in orden]

    fig, ax = plt.subplots(figsize=(8.6, 5.0))
    barras = ax.bar([v for _, v in orden], vals,
                    color=[AZUL, NARANJA, ROJO_CUESTA][:len(vals)], width=0.6)
    for b, v in zip(barras, vals):
        ax.annotate(f"{v:.1f} ns", xy=(b.get_x() + b.get_width() / 2, v),
                    xytext=(0, 5), textcoords="offset points", ha="center",
                    fontsize=12, fontweight="bold", color=TINTA)
    ax.set_ylabel("latencia de lectura (ns)")
    ax.set_ylim(0, max(vals) * 1.32)

    # Sin flechas ni factores dentro del grafico: los dos factores (x4,66 y x1,67)
    # se dicen en el texto del slide y en la nota al pie.

    ax.set_title("Fase 0 — la frontera que domina es el CHIPLET, no el socket",
                 fontsize=13, color=TINTA)
    fig.tight_layout()
    nota(fig, "Salir del chiplet multiplica la latencia por 4,66. Salir del socket, que es "
              "la frontera que las herramientas NUMA si ven, solo la multiplica por 1,67. "
              "La maquina tiene 16 chiplets y solo 2 nodos NUMA.")
    return guardar(fig, figdir, "p02_fronteras")


# ═════════════════════════════════════════════════════════════════════════════
# BLOQUE 2 — por que S0-S2 sale del analisis
# ═════════════════════════════════════════════════════════════════════════════

def f03_por_que_no_s0s2(df, figdir):
    """La figura que justifica retirar S0-S2 del resto del mazo.

    Dos razones INDEPENDIENTES, una por panel, y las dos apuntan al mismo sitio:

    (a) No hay nada que corregir. La fraccion de accesos que van a memoria
        remota en el caso base es del 0,02 % al 0,84 % en S0-S2. Es la
        confirmacion en la campana de lo que la Fase 0 ya habia predicho.

    (b) No hay tiempo para actuar. El mecanismo necesita WARMUP_WINDOWS x
        MONITOR_MS = 500 ms de observacion antes de poder migrar. En S0-S2 la
        medicion ENTERA dura entre 2 y 304 ms: termina antes de que el
        scheduler pueda encenderse.

    Con las dos, el resultado "0 migraciones en S0-S2" deja de ser un fallo y
    pasa a ser el comportamiento correcto."""
    d = df.copy()
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14.5, 5.2))

    # ── (a) cuanto trafico remoto hay que corregir, en el caso base ──────────
    b = d[d["config"] == "base"]
    r = (b.groupby("size_tag", observed=True)["ratio_rm"].mean() * 100).reindex(ORDEN_TAMANOS)
    colores = [TINTA_MUTE if t in ("S0", "S1", "S2") else ROJO_CUESTA for t in r.index]
    barras = ax1.bar(r.index.astype(str), r.values, color=colores, width=0.62)
    for bb, v in zip(barras, r.values):
        ax1.annotate(f"{v:.2f} %", xy=(bb.get_x() + bb.get_width() / 2, v),
                     xytext=(0, 4), textcoords="offset points", ha="center",
                     fontsize=10, fontweight="bold", color=TINTA)
    ax1.set_ylabel("accesos servidos desde el OTRO nodo (%)")
    ax1.set_xlabel("tamano de la carga")
    ax1.set_ylim(0, max(r.values) * 1.25)
    ax1.set_title("(a) No hay nada que corregir en S0-S2", fontsize=12, color=TINTA)
    ax1.axvspan(-0.5, 2.5, color=REJILLA, alpha=0.55, zorder=0)
    ax1.annotate("menos del 1 %\nde accesos remotos", xy=(1, max(r.values) * 0.55),
                 ha="center", fontsize=10, color=TINTA_2)

    # ── (b) cuantas ventanas de observacion llega a vivir el mecanismo ──────
    # Este panel NO estima: lee el propio registro del instrumento
    # (ompt_window_metrics.csv). El scheduler necesita WARMUP_WINDOWS = 5
    # ventanas de 100 ms observando antes de que se le permita migrar. En
    # S0-S2 el proceso entero se acaba en la ventana 3.
    ruta_w = os.path.join(CAMPANA, "ompt_window_metrics.csv")
    if os.path.exists(ruta_w):
        w = pd.read_csv(ruta_w, usecols=["tag", "win_idx"])
        w = w[w["tag"].str.endswith("_scheduler")]
        w["size_tag"] = w["tag"].str.extract(r"_(S\d)_")
        porta = w.groupby(["size_tag", "tag"])["win_idx"].max().reset_index()
        agg = porta.groupby("size_tag")["win_idx"].agg(["median", "min", "max"])
        agg = agg.reindex(ORDEN_TAMANOS).dropna()
        colores2 = [ROJO_CUESTA if v < 5 else VERDE_GANA for v in agg["median"]]
        barras2 = ax2.bar(agg.index.astype(str), agg["median"], color=colores2, width=0.62)
        for bb, (_, fila) in zip(barras2, agg.iterrows()):
            ax2.annotate(f"{fila['median']:.0f}\n[{fila['min']:.0f}-{fila['max']:.0f}]",
                         xy=(bb.get_x() + bb.get_width() / 2, fila["median"]),
                         xytext=(0, 4), textcoords="offset points", ha="center",
                         fontsize=9, fontweight="bold", color=TINTA, linespacing=1.1)
        ax2.axhline(5, color=TINTA, lw=2, ls="--")
        ax2.annotate("5 ventanas — lo que el mecanismo debe\nobservar antes de poder migrar",
                     xy=(len(agg) - 0.45, 5), xytext=(0, 8), textcoords="offset points",
                     ha="right", fontsize=10, color=TINTA, fontweight="bold")
        ax2.set_yscale("log")
        ax2.set_ylabel("ventanas de 100 ms observadas (escala log)")
        ax2.set_xlabel("tamano de la carga")
        ax2.set_title("(b) No hay tiempo para actuar en S0-S2", fontsize=12, color=TINTA)
        ax2.margins(y=0.25)

    fig.suptitle("Por que S0-S2 queda fuera del analisis: 0 migraciones, y por dos razones",
                 fontsize=13.5, color=TINTA, y=1.02)
    fig.tight_layout()
    nota(fig, "Migraciones observadas en la campana: S0=0, S1=0, S2=0, S3=1, S4=28, S5=85. "
              "No es que el mecanismo falle en las cargas pequenas: es que no hay problema "
              "NUMA que resolver (panel a) y ademas el proceso entero se acaba en la "
              "ventana 3 de las 5 que el mecanismo necesita observar antes de que se le "
              "permita migrar (panel b: mediana, y entre corchetes el rango sobre las 12 "
              "ejecuciones de cada peldano). El panel b no es una estimacion: sale del "
              "registro que el propio instrumento escribe. A partir de aqui el mazo "
              "trabaja solo con S3, S4 y S5.")
    return guardar(fig, figdir, "p03_por_que_no_s0s2")


def f04_migraciones(df, figdir):
    """Donde y cuanto actua el mecanismo, ya solo en S3-S5."""
    d = solo_tamanos(df[df["config"] == "scheduler"])
    if d.empty:
        return None
    fig, ejes = rejilla_tam(alto=3.9)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        piv = sub.pivot_table(index="threads", columns="kernel",
                              values="migrations", aggfunc="sum")
        if piv.empty:
            continue
        x = np.arange(len(piv.index))
        ancho = 0.38
        for i, k in enumerate([c for c in ["stencil", "spmv_static"] if c in piv.columns]):
            v = piv[k].values
            bb = ax.bar(x + (i - 0.5) * ancho, v, width=ancho,
                        color=[AZUL, NARANJA][i], label=KERNELS[k])
            for r, vv in zip(bb, v):
                if vv > 0:
                    ax.annotate(f"{int(vv)}", xy=(r.get_x() + r.get_width() / 2, vv),
                                xytext=(0, 3), textcoords="offset points",
                                ha="center", fontsize=9, fontweight="bold", color=TINTA)
        ax.set_xticks(x)
        ax.set_xticklabels([str(t) for t in piv.index])
        ax.set_xlabel("numero de hilos")
        ax.set_ylabel("migraciones realizadas")
        ax.set_title(TITULO_TAM_CORTO.get(tam, tam), fontsize=11.5, color=TINTA)
    ejes[TAMANOS[0]].legend(frameon=False, fontsize=9)
    fig.suptitle("Donde actua el scheduler: cuanto mas grande la carga y menos hilos, mas migra",
                 fontsize=13.5, color=TINTA, y=1.03)
    fig.tight_layout()
    nota(fig, "114 migraciones en total, repartidas en 13 de las 72 ejecuciones. A 256 hilos "
              "no migra nunca: el disparador compara cada hilo contra la dispersion del "
              "equipo, y con el equipo entero mal colocado esa referencia se contamina.")
    return guardar(fig, figdir, "p04_migraciones")


# ═════════════════════════════════════════════════════════════════════════════
# BLOQUE 3 — resultados, ya solo en S3-S5
# ═════════════════════════════════════════════════════════════════════════════

def f05_tiempo(df, kernel, figdir):
    """Tiempo por repeticion. La magnitud de resultado del trabajo."""
    d = solo_tamanos(df[df["kernel"] == kernel])
    if d.empty:
        return None
    fig, ejes = rejilla_tam(alto=4.0)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        for cfg, estilo_l, grosor in [("base", "-", 2.2), ("obs", ":", 1.4),
                                      ("scheduler", "-", 2.2)]:
            r = sub[sub["config"] == cfg].sort_values("threads")
            if r.empty:
                continue
            ax.plot(r["threads"], r["avg_ms"], marker="o", ms=5, lw=grosor,
                    ls=estilo_l, color=COLOR[cfg], label=NOMBRE[cfg])
            if cfg != "obs":
                etiqueta_puntos(ax, r["threads"], r["avg_ms"],
                                dy=9 if cfg == "base" else -17, color=COLOR[cfg])
        eje_hilos(ax, hilos)
        ax.set_ylabel(tx("tiempo por repeticion (ms)", "time per repetition (ms)"))
        ax.set_title(TITULO_TAM.get(tam, tam), fontsize=11.5, color=TINTA)
        ax.margins(y=0.22)
    ejes[TAMANOS[0]].legend(frameon=False, fontsize=9, loc="best")
    fig.suptitle(f"{KERNELS[kernel]} — {tx('tiempo por repeticion', 'time per repetition')}"
                 f"  ·  {tx('MENOS ES MEJOR', 'LOWER IS BETTER')}",
                 fontsize=13.5, color=TINTA, y=1.03)
    fig.tight_layout()
    nota(fig, tx("Media de 150 repeticiones. La linea punteada es el control: la herramienta "
                 "cargada y midiendo, pero con la migracion apagada. Sirve para separar lo que "
                 "cuesta medir de lo que aporta migrar.",
                 "Mean of 150 repetitions. The dotted line is the control: the tool loaded and "
                 "measuring, but with migration switched off. It separates what measuring costs "
                 "from what migrating contributes."))
    return guardar(fig, figdir, f"p05_{kernel}_tiempo{SUFIJO}")


def f06_ganancia_pct(df, kernel, figdir):
    """Ganancia y sobrecoste en PORCENTAJE, con el signo fuera del mensaje.

    El diseno es el de la figura de referencia que pidio el director: una sola
    serie de barras, y lo que dice si es bueno o malo lo carga el COLOR y la
    leyenda, no el signo del numero. Verde = la propuesta va mas rapido que
    OpenMP sin optimizar; rojo = va mas lenta. Cada barra lleva ademas el factor
    (x veces) para no obligar a nadie a interpretar un porcentaje negativo."""
    d = solo_tamanos(df[df["kernel"] == kernel])
    if d.empty:
        return None
    fig, ejes = rejilla_tam(alto=4.2)
    for tam, ax in ejes.items():
        piv = d[d["size_tag"] == tam].pivot_table(index="threads", columns="config",
                                                  values="avg_ms", aggfunc="mean")
        if not {"base", "scheduler"}.issubset(piv.columns):
            continue
        pct = (piv["scheduler"] / piv["base"] - 1.0) * 100.0
        x = np.arange(len(pct.index))
        colores = [ROJO_CUESTA if v > 0 else VERDE_GANA for v in pct.values]

        # Un solo punto fuera de escala (S3 a 256 hilos, +468 %) aplastaba todo
        # el resto del panel contra el cero. Se acota el eje a lo que hace legible
        # la mayoria y la barra que se sale se marca con una flecha y su valor
        # real: se ve que existe y cuanto vale, sin perder los demas.
        tope = max(40.0, float(np.percentile(np.abs(pct.values), 80)) * 1.9)
        recortado = np.clip(pct.values, -tope, tope)
        barras = ax.bar(x, recortado, color=colores, width=0.62)

        for bb, v, vr, t in zip(barras, pct.values, recortado, pct.index):
            arriba = v > 0
            se_sale = abs(v) > tope
            factor = piv.loc[t, "base"] / piv.loc[t, "scheduler"]
            if arriba:
                txt = f"{'^ ' if se_sale else '+'}{v:.1f} %"
            else:
                txt = f"{v:.1f} %\n{factor:.2f}x " + tx("mas rapido", "faster")
            ax.annotate(txt, xy=(bb.get_x() + bb.get_width() / 2, vr),
                        xytext=(0, 5 if arriba else -6), textcoords="offset points",
                        ha="center", va="bottom" if arriba else "top",
                        fontsize=8, fontweight="bold", linespacing=1.15,
                        color=ROJO_CUESTA if arriba else VERDE_GANA)
        ax.axhline(0, color=TINTA, lw=1.2)
        ax.set_xticks(x)
        ax.set_xticklabels([str(t) for t in pct.index])
        ax.set_xlabel(tx("numero de hilos", "number of threads"))
        ax.set_ylabel(tx("frente a OpenMP sin optimizar (%)", "vs plain OpenMP (%)"))
        ax.set_title(TITULO_TAM_CORTO.get(tam, tam), fontsize=11.5, color=TINTA)
        ax.set_ylim(min(recortado.min(), 0) * 1.45 - 6, max(recortado.max(), 0) * 1.30 + 8)
    manejadores = [plt.Rectangle((0, 0), 1, 1, color=VERDE_GANA,
                                 label=tx("la propuesta GANA tiempo",
                                          "the proposal SAVES time")),
                   plt.Rectangle((0, 0), 1, 1, color=ROJO_CUESTA,
                                 label=tx("la propuesta CUESTA tiempo",
                                          "the proposal COSTS time"))]
    fig.legend(handles=manejadores, loc="upper center", ncol=2,
               bbox_to_anchor=(0.5, 1.02), frameon=False, fontsize=10)
    fig.suptitle(f"{KERNELS[kernel]} — "
                 + tx("que aporta el scheduler frente a OpenMP sin optimizar",
                      "what the scheduler contributes vs plain OpenMP"),
                 fontsize=13.5, color=TINTA, y=1.10)
    fig.tight_layout()
    nota(fig, tx("El color y la leyenda dicen el sentido; el numero solo dice la magnitud. Las "
                 "barras verdes llevan ademas el factor de aceleracion, para no tener que leer un "
                 "porcentaje negativo como si fuera algo malo. Una barra marcada con ^ se sale del "
                 "eje: se ha acotado la escala para que el resto del panel siga siendo legible, "
                 "pero la etiqueta lleva su valor real.",
                 "Colour and legend carry the direction; the number only carries the magnitude. "
                 "Green bars also show the speed-up factor, so a negative percentage need not be "
                 "read as something bad. A bar marked ^ runs off the axis: the scale is capped so "
                 "the rest of the panel stays legible, but its label shows the real value."))
    return guardar(fig, figdir, f"p06_{kernel}_ganancia_pct{SUFIJO}")


def _segmento(ax, x, ancho, y0, y1, color, alpha=1.0, hatch=None, z=3):
    """Rectangulo entre dos alturas, en el orden que sea. Devuelve el centro."""
    lo, hi = (y0, y1) if y1 >= y0 else (y1, y0)
    if hi - lo <= 0:
        return None
    ax.add_patch(plt.Rectangle((x - ancho / 2, lo), ancho, hi - lo,
                               facecolor=color, alpha=alpha, hatch=hatch,
                               edgecolor=SUPERFICIE, linewidth=1.2, zorder=z))
    return (lo + hi) / 2


def f07_descomposicion_apilada(df, kernel, figdir):
    """De donde sale la ganancia: dos barras por punto y las capas por dentro.

    Barra izquierda: el tiempo de OpenMP sin optimizar, entero. Es la referencia.

    Barra derecha, la propuesta, construida de forma que SIEMPRE sea correcta en
    los dos sentidos (una version anterior apilaba las capas suponiendo que el
    scheduler siempre gana, y donde pierde las capas se solapaban y el dibujo
    afirmaba algo falso):

      · si la propuesta GANA:    solido hasta su tiempo, y encima una capa verde
                                 rayada hasta la altura de la referencia. Esa capa
                                 es el ahorro, y las dos barras acaban a la misma
                                 altura.
      · si la propuesta PIERDE:  solido hasta la altura de la referencia, y encima
                                 una capa roja rayada hasta su tiempo. Esa capa es
                                 el tiempo anadido.

    Y en los dos casos, una marca horizontal senala el tiempo del control (medir
    sin migrar). La llave entre esa marca y el tiempo de la propuesta es LO QUE
    APORTA MIGRAR. La relacion exacta que la figura hace visible es:

        lo_que_aporta_migrar = ahorro_final + coste_del_instrumento

    es decir, migrar recupera MAS de lo que el usuario acaba notando, porque una
    parte de lo recuperado se gasta en pagar el instrumento que decide migrar."""
    d = solo_tamanos(df[df["kernel"] == kernel])
    if d.empty:
        return None
    n_tam = len(TAMANOS)
    tam_fig = PANEL_UNICO if (PANEL_UNICO and n_tam == 1) else (6.5 * n_tam, 5.6)
    fig, ejes_a = plt.subplots(1, n_tam, figsize=tam_fig, squeeze=False)
    ejes = {t: ejes_a[0][i] for i, t in enumerate(TAMANOS)}

    for tam, ax in ejes.items():
        piv = d[d["size_tag"] == tam].pivot_table(index="threads", columns="config",
                                                  values="avg_ms", aggfunc="mean")
        if not {"base", "obs", "scheduler"}.issubset(piv.columns):
            continue
        hilos = list(piv.index)
        x = np.arange(len(hilos), dtype=float)
        w, sep = 0.36, 0.02
        for i, t in enumerate(hilos):
            tb, to, ts = piv.loc[t, "base"], piv.loc[t, "obs"], piv.loc[t, "scheduler"]
            xi, xd = x[i] - (w / 2 + sep), x[i] + (w / 2 + sep)

            # referencia
            _segmento(ax, xi, w, 0, tb, AZUL)
            ax.annotate(f"{tb:.3g}", xy=(xi, tb), xytext=(0, 4),
                        textcoords="offset points", ha="center", fontsize=8,
                        color=AZUL, fontweight="bold")

            # propuesta
            gana = ts < tb
            _segmento(ax, xd, w, 0, min(ts, tb), AGUA)
            cy = _segmento(ax, xd, w, min(ts, tb), max(ts, tb),
                           VERDE_GANA if gana else ROJO_CUESTA,
                           alpha=0.42, hatch="//")
            ax.annotate(f"{ts:.3g}", xy=(xd, 0), xytext=(0, 6),
                        textcoords="offset points", ha="center", va="bottom",
                        fontsize=8, color="white", fontweight="bold", zorder=6)
            if cy is not None and abs(tb - ts) > 0.07 * max(tb, ts, to):
                etq = tx("ahorra", "saves") if gana else tx("anade", "adds")
                ax.annotate(f"{etq}\n{abs(tb - ts):.3g}",
                            xy=(xd, cy), ha="center", va="center", fontsize=8,
                            color=TINTA, fontweight="bold", zorder=7, linespacing=1.05)

            # el control (medir sin migrar) como marca, no como capa: asi nunca
            # se solapa con las anteriores, pase lo que pase con los signos.
            ax.plot([xd - w / 2, xd + w / 2], [to, to], color=ROJO_CUESTA,
                    lw=2.0, ls=(0, (2, 1.4)), zorder=8)
            # llave: de la propuesta al control = lo que aporta migrar
            xb = xd + w / 2 + 0.03
            ax.plot([xb - 0.02, xb, xb, xb - 0.02], [ts, ts, to, to],
                    color=TINTA_2, lw=1.0, zorder=5)

        ax.axhline(0, color=TINTA, lw=1)
        ax.set_xticks(x)
        ax.set_xticklabels([str(t) for t in hilos])
        ax.set_xlabel(tx("numero de hilos", "number of threads"))
        ax.set_ylabel(tx("tiempo por repeticion (ms)", "time per repetition (ms)"))
        ax.set_title(TITULO_TAM_CORTO.get(tam, tam), fontsize=11.5, color=TINTA)
        ax.margins(y=0.18)

    manejadores = [
        plt.Rectangle((0, 0), 1, 1, color=AZUL,
                      label=tx("OpenMP sin optimizar (referencia)", "plain OpenMP (reference)")),
        plt.Rectangle((0, 0), 1, 1, color=AGUA,
                      label=tx("tiempo final con la propuesta", "final time with the proposal")),
        plt.Rectangle((0, 0), 1, 1, facecolor=VERDE_GANA, alpha=0.42, hatch="//",
                      label=tx("lo que el usuario se ahorra", "what the user saves")),
        plt.Rectangle((0, 0), 1, 1, facecolor=ROJO_CUESTA, alpha=0.42, hatch="//",
                      label=tx("lo que la propuesta anade", "what the proposal adds")),
        plt.Line2D([], [], color=ROJO_CUESTA, lw=2, ls=(0, (2, 1.4)),
                   label=tx("control: medir sin migrar", "control: measure without migrating")),
    ]
    fig.legend(handles=manejadores, loc="upper center", ncol=5,
               bbox_to_anchor=(0.5, 1.03), frameon=False, fontsize=9.5)
    fig.suptitle(f"{KERNELS[kernel]} — "
                 + tx("de donde sale la ganancia: migrar recupera mas de lo que se ve",
                      "where the gain comes from: migrating recovers more than you see"),
                 fontsize=13.5, color=TINTA, y=1.10)
    fig.tight_layout()
    nota(fig, tx("La llave de la derecha va del tiempo de la propuesta a la marca del control: eso "
                 "es LO QUE APORTA MIGRAR. La capa rayada sola es lo que el usuario acaba notando. "
                 "La diferencia entre las dos es lo que cuesta el instrumento. Es una identidad "
                 "exacta, no una estimacion: lo_que_aporta_migrar = ahorro_final + coste_del_"
                 "instrumento. Cuando la marca roja del control queda POR ENCIMA de la barra azul, "
                 "el instrumento esta costando tiempo; cuando queda por debajo, esa diferencia es "
                 "ruido de medida entre procesos distintos.",
                 "The bracket on the right spans from the proposal's time to the control mark: "
                 "that is WHAT MIGRATING CONTRIBUTES. The hatched layer alone is what the user "
                 "actually notices. The difference between the two is what the instrument costs. "
                 "It is an exact identity, not an estimate: what_migrating_contributes = "
                 "final_saving + instrument_cost. When the red control mark sits ABOVE the blue "
                 "bar, the instrument is costing time; when it sits below, that difference is "
                 "measurement noise between separate processes."))
    return guardar(fig, figdir, f"p07_{kernel}_descomposicion{SUFIJO}")


# Las cinco que entran en la comparacion del mazo. Se dejan FUERA a proposito:
#   · `obs`  — es el control del instrumento, no una alternativa que nadie usaria.
#   · `bind_close_il` y `bind_spread_il` — mezclan las dos optimizaciones a la vez.
# Quitandolas, cada linea cambia UNA SOLA COSA respecto al punto de partida:
# o donde se colocan los hilos, o donde se colocan los datos, o el mecanismo
# dinamico. Asi la frase que se puede decir en voz alta es limpia: "la propuesta
# frente a optimizar la afinidad de hilos" y "la propuesta frente a optimizar la
# colocacion de los datos", sin un cuarto caso que confunda las dos cosas.
CFG_COMPARACION = ["base", "bind_close", "bind_spread", "interleave", "scheduler"]


def f08_estaticas_lineas(df, kernel, figdir):
    """Las alternativas estaticas, en lineas y no en mapa de calor.

    El mapa de calor obligaba a comparar celdas por tono, que es justo lo que un
    ojo humano hace peor; y ademas mezclaba las combinaciones afinidad+datos con
    las puras. Aqui cada serie cambia exactamente una cosa."""
    d = solo_tamanos(df[(df["kernel"] == kernel) & df["config"].isin(CFG_COMPARACION)])
    if d.empty:
        return None
    fig, ejes = rejilla_tam(alto=4.3, ancho=5.9)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        for cfg in CFG_COMPARACION:
            r = sub[sub["config"] == cfg].sort_values("threads")
            if r.empty:
                continue
            propuesta = (cfg == "scheduler")
            ax.plot(r["threads"], r["avg_ms"], marker="o" if propuesta else "s",
                    ms=6 if propuesta else 4.5,
                    lw=2.6 if propuesta else 1.7,
                    color=COLOR[cfg], label=NOMBRE[cfg], zorder=5 if propuesta else 3)
            if cfg in ("base", "scheduler"):
                etiqueta_puntos(ax, r["threads"], r["avg_ms"],
                                dy=10 if cfg == "base" else -18, color=COLOR[cfg])
        eje_hilos(ax, hilos)
        ax.set_yscale("log")
        ax.set_ylabel(tx("tiempo por repeticion (ms, escala log)",
                         "time per repetition (ms, log scale)"))
        ax.set_title(TITULO_TAM.get(tam, tam), fontsize=11.5, color=TINTA)
        ax.margins(y=0.20)
    ejes[TAMANOS[0]].legend(frameon=False, fontsize=8.5, loc="best")
    fig.suptitle(f"{KERNELS[kernel]} — "
                 + tx("la propuesta frente a las optimizaciones estaticas  ·  MENOS ES MEJOR",
                      "the proposal vs the static optimizations  ·  LOWER IS BETTER"),
                 fontsize=13.5, color=TINTA, y=1.03)
    fig.tight_layout()
    texto_est = tx("Cada linea cambia UNA cosa respecto a OpenMP sin optimizar: o donde se "
                   "colocan los hilos (juntos / repartidos), o donde se colocan los datos "
                   "(repartidos entre nodos), o el mecanismo dinamico. Las combinaciones que "
                   "cambian las dos a la vez se dejan fuera a proposito, para que cada "
                   "comparacion tenga una sola variable.",
                   "Each line changes ONE thing with respect to plain OpenMP: either where the "
                   "threads are placed (packed / spread), or where the data is placed "
                   "(interleaved across nodes), or the dynamic mechanism. The combinations that "
                   "change both at once are deliberately left out, so that every comparison has "
                   "a single variable.")
    if len(TAMANOS) > 1:
        texto_est += tx(" Escala logaritmica porque los tres tamanos abarcan tres ordenes "
                        "de magnitud.",
                        " Logarithmic scale because the three sizes span three orders of "
                        "magnitude.")
    nota(fig, texto_est)
    return guardar(fig, figdir, f"p08_{kernel}_estaticas{SUFIJO}")


def f09_trafico(df, kernel, figdir):
    """El mecanismo, medido: que fraccion de los accesos cruza al otro nodo.

    Es la variable que el scheduler observa y sobre la que decide, asi que es la
    prueba directa de que la cadena causal funciona: si migra, esto baja."""
    d = solo_tamanos(df[(df["kernel"] == kernel) &
                        df["config"].isin(["base", "obs", "scheduler"])])
    if d.empty or d["ratio_rm"].isna().all():
        return None
    fig, ejes = rejilla_tam(alto=4.0)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        piv = sub.pivot_table(index="threads", columns="config",
                              values="ratio_rm", aggfunc="mean") * 100
        if piv.empty:
            continue
        x = np.arange(len(piv.index))
        ancho = 0.38
        pares = [c for c in ["obs", "scheduler"] if c in piv.columns]
        for i, cfg in enumerate(pares):
            v = piv[cfg].values
            bb = ax.bar(x + (i - (len(pares) - 1) / 2) * ancho, v, width=ancho,
                        color=COLOR[cfg], label=NOMBRE[cfg])
            for rr, vv in zip(bb, v):
                if np.isfinite(vv):
                    ax.annotate(f"{vv:.1f}", xy=(rr.get_x() + rr.get_width() / 2, vv),
                                xytext=(0, 3), textcoords="offset points",
                                ha="center", fontsize=8, color=TINTA)
        ax.set_xticks(x)
        ax.set_xticklabels([str(t) for t in piv.index])
        ax.set_xlabel("numero de hilos")
        ax.set_ylabel("accesos servidos desde el otro nodo (%)")
        ax.set_title(TITULO_TAM_CORTO.get(tam, tam), fontsize=11.5, color=TINTA)
        ax.margins(y=0.20)
    ejes[TAMANOS[0]].legend(frameon=False, fontsize=9)
    fig.suptitle(f"{KERNELS[kernel]} — el mecanismo, medido: migrar reduce el trafico al otro nodo"
                 "  ·  MENOS ES MEJOR",
                 fontsize=13.5, color=TINTA, y=1.03)
    fig.tight_layout()
    if kernel == "spmv_static":
        nota(fig, "Se comparan las dos configuraciones que llevan el instrumento cargado, para que "
                  "la unica diferencia entre ellas sea migrar o no migrar. En las 13 ejecuciones en "
                  "que el scheduler llego a migrar, esta barra bajo SIN EXCEPCION: hasta 27 veces "
                  "menos en SpMV a 4,1 GiB con 8 hilos.")
    else:
        nota(fig, _nota_trafico_desde_datos(d, kernel))
    return guardar(fig, figdir, f"p09_{kernel}_trafico{SUFIJO}")


def _nota_trafico_desde_datos(d, kernel):
    """Nota de la figura de trafico calculada con los datos del kernel, para que
    nunca vuelva a citar cifras de otro kernel. Solo afirma que las barras que
    suben son de ejecuciones sin migrar si los datos lo confirman."""
    etiqueta = {"S3": "S3 (191 MiB)", "S4": "S4 (1,0 GiB)", "S5": "S5 (4,1 GiB)"}
    sc = d[d["config"] == "scheduler"].set_index(["size_tag", "threads"])
    ob = d[d["config"] == "obs"].set_index(["size_tag", "threads"])
    comun = sc.index.intersection(ob.index)
    sc, ob = sc.loc[comun], ob.loc[comun]
    migro = sc["migrations"].fillna(0) > 0
    bajo = sc["ratio_rm"] < ob["ratio_rm"]
    subio = sc["ratio_rm"] > ob["ratio_rm"]
    texto = ("Se comparan las dos configuraciones que llevan el instrumento cargado, para que "
             "la unica diferencia entre ellas sea migrar o no migrar. ")
    n = int(migro.sum())
    if n == 0:
        return texto + f"En el {KERNELS[kernel]} el scheduler no llego a migrar en S3-S5."
    factor = (ob["ratio_rm"] / sc["ratio_rm"])[migro]
    tam, hil = factor.idxmax()
    texto += (f"El {KERNELS[kernel]} migro en {n} ejecuciones y en {int((migro & bajo).sum())} "
              f"de ellas esta barra bajo; el mayor descenso es {factor.max():.0f} veces menos, "
              f"en {etiqueta.get(tam, tam)} con {hil} hilos.")
    if subio.any() and not (subio & migro).any():
        texto += (" Las barras que suben son todas de ejecuciones sin ninguna migracion: dos "
                  "procesos distintos, no un efecto del mecanismo.")
    return texto


# ═════════════════════════════════════════════════════════════════════════════
# BLOQUE 4 — throughput en las unidades del plan de trabajo: MLUPS, GFLOPS, GiB/s
# ═════════════════════════════════════════════════════════════════════════════

def fmt_valor(v):
    """Etiqueta legible en cualquier escala: nada de 1.45e+04 sobre un punto que
    hay que senalar en voz alta."""
    a = abs(v)
    if a >= 1000:
        # Separador de millares: espacio en espanol, coma en ingles.
        return f"{v:,.0f}" if IDIOMA == "en" else f"{v:,.0f}".replace(",", " ")
    if a >= 100:
        return f"{v:.0f}"
    if a >= 10:
        return f"{v:.1f}"
    return f"{v:.2f}"


def gibs_del_plan(d):
    """GiB/s como lo define el marco teorico: bytes que recorre una pasada del
    kernel, dividido por (t * 2^30), con t el tiempo medio de una pasada.

    Usa caudal_util_gibs si el CSV la trae (campanas hasta V5) y la reconstruye si
    no, porque V6 la quito de los kernels. La reconstruccion es EXACTA: los bytes
    estan fijados por el tamano del problema, asi que la columna siempre fue
    bytes_constantes / t. Contra perf_out_v5 el error relativo maximo es 4e-6, que
    es el redondeo a 6 decimales con que el kernel escribe avg_ms.

      SpMV:    ws_bytes = 8*nnz + 4*nnz + 4*(N+1) + 8*N + 8*N  -> la formula del
               marco teorico (ValueType = double, IndexType = int)
      Stencil: 6 accesos * 4 B * (N-2)^2  -> 5 lecturas + 1 escritura por punto.
               Modelo nominal que sobreestima el trafico real ~3x (convencion
               declarada en MARCO_TEORICO.md); vale para comparar configuraciones
               del mismo tamano, no como ancho de banda fisico."""
    t_s = d["avg_ms"].astype(float) / 1000.0
    bytes_pasada = np.where(d["kernel"] == "stencil",
                            (d["n_or_rows"].astype(float) - 2) ** 2 * 6 * 4,
                            d["ws_bytes"].astype(float))
    derivado = pd.Series(bytes_pasada, index=d.index) / 2**30 / t_s
    if "caudal_util_gibs" in d.columns:
        return d["caudal_util_gibs"].where(d["caudal_util_gibs"].notna(), derivado)
    return derivado


METRICAS = {
    "mlups": dict(
        prefijo="p10", kernels=("stencil",), eje="MLUPS",
        titulo=tx("MLUPS: millones de puntos de malla actualizados por segundo",
                  "MLUPS: millions of grid points updated per second"),
        nota={"stencil": tx(
              "MLUPS = (N-2)^2 / (t * 10^6): puntos interiores de la malla actualizados por "
              "segundo, en millones, con t = tiempo medio de una pasada. Es la metrica de "
              "throughput que el plan de trabajo fija para el Stencil 2D. Se calcula a partir del "
              "tiempo medio: la razon entre dos lineas es exactamente la inversa de la razon entre "
              "sus tiempos, asi que no es evidencia independiente sino el mismo resultado en la "
              "unidad del plan. La linea punteada es el control (medir sin migrar).",
              "MLUPS = (N-2)^2 / (t * 10^6): interior grid points updated per second, in "
              "millions, with t = mean time of one sweep. It is the throughput metric the thesis "
              "plan sets for the 2D Stencil. It is computed from the mean time: the ratio between "
              "two lines is exactly the inverse of the ratio between their times, so it is not "
              "independent evidence but the same result in the plan's unit. The dotted line is "
              "the control (measuring without migrating).")}),
    "gflops": dict(
        prefijo="p11", kernels=("spmv_static", "stencil"), eje="GFLOPS",
        titulo=tx("GFLOPS: miles de millones de operaciones de coma flotante por segundo",
                  "GFLOPS: billions of floating-point operations per second"),
        nota={"spmv_static": tx(
              "GFLOPS = 2 * nnz / (t * 10^9): una multiplicacion y una suma por cada no-cero de la "
              "matriz, en miles de millones por segundo. Se calcula a partir del tiempo medio: la "
              "razon entre dos lineas es la inversa de la razon entre sus tiempos. Es la unica "
              "unidad que pone los dos kernels en la misma escala. La linea punteada es el control.",
              "GFLOPS = 2 * nnz / (t * 10^9): one multiplication and one addition for every "
              "non-zero of the matrix, in billions per second. It is computed from the mean time: "
              "the ratio between two lines is the inverse of the ratio between their times. It is "
              "the only unit that puts both kernels on the same scale. The dotted line is the "
              "control."),
              "stencil": tx(
              "GFLOPS = 5 * (N-2)^2 / (t * 10^9): cuatro sumas y una multiplicacion por punto. En el "
              "Stencil es EXACTAMENTE MLUPS / 200, la misma curva que la figura de MLUPS con otra "
              "escala. Su utilidad es poner los dos kernels en la misma unidad. La linea punteada "
              "es el control.",
              "GFLOPS = 5 * (N-2)^2 / (t * 10^9): four additions and one multiplication per point. "
              "In the Stencil it is EXACTLY MLUPS / 200, the same curve as the MLUPS figure on "
              "another scale. Its use is to put both kernels in the same unit. The dotted line is "
              "the control.")}),
    "gibs": dict(
        prefijo="p12", kernels=("spmv_static", "stencil"), eje="GiB/s",
        titulo=tx("GiB/s: datos del kernel recorridos por segundo",
                  "GiB/s: kernel data traversed per second"),
        nota={"spmv_static": tx(
              "GiB/s = (8*nnz + 4*nnz + 4*(N+1) + 8*N + 8*N) / (t * 2^30): los bytes de val, colidx, "
              "rowptr, x e y que recorre un producto matriz-vector, por segundo. Es la formula del "
              "marco teorico. Sale del tiempo medio: NO es el trafico medido con contadores, que "
              "esta en la figura del mecanismo. En S3 los datos caben en la L3 del nodo y los sirve "
              "la cache, por eso los valores son mayores. La linea punteada es el control.",
              "GiB/s = (8*nnz + 4*nnz + 4*(N+1) + 8*N + 8*N) / (t * 2^30): the bytes of val, "
              "colidx, rowptr, x and y that one matrix-vector product traverses, per second. It is "
              "the formula from the theoretical framework. It comes from the mean time: it is NOT "
              "the traffic measured with counters, which is in the mechanism figure. In S3 the "
              "data fits in the node's L3 and the cache serves it, which is why the values are "
              "higher. The dotted line is the control."),
              "stencil": tx(
              "GiB/s = 6 * 4 B * (N-2)^2 / (t * 2^30): cinco lecturas y una escritura por punto, por "
              "segundo. Modelo NOMINAL: supone que cada acceso va a memoria; con la reutilizacion "
              "de cache del barrido por filas sobreestima el trafico real unas 3 veces, y en S3, "
              "donde los datos caben en la cache, no representa trafico de memoria. Sirve para "
              "comparar configuraciones del mismo tamano, no como ancho de banda fisico. La linea "
              "punteada es el control.",
              "GiB/s = 6 * 4 B * (N-2)^2 / (t * 2^30): five reads and one write per point, per "
              "second. NOMINAL model: it assumes every access goes to memory; with the cache reuse "
              "of the row-wise sweep it overestimates the real traffic by about 3x, and in S3, "
              "where the data fits in cache, it does not represent memory traffic. Use it to "
              "compare configurations of the same size, not as physical bandwidth. The dotted "
              "line is the control.")}),
}


# Frases de las notas que solo tienen sentido si S3 esta EN la figura. En las
# variantes de un solo tamano (TAMANOS_FIG=S5) describirian un panel que no esta,
# asi que se quitan del texto ya compuesto.
FRASES_SOLO_SI_S3 = {
    "spmv_static": tx(" En S3 los datos caben en la L3 del nodo y los sirve la cache, "
                      "por eso los valores son mayores.",
                      " In S3 the data fits in the node's L3 and the cache serves it, "
                      "which is why the values are higher."),
    "stencil": tx(", y en S3, donde los datos caben en la cache, no representa trafico "
                  "de memoria",
                  ", and in S3, where the data fits in cache, it does not represent "
                  "memory traffic"),
}


def f10_throughput(df, kernel, metrica, figdir):
    """Una figura por metrica y kernel, con las mismas tres series que la figura
    del tiempo (p05) para que se lean una al lado de la otra: OpenMP sin
    optimizar, el control punteado y la propuesta. MAS ES MEJOR.

    Etiquetas: en cada numero de hilos, la mayor de las dos lineas lleva su valor
    ENCIMA y la menor DEBAJO. Con una posicion fija por serie, las etiquetas se
    montaban justo en los puntos donde las lineas se cruzan, que es donde mas
    interesa senalar."""
    spec = METRICAS[metrica]
    if kernel not in spec["kernels"]:
        return None
    d = solo_tamanos(df[(df["kernel"] == kernel) &
                        df["config"].isin(["base", "obs", "scheduler"])]).copy()
    if d.empty:
        return None
    if metrica == "gibs":
        d["valor"] = gibs_del_plan(d)
    elif metrica in d.columns:
        d["valor"] = pd.to_numeric(d[metrica], errors="coerce")
    else:
        return None
    if d["valor"].isna().all():
        return None

    fig, ejes = rejilla_tam(alto=4.0)
    for tam, ax in ejes.items():
        sub = d[d["size_tag"] == tam]
        hilos = sorted(sub["threads"].unique())
        serie = {}
        for cfg, estilo_l, grosor in [("base", "-", 2.2), ("obs", ":", 1.4),
                                      ("scheduler", "-", 2.2)]:
            r = sub[sub["config"] == cfg].sort_values("threads")
            if r.empty:
                continue
            ax.plot(r["threads"], r["valor"], marker="o", ms=5, lw=grosor,
                    ls=estilo_l, color=COLOR[cfg], label=NOMBRE[cfg])
            serie[cfg] = r.set_index("threads")["valor"]
        if {"base", "scheduler"} <= serie.keys():
            for h in hilos:
                vb, vs = serie["base"].get(h), serie["scheduler"].get(h)
                if vb is None or vs is None:
                    continue
                for cfg, v, arriba in [("scheduler", vs, vs >= vb), ("base", vb, vs < vb)]:
                    if not np.isfinite(v):
                        continue
                    # Fondo del color de la superficie: los marcadores grises del
                    # control caen a menudo justo debajo de una etiqueta y la dejaban
                    # ilegible ("19 143" se leia "19 .43").
                    ax.annotate(fmt_valor(v), xy=(h, v), xytext=(0, 8 if arriba else -8),
                                textcoords="offset points", ha="center",
                                va="bottom" if arriba else "top", fontsize=7.5,
                                color=COLOR[cfg], zorder=7, clip_on=False,
                                bbox=dict(boxstyle="round,pad=0.12", fc=SUPERFICIE,
                                          ec="none", alpha=0.9))
        eje_hilos(ax, hilos)
        ax.set_ylabel(spec["eje"])
        ax.set_title(TITULO_TAM.get(tam, tam), fontsize=11.5, color=TINTA)
        ax.margins(y=0.22)
    ejes[TAMANOS[0]].legend(frameon=False, fontsize=9, loc="best")
    fig.suptitle(f"{KERNELS[kernel]} — {spec['titulo']}"
                 f"  ·  {tx('MAS ES MEJOR', 'HIGHER IS BETTER')}",
                 fontsize=13.5, color=TINTA, y=1.03)
    fig.tight_layout()
    texto = spec["nota"][kernel]
    if "S3" not in TAMANOS:
        texto = texto.replace(FRASES_SOLO_SI_S3.get(kernel, "\0"), "")
    nota(fig, texto)
    return guardar(fig, figdir, f"{spec['prefijo']}_{kernel}_{metrica}{SUFIJO}")


# ═════════════════════════════════════════════════════════════════════════════

def main():
    estilo()
    os.makedirs(FIGDIR, exist_ok=True)
    df = cargar(CAMPANA)
    generadas = []

    kernels_pedidos = os.environ.get("KERNELS_FIG", "").replace(",", " ").split()
    figs_pedidas = os.environ.get("FIGURAS", "").replace(",", " ").split()
    subconjunto = bool(_pedidos or kernels_pedidos or figs_pedidas)

    # Las 01-04 son figuras de contexto (Fase 0, y por que S0-S2 queda fuera), no
    # del recorrido de un kernel: no tienen version "solo S5" y se saltan en cuanto
    # se pide un subconjunto, para no reescribirlas con el mismo contenido.
    if not subconjunto:
        for f in (f01_curva_latencia, f02_fronteras):
            r = f(FIGDIR)
            if r:
                generadas.append(r)
        for f in (f03_por_que_no_s0s2, f04_migraciones):
            r = f(df, FIGDIR)
            if r:
                generadas.append(r)

    for kernel in KERNELS:
        if (df["kernel"] == kernel).sum() == 0:
            continue
        if kernels_pedidos and kernel not in kernels_pedidos:
            continue
        for codigo, f in (("05", f05_tiempo), ("06", f06_ganancia_pct),
                          ("07", f07_descomposicion_apilada),
                          ("08", f08_estaticas_lineas), ("09", f09_trafico)):
            if figs_pedidas and codigo not in figs_pedidas:
                continue
            r = f(df, kernel, FIGDIR)
            if r:
                generadas.append(r)
        for metrica, spec in METRICAS.items():
            if figs_pedidas and spec["prefijo"].lstrip("p") not in figs_pedidas:
                continue
            r = f10_throughput(df, kernel, metrica, FIGDIR)
            if r:
                generadas.append(r)

    print(f"{len(generadas)} figuras en {FIGDIR}")
    for r in generadas:
        print("  " + os.path.basename(r))


if __name__ == "__main__":
    main()
