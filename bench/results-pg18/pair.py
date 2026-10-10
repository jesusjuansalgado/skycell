"""Paired warm ratios behind rewrite_vs_custom.txt: the range rewrite against
pgSphere, and the custom scan against the rewrite (same query, same trial).

Usage: python3 pair.py bench_ab.csv
"""
import sys
from collections import defaultdict
sys.path.insert(0, __file__.rsplit('/', 2)[0])
from ab_report import med, boot_ci, load, RADII

pairs = [('skycell-rw', 'pgsphere'), ('skycell', 'skycell-rw')]
rows = [r for r in load(sys.argv[1]) if r['cache'] == 'warm']
pq = defaultdict(list)
for r in rows:
    pq[(r['corpus'], r['label'], int(r['qid']), r['method'])].append(float(r['ms']))
qm = {k: med(v) for k, v in pq.items()}
for corpus in sorted({r['corpus'] for r in rows}):
    print(f'\n{corpus} warm | ' + ' | '.join(f'{a}/{b}' for a, b in pairs))
    for l in RADII:
        qs = sorted({k[2] for k in qm if k[:2] == (corpus, l)})
        out = []
        for a, b in pairs:
            rs = [qm[(corpus, l, q, a)] / qm[(corpus, l, q, b)] for q in qs]
            lo, hi = boot_ci(rs)
            out.append(f'{med(rs):.2f} [{lo:.2f}, {hi:.2f}]')
        print(f'{l:5} n={len(qs):3} | ' + ' | '.join(out))
