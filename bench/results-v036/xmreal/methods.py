#!/usr/bin/env python3
"""Per method: the geometric mean over the 24 cells of the ratio of its median
time in run B to its median time in run A (same cells).  Usage: methods.py A.csv B.csv [...]
with several B files pooled against several A files: methods.py A1,A2 B1,B2"""
import csv, math, statistics, sys
from collections import defaultdict
def med(files):
    ms = defaultdict(list)
    for f in files.split(','):
        for r in csv.DictReader(open(f)):
            ms[(r['n'], r['targets'], r['radius_arcsec']), r['method']].append(float(r['ms']))
    return {k: statistics.median(v) for k, v in ms.items()}
a, b = med(sys.argv[1]), med(sys.argv[2])
for m in ['q3c_join', 'skycell_cs', 'skycell_join', 'skycell_slots', 'pgsphere']:
    xs = [b[k] / a[k] for k in a if k[1] == m]
    print(f'{m:14s} {math.exp(sum(map(math.log, xs)) / len(xs)):.2f}')
