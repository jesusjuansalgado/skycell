"""Per shape: the composite REGION's summed per-shape median time over the
boolean spellings' (skycell and pgSphere), with a 95% bootstrap interval over
shapes."""
import sys, csv, random, statistics as st
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
t = defaultdict(list)
size = {}
for r in rows:
    t[(r['shape'], r['c'], r['method'])].append(float(r['us']))
    size[(r['shape'], r['c'])] = float(r['s'])
def ratio_ci(per, a, b, ks):
    def ratio(sample): return sum(per[k][a] for k in sample) / sum(per[k][b] for k in sample)
    random.seed(1)
    bs = sorted(ratio(random.choices(ks, k=len(ks))) for _ in range(2000))
    return f"{ratio(ks):.2f} [{bs[50]:.2f}, {bs[1949]:.2f}]"
def table(groups, title):
    print(f"| {title} | shapes | REGION ms | boolean ms | pgSphere ms | REGION / boolean [95%] | REGION / pgSphere [95%] |")
    print("|---|---|---|---|---|---|---|")
    for name, keys in groups:
        per = {k: tuple(st.median(t[k + (m,)]) for m in ('region', 'boolean', 'pgsphere')) for k in keys}
        ks = list(per)
        if not ks:
            continue
        med = [st.median(p[i] for p in per.values()) / 1000 for i in range(3)]
        print(f"| {name} | {len(ks)} | {med[0]:.3f} | {med[1]:.3f} | {med[2]:.3f} | "
              f"{ratio_ci(per, 0, 1, ks)} | {ratio_ci(per, 0, 2, ks)} |")
    print()
shapes = ['annulus', 'pair', 'lens', 'holed', 'mosaic']
table([(s, [k for k in size if k[0] == s]) for s in shapes], "shape")
for lo, hi, label in [(0, 0.2, "0.05-0.2 deg"), (0.2, 0.6, "0.2-0.6 deg"), (0.6, 3, "0.6-2 deg")]:
    table([(s, [k for k in size if k[0] == s and lo <= size[k] < hi]) for s in shapes], f"shape, {label}")
