"""Where skycell's MOC of a cone and MOCPy's MOC.from_cone differ, decide by
dense sampling which is right: no cell MOCPy has and skycell lacks may hold a
point inside the cone, and each cell skycell has and MOCPy lacks should.
The same 200 regions (the 50 cones among them) as validate.py."""
import os, random, subprocess
import numpy as np
from mocpy import MOC
import astropy.units as u
from astropy.coordinates import SkyCoord

DB = os.environ.get("PGDATABASE", "real36")

def q(sql):
    return subprocess.run(['psql', '-X', '-At', '-d', DB, '-c', sql],
                          capture_output=True, text=True, check=True).stdout.strip()

rnd = random.Random(43)
rng = np.random.default_rng(5)
N = 3_000_000

def ring(c, lo, hi):
    """N points uniform on the sphere between lo and hi degrees from c"""
    rho = np.degrees(np.arccos(rng.uniform(np.cos(np.radians(hi)), np.cos(np.radians(lo)), N)))
    return c.directional_offset_by(rng.uniform(0, 360, N) * u.deg, rho * u.deg)

bad_ref = n_ref = n_ours = unsampled = 0
for i in range(200):
    ra, dec = rnd.uniform(0, 360), rnd.uniform(-89, 89)
    order = rnd.randint(0, 12)
    s = rnd.choice([0.01, 0.1, 1, 5, 20])
    if i % 4:
        continue
    m = MOC.from_string(q(f"SELECT skycell_moc(circle('ICRS',{ra},{dec},{s}),{order})"), format='ascii')
    ref = MOC.from_cone(lon=ra * u.deg, lat=dec * u.deg, radius=s * u.deg, max_depth=order)
    c = SkyCoord(ra * u.deg, dec * u.deg)
    edge = np.degrees(np.sqrt(4 * np.pi / (12 * 4 ** order))) * 2   # two cell sizes
    p = ring(c, max(0, s - edge), s)          # inside the cone, near its edge
    only_ref, only_ours = ref - m, m - ref
    if not only_ref.empty():
        n_ref += len(only_ref.flatten())
        k = only_ref.contains_lonlat(p.ra, p.dec).sum()
        if k:
            bad_ref += 1
            print('a cell only MOCPy has meets the cone:', ra, dec, s, order, k)
    if not only_ours.empty():
        n_ours += len(only_ours.flatten())
        missing = only_ours - MOC.from_lonlat(lon=p.ra, lat=p.dec, max_norder=int(only_ours.max_order))
        if not missing.empty():
            unsampled += 1
            print('a cell only skycell has holds no sampled point of the cone:', ra, dec, s, order,
                  missing.to_string(format='ascii')[:200])
print(f'cells only MOCPy has: {n_ref}, meeting the cone: {bad_ref}; '
      f'cells only skycell has: {n_ours}, regions where one held no sampled point: {unsampled}')
