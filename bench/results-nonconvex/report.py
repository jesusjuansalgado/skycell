"""Per shape and size: skycell's summed per-polygon median time over pgSphere's,
with a 95% bootstrap interval over polygons."""
import sys, csv, random, statistics as st
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
t = defaultdict(list)
size = {}
for r in rows:
    t[(r['shape'], r['c'], r['method'])].append(float(r['us']))
    size[(r['shape'], r['c'])] = float(r['s'])
def table(groups, title):
    print(f"| {title} | polygons | skycell ms | pgSphere ms | skycell / pgSphere [95%] |")
    print("|---|---|---|---|---|")
    for name, keys in groups:
        per = {k: (st.median(t[k + ('skycell',)]), st.median(t[k + ('pgsphere',)])) for k in keys}
        ks = list(per)
        def ratio(sample): return sum(per[k][0] for k in sample) / sum(per[k][1] for k in sample)
        random.seed(1)
        bs = sorted(ratio(random.choices(ks, k=len(ks))) for _ in range(2000))
        print(f"| {name} | {len(ks)} | {st.median(p[0] for p in per.values())/1000:.3f} | "
              f"{st.median(p[1] for p in per.values())/1000:.3f} | {ratio(ks):.2f} [{bs[50]:.2f}, {bs[1949]:.2f}] |")
    print()
shapes = ['quad', 'U', 'L', 'comb', 'star']
table([(s, [k for k in size if k[0] == s]) for s in shapes], "shape")
bins = [(0, 0.2, "0.05-0.2 deg"), (0.2, 0.6, "0.2-0.6 deg"), (0.6, 3, "0.6-2 deg")]
for lo, hi, label in bins:
    table([(s, [k for k in size if k[0] == s and lo <= size[k] < hi]) for s in shapes], f"shape, {label}")
