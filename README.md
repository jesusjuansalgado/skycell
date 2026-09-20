# skycell

A PostgreSQL extension for sky positions: one index for cone searches,
polygons, cross-matches and stored footprints, written in the spelling ADQL
already uses.

```sql
CREATE EXTENSION skycell;

CREATE TABLE cat AS SELECT id, ra, dec, mag FROM source ORDER BY skycell_ang2cell(ra, dec);
CREATE INDEX ON cat (skycell_ang2cell(ra, dec));
ANALYZE cat;

-- 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 266.4, -28.9, 0.05))
SELECT * FROM cat WHERE point('ICRS', ra, dec) <@ circle('ICRS', 266.4, -28.9, 0.05);
```

On a 10-million-row synthetic catalogue with Gaia-like crowding, that query is
faster than both established extensions, the index is a third the size of
pgSphere's, and the planner's row estimates are 1.2× off instead of 2–3×.
The measurements, including where skycell *loses*, are in
[Comparison](#comparison-with-q3c-and-pgsphere).

---

## What it does differently

Q3C and pgSphere sit at opposite ends of a design space. Q3C keys each source
by a 64-bit number from a quad-tree on a cube and puts it in a B-tree: small
index, cheap probes, and a table that can be sorted along the curve. pgSphere
stores real geometry — points, circles, polygons — in a GiST tree of bounding
boxes: general, and able to index regions rather than only points.

skycell keeps the B-tree and gets the generality elsewhere.

**1. The key is an equal-area cell.** Positions are keyed by their order-29
HEALPix cell in the nested scheme: a 64-bit integer, cells of about 0.4 mas.
Equal area is what makes the rest work — because every cell covers the same
solid angle and nested cells are contiguous ranges of ids, the histogram
PostgreSQL's `ANALYZE` already keeps for that column *is* a map of how many
sources sit per square degree of sky. It is also the numbering Gaia's
`source_id` and the IVOA's MOC standard use, so ids are interoperable rather
than private to the extension.

**2. A region becomes a set of ranges, chosen by cost.** Any query region —
cone, box, convex polygon — is covered by cells, and the covering is turned
into `cell BETWEEN lo AND hi` conditions. How finely to cut is a cost
question, not a fixed depth:

```
cost(s) = ranges(s) · range_cost  +  ρ · (area covered − area of the region)
```

For cells of size *s* covering a region of radius *r*, the covering follows
the boundary, so the number of ranges goes as *r/s* and the wasted area as
perimeter·*s*. Minimising gives **s\* = √(α · range_cost / ρ)** — independent
of the region's size: the sky's local density ρ and the price of one extra
index range decide how finely it is worth cutting. skycell evaluates that cost
at all 30 orders (a few flops each) and takes the cheapest, so a crowded field
is cut finer than empty sky with no refinement loop at all.

**3. Cells are tested by their own geometry.** A cell is bounded by its four
corners, and its boundary by the chords between them, with the edge bulge
measured per cell from the edge midpoints — near the poles an edge departs
from its chord by a fraction of the cell size, not its square, which a global
constant gets wrong. This is much tighter than approximating every cell by a
cap of the worst-case pixel radius over the whole sphere.

**4. The predicate is rewritten by the planner, not by the user.** A support
function (`SupportRequestSimplify`, the mechanism PostGIS uses for
`ST_DWithin`) turns

```sql
point('ICRS', ra, dec) <@ circle('ICRS', 266.4, -28.9, 0.05)
```

into

```
(cell BETWEEN lo1 AND hi1 OR cell BETWEEN lo2 AND hi2 …)   -- B-tree ranges
AND skycell_in_region(point(ra, dec), circle(…), 0.29)      -- exact test
```

before path generation, finding the cell expression in the relation's own
indexes. Because the index conditions are then plain range predicates on a
plain expression, the planner estimates their selectivity from the same
histogram — which is why the row estimates come out close.

**5. Regions are stored as MOC cells.** A footprint is covered by a handful of
cells written in the IVOA `NUNIQ` encoding and stored one per row in a
B-tree. The footprints containing a point are those whose cell is one of the
point's ancestors — a short `= ANY(…)` lookup, no second index type.

Nothing in the covering is heuristic about correctness: every cell test is
conservative, so a covering never misses a row, and that is verified by brute
force over millions of points (see [Testing](#testing)).

---

## Installation

Needs PostgreSQL 15 or newer (18 recommended) and its server headers.

```bash
git clone https://github.com/jesusjuansalgado/skycell
cd skycell/ext
make && sudo make install
psql -c "CREATE EXTENSION skycell"
```

With Docker, including Q3C and pgSphere for comparison:

```bash
docker build -t skycell-pg:18 docker/
docker run -d --name skycell-pg -e POSTGRES_PASSWORD=skycell -e POSTGRES_DB=skycell \
  -v "$PWD":/work skycell-pg:18
docker exec -w /work/ext skycell-pg make with_llvm=no install
```

### ADQL names and `search_path`

The functions are named as ADQL names them. PostgreSQL has its own
`point(float8, float8)`, so install into a schema of its own and put that
schema before `pg_catalog` to use the bare names:

```sql
CREATE SCHEMA sky;
CREATE EXTENSION skycell SCHEMA sky;
SET search_path = sky, public, pg_catalog;
```

Without that, the forms carrying ADQL's coordinate-system argument —
`point('ICRS', ra, dec)`, `circle('ICRS', …)` — are unambiguous anyway, and
`skycell_point`, `skycell_circle`, `skycell_box`, `skycell_polygon` are
prefixed aliases for the rest.

### Indexing a table

Either key on the coordinate columns:

```sql
CREATE INDEX ON cat (skycell_ang2cell(ra, dec));
```

or store positions as a `skypos` column:

```sql
ALTER TABLE cat ADD COLUMN pos skypos;
UPDATE cat SET pos = skycell_point(ra, dec);
CREATE INDEX ON cat (skycell_cell(pos));
```

Two things pay off and neither is required: sorting the table along the cell
order (`CREATE TABLE … ORDER BY skycell_ang2cell(ra, dec)`, so neighbouring
sources share heap pages), and a finer histogram on the key
(`ALTER TABLE … ALTER COLUMN … SET STATISTICS 1000`, so the density map is
sharper).

---

## Operators and functions

### The ADQL surface

| skycell | ADQL |
|---|---|
| `point(ra, dec)`, `point('ICRS', ra, dec)` → `skypos` | `POINT` |
| `circle(ra, dec, radius)`, `circle('ICRS', …)` → `skyregion` | `CIRCLE` |
| `box(ra, dec, width, height)`, `box('ICRS', …)` | `BOX` |
| `polygon(ra1, dec1, ra2, dec2, …)`, `polygon('ICRS', …)` | `POLYGON` |
| `contains(skypos, skyregion) → int`, `contains(skyregion, skyregion)` | `CONTAINS` |
| `intersects(skypos, skyregion) → int`, `intersects(skyregion, skyregion)` | `INTERSECTS` |
| `distance(skypos, skypos) → float8`, `distance(ra1, dec1, ra2, dec2)` | `DISTANCE` |
| `area(skyregion) → float8` (square degrees) | `AREA` |
| `coord1(skypos)`, `coord2(skypos)`, `centroid(skyregion)` | `COORD1`, `COORD2`, `CENTROID` |

All angles are degrees, ICRS. `contains` and `intersects` return 1 or 0, so
`1 = CONTAINS(...)` translates literally.

### Operators — the indexable spelling

| operator | meaning |
|---|---|
| `skypos <@ skyregion` | position inside region (**rewritten into an index scan**) |
| `skyregion @> skypos` | the same, commuted |
| `skyregion && skyregion` | regions overlap |
| `skyregion @> skyregion` | region covers region |
| `skypos <-> skypos` | angular distance, degrees |

`contains(p, r) = 1` and `p <@ r` return the same rows; the operator is the
form the planner can answer from the index.

### Astronomy

Named after the IVOA UDF registry, so ADQL written for other services runs
unchanged:

| function | what it does |
|---|---|
| `ivo_epoch_prop(ra, dec, parallax, pmra, pmdec, rv, ref_epoch, out_epoch)` | the whole six-parameter solution propagated, as `{ra, dec, parallax, pmra, pmdec, rv}` |
| `ivo_epoch_prop_pos(…8 args…)` → `skypos` | just the propagated position |
| `ivo_epoch_prop_pos(ra, dec, pmra, pmdec, ref_epoch, out_epoch)` | the same without parallax or radial velocity |
| `ivo_apply_pm(ra, dec, pmra, pmdec, epdiff)` | proper motion over `epdiff` years |
| `skycell_pm_margin(pmra, pmdec, dt)` | how far that motion can carry a source, in degrees |
| `icrs2gal`, `gal2icrs`, `icrs2ecl`, `ecl2icrs` | frame conversions on a `skypos` |
| `ivo_healpix_index(order, ra, dec)`, `ivo_healpix_index(order, skypos)` | HEALPix index, nested |
| `ivo_healpix_center(order, index)` → `skypos` | the cell's centre |

Epoch propagation is the rigorous Hipparcos/Gaia formulation (ESA SP-1200
§1.5.5): the star travels a straight line in space, so parallax, proper motion
and radial velocity all evolve, and propagating a solution forward and back
returns the starting position to below a µas.

**Proper motion in a cone search.** An index on catalogue positions cannot
know where a star moved to, so widen the search and filter exactly:

```sql
SELECT * FROM cat
WHERE pos <@ circle('ICRS', :ra, :dec, :r + skycell_pm_margin(:max_pmra, :max_pmdec, :dt))
  AND ivo_epoch_prop_pos(ra, dec, parallax, pmra, pmdec, rv, 2016, :epoch) <@ circle('ICRS', :ra, :dec, :r);
```

The first condition is the index scan, the second is exact.

### Coverings, cross-matches and stored regions

| function | purpose |
|---|---|
| `skycell_ang2cell(ra, dec)`, `skycell_cell(skypos)` | the order-29 key |
| `skycell_ang2pix(order, ra, dec)` | the key at any order |
| `skycell_cone(cell, ra, dec, ra0, dec0, radius)` | the low-level indexable cone predicate |
| `skycell_poly(cell, ra, dec, float8[])` | the same for a convex polygon |
| `skycell_cone_ranges(ra0, dec0, radius, tbl, col)` | a covering as rows, for `LATERAL` cross-matches |
| `skycell_cover_info(ra0, dec0, radius, tbl, col)` | ranges, steps, depth, expected rows, area ratio |
| `skycell_cone_moc(ra0, dec0, r, max_cells)`, `skycell_poly_moc(…)` | a region as NUNIQ cells |
| `skycell_ancestors(cell, min_order, max_order)` | a point's ancestor cells, for footprint lookups |
| `skycell_nuniq_lo/hi/order(nuniq)` | NUNIQ helpers |

Cross-matching an uploaded table is fastest through the ranges function, which
gives each probe its own covering and plain B-tree range scans:

```sql
SELECT p.id, c.id
FROM probe p
CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, 1/3600.0, 'cat') g
JOIN cat c ON c.cell BETWEEN g.lo AND g.hi
WHERE skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 1/3600.0);
```

Storing footprints:

```sql
CREATE TABLE fp_cells AS
SELECT fid, unnest(skycell_cone_moc(ra0, dec0, r, 8)) AS nuniq FROM footprint;
CREATE INDEX ON fp_cells (nuniq);

SELECT f.* FROM probe p
JOIN fp_cells fc ON fc.nuniq = ANY (skycell_ancestors(p.cell, 4, 11))
JOIN footprint f ON f.fid = fc.fid
WHERE skycell_in_cone(p.ra, p.dec, f.ra0, f.dec0, f.r);
```

### Settings

| GUC | default | meaning |
|---|---|---|
| `skycell.range_cost` | 30 | price of one extra index range, in rows |
| `skycell.max_ranges` | 64 | cap on ranges per covering |
| `skycell.max_area_ratio` | 64 | a partly covered cell may not exceed this × the region's area |
| `skycell.use_stats` | on | use the histogram as a density map |
| `skycell.cache_coverings` | on | memoise coverings per backend |
| `skycell.join_slots` | 4 | range slots emitted for a non-constant region |
| `skycell.split_cost`, `skycell.max_steps` | 1, 4000 | refinement guards for polygons and very large cones |

---

## Comparison with Q3C and pgSphere

10M-source synthetic catalogue with Gaia-like crowding (uniform background, a
Galactic disk and bulge, 200 dense clusters), PostgreSQL 18.6, warm cache, one
core. Full tables in [`bench/results/summary.md`](bench/results/summary.md);
reproduce with `bench/run.sh`.

| workload | Q3C 2.0.5 | pgSphere 1.5.2 | skycell |
|---|---|---|---|
| index size / build | 214 MB / 1.8 s | 683 MB / 44 s | **214 MB / 2.3 s** |
| cone 1″ / 1′, median | 1.330 / 1.366 ms | 0.035 / 0.032 ms | **0.024 / 0.024 ms** |
| cone 30′ / 1° / 3°, median | 1.37 / 1.43 / 3.02 ms | 0.095 / 0.315 / 1.010 ms | **0.075 / 0.139 / 0.573 ms** |
| buffers touched, 1° / 3° | 329 / 649 | 154 / 617 | **55 / 343** |
| convex polygons, median | 1.53 ms | 0.63 ms | **0.14 ms** |
| cross-match 200k probes, 1″ | 1.95 s | 2.85 s | **0.47 s** |
| cross-match 200k probes, 10″ | 1.72 s | 3.34 s | **0.59 s** |
| 200k points in 20k footprints | not supported | 1.10 s, 1.2 MB | **0.67 s**, 10 MB |
| planner row-estimate error, 1° | 1.9× | 2.8× | **1.22×** |

Measured **interleaved and warm**, alternating the two methods per query with
skycell going first (the order that disadvantages it), cone searches come out
at 0.85 / 1.04 / 0.92 / 0.72 / 0.61 × pgSphere's time at 1″ / 1′ / 30′ / 1° /
3°. Read the table above as the optimistic end and these as the conservative
one.

What the numbers say:

- **Q3C** expands every `q3c_radial_query` into 100 B-tree ranges whatever the
  radius — about 300 buffers and 1.2 ms of planning — which dominates small
  cone searches. Its cross-match path (4 ranges per probe) is much better.
- **Cross-matching** is won on plan shape rather than geometry: `q3c_join` and
  skycell's own join form both run a BitmapOr of bitmap scans per probe, while
  `LATERAL skycell_cone_ranges()` runs plain range scans with as many ranges as
  the probe needs.
- **Where skycell loses.** On a 500k-row ObsCore table inside a real TAP
  service it is 1.2–1.45× *slower* per cone query than pgSphere: planning a
  covering is work pgSphere does not do, and on a relation that small there is
  no scan to win it back. The crossover in these measurements is a few million
  rows.
- **End to end, in a TAP server**, none of this moved the published numbers:
  in an A/B against [egernia](https://github.com/ska-telescope/egernia) on a
  500k-row corpus, all 18 comparison cells tied, because the database is 1–3%
  of a request whose cost is dominated by Python. Protocol and full results:
  [`bench/tap-ab/`](bench/tap-ab/).

The method, the validation and all of these measurements are written up in
[`paper/`](paper/) (Astronomy & Astrophysics format; `make -C paper` builds a
readable PDF without the journal's class, `make -C paper aa` with it).

---

## Testing

Correctness is checked by brute force, not by inspection:

```bash
make -C ext selftest       # geometry, no PostgreSQL needed
docker exec -w /work/ext skycell-pg su postgres -c "make installcheck"
```

- `ext/test/healpix_selftest.c` — known cell centres, round trips at all 30
  orders, nesting, equal area, and the corner bound the covering relies on,
  over 1.6M sampled points including poles, the zone boundary and face edges.
- `ext/test/cover_selftest.c` — coverings against a 2M-point clustered
  catalogue, plus 30,000 adversarial cones with 200 points sampled inside
  each. Asserts **zero false negatives**.
- `ext/test/sql/skycell.sql` — indexed and sequential-exact results must be
  identical for 240 cones in two configurations, 60 polygons, both join forms
  and MOC footprint lookups.
- `ext/test/sql/adql.sql` — the ADQL surface: operator and function forms
  agree over 120 random regions, the rewrite produces index plans (and does
  *not* when the region is not constant), epoch propagation round-trips below
  a µas, and frame conversions land the Galactic centre at l = b = 0.

---

## Limitations

- Polygons must be convex, and there is no `REGION`, union of regions or
  non-convex support.
- No KNN ordering yet (`ORDER BY pos <-> point(…) LIMIT k` is not indexed).
- The density model reads the histogram of a plain column or expression
  index; it is a coarse synopsis, and a dense cluster split by the
  space-filling curve can be underestimated — `max_area_ratio` bounds the
  damage rather than fixing it. A custom `typanalyze` storing a multi-order
  density map would.
- Planning a covering costs 0.01–0.12 ms, which is the reason small relations
  favour pgSphere. Removing it means not planning ranges at all: an SP-GiST
  operator class over the cell hierarchy, where one index qual is planned and
  the covering happens during the descent.
- Tested to 10M rows on one machine. Nothing has run on real Gaia data, at
  Gaia density, or with cold caches.

## License

MIT. See [LICENSE](LICENSE).
