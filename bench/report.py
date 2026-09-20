#!/usr/bin/env python3
"""Summarise bench/results/*.csv as markdown tables (and results/summary.json)."""
import csv
import json
import os
import statistics as st
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")


def load(name):
    path = os.path.join(RES, name + ".csv")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * p / 100
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def fmt(x, nd=2):
    if x is None or x != x:
        return "–"
    if abs(x) >= 100:
        return f"{x:,.0f}"
    if abs(x) >= 10:
        return f"{x:.1f}"
    return f"{x:.{nd}f}"


def table(headers, rows):
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


summary = {}
md = []

# --- build -----------------------------------------------------------------
build = load("bench_build")
if build:
    by = defaultdict(dict)
    for r in build:
        by[r["method"]][r["step"]] = r
    rows = []
    for m in ["q3c", "pgsphere", "skycell"]:
        b = by.get(m, {})
        g = lambda s, k: float(b[s][k]) if s in b and b[s][k] not in ("", None) else None
        rows.append([m, fmt(g("table", "mb")), fmt(g("index", "mb")), fmt(g("covering index", "mb")),
                     fmt(g("table", "seconds")), fmt(g("index", "seconds")), fmt(g("analyze", "seconds"))])
        summary.setdefault("build", {})[m] = {"table_mb": g("table", "mb"), "index_mb": g("index", "mb"),
                                              "index_s": g("index", "seconds"), "table_s": g("table", "seconds")}
    md.append("## Build\n\n" + table(
        ["method", "table MB", "index MB", "covering idx MB", "load+sort s", "index build s", "analyze s"], rows))

# --- cone searches -----------------------------------------------------------
centers = {r["qid"]: r for r in load("bench_centers")}
cone = [r for r in load("bench_cone") if r["pass"] == "2"]
conex = load("bench_cone_x")
label_order = ['1"', '10"', "1'", "6'", "30'", "1deg", "3deg"]
if cone:
    t = defaultdict(list)          # (method, variant, label) -> ms
    tk = defaultdict(list)         # (method, variant, label, kind) -> ms
    n = defaultdict(dict)          # (method, variant) -> qid -> n
    for r in cone:
        c = centers[r["qid"]]
        key = (r["method"], r["variant"])
        t[key + (c["label"],)].append(float(r["ms"]))
        tk[key + (c["label"], c["kind"])].append(float(r["ms"]))
        n[key][r["qid"]] = int(r["n"])
    x = defaultdict(list)
    for r in conex:
        c = centers[r["qid"]]
        x[(r["method"], r["variant"], c["label"])].append(r)

    ref = n.get(("skycell", ""), {})
    methods = [("q3c", ""), ("pgsphere", ""), ("skycell", "")]
    rows = []
    summary["cone"] = {}
    for lab in label_order:
        for m in methods:
            ms = t.get(m + (lab,), [])
            if not ms:
                continue
            xs = x.get(m + (lab,), [])
            buf = st.mean(float(r["buffers"]) for r in xs) if xs else None
            planm = st.median(float(r["plan_ms"]) for r in xs) if xs else None
            est = [max(float(r["est_rows"]) / max(float(r["act_rows"]), 1), max(float(r["act_rows"]), 1) / max(float(r["est_rows"]), 1))
                   for r in xs]
            rows_ret = st.mean(n[m][q] for q in n[m] if centers[q]["label"] == lab)
            mism = sum(1 for q in n[m] if centers[q]["label"] == lab and q in ref and n[m][q] != ref[q])
            dense = tk.get(m + (lab, "data"), [])
            sparse = tk.get(m + (lab, "uniform"), [])
            rows.append([lab, m[0], len(ms), fmt(rows_ret, 1), fmt(st.median(ms), 3), fmt(st.mean(ms), 3), fmt(pct(ms, 95), 3),
                         fmt(st.mean(dense), 3) if dense else "–", fmt(st.mean(sparse), 3) if sparse else "–",
                         fmt(buf, 1), fmt(planm, 3), fmt(st.median(est), 2) if est else "–", mism])
            summary["cone"].setdefault(lab, {})[m[0]] = {
                "median_ms": st.median(ms), "mean_ms": st.mean(ms), "p95_ms": pct(ms, 95),
                "mean_dense_ms": st.mean(dense) if dense else None, "mean_sparse_ms": st.mean(sparse) if sparse else None,
                "buffers": buf, "plan_ms": planm, "est_err": st.median(est) if est else None, "rows": rows_ret}
    md.append("## Cone searches (warm cache, planning included)\n\n"
              "`dense`/`sparse` = mean ms for centres on catalogue sources / uniform sky. "
              "`est err` = median max(est/actual, actual/est) of the planner's row estimate. "
              "`≠skycell` = queries whose row count differs from skycell's.\n\n" + table(
                  ["radius", "method", "queries", "rows", "median ms", "mean ms", "p95 ms", "dense ms", "sparse ms",
                   "buffers", "plan ms", "est err", "≠skycell"], rows))

    variants = sorted({v for (m, v) in n if m == "skycell"})
    if len(variants) > 1:
        rows = []
        for lab in label_order:
            for v in variants:
                ms = t.get(("skycell", v, lab), [])
                xs = x.get(("skycell", v, lab), [])
                if not ms:
                    continue
                rows.append([lab, v or "default", fmt(st.mean(ms), 3), fmt(pct(ms, 95), 3),
                             fmt(st.mean(float(r["buffers"]) for r in xs), 1) if xs else "–",
                             fmt(st.median(float(r["plan_ms"]) for r in xs), 3) if xs else "–"])
        md.append("## skycell ablations\n\n" + table(["radius", "variant", "mean ms", "p95 ms", "buffers", "plan ms"], rows))

# --- polygons ------------------------------------------------------------------
poly = [r for r in load("bench_poly") if r["pass"] == "2"]
if poly:
    by = defaultdict(list)
    cnt = defaultdict(dict)
    for r in poly:
        by[r["method"]].append(float(r["ms"]))
        cnt[r["method"]][r["q"]] = int(r["n"])
    rows = []
    for m in ["q3c", "pgsphere", "skycell"]:
        if m in by:
            mism = sum(1 for q in cnt[m] if cnt[m][q] != cnt["skycell"].get(q))
            rows.append([m, len(by[m]), fmt(st.median(by[m]), 3), fmt(st.mean(by[m]), 3), fmt(pct(by[m], 95), 3), mism])
    summary["poly"] = {m: {"median_ms": st.median(v), "mean_ms": st.mean(v)} for m, v in by.items()}
    md.append("## Convex polygons (0.05–2 deg)\n\n" + table(["method", "queries", "median ms", "mean ms", "p95 ms", "≠skycell"], rows))

# --- cross-match -----------------------------------------------------------------
xm = [r for r in load("bench_xmatch") if r["pass"] == "2"]
if xm:
    rows = []
    summary["xmatch"] = {}
    for rad in sorted({float(r["radius_arcsec"]) for r in xm}):
        for r in xm:
            if float(r["radius_arcsec"]) == rad:
                rows.append([f'{rad:g}"', r["method"], f'{int(r["n"]):,}', fmt(float(r["ms"]) / 1000, 2)])
                summary["xmatch"].setdefault(f"{rad:g}", {})[r["method"]] = {"s": float(r["ms"]) / 1000, "n": int(r["n"])}
    md.append("## Cross-match (probe table joined to catalogue)\n\n" + table(["radius", "method", "pairs", "seconds"], rows))

# --- footprints -------------------------------------------------------------------
fp = load("bench_fp")
if fp:
    rows = []
    summary["footprints"] = {}
    for m in ["pgsphere", "skycell"]:
        b = [r for r in fp if r["method"] == m and r["pass"] == "0"]
        q = [r for r in fp if r["method"] == m and r["pass"] == "2"]
        if b and q:
            rows.append([m, fmt(float(b[0]["build_ms"]) / 1000, 2), fmt(float(b[0]["index_mb"]), 2),
                         f'{int(q[0]["n"]):,}', fmt(float(q[0]["ms"]) / 1000, 3)])
            summary["footprints"][m] = {"build_s": float(b[0]["build_ms"]) / 1000, "index_mb": float(b[0]["index_mb"]),
                                        "n": int(q[0]["n"]), "s": float(q[0]["ms"]) / 1000}
    md.append("## Stored footprints: point-in-region join\n\n" + table(
        ["method", "index build s", "index MB", "pairs", "query s"], rows))

text = "\n\n".join(md)
print(text)
with open(os.path.join(RES, "summary.md"), "w") as f:
    f.write(text + "\n")
with open(os.path.join(RES, "summary.json"), "w") as f:
    json.dump(summary, f, indent=1)
