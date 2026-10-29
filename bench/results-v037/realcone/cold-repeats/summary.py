#!/usr/bin/env python3
"""tab:cones' real-DR3 cold column under 0.37: four cold passes of
31_real_cones.sh on local flash (cold_run1..4.csv; the first-touch pass on
slow storage is ../cold-firsttouch/, not used).  Per pass, realcone_report.py's
measure -- the median over queries of the paired skycell / pgSphere ratio of
total time; then, per radius, the median and range over the four passes.
Also checks row counts and the I/O time per page read of each pass.

Usage: summary.py   (from this directory)
"""
import csv
import statistics as st
from collections import defaultdict

LABELS = ['1"', '10"', "1'", "6'", "30'", '1deg', '3deg']
runs = []
for i in range(1, 5):
    per = defaultdict(dict)
    io = defaultdict(list)
    for r in csv.DictReader(open(f'cold_run{i}.csv')):
        per[r['label'], int(r['qid'])][r['method']] = r
        if int(r['rd']):
            io[r['method']].append(float(r['io_ms']) / int(r['rd']))
    ratios, mism = {}, 0
    for lab in LABELS:
        xs = []
        for (l, q), m in per.items():
            if l != lab:
                continue
            s, p = m['skycell'], m['pgsphere']
            mism += s['n'] != p['n']
            xs.append((float(s['plan_ms']) + float(s['exec_ms'])) / (float(p['plan_ms']) + float(p['exec_ms'])))
        ratios[lab] = st.median(xs)
    runs.append((ratios, mism, {m: st.median(v) for m, v in io.items()}))

print('| radius | ' + ' | '.join(f'pass {i}' for i in range(1, 5)) + ' | median [min, max] |')
print('|---|' + '---|' * 5)
for lab in LABELS:
    xs = [r[0][lab] for r in runs]
    print(f'| {lab} | ' + ' | '.join(f'{x:.2f}' for x in xs) +
          f' | {st.median(xs):.2f} [{min(xs):.2f}, {max(xs):.2f}] |')
print()
for i, (_, mism, io) in enumerate(runs, 1):
    print(f'pass {i}: row-count mismatches {mism}; median I/O ms per page read: '
          + ', '.join(f'{m} {v:.3f}' for m, v in sorted(io.items())))
