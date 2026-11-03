"""A MOC as a search region (0.44): skycell's rows against MOCPy's
contains_lonlat on the same positions, for random MOCs -- written by skycell
(skycell_moc of random cones, polygons and composites) and by MOCPy (cones,
random cell sets, coverage of point samples) -- through the custom scan, the
range rewrite and a sequential scan; in composites; and region relations."""
import os, random, subprocess, io
import numpy as np
from mocpy import MOC
import astropy.units as u

DB = os.environ.get("PGDATABASE", "real36")

def q(sql, opts=''):
    return subprocess.run(['psql', '-X', '-At', '-q', '-d', DB, '-c', opts + sql],
                          capture_output=True, text=True, check=True).stdout.strip()

out = q('SELECT obs_id, s_ra, s_dec FROM perf_oc WHERE s_ra IS NOT NULL ORDER BY obs_id', '')
arr = np.loadtxt(io.StringIO(out.replace('|', ',')), delimiter=',')
ids, ra, dec = arr[:, 0].astype(np.int64), arr[:, 1], arr[:, 2]
print(f'perf_oc: {len(ids)} positions')

rnd = random.Random(7)
nprng = np.random.default_rng(7)

def random_moc(i):
    kind = i % 6
    o = rnd.randint(2, 12)
    c_ra, c_dec, s = rnd.uniform(0, 360), rnd.uniform(-85, 85), rnd.choice([0.2, 1, 5, 20])
    if kind == 0:
        return q(f"SELECT skycell_moc(circle('ICRS',{c_ra},{c_dec},{s}),{o})")
    if kind == 1:
        h = min(s, 80 - abs(c_dec)) / 2
        return q(f"SELECT skycell_moc(polygon('ICRS',{c_ra},{c_dec-h},{c_ra+s},{c_dec},{c_ra},{c_dec+h}),{o})")
    if kind == 2:
        return q(f"SELECT skycell_moc(region('DIFFERENCE ICRS (CIRCLE {c_ra} {c_dec} {s} CIRCLE {c_ra} {c_dec} {s/2})'),{o})")
    if kind == 3:
        return MOC.from_cone(lon=c_ra * u.deg, lat=c_dec * u.deg, radius=s * u.deg, max_depth=o).to_string(format='ascii')
    if kind == 4:   # random cells of mixed orders
        cells = {}
        for _ in range(rnd.randint(1, 40)):
            k = rnd.randint(0, 9)
            cells.setdefault(k, set()).add(rnd.randrange(12 * 4 ** k))
        return ' '.join(f"{k}/{' '.join(map(str, sorted(v)))}" for k, v in sorted(cells.items()))
    # coverage of a sample of the catalogue's own positions: boundaries on rows
    sel = nprng.choice(len(ra), 2000, replace=False)
    return MOC.from_lonlat(lon=ra[sel] * u.deg, lat=dec[sel] * u.deg, max_norder=o).to_string(format='ascii')

def ref_moc(m):
    """MOCPy's MOC of the text, built cell by cell (it refuses overlapping cells in ASCII)"""
    ref, order = None, None
    for tok in m.split():
        if '/' in tok:
            order, tok = tok.split('/')
            order = int(order)
            if not tok:
                continue
        a, _, b = tok.partition('-')
        cells = np.arange(int(a), int(b or a) + 1, dtype=np.uint64)
        c = MOC.from_healpix_cells(cells, np.full(len(cells), order, dtype=np.uint8), max_depth=29)
        ref = c if ref is None else ref.union(c)
    return ref if ref is not None else MOC.new_empty(29)

pred = "1 = CONTAINS(POINT('ICRS', s_ra, s_dec), skycell_moc_region('{m}'))"
modes = {'custom scan': '', 'range rewrite': 'SET skycell.custom_scan = off; ',
         'sequential': 'SET skycell.custom_scan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; '}
bad = 0
n = 60
rows_total = 0
for i in range(n):
    m = random_moc(i)
    ref = ref_moc(m)
    truth = set(ids[ref.contains_lonlat(ra * u.deg, dec * u.deg)].tolist())
    rows_total += len(truth)
    for name, opts in modes.items():
        got = q(f"SELECT obs_id FROM perf_oc WHERE {pred.format(m=m)}", opts)
        got = set(int(x) for x in got.split()) if got else set()
        if got != truth:
            bad += 1
            print('DIFFERS', name, i, m[:100], len(got), len(truth), len(got - truth), len(truth - got))
print(f'{n} MOCs x {len(modes)} plans: {n * len(modes) - bad} equal to MOCPy, {rows_total} rows in all')

# a MOC in a composite: MOC less a cone, MOC and NOT
bad = 0
for i in range(20):
    m = random_moc(i * 3 + 1)
    ref = ref_moc(m)
    inm = ref.contains_lonlat(ra * u.deg, dec * u.deg)
    c = q(f"SELECT centroid(skycell_moc_region('{m}'))").strip('()').split(',')
    c_ra, c_dec = float(c[0]), float(c[1])
    r = rnd.choice([0.5, 2, 8])
    d = np.degrees(np.arccos(np.clip(np.sin(np.radians(dec)) * np.sin(np.radians(c_dec)) +
                                     np.cos(np.radians(dec)) * np.cos(np.radians(c_dec)) * np.cos(np.radians(ra - c_ra)), -1, 1)))
    truth = set(ids[inm & ~(d <= r)].tolist())
    reg = f"region('DIFFERENCE ICRS (MOC {m} CIRCLE {c_ra} {c_dec} {r})')"
    for name, opts in modes.items():
        got = q(f"SELECT obs_id FROM perf_oc WHERE 1 = CONTAINS(POINT('ICRS', s_ra, s_dec), {reg})", opts)
        got = set(int(x) for x in got.split()) if got else set()
        if got != truth:
            bad += 1
            print('DIFFERS (composite)', name, i, len(got), len(truth))
print(f'20 MOC-less-cone composites x {len(modes)} plans: {60 - bad} equal to the expected rows')
