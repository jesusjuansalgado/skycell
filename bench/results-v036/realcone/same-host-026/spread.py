#!/usr/bin/env python3
"""Per section and radius: the point ratio of results-pg18's 0.26 run, of
this directory's 0.36 run (..), and the median and min-max of the four
same-host runs of each version (run<i>_0<v>/).  Usage: spread.py"""
import glob
import re
import statistics

def parse(path):
    out, sec = {}, None
    for line in open(path):
        if line.startswith('## '):
            sec = line.split(':')[-1].strip() + (' cold' if 'cold' in line else ' warm')
        m = re.match(r'\| ([^|]+) \| (\d+) \| ([0-9.]+) \[([0-9.]+), ([0-9.]+)\] \|', line)
        if sec and m:
            out[sec, m.group(1)] = (float(m.group(3)), m.group(4), m.group(5))
    return out

old = parse('../../../results-pg18/realcone/report.md')
new = parse('../report.md')
runs = {v: [parse(f) for f in sorted(glob.glob(f'run*_0{v}/report.md'))] for v in ('26', '36')}
sec0 = None
for (sec, rad), (p, lo, hi) in new.items():
    if sec != sec0:
        print(f'\n### {sec}\n')
        print('| radius | 0.26, results-pg18 | 0.36, this run | same host 0.26: median [min, max] of 4 runs | same host 0.36: median [min, max] of 4 runs |')
        print('|---|---|---|---|---|')
        sec0 = sec
    o = old[sec, rad]
    cells = []
    for v in ('26', '36'):
        xs = [r[sec, rad][0] for r in runs[v]]
        cells.append(f'{statistics.median(xs):.2f} [{min(xs):.2f}, {max(xs):.2f}]')
    print(f'| {rad} | {o[0]:.2f} [{o[1]}, {o[2]}] | {p:.2f} [{lo}, {hi}] | ' + ' | '.join(cells) + ' |')
