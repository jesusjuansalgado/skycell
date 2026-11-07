"""Per shape: the folded boolean's summed per-shape median time over the
unfolded boolean's, the REGION's and pgSphere's, with a 95% bootstrap interval
over shapes."""
import sys, csv, random, statistics as st
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
t = defaultdict(list)
size = {}
for r in rows:
    t[(r['shape'], r['c'], r['method'])].append(float(r['us']))
    size[(r['shape'], r['c'])] = float(r['s'])
M = ('boolean', 'unfolded', 'region', 'pgsphere')
def ratio_ci(per, a, b, ks):
    def ratio(sample): return sum(per[k][a] for k in sample) / sum(per[k][b] for k in sample)
    random.seed(1)
    bs = sorted(ratio(random.choices(ks, k=len(ks))) for _ in range(2000))
    return f"{ratio(ks):.2f} [{bs[50]:.2f}, {bs[1949]:.2f}]"
print("| shape | shapes | folded ms | unfolded ms | REGION ms | pgSphere ms | folded / unfolded [95%] | folded / REGION [95%] | folded / pgSphere [95%] |")
print("|---|---|---|---|---|---|---|---|---|")
for s in ['annulus', 'pair', 'lens', 'holed', 'mosaic']:
    keys = [k for k in size if k[0] == s]
    per = {k: tuple(st.median(t[k + (m,)]) for m in M) for k in keys}
    ks = list(per)
    med = [st.median(p[i] for p in per.values()) / 1000 for i in range(4)]
    print(f"| {s} | {len(ks)} | {med[0]:.3f} | {med[1]:.3f} | {med[2]:.3f} | {med[3]:.3f} | "
          f"{ratio_ci(per, 0, 1, ks)} | {ratio_ci(per, 0, 2, ks)} | {ratio_ci(per, 0, 3, ks)} |")
