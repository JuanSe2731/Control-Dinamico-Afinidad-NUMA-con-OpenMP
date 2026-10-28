#!/usr/bin/env python3
"""Análisis de robustez frente a la única ejecución del scheduler: Sección 3.8.4
del libro.

    (a) Punto de quiebre de la variabilidad del scheduler en los contrastes
        significativos:  s* = sqrt((G / t_{0,975; nB+nC-2})^2 - s_p^2 / nB).
        Para los contrastes frente a Interleave la condición se resuelve
        numéricamente con los grados de libertad de Welch-Satterthwaite (al
        término del scheduler se le asignan los de s_p^2).
    (b) Reproducibilidad de la condición inicial: hilos en el nodo opuesto a los
        datos en las ejecuciones con registro de rellenos (trazas por ventana del
        control y del scheduler; contadores por hilo de la base).
    (c) Cota del peor caso del transitorio: el transitorio de cada ejecución con
        ganancia clara se sustituye por el más largo observado (21 repeticiones).
        Si el transitorio observado ya alcanza esa longitud no hay nada que
        sustituir y se conservan los valores observados.

Requiere haber ejecutado antes analisis_entre_procesos.py.
Uso:  python3 scripts/analisis/robustez_scheduler.py   (escribe robustez_*.csv en data/analisis/)
"""

import os
import numpy as np
import pandas as pd
from scipy import optimize, stats

from analisis_entre_procesos import DATOS, RAIZ, SALIDA

# Series con registro de rellenos (la tercera no los registró).
SERIES = {"principal": os.path.join(RAIZ, "data", "perf_out_v5"),
          "adicional": os.path.join(DATOS, "perf_out_v6")}
UMBRAL_REMOTO = {"spmv_static": 0.30, "stencil": 0.13}   # mitad de la proporción de un hilo remoto
TRANSITORIO_MAX = 21                                       # el más largo observado


def quiebre():
    c = pd.read_csv(os.path.join(SALIDA, "contrastes_scheduler.csv"))
    p = pd.read_csv(os.path.join(SALIDA, "procesos_por_celda.csv"))
    filas = []
    for _, r in c[(c.G_neta_p < 0.05) & (c.G_neta > 0)].iterrows():
        gl = r.n_base + r.n_ctrl - 2
        s = np.sqrt((r.G_neta / stats.t.ppf(0.975, gl)) ** 2 - r.sd_agrup ** 2 / r.n_base)
        filas.append(dict(contraste="frente a Base", kernel=r.kernel, size_tag=r.size_tag,
                          threads=r.threads, G=r.G_neta, s_p=r.sd_agrup, s_star=s,
                          cv_star=100 * s / r.T_sched, veces_sp=s / r.sd_agrup))
    for t in (8, 16):
        r = c[(c.kernel == "spmv_static") & (c.size_tag == "S5") & (c.threads == t)].iloc[0]
        il = p[(p.kernel == "spmv_static") & (p.size_tag == "S5") & (p.threads == t)
               & (p.config == "interleave")].iloc[0]
        dif, v_il = il.media - r.T_sched, il.sd ** 2 / il.n
        gl_s, gl_il = r.n_base + r.n_ctrl - 2, il.n - 1

        def f(s):
            se2 = s ** 2 + v_il
            gl = se2 ** 2 / (s ** 4 / gl_s + v_il ** 2 / gl_il)
            return dif - stats.t.ppf(0.975, gl) * np.sqrt(se2)

        s = optimize.brentq(f, 1e-9, dif)
        filas.append(dict(contraste="frente a Interleave", kernel="spmv_static", size_tag="S5",
                          threads=t, G=dif, s_p=r.sd_agrup, s_star=s,
                          cv_star=100 * s / r.T_sched, veces_sp=s / r.sd_agrup))
    q = pd.DataFrame(filas)
    q.to_csv(os.path.join(SALIDA, "robustez_quiebre.csv"), index=False, float_format="%.4f")
    return q


def condicion_inicial():
    filas = []
    for serie, carpeta in SERIES.items():
        w = pd.read_csv(os.path.join(carpeta, "ompt_window_metrics.csv"),
                        usecols=["tag", "win_idx", "tid", "ratio_rm"])
        for k, umbral in UMBRAL_REMOTO.items():
            # del scheduler solo se usa la serie principal: en la otra la herramienta era otra versión
            for cfg in (("obs", "scheduler") if serie == "principal" else ("obs",)):
                s = w[(w.tag == f"{k}_S5_t8_{cfg}") & (w.ratio_rm >= 0)]
                if s.empty:
                    continue
                n = s.groupby("win_idx").apply(lambda x: int((x.ratio_rm > umbral).sum()))
                tres = (n >= 3)
                filas.append(dict(serie=serie, kernel=k, config=cfg, fuente="trazas por ventana",
                                  remotos_inicio=int(n.iloc[0]), remotos_max=int(n.max()),
                                  frac_ventanas_3=float(tres.mean()),
                                  s_hasta_3=(float(tres.idxmax() - n.index.min()) / 10) if tres.any() else np.nan))
            cont = os.path.join(carpeta, "metrics", f"{k}_S5_t8_base_counters.csv")
            b = pd.read_csv(cont)
            b = b[b.fill_all > 0]
            if not b.empty:
                ratio = b.fill_remoto / b.fill_all
                filas.append(dict(serie=serie, kernel=k, config="base", fuente="contadores por hilo",
                                  remotos_region=int((ratio > umbral).sum()),
                                  ratios=";".join(f"{x:.3f}" for x in sorted(ratio, reverse=True)[:3])))
    d = pd.DataFrame(filas)
    d.to_csv(os.path.join(SALIDA, "robustez_condicion_inicial.csv"), index=False, float_format="%.4f")
    return d


def peor_transitorio():
    c = pd.read_csv(os.path.join(SALIDA, "contrastes_scheduler.csv"))
    filas = []
    for k, tam, h in [("spmv_static", "S4", 8), ("spmv_static", "S5", 8), ("spmv_static", "S5", 16),
                      ("spmv_static", "S5", 32), ("stencil", "S5", 8)]:
        x = pd.read_csv(os.path.join(RAIZ, "data", "perf_out_v5", "metrics",
                                     f"{k}_{tam}_t{h}_scheduler_times.csv")).time_ms.values
        est = x[-100:].mean()
        trans = int(np.where(np.abs(x - est) / est <= 0.05)[0][0])
        previo = x[:trans - 1].mean() if trans > 1 else x[0]
        if trans >= TRANSITORIO_MAX:
            # el transitorio observado ya es el más largo: se conservan los tiempos reales
            peor = x.mean()
        else:
            peor = (TRANSITORIO_MAX * previo + (len(x) - TRANSITORIO_MAX) * est) / len(x)
        r = c[(c.kernel == k) & (c.size_tag == tam) & (c.threads == h)].iloc[0]
        se = r.sd_agrup * np.sqrt(1 + 1 / r.n_base)
        tc = stats.t.ppf(0.975, r.n_base + r.n_ctrl - 2)
        g = r.T_base - peor
        filas.append(dict(kernel=k, size_tag=tam, threads=h, transitorio=trans, t_previo=previo,
                          t_estacionario=est, media_obs=x.mean(), media_peor=peor,
                          G_obs=r.G_neta, G_peor=g, G_peor_lo=g - tc * se, G_peor_hi=g + tc * se,
                          G_peor_pct=100 * g / r.T_base))
    d = pd.DataFrame(filas)
    d.to_csv(os.path.join(SALIDA, "robustez_transitorio.csv"), index=False, float_format="%.4f")
    return d


def main():
    pd.set_option("display.width", 200)
    print(quiebre().round(3).to_string(index=False), "\n")
    print(condicion_inicial().round(3).to_string(index=False), "\n")
    print(peor_transitorio().round(3).to_string(index=False))


if __name__ == "__main__":
    main()
