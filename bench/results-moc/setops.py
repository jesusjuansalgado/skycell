"""MOC set operations (0.46) against MOCPy's: union, intersection, difference,
complement and degrade of random MOCs, written by skycell and by MOCPy, and
the union aggregate over many; each result must be MOCPy's cell for cell.
The difference is compared with MOCPy's A and not B: MOCPy 0.21.0's own
difference() returns an empty MOC when the two MOCs do not overlap."""
import os, random, subprocess
import numpy as np
from mocpy import MOC
import astropy.units as u

DB = os.environ.get("PGDATABASE", "real36")

def q(sql):
    return subprocess.run(['psql', '-X', '-At', '-q', '-v', 'ON_ERROR_STOP=1', '-d', DB],
                          input=sql + ';', capture_output=True, text=True, check=True).stdout.strip()

rnd = random.Random(11)

def random_moc(i):
    """an ASCII MOC, from skycell or from MOCPy"""
    o = rnd.randint(3, 12)
    ra, dec, s = rnd.uniform(0, 360), rnd.uniform(-80, 80), rnd.choice([0.5, 2, 8, 25])
    kind = i % 4
    if kind == 0:
        return q(f"SELECT skycell_moc(circle('ICRS',{ra},{dec},{s}),{o})")
    if kind == 1:
        return q(f"SELECT skycell_moc(region('DIFFERENCE ICRS (CIRCLE {ra} {dec} {s} CIRCLE {ra} {dec} {s/2})'),{o})")
    if kind == 2:
        return MOC.from_cone(lon=ra * u.deg, lat=dec * u.deg, radius=s * u.deg, max_depth=o).to_string(format='ascii')
    cells = {}
    for _ in range(rnd.randint(1, 30)):
        k = rnd.randint(0, 9)
        cells.setdefault(k, set()).add(rnd.randrange(12 * 4 ** k))
    # MOCPy builds it from the cells; its ASCII is then a valid, normalised MOC
    mocs = [MOC.from_healpix_cells(np.array(sorted(v), dtype=np.uint64),
                                   np.full(len(v), k, dtype=np.uint8), max_depth=29)
            for k, v in cells.items()]
    m = mocs[0]
    for x in mocs[1:]:
        m = m.union(x)
    return m.degrade_to_order(9).to_string(format='ascii')

def mocpy(ascii_moc):
    return MOC.from_string(ascii_moc, format='ascii')

def ours(sql):
    """skycell's MOC region, as MOCPy reads its cells"""
    t = q(f"SELECT skycell_moc(({sql}), 29)")
    return mocpy(t)

def same(a, b):
    return (a - b).empty() and (b - a).empty()

def reg(m):
    return f"skycell_moc_region('{m}')"

n = 60
mocpy_difference_wrong = 0
bad = {k: 0 for k in ('union', 'intersection', 'difference', 'complement', 'degrade')}
for i in range(n):
    a, b = random_moc(i), random_moc(i + 1)
    A, B = mocpy(a), mocpy(b)
    k = rnd.randint(0, 8)
    checks = {
        'union': (f"skycell_moc_union({reg(a)}, {reg(b)})", A.union(B)),
        'intersection': (f"skycell_moc_intersection({reg(a)}, {reg(b)})", A.intersection(B)),
        # MOCPy 0.21.0's difference() is wrong when the two do not overlap
        # (MOC('1/1').difference(MOC('1/5')) is empty): A and not B instead
        'difference': (f"skycell_moc_difference({reg(a)}, {reg(b)})", A.intersection(B.complement())),
        'complement': (f"skycell_moc_complement({reg(a)})", A.complement()),
        'degrade': (f"skycell_moc_degrade({reg(a)}, {k})", A.degrade_to_order(k)),
    }
    if not same(A.difference(B), A.intersection(B.complement())):
        mocpy_difference_wrong += 1
    for name, (sql, ref) in checks.items():
        if not same(ours(sql), ref):
            bad[name] += 1
            print('DIFFERS', name, i, a[:60], b[:60])
print(f'{n} pairs of MOCs: ' + ', '.join(f'{k} {n - v}/{n} equal to MOCPy' for k, v in bad.items()))
print(f'(MOCPy 0.21.0 difference() itself differed from A and not B in {mocpy_difference_wrong} of {n})')

# the union aggregate over 200 MOCs at once, against MOCPy's union of them
mocs = [random_moc(i) for i in range(200)]
values = ','.join(f"({reg(m)})" for m in mocs)
agg = ours(f"SELECT skycell_moc_union_agg(r) FROM (VALUES {values}) v(r)")
ref = mocpy(mocs[0])
for m in mocs[1:]:
    ref = ref.union(mocpy(m))
print(f'union aggregate of 200 MOCs equal to MOCPy: {same(agg, ref)}')
