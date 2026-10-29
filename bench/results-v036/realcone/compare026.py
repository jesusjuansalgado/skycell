#!/usr/bin/env python3
"""Per radius and method, 0.26 (../../results-pg18/realcone/) against 0.36
(this directory): median planning and execution time and mean buffers over
the same queries (the centre files are identical), warm and cold, and the
queries whose row count or buffer count differs between the versions.

Usage: compare026.py OLD_DIR NEW_DIR
"""
import csv
import statistics
import sys
from collections import defaultdict

LABELS = ['1"', '10"', "1'", "6'", "30'", '1deg', '3deg']


def load(d, name):
    return list(csv.DictReader(open(f'{d}/{name}.csv')))


def label_of(d):
    return {r['qid']: r['label'] for r in load(d, 'realcone_centers')}


def warm(d):
    lab = label_of(d)
    agg = defaultdict(lambda: defaultdict(list))
    perq = {}
    for r in load(d, 'bench_realcone'):
        k = (r['method'], lab[r['qid']])
        agg[k]['plan'].append(float(r['plan_ms']))
        agg[k]['exec'].append(float(r['exec_ms']))
        agg[k]['buf'].append(int(r['buffers']))
        perq[r['method'], r['qid']] = (r['n'], r['buffers'])
    return agg, perq


def cold(d):
    agg = defaultdict(lambda: defaultdict(list))
    for r in load(d, 'bench_realcone_cold'):
        k = (r['method'], r['label'])
        agg[k]['plan'].append(float(r['plan_ms']))
        agg[k]['exec'].append(float(r['exec_ms']))
        agg[k]['rd'].append(int(r['rd']))
        agg[k]['io'].append(float(r['io_ms'] or 0))
        rd = int(r['rd'])
        if rd:
            agg[k]['io_per_read'].append(float(r['io_ms'] or 0) / rd)
    return agg


def med(xs):
    return statistics.median(xs) if xs else float('nan')


def main():
    old, new = sys.argv[1], sys.argv[2]
    (wo, qo), (wn, qn) = warm(old), warm(new)
    print('## Warm, median per query (ms) and mean buffers, 0.26 -> 0.36\n')
    print('| method | radius | planning | execution | buffers |')
    print('|---|---|---|---|---|')
    for m in ['skycell', 'skycell-rw', 'pgsphere']:
        for lb in LABELS:
            a, b = wo[m, lb], wn[m, lb]
            print(f"| {m} | {lb} | {med(a['plan']):.4f} -> {med(b['plan']):.4f} "
                  f"| {med(a['exec']):.4f} -> {med(b['exec']):.4f} "
                  f"| {statistics.mean(a['buf']):.1f} -> {statistics.mean(b['buf']):.1f} |")
    nd = sum(1 for k in qo if k in qn and qo[k][0] != qn[k][0])
    bd = sum(1 for k in qo if k in qn and qo[k][1] != qn[k][1])
    print(f'\nWarm queries (method, centre) whose row count differs between the versions: {nd}; '
          f'whose buffer count differs: {bd} of {len(qo)}.\n')
    co, cn = cold(old), cold(new)
    print('## Cold, median per query (ms), mean pages read, median I/O ms per page read, 0.26 -> 0.36\n')
    print('| method | radius | planning | execution | pages read | I/O ms per read |')
    print('|---|---|---|---|---|---|')
    for m in ['skycell', 'pgsphere']:
        for lb in LABELS:
            a, b = co[m, lb], cn[m, lb]
            print(f"| {m} | {lb} | {med(a['plan']):.4f} -> {med(b['plan']):.4f} "
                  f"| {med(a['exec']):.4f} -> {med(b['exec']):.4f} "
                  f"| {statistics.mean(a['rd']):.1f} -> {statistics.mean(b['rd']):.1f} "
                  f"| {med(a['io_per_read']):.4f} -> {med(b['io_per_read']):.4f} |")


if __name__ == '__main__':
    main()
