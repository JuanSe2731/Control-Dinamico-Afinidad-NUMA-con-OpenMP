#!/usr/bin/env python3
"""Tablas 3.1 a 3.6 del Capítulo 3 (Resultados) del libro, desde los datos de data/.

Las Tablas 3.7 a 3.11 las escriben analisis_entre_procesos.py y
robustez_scheduler.py; este script cubre las anteriores, que solo usan la serie
principal (data/perf_out_v5) y la caracterización de latencias:

    3.1  Latencia media por nivel de la jerarquía de memoria (Sección 3.1).
         Modo A del microbenchmark con THP habilitado; la penalización es la
         media, dentro de cada nivel, del cociente remoto/local de cada tamaño.
    3.2  Ejecuciones en las que el scheduler migró, S3-S5 (Sección 3.3), con la
         descomposición de la Ecuación 2.7 y ratio_rm del control y del scheduler.
    3.3  Ganancia media frente a ganancia en régimen estacionario (Sección 3.4.5).
         Estacionario = últimas 100 repeticiones; transitorio = repeticiones
         anteriores a la primera que queda a menos del 5 % de esa media.
    3.4  Tiempo dentro de los callbacks de OMPT por hilo y por región paralela en
         el control (Sección 3.6). ompt_overhead.csv acumula los intentos
         anteriores de cada etiqueta, así que se toma la ÚLTIMA fila, que es la de
         la serie principal; cada ejecución atraviesa 155 regiones (150
         repeticiones medidas + 5 de calentamiento).
    3.5  Tiempo por repetición en S5 de Base, las tres estáticas y el scheduler
         (Sección 3.7).
    3.6  Mediana, sobre los seis recuentos de hilos, del cociente frente a Base,
         S3-S5 (Sección 3.7).

Los valores se escriben con más decimales que en el libro, que los redondea.

Uso:  python3 scripts/analisis/tablas_libro.py      (escribe tabla_3_*.csv en data/analisis/)
"""

import os
import numpy as np
import pandas as pd

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.abspath(os.path.join(AQUI, "..", ".."))      # este fichero vive en scripts/analisis/
SERIE = os.path.join(RAIZ, "data", "perf_out_v5")
LATENCIAS = os.path.join(RAIZ, "data", "caracterizacion", "latencias_exadell.csv")
SALIDA = os.path.join(RAIZ, "data", "analisis")

TAMANOS = ["S3", "S4", "S5"]
HILOS = [8, 16, 32, 64, 128, 256]
NOMBRE = {"base": "Base", "bind_close": "Close", "bind_spread": "Spread",
          "interleave": "Interleave", "obs": "Control", "scheduler": "Scheduler"}
REGIONES = 150 + 5          # repeticiones medidas + calentamiento
# Celdas de la Tabla 3.3: las de ganancia clara del scheduler.
CELDAS_TRANSITORIO = [("spmv_static", "S4", 8), ("spmv_static", "S5", 8),
                      ("spmv_static", "S5", 16), ("spmv_static", "S5", 32),
                      ("stencil", "S5", 8)]


def guardar(df, nombre):
    df.to_csv(os.path.join(SALIDA, nombre), index=False, float_format="%.4f")
    return df


def tabla_3_1():
    d = pd.read_csv(LATENCIAS)
    d = d[(d.modo == "A") & (d.thp == 1)]
    piv = d.pivot_table(index=["bytes", "nivel_esperado"], columns="clase",
                        values="ns_media").dropna().reset_index()
    piv["razon"] = piv.remoto / piv.local
    t = piv.groupby("nivel_esperado").agg(local_ns=("local", "mean"), remoto_ns=("remoto", "mean"),
                                          penalizacion=("razon", "mean"),
                                          penalizacion_min=("razon", "min"),
                                          penalizacion_max=("razon", "max"))
    t = t.reindex(["L1d", "L2", "L3_propia", "DRAM"]).reset_index()
    t["nivel_esperado"] = t.nivel_esperado.replace({"L3_propia": "L3"})
    return guardar(t.rename(columns={"nivel_esperado": "nivel"}), "tabla_3_1_latencias.csv")


def cargar_kernel():
    k = pd.read_csv(os.path.join(SERIE, "kernel_metrics.csv"))
    return k.pivot_table(index=["kernel", "size_tag", "threads"], columns="config",
                         values=["avg_ms", "ratio_rm", "migrations"])


def tabla_3_2(k):
    filas = []
    for (kern, tam, h), r in k.iterrows():
        if tam not in TAMANOS or not r[("migrations", "scheduler")] > 0:
            continue
        tb, to, ts = r[("avg_ms", "base")], r[("avg_ms", "obs")], r[("avg_ms", "scheduler")]
        ro, rs = r[("ratio_rm", "obs")], r[("ratio_rm", "scheduler")]
        filas.append(dict(kernel=kern, size_tag=tam, threads=h, T_base=tb, T_obs=to, T_sched=ts,
                          C_mon=to - tb, G_mig=to - ts, G_neta=tb - ts,
                          migraciones=int(r[("migrations", "scheduler")]),
                          ratio_rm_control_pct=100 * ro, ratio_rm_sched_pct=100 * rs,
                          factor=ro / rs))
    t = pd.DataFrame(filas).sort_values(["kernel", "size_tag", "threads"])
    return guardar(t, "tabla_3_2_ejecuciones_migradas.csv")


def tabla_3_3(k):
    filas = []
    for kern, tam, h in CELDAS_TRANSITORIO:
        x = pd.read_csv(os.path.join(SERIE, "metrics",
                                     f"{kern}_{tam}_t{h}_scheduler_times.csv")).time_ms.values
        est = x[-100:].mean()
        trans = int(np.where(np.abs(x - est) / est <= 0.05)[0][0])
        tb = k.loc[(kern, tam, h), ("avg_ms", "base")]
        filas.append(dict(kernel=kern, size_tag=tam, threads=h, media_ms=x.mean(),
                          ganancia_media=tb / x.mean(), reps_transitorio=trans,
                          estacionario_ms=est, ganancia_estacionaria=tb / est))
    return guardar(pd.DataFrame(filas), "tabla_3_3_transitorio.csv")


def tabla_3_4(k):
    ov = pd.read_csv(os.path.join(SERIE, "ompt_overhead.csv"))
    ov = ov.groupby("tag", sort=False).tail(1).set_index("tag")     # la serie principal
    filas = []
    for kern in ("spmv_static", "stencil"):
        for tam in TAMANOS:
            for h in HILOS:
                cb = ov.loc[f"{kern}_{tam}_t{h}_obs", "callback_us_total"] / 1000 / h / REGIONES
                tb = k.loc[(kern, tam, h), ("avg_ms", "base")]
                filas.append(dict(kernel=kern, size_tag=tam, threads=h, callback_ms=cb,
                                  pct_de_base=100 * cb / tb))
    return guardar(pd.DataFrame(filas), "tabla_3_4_costo_callbacks.csv")


def tabla_3_5(k):
    cfg = ["base", "bind_close", "bind_spread", "interleave", "scheduler"]
    t = k["avg_ms"][cfg].xs("S5", level="size_tag").reset_index()
    return guardar(t.rename(columns=NOMBRE), "tabla_3_5_s5_estaticas.csv")


def tabla_3_6(k):
    cfg = ["bind_close", "bind_spread", "interleave", "scheduler"]
    t = k["avg_ms"]
    t = t[t.index.get_level_values("size_tag").isin(TAMANOS)]
    cociente = t[cfg].div(t["base"], axis=0)
    m = cociente.groupby(level=["kernel", "size_tag"]).median().reset_index()
    return guardar(m.rename(columns=NOMBRE), "tabla_3_6_mediana_cocientes.csv")


def main():
    pd.set_option("display.width", 200)
    os.makedirs(SALIDA, exist_ok=True)
    k = cargar_kernel()
    for titulo, t in [("Tabla 3.1", tabla_3_1()), ("Tabla 3.2", tabla_3_2(k)),
                      ("Tabla 3.3", tabla_3_3(k)), ("Tabla 3.4", tabla_3_4(k)),
                      ("Tabla 3.5", tabla_3_5(k)), ("Tabla 3.6", tabla_3_6(k))]:
        print(f"── {titulo} " + "─" * 60)
        print(t.round(4).to_string(index=False), "\n")
    print(f"CSV en {SALIDA}")


if __name__ == "__main__":
    main()
