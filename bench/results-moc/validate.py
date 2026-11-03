import json, random, subprocess, io
import numpy as np
from mocpy import MOC
import astropy.units as u

import os
DB = os.environ.get("PGDATABASE", "real36")

def q(sql):
    r = subprocess.run(['psql', '-X', '-At', '-d', DB, '-c', sql], capture_output=True, text=True, check=True)
    return r.stdout.rstrip('\n')

rnd = random.Random(43)
fmt_ok = json_ok = superset_ok = 0
n = 0
extra_frac = []
for i in range(200):
    ra, dec = rnd.uniform(0, 360), rnd.uniform(-89, 89)
    order = rnd.randint(0, 12)
    kind = i % 4
    s = rnd.choice([0.01, 0.1, 1, 5, 20])
    if kind == 0:
        reg = f"circle('ICRS', {ra}, {dec}, {s})"
    elif kind == 1:
        reg = f"box('ICRS', {ra}, {max(-80 + s, min(80 - s, dec))}, {s}, {s/2})"
    elif kind == 2:
        reg = f"region('DIFFERENCE ICRS (CIRCLE {ra} {dec} {s} CIRCLE {ra} {dec} {s/2})')"
    else:
        reg = f"region('UNION ICRS (CIRCLE {ra} {dec} {s/2} CIRCLE {(ra + s) % 360} {dec} {s/3})')"
    a = q(f"SELECT skycell_moc({reg}, {order})")
    j = q(f"SELECT skycell_moc({reg}, {order}, 'json')")
    m = MOC.from_string(a, format='ascii')
    n += 1
    if m.to_string(format='ascii') == a:
        fmt_ok += 1
    else:
        print('ASCII differs:', reg, order, a[:120], '|', m.to_string(format='ascii')[:120])
    mj = MOC.from_json({k: v for k, v in json.loads(j).items()})
    if mj == m:
        json_ok += 1
    else:
        print('JSON differs:', reg, order)
    if kind == 0 and s <= 20:
        ref = MOC.from_cone(lon=ra * u.deg, lat=dec * u.deg, radius=s * u.deg, max_depth=order)
        if (ref - m).empty():
            superset_ok += 1
        else:
            print('MOCPy-only cells:', reg, order, (ref - m).to_string(format='ascii'))
        if not ref.empty():
            extra_frac.append((m - ref).sky_fraction / ref.sky_fraction)
print(f'{n} regions: ASCII normalised and re-serialised identically {fmt_ok}, JSON = ASCII {json_ok}, '
      f'cones whose MOC holds all of MOCPy\'s (the rest are within it) {superset_ok}/{sum(1 for i in range(200) if i % 4 == 0)}; '
      f'extra area beyond MOCPy\'s cone MOC: median {np.median(extra_frac):.3f}, max {max(extra_frac):.3f}')

# coverage of the ObsCore-shaped table against MOCPy from the same positions
out = subprocess.run(['psql', '-X', '-At', '-F', ',', '-d', DB, '-c', 'SELECT s_ra, s_dec FROM perf_oc WHERE s_ra IS NOT NULL AND s_dec IS NOT NULL'],
                     capture_output=True, text=True, check=True).stdout
pts = np.loadtxt(io.StringIO(out), delimiter=',')
for order in (6, 10, 12):
    mine = MOC.from_string(q(f"SELECT skycell_coverage_moc('perf_oc', {order})"), format='ascii')
    ref = MOC.from_lonlat(lon=pts[:, 0] * u.deg, lat=pts[:, 1] * u.deg, max_norder=order)
    print(f'coverage of perf_oc ({len(pts)} rows) at order {order}: equal to MOCPy {mine == ref}, '
          f'{mine.to_string(format="ascii").count(" ") + 1} tokens')
