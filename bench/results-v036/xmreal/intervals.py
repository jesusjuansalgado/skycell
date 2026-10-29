#!/usr/bin/env python3
"""report.py's table with 95% bootstrap intervals, for comparing two runs.

report.py gives, per radius and over all cells, the geometric mean of the
per-cell ratio of medians.  Here each (cell, method)'s repetitions are
resampled with replacement, the medians, ratios and geometric means
recomputed, 4000 times, seed 18; the interval is the 2.5th-97.5th percentile.

Usage: intervals.py xms.csv
"""
import csv
import math
import random
import statistics
import sys
from collections import defaultdict

FORMS = [('skycell_cs', 'cone, custom'), ('skycell_join', 'join, custom'), ('skycell_slots', 'cone, slots')]
RIVALS = [('q3c_join', 'q3c_join'), ('pgsphere', 'pgSphere')]
B = 4000


def gmean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def table(ms, cells, radii, draw):
    med = {k: statistics.median(draw(v)) for k, v in ms.items()}
    out = {}
    for rad in radii + [None]:
        cs = [c for c in cells if rad is None or c[2] == rad]
        for f, _ in FORMS:
            for rv, _ in RIVALS:
                out[rad, f, rv] = gmean([med[c, f] / med[c, rv] for c in cs])
    return out


def main():
    ms = defaultdict(list)
    for r in csv.DictReader(open(sys.argv[1])):
        if r['timed_out'] == 't':
            continue
        ms[(int(r['n']), r['targets'], float(r['radius_arcsec'])), r['method']].append(float(r['ms']))
    cells = sorted({c for c, _ in ms})
    radii = sorted({c[2] for c in cells})
    point = table(ms, cells, radii, lambda v: v)
    rng = random.Random(18)
    boots = [table(ms, cells, radii, lambda v: [rng.choice(v) for _ in v]) for _ in range(B)]
    head = ' | '.join(f'{fl} vs {rl}' for f, fl in FORMS for _, rl in RIVALS)
    print(f'Geometric mean of the per-cell ratio, 95% bootstrap interval over repetitions (B={B}, seed 18)\n')
    print(f'| radius | {head} |')
    print('|---|' + '---|' * (len(FORMS) * len(RIVALS)))
    for rad in radii + [None]:
        vals = []
        for f, _ in FORMS:
            for rv, _ in RIVALS:
                xs = sorted(b[rad, f, rv] for b in boots)
                vals.append(f'{point[rad, f, rv]:.2f} [{xs[int(0.025 * B)]:.2f}, {xs[int(0.975 * B) - 1]:.2f}]')
        label = 'all' if rad is None else f'{rad:g}"'
        print(f'| {label} | ' + ' | '.join(vals) + ' |')


if __name__ == '__main__':
    main()
