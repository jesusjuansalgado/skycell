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

## Why the two dialects differ

The `pgsphere` dialect is what makes the comparison in the paper concrete. Q3C
gives the fast cone and cross-match paths but only over scalar `ra`/`dec`
columns and has no region type; pgSphere gives the region types but is slower at
cones and cross-matches. A translator targeting the pair therefore **chooses per
query shape**, and three constructs have no direct form at all:

| ADQL | pgSphere + Q3C | skycell |
|---|---|---|
| `CONTAINS(POINT, CIRCLE)` | `q3c_radial_query(...)` if the position is two columns, else `pos <@ scircle(...)` | `point(...) <@ circle(...)` |
| `BOX('ICRS', a, d, w, h)` | `sbox` takes two **corners**, not a centre and extent — converted, and wrong at the poles | `box('ICRS', a, d, w, h)` |
| `AREA(region)` | `area()` returns **steradians**, scaled by (180/π)² | `area(region)` |
| `INTERSECTS(r1, r2)` | pgSphere only; Q3C has no region type | `intersects(r1, r2)` |
| `CENTROID(region)` | **no direct equivalent** for a polygon | `centroid(region)` |

For skycell the translation is the identity except where an indexable form is
wanted, because the extension uses ADQL's own spellings.

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
