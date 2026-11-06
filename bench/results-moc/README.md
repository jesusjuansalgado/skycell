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

## A MOC as a search region (0.44)

`region.py` builds 60 random MOCs. Half come from skycell (`skycell_moc` of
cones, polygons and annuli at orders 2–12), half from MOCPy (cones, the
coverage of 2,000 of the table's own positions, and random cells of orders
0–9, overlapping ones included). Each is used as a search region on
`perf_oc`, through the custom scan, the range rewrite (custom scan off) and a
sequential scan. The rows are compared with MOCPy's `contains_lonlat` on the
same positions. Then 20 composites, a MOC less a cone, are compared with the
rows expected from both tests.

```bash
PGDATABASE=real36 python3 region.py
```

Result ([`region.txt`](region.txt)): 180 of 180 answers equal MOCPy's (3.67
million rows in all), and 60 of 60 composite answers equal the expected rows.

## MOC set operations (0.46)

`setops.py` makes 60 pairs of random MOCs, half written by skycell (cones and
annuli at orders 3–12), half by MOCPy (cones, and random cells of orders 0–9
degraded to order 9). It compares `skycell_moc_union`, `_intersection`,
`_difference`, `_complement` and `_degrade` with MOCPy's operations, and
`skycell_moc_union_agg` over 200 MOCs at once with MOCPy's union of them.

```bash
PGDATABASE=real36 python3 setops.py
```

Result ([`setops.txt`](setops.txt)): every operation equals MOCPy's, cell for
cell, for all 60 pairs, and so does the aggregate. The difference is compared
with MOCPy's A ∩ (complement of B): MOCPy 0.21.0's own `difference()` returns
an empty MOC when the two MOCs do not overlap (`MOC("1/1").difference(MOC("1/5"))`
gives `1/`), and so differed from it for 21 of the 60 pairs.
