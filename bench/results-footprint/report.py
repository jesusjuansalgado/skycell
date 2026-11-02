"""Per workload and footprint kind: skycell's summed per-query median time over
pgSphere's, with a 95% bootstrap interval over queries."""
import sys, csv, random, statistics as st
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
t = defaultdict(list)
kind = {}
for r in rows:
    t[(r['workload'], r['q'], r['method'])].append(float(r['us']))
    kind[(r['workload'], r['q'])] = r['kind']
def table(workload, title):
    print(f"| {title} | queries | skycell ms | pgSphere ms | skycell / pgSphere [95%] |")
    print("|---|---|---|---|---|")
    for k in ['nircam', 'uvis', 'parallel', 'corona', 'all']:
        keys = [q for q in kind if q[0] == workload and (k == 'all' or kind[q] == k)]
        if not keys:
            continue
        per = {q: (st.median(t[q + ('skycell',)]), st.median(t[q + ('pgsphere',)])) for q in keys}
        def ratio(sample): return sum(per[q][0] for q in sample) / sum(per[q][1] for q in sample)
        random.seed(1)
        bs = sorted(ratio(random.choices(keys, k=len(keys))) for _ in range(2000))
        print(f"| {k} | {len(keys)} | {st.median(p[0] for p in per.values())/1000:.3f} | "
              f"{st.median(p[1] for p in per.values())/1000:.3f} | {ratio(keys):.2f} [{bs[50]:.2f}, {bs[1949]:.2f}] |")
    print()
table('overlap', 'footprints meeting a search polygon')
table('position', 'footprints containing a position')
