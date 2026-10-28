#!/usr/bin/env python3
"""Análisis estadístico con la ejecución (proceso) como unidad: Sección 2.4.4
(método) y Secciones 3.8.1 a 3.8.3 (resultados) del libro.

Usa las tres series de la batería, que comparten el código de los kernels en la
inicialización y en el bucle medido y los parámetros de lanzamiento (150
repeticiones, 5 de calentamiento, semilla 42, mismos tamaños, hilos y
configuraciones). La serie principal está en data/perf_out_v5/; las otras dos,
en data/series_adicionales/:

    29355  perf_out_v5_29355   la herramienta no pudo leer los contadores de
                               rellenos: su scheduler no migró y NO se usa como
                               réplica del scheduler.
    29390  perf_out_v5         campaña principal del documento.
    29915  perf_out_v6         la herramienta incluía una variante del criterio de
                               disparo: su scheduler NO se usa como réplica.

Réplicas por celda (kernel, tamaño, hilos):
    base, close, spread, interleave, control  -> hasta 3 procesos (las tres campañas)
    scheduler                         -> 1 proceso (29390)

Unidad estadística: el tiempo medio por repetición de cada ejecución.

Contrastes:
    control vs base   -> t de Welch sobre las medias por ejecución.
    scheduler vs X    -> el scheduler tiene una sola ejecución; como su varianza
                         entre procesos se usa la varianza agrupada de base y
                         control, las dos configuraciones lanzadas igual que él
                         (sin afinidad fija, first-touch).  Para X = base o
                         control: t con varianza agrupada, gl = n_base+n_ctrl-2.
                         Para X estática: t de Welch-Satterthwaite.
    ANOVA de un factor con las seis configuraciones (scheduler con n = 1).

Uso:  python3 scripts/analisis/analisis_entre_procesos.py   (escribe los CSV en data/analisis/)
"""

import os
import numpy as np
import pandas as pd
from scipy import stats

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))      # este fichero vive en scripts/analisis/
DATOS = os.path.join(RAIZ, "data", "series_adicionales")
SALIDA = os.path.join(RAIZ, "data", "analisis")
CAMPANAS = {
    "29355": os.path.join(DATOS, "perf_out_v5_29355", "kernel_metrics.csv"),
    "29390": os.path.join(RAIZ, "data", "perf_out_v5", "kernel_metrics.csv"),
    "29915": os.path.join(DATOS, "perf_out_v6", "kernel_metrics.csv"),
}
REPLICADAS = ["base", "bind_close", "bind_spread", "interleave", "obs"]
NOMBRE = {"base": "Base", "bind_close": "Close", "bind_spread": "Spread",
          "interleave": "Interleave", "obs": "Control", "scheduler": "Scheduler"}
TAMANOS = ["S3", "S4", "S5"]
CLAVE = ["kernel", "size_tag", "threads"]


def cargar():
    filas = []
    for camp, ruta in CAMPANAS.items():
        d = pd.read_csv(ruta)
        d = d[d.size_tag.isin(TAMANOS)]
        usar = REPLICADAS + (["scheduler"] if camp == "29390" else [])
        d = d[d.config.isin(usar)].copy()
        d["campana"] = camp
        filas.append(d[CLAVE + ["config", "campana", "avg_ms", "ratio_rm", "migrations"]])
    return pd.concat(filas, ignore_index=True).dropna(subset=["avg_ms"])


def t_test(diff, se, gl):
    t = diff / se
    p = 2 * stats.t.sf(abs(t), gl)
    m = stats.t.ppf(0.975, gl) * se
    return t, p, diff - m, diff + m


def welch_gl(v1, n1, gl1, v2, n2, gl2):
    a, b = v1 / n1, v2 / n2
    return (a + b) ** 2 / (a ** 2 / gl1 + b ** 2 / gl2)


def main():
    d = cargar()
    g = d.groupby(CLAVE + ["config"])["avg_ms"]
    res = g.agg(n="count", media="mean", sd=lambda x: x.std(ddof=1)).reset_index()
    res["cv_pct"] = 100 * res.sd / res.media
    porcamp = d.pivot_table(index=CLAVE + ["config"], columns="campana", values="avg_ms").reset_index()
    res.merge(porcamp, on=CLAVE + ["config"], how="left").to_csv(
        os.path.join(SALIDA, "procesos_por_celda.csv"), index=False, float_format="%.4f")

    # ---- variabilidad entre procesos, por tamaño
    r = res[res.n >= 2].copy()
    r["grupo"] = np.where(r.config == "obs", "Control",
                          np.where(r.config == "base", "Base", "Estáticas"))
    resumen = r.groupby(["size_tag", "grupo"])["cv_pct"].describe(percentiles=[.5, .9])
    resumen = resumen[["count", "50%", "90%", "max"]].rename(
        columns={"count": "celdas", "50%": "cv_mediana", "90%": "cv_p90", "max": "cv_max"})
    resumen.to_csv(os.path.join(SALIDA, "variabilidad_resumen.csv"), float_format="%.3f")
    r.groupby("size_tag")["cv_pct"].describe(percentiles=[.5, .9])[["count", "50%", "90%", "max"]] \
        .to_csv(os.path.join(SALIDA, "variabilidad_resumen_todas.csv"), float_format="%.3f")

    # ---- contrastes por celda
    contr, anova, vs_est = [], [], []
    for clave, sub in d.groupby(CLAVE):
        val = {c: sub[sub.config == c]["avg_ms"].values for c in sub.config.unique()}
        if "scheduler" not in val or len(val.get("base", [])) < 2 or len(val.get("obs", [])) < 2:
            continue
        b, c, s = val["base"], val["obs"], val["scheduler"][0]
        nb, nc = len(b), len(c)
        vb, vc = b.var(ddof=1), c.var(ddof=1)
        glp = nb + nc - 2
        sp2 = ((nb - 1) * vb + (nc - 1) * vc) / glp
        fila = dict(zip(CLAVE, clave))
        fila.update(n_base=nb, n_ctrl=nc, T_base=b.mean(), T_ctrl=c.mean(), T_sched=s,
                    sd_base=np.sqrt(vb), sd_ctrl=np.sqrt(vc), sd_agrup=np.sqrt(sp2), gl_agrup=glp)
        # ganancia neta: base - scheduler
        diff = b.mean() - s
        se = np.sqrt(sp2 * (1 / nb + 1))
        t, p, lo, hi = t_test(diff, se, glp)
        fila.update(G_neta=diff, G_neta_pct=100 * diff / b.mean(), G_neta_lo=lo, G_neta_hi=hi,
                    G_neta_p=p, G_neta_d=diff / np.sqrt(sp2))
        # ganancia por migrar: control - scheduler
        diff = c.mean() - s
        se = np.sqrt(sp2 * (1 / nc + 1))
        t, p, lo, hi = t_test(diff, se, glp)
        fila.update(G_migrar=diff, G_migrar_pct=100 * diff / c.mean(), G_migrar_lo=lo,
                    G_migrar_hi=hi, G_migrar_p=p, G_migrar_d=diff / np.sqrt(sp2))
        # costo de monitorizar: control - base (ambos replicados, Welch)
        diff = c.mean() - b.mean()
        se = np.sqrt(vb / nb + vc / nc)
        gl = welch_gl(vc, nc, nc - 1, vb, nb, nb - 1)
        t, p, lo, hi = t_test(diff, se, gl)
        fila.update(C_monitor=diff, C_monitor_pct=100 * diff / b.mean(), C_monitor_lo=lo,
                    C_monitor_hi=hi, C_monitor_p=p,
                    C_monitor_d=diff / np.sqrt((vb + vc) / 2), C_monitor_gl=gl)
        contr.append(fila)

        # scheduler frente a cada estática
        mejor = None
        for e in ["bind_close", "bind_spread", "interleave"]:
            x = val.get(e, np.array([]))
            if len(x) < 2:
                continue
            ve = x.var(ddof=1)
            diff = x.mean() - s
            se = np.sqrt(ve / len(x) + sp2)
            gl = welch_gl(ve, len(x), len(x) - 1, sp2, 1, glp)
            t, p, lo, hi = t_test(diff, se, gl)
            f2 = dict(zip(CLAVE, clave))
            f2.update(estatica=NOMBRE[e], T_est=x.mean(), T_sched=s, dif=diff,
                      dif_pct=100 * diff / x.mean(), lo=lo, hi=hi, p=p, gl=gl,
                      d=diff / np.sqrt((ve + sp2) / 2))
            vs_est.append(f2)
            if mejor is None or x.mean() < mejor[1]:
                mejor = (NOMBRE[e], x.mean())
        contr[-1].update(mejor_estatica=mejor[0], T_mejor_estatica=mejor[1])

        # ANOVA de un factor, seis configuraciones, con la ejecución como unidad
        grupos = [v for v in val.values() if len(v) >= 1]
        todos = np.concatenate(grupos)
        N, k = len(todos), len(grupos)
        ssb = sum(len(v) * (v.mean() - todos.mean()) ** 2 for v in grupos)
        ssw = sum(((v - v.mean()) ** 2).sum() for v in grupos)
        gl1, gl2 = k - 1, N - k
        F = (ssb / gl1) / (ssw / gl2)
        f3 = dict(zip(CLAVE, clave))
        f3.update(k=k, N=N, F=F, gl1=gl1, gl2=gl2, p=stats.f.sf(F, gl1, gl2), eta2=ssb / (ssb + ssw))
        anova.append(f3)

    C = pd.DataFrame(contr)
    C.to_csv(os.path.join(SALIDA, "contrastes_scheduler.csv"), index=False, float_format="%.6g")
    pd.DataFrame(anova).to_csv(os.path.join(SALIDA, "anova_procesos.csv"), index=False, float_format="%.6g")
    pd.DataFrame(vs_est).to_csv(os.path.join(SALIDA, "scheduler_vs_estaticas.csv"), index=False,
                                float_format="%.6g")
    print(f"celdas analizadas: {len(C)}  ->  CSV en {SALIDA}")


if __name__ == "__main__":
    main()
