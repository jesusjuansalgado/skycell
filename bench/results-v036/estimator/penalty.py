#!/usr/bin/env python3
"""The cost of a badly chosen fixed order, from part (b) of 29_estimator.sql
(est_decision.csv): per cone (field, radius), each setting's median time over
the timed repetitions, then the worst fixed order (4-13) over the best fixed
order, beside the chosen covering over the best.  This run's counterpart of the
paper's "a badly chosen fixed size is ... times worse", which was 17_ablation.sql's
factor 2 (orders 3-13, 15 catalogue centres at 10", 0.1 and 1 deg).

Usage: penalty.py est_decision.csv
"""
import csv
import statistics
import sys
from collections import defaultdict

ORDER = ['omega_cen', '47tuc', 'lmc', 'baade', 'ngp', 'gal_centre']
CORES = {'omega_cen', '47tuc'}

ms = defaultdict(list)
for r in csv.DictReader(open(sys.argv[1] if len(sys.argv) > 1 else 'est_decision.csv')):
    ms[(r['field'], float(r['r'])), r['setting']].append(float(r['ms']))
med = {k: statistics.median(v) for k, v in ms.items()}
cones = sorted({c for c, _ in med}, key=lambda c: (ORDER.index(c[0]), c[1]))

print('| field | r (deg) | best fixed | worst fixed | worst / best | chosen / best |')
print('|---|---|---|---|---|---|')
groups = defaultdict(list)
for c in cones:
    fixed = {s: v for (cc, s), v in med.items() if cc == c and s != 'chosen'}
    b, w = min(fixed, key=fixed.get), max(fixed, key=fixed.get)
    pen = fixed[w] / fixed[b]
    groups['cluster cores' if c[0] in CORES else 'elsewhere'].append(pen)
    groups['radius %g' % c[1]].append(pen)
    groups['all'].append(pen)
    print(f'| {c[0]} | {c[1]:g} | {b} ({fixed[b]:.3f} ms) | {w} ({fixed[w]:.3f} ms) | {pen:.0f} '
          f'| {med[c, "chosen"] / fixed[b]:.2f} |')
print()
for g, xs in groups.items():
    print(f'{g}: worst / best {min(xs):.0f}-{max(xs):.0f}, median {statistics.median(xs):.0f}')
