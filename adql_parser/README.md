# ADQL → PostgreSQL translation

Reference translators that turn ADQL geometry into SQL, for **skycell** or for
the **pgSphere + Q3C** pair an archive uses today. Two implementations, kept
byte-for-byte identical in output, so a service in either language gets the
same SQL:

| | |
|---|---|
| [`python/skycell_adql.py`](python/skycell_adql.py) | no dependencies, Python 3.9+ |
| [`java/SkycellAdql.java`](java/SkycellAdql.java) | no dependencies, Java 11+ |

```bash
python3 python/skycell_adql.py "SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 0.5)) = 1"
# SELECT * FROM t WHERE (point('ICRS', ra, dec) <@ circle('ICRS', 10, 20, 0.5))

python3 python/skycell_adql.py "...same query..." pgsphere
# SELECT * FROM t WHERE q3c_radial_query(ra, dec, 10, 20, 0.5)
```

```java
new SkycellAdql(Dialect.SKYCELL, "2.1").translate(adql);
```

## What it is, and is not

It is **not an ADQL parser**. It tokenises the statement, rewrites the geometry
constructs, and passes everything else through untouched — enough for a TAP
layer whose SQL is otherwise already PostgreSQL. If your service already parses
ADQL properly (the [CDS ADQL library](https://github.com/gmantele/taplib) in
Java, for instance), call `translateExpression()` on the geometry nodes rather
than handing it the whole statement.

What it handles: `POINT`, `CIRCLE`, `BOX`, `POLYGON`, `CONTAINS`, `INTERSECTS`,
`DISTANCE` (both the two-point and the ADQL 2.1 four-scalar form), `AREA`,
`COORD1`, `COORD2`, `CENTROID`, the `= 1` / `= 0` / `1 = …` predicate forms, and
nesting. Case-insensitive; string literals are left alone.

**Versions.** ADQL 2.0 requires a coordinate-system argument on the geometry
constructors; 2.1 makes it optional. Pass `version="2.0"` to require it.

**Frames.** Only ICRS is translated. `GALACTIC`, `ECLIPTIC`, `FK4`, `FK5` are
**refused with an error** rather than silently mis-translated — convert with
`gal2icrs()` / `ecl2icrs()` first. Both dialects work in ICRS.

## Storing regions as text: skyregion speaks STC-S

Separate from query translation, but usually needed alongside it: a TAP
service's ObsCore table carries `s_region` as an IVOA STC-S string --
`CIRCLE ICRS ra dec radius` / `POLYGON ICRS ra1 dec1 ra2 dec2 ...`,
whitespace-separated, no commas or parentheses. skycell's `skyregion` type's
text representation *is* that same STC-S syntax, so an `s_region skyregion`
column casts straight from (and reads back as) the archive's own STC-S text,
with no translation step:

```sql
CREATE TABLE observations (obs_id int, s_region skyregion);
INSERT INTO observations VALUES
  (1, 'CIRCLE ICRS 266.404994801046 -28.936173960138692 0.05');

SELECT s_region FROM observations;
-- CIRCLE ICRS 266.404994801046 -28.9361739601387 0.05
```

The frame token must be `ICRS`, matching everything else in this extension
(and in this translator's own "Frames" note above); a stray flavor/refpos
token some archives include (`CIRCLE ICRS TOPOCENTER ...`) is accepted and
skipped. pgSphere has no equivalent: `scircle`/`spoly` are separate types
with their own, non-STC-S text formats, so a mixed-shape `s_region` column
needs two columns (or a view) instead of one.

This is unrelated to what this translator does -- ADQL's `CIRCLE(...)` /
`POLYGON(...)` *function-call* syntax inside a query is translated as
described above regardless of how `s_region` is stored; STC-S is only the
text form of the stored `skyregion` value itself, e.g. for loading or
re-exporting `s_region` as-is.

## Why the two dialects differ

The `pgsphere` dialect is what makes the comparison in the paper concrete. Q3C
gives the fast cone and cross-match paths but only over scalar `ra`/`dec`
columns and has no region type; pgSphere gives the region types but is slower at
cones and cross-matches. A translator targeting the pair therefore **chooses per
query shape**, and three constructs have no direct form at all:

| ADQL | pgSphere + Q3C | skycell |
|---|---|---|
| `CONTAINS(POINT, CIRCLE)` | `q3c_radial_query(...)` if the position is two columns, else `pos <@ scircle(...)` | `point(...) <@ circle(...)`, or `skycell_radial_query(...)` for a cross-match (below) |
| `BOX('ICRS', a, d, w, h)` | `sbox` takes two **corners**, not a centre and extent — converted, and wrong at the poles | `box('ICRS', a, d, w, h)` |
| `AREA(region)` | `area()` returns **steradians**, scaled by (180/π)² | `area(region)` |
| `INTERSECTS(r1, r2)` | pgSphere only; Q3C has no region type | `intersects(r1, r2)` |
| `CENTROID(region)` | **no direct equivalent** for a polygon | `centroid(region)` |

For skycell the translation is the identity except where an indexable form is
wanted, because the extension uses ADQL's own spellings.

**Cross-matches.** `pos <@ circle(...)`'s planner rewrite only fires when the
circle is a compile-time constant; a `CIRCLE` whose centre (or radius) comes
from another table's row -- `CONTAINS(POINT('ICRS', a.ra, a.dec), CIRCLE('ICRS',
b.ra, b.dec, r)) = 1`, a cross-match written the ADQL way -- would otherwise
translate to `<@` over a non-constant circle and reach no index at all. The
translator detects that shape (position over plain columns, circle not all
literal numbers) and emits `skycell_radial_query(a.ra, a.dec, b.ra, b.dec, r)`
instead: Q3C's own argument order, which reaches skycell's non-constant
covering path and stays index-backed, capped to `skycell.join_slots` ranges
per probe. A literal `CIRCLE` is untouched -- `<@` already covers that case
without a cap. `INTERSECTS(POINT, CIRCLE)` gets the same treatment, either
argument order -- a point has no area, so intersecting a region is exactly
containment (skycell's own `skycell_intersects_pos` says as much).

A non-constant `POLYGON` cross-match -- `CONTAINS(POINT('ICRS', a.ra, a.dec),
POLYGON('ICRS', b.v1, b.v2, ...)) = 1`, matching points against a per-row
footprint rather than a literal shape -- hits the same gap and gets the same
fix: `skycell_poly_join(a.ra, a.dec, ARRAY[b.v1, b.v2, ...]::float8[])`, the
polygon analogue of `skycell_join`, added alongside its own non-constant
covering path in the extension (`skycell_poly_bound`, mirroring
`skycell_cone_bound`). One caveat a `CIRCLE` cross-match doesn't share: the
`skycell_cell_ops` selectivity estimator only recognises a circle's constant
radius argument, so unlike `skycell_radial_query`, `skycell_poly_join`'s range
predicates fall back to PostgreSQL's stock selectivity estimate even against a
`skycell_cell_ops` index -- there is no equivalent scalar to recover a
polygon's area from a non-constant, per-row shape. The result is still
correct and index-backed either way; only the plan's row-count estimate is
coarser than the circle case's. `BOX` and a stored `skyregion` column (an
arbitrary per-row footprint, as opposed to a literal `POLYGON(...)` call) are
not covered by this -- both remain open.

## Tests

```bash
python3 -m pytest tests -q          # 15 cases, both dialects, both versions
```

`tests/cases.txt` is also used to check that the Python and Java translators
produce identical output:

```bash
javac -d /tmp/jb java/SkycellAdql.java
python3 tests/cross_check.py
```
