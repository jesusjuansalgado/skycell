"""Paired cross-match ratios from 11_xmatch_ab.sql's bench_xm_ab: per (radius,
rep, block) trial, skycell's time over the rival's; median over trials and a
percentile bootstrap over them, as ab_report.py does for cones.

Usage: python3 report.py bench_xm_ab.csv
"""
import csv
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit('/', 3)[0])
from ab_report import med, boot_ci

t = defaultdict(dict)
n = defaultdict(dict)
for r in csv.DictReader(open(sys.argv[1])):
    k = (float(r['rows_m']), float(r['radius_arcsec']), int(r['rep']), int(r['blk']))
    t[k][r['method']] = float(r['ms'])
    n[k][r['method']] = int(r['n'])
for m in ('skycell_lateral', 'skycell_slots', 'pgsphere'):
    d = [v[m] - v['q3c'] for v in n.values() if m in v and 'q3c' in v and v[m] != v['q3c']]
    print(f'{m}: row count differs from q3c in {len(d)} of {len(n)} trials'
          + (f' (by {min(d)}..{max(d)} rows)' if d else ''))
for rows_m, rad in sorted({k[:2] for k in t}):
    ks = [k for k in t if k[:2] == (rows_m, rad)]
    print(f'\n{rows_m:g}M rows, {rad:g}" ({len(ks)} trials); median ms per block:',
          ', '.join(f'{m} {med([t[k][m] for k in ks]):.0f}' for m in sorted(t[ks[0]])))
    for a in ('skycell_lateral', 'skycell_slots'):
        for b in ('q3c', 'pgsphere'):
            rs = [t[k][a] / t[k][b] for k in ks]
            lo, hi = boot_ci(rs)
            print(f'  {a:16} / {b:8} {med(rs):.2f} [{lo:.2f}, {hi:.2f}]')
