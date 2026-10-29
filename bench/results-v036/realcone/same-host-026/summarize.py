#!/usr/bin/env python3
"""The 'total' ratio of each section of each run's report.md, side by side.
Usage: summarize.py run1_026 run2_036 ..."""
import re
import sys
runs = sys.argv[1:]
data = {}
for r in runs:
    sec = None
    for line in open(f'{r}/report.md'):
        if line.startswith('## '):
            sec = re.sub(r'^## Real corpus, ', '', line.strip()).split(':')[-1].strip() + (' cold' if 'cold' in line else ' warm')
        m = re.match(r'\| ([^|]+) \| (\d+) \| ([0-9.]+ \[[0-9., ]+\]) \|', line)
        if sec and m:
            data.setdefault(sec, {}).setdefault(m.group(1), {})[r] = m.group(3)
for sec, rows in data.items():
    print(f'\n### {sec}\n')
    print('| radius | ' + ' | '.join(runs) + ' |')
    print('|---|' + '---|' * len(runs))
    for rad, v in rows.items():
        print(f'| {rad} | ' + ' | '.join(v.get(r, '') for r in runs) + ' |')
