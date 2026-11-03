# MOC export against MOCPy

`skycell_moc`, `skycell_moc_agg` and `skycell_coverage_moc` (SQL 0.43)
checked against [MOCPy](https://github.com/cds-astro/mocpy) 0.21.0, on the
`real36` database (any database with skycell 0.43 does for the regions; the
coverage check needs `perf_oc`, the ObsCore-shaped table of
[`bench/perfcheck/`](../perfcheck/)).

```bash
pip install mocpy==0.21.0
PGDATABASE=real36 python3 validate.py
PGDATABASE=real36 python3 disputed.py
```

`validate.py`, 200 random regions (cones, boxes, differences and unions) at
random orders 0–12: MOCPy reads each ASCII MOC and writes it back identically
(so it is normalised as MOCPy normalises), and reads the JSON one as the same
MOC; for the 50 cones, it compares with `MOC.from_cone`. Then the coverage of
`perf_oc` against `MOC.from_lonlat` on the same positions at orders 6, 10 and 12.

`disputed.py` decides the cones where the two differ, by 3M points sampled
inside each cone near its edge.

Results ([`validate.txt`](validate.txt), [`disputed.txt`](disputed.txt)):
all 200 ASCII and JSON MOCs read back identically; the three coverages are
equal to MOCPy's. skycell's cone MOC lies within MOCPy's in all 50 cones; in
21, MOCPy's holds cells more, and none of them holds a sampled point inside
its cone. `MOC.from_cone` is documented as an approximation; skycell's holds
every cell the cone meets and at most cells it misses by under 1/16 of a cell.

The first version of `skycell_moc`, like `skycell_cell_overlaps` in 0.42,
kept any cell whose circumscribing cap met the region, so a cell up to its
own size away (0.87° from a 0.01° cone at order 3). 0.43 refines such a cell
four orders deeper before keeping it, in both.
