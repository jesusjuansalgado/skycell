#!/usr/bin/env python3
"""tab:cones and tab:scale, 0.36 -> 0.37: skycell / pgSphere with its 95%
interval for every corpus, cache and radius, from both versions' reports
(ab_report.md, scale50/ab_report.md, realcone/report.md).  Marks a pair whose
intervals no longer overlap: 'better' (0.37 lower) or 'worse'.

Usage: compare036.py   (from bench/results-v037)
"""
import re

LABELS = ['1"', '10"', "1'", "6'", "30'", '1deg', '3deg']
PAT = re.compile(r'\*?\*?([0-9.]+)\*?\*? \[([0-9.]+), ([0-9.]+)\]')


def ab(path):
    out, sec = {}, None
    for line in open(path):
        m = re.match(r'## (\w+) corpus, (\w+) cache', line)
        if m:
            sec = m.groups()
        elif line.startswith('## '):
            sec = None
        f = [x.strip() for x in line.split('|')]
        if sec and len(f) > 7 and f[1] in LABELS:
            out[sec + (f[1],)] = tuple(map(float, PAT.match(f[6]).groups()))
    return out


def real(path):
    out, sec = {}, None
    for line in open(path):
        if line.startswith('## '):
            sec = ('real', 'cold' if 'cold' in line else 'warm') if line.rstrip().endswith('skycell / pgsphere') else None
        f = [x.strip() for x in line.split('|')]
        if sec and len(f) > 4 and f[1] in LABELS:
            out[sec + (f[1],)] = tuple(map(float, PAT.match(f[3]).groups()))
    return out


old, new = {}, {}
for d, store in (('../results-v036', old), ('.', new)):
    store.update(ab(f'{d}/cones/ab_report.md'))
    store.update(ab(f'{d}/cones/scale50/ab_report.md'))
    store.update(real(f'{d}/realcone/report.md'))

names = [('designed', 'designed'), ('gaia', 'Gaia resampled, 1e7'), ('gaia50', 'Gaia resampled, 5e7'), ('real', 'real DR3')]
flags = []
for corpus, title in names:
    print(f'\n### {title}\n')
    print('| radius | warm, 0.36 | warm, 0.37 | cold, 0.36 | cold, 0.37 |')
    print('|---|---|---|---|---|')
    for lab in LABELS:
        cells = []
        for cache in ('warm', 'cold'):
            a, b = old[(corpus, cache, lab)], new[(corpus, cache, lab)]
            mark = ''
            if b[2] < a[1]:
                mark = ' (better)'
            elif b[1] > a[2]:
                mark = ' (worse)'
            if mark:
                flags.append(f'{title}, {cache}, {lab}: {a[0]:.2f} [{a[1]:.2f}, {a[2]:.2f}] -> {b[0]:.2f} [{b[1]:.2f}, {b[2]:.2f}]{mark}')
            cells += [f'{a[0]:.2f} [{a[1]:.2f}, {a[2]:.2f}]', f'{b[0]:.2f} [{b[1]:.2f}, {b[2]:.2f}]{mark}']
        print(f'| {lab} | ' + ' | '.join([cells[0], cells[1], cells[2], cells[3]]) + ' |')
print('\nIntervals that no longer overlap:\n')
for f in flags:
    print('-', f)
