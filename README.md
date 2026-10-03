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

Its cost is paid per query and its benefit per row scanned, so it wins where
enough rows are scanned to amortise a covering: **degree-scale regions
(1.6–3.7× faster), polygons (2.7×), cross-matches (6–11×), cold caches** — with
an index a third the size of pgSphere's that builds more than 10× faster and
row estimates 1.2× off instead of 2–3×. For small warm cone searches it is a
wash at 10M rows and **7–13% slower at 50M**. Measured in randomized paired
trials on two corpora, one of them the real Gaia DR3 density field; the numbers,
including where skycell loses, are in
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

**3. Cells are tested by their own geometry — but only where that is sound.**
Of the three possible verdicts on a cell, only *outside* can lose rows: inside
and straddling both keep it, and the exact predicate re-tests every row the
index returns. So *outside* is decided by a bound —
`min(max_pixrad(order), max corner distance)`, plus an explicit 2e-13 rad
margin — while *inside* may use the cell's own corners and edge midpoints
tightly, since being optimistic there costs false positives and never rows.

(An earlier version also decided *outside* from the corner chords, inflated by
4× the edge's departure from its chord at the midpoint. That is not a bound:
where an edge crosses its chord plane near the midpoint the estimate collapses,
and the true departure reaches 64× it. The error was ~1e-8 rad — below a
0.4 mas leaf cell, and invisible to every test — but it was an assumption
standing where a guarantee was claimed. Removing it cost 2.5% more index
ranges.)

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

### Upgrading an existing database

`make install` only copies files into PostgreSQL's share directory — databases
that already have the extension keep the version they were created with until
they are told otherwise, so a newly installed version is invisible to them:

```bash
make upgrade                     # every database the connection can see
make upgrade PGDATABASE=mydb     # just one
psql -d mydb -c "ALTER EXTENSION skycell UPDATE"   # or do it by hand
```

`make upgrade` reports what it finds and skips databases that are already
current; connection settings come from the usual `PG*` variables. It is a
separate step on purpose — `make install` may run with no server up, and
issuing DDL against live databases as a side effect of copying files is not
something an install should do.

With Docker, including Q3C and pgSphere for comparison:

```bash
docker build -t skycell-pg:18 docker/
docker run -d --name skycell-pg -e POSTGRES_PASSWORD=skycell -e POSTGRES_DB=skycell \
  -v "$PWD":/work skycell-pg:18
docker exec -w /work/ext skycell-pg make with_llvm=no install
```

### Translating ADQL

[`adql_parser/`](adql_parser/) holds reference translators in Python and Java
that turn ADQL geometry into SQL — for skycell, or for the pgSphere+Q3C pair, so
you can compare the two on your own queries:

```bash
python3 adql_parser/python/skycell_adql.py \
  "SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 0.5)) = 1"
# SELECT * FROM t WHERE (point('ICRS', ra, dec) <@ circle('ICRS', 10, 20, 0.5))
```

Both ADQL 2.0 and 2.1 are accepted; the two implementations are checked to
produce identical output.

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

### Setting up a table

What you index depends on how the positions arrive. In all three cases the
index is an ordinary B-tree on a 64-bit key, and `ANALYZE` afterwards is not
optional — the covering reads that histogram as its density map.

**1. A catalogue with `ra`/`dec` columns.** The common case. Index the
expression; no schema change is needed:

```sql
CREATE INDEX cat_cell_idx ON cat (skycell_ang2cell(ra, dec));
ALTER INDEX cat_cell_idx ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE cat;
```

**2. A catalogue you can reorganise.** Storing the key and sorting the heap by
it is worth it for a large table: neighbouring sky lands on neighbouring pages,
so a cone reads fewer of them. This is what the benchmark tables use:

```sql
CREATE TABLE cat AS
  SELECT id, ra, dec, mag, skycell_ang2cell(ra, dec) AS cell
  FROM staging ORDER BY 5;                      -- heap in key order
CREATE INDEX ON cat (cell);
ALTER TABLE cat ALTER COLUMN cell SET STATISTICS 1000;
ANALYZE cat;
VACUUM (FREEZE) cat;
```

**3. Regions rather than positions** (observation footprints, tiles, MOCs).
Store each region's covering as MOC cells, one row per cell, and index those:

```sql
CREATE TABLE fp_cells AS
  SELECT fid, unnest(skycell_cone_moc(ra0, dec0, radius, 8)) AS nuniq
  FROM footprint;                               -- 8 cells per region
CREATE INDEX ON fp_cells (nuniq);
ANALYZE fp_cells;
```

A position's candidate regions are then those whose cell is one of its
ancestors — a handful of equality lookups, followed by the exact test:

```sql
SELECT f.* FROM probe p
JOIN fp_cells fc ON fc.nuniq = ANY (skycell_ancestors(p.cell, 4, 11))
JOIN footprint f ON f.fid = fc.fid
WHERE skycell_in_cone(p.ra, p.dec, f.ra0, f.dec0, f.radius);
```

**Settings worth knowing.** The statistics target governs how finely the
density map resolves: 1000 is a good default for 10⁷ rows and above, the
PostgreSQL default of 100 is coarse for a crowded sky. Nothing else needs
setting — `range_cost` derives itself from the relation (see
[Settings](#settings)).

**After bulk ingestion**, re-run `ANALYZE`: the covering is computed from the
statistics, so a table whose density structure has changed since the last one
is covered for the sky it used to have.

**Checking it worked.** The plan should show an index scan and a
`skycell_in_*` filter:

```sql
EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM cat WHERE point('ICRS', ra, dec) <@ circle('ICRS', 266.4, -28.9, 0.05);
```

If you get a sequential scan, the usual causes are a missing `ANALYZE`, a
region that is not a constant (the rewrite needs constant geometry — use
`skycell_cone_ranges()` in a `LATERAL` for per-row regions), or the index being
on a different expression than the query's position argument.

## Operators and functions

Every function skycell installs, one by one. Types are `skypos` (a position) and
`skyregion` (a cone, box or convex polygon). The `coordsys` argument is accepted
for ADQL compatibility and must be `'ICRS'`; convert other frames first with
`gal2icrs()` / `ecl2icrs()`.

### Geometry — ADQL's own spellings

| Function | Returns | Notes |
|---|---|---|
| `point(ra, dec)`<br>`point(coordsys, ra, dec)` | `skypos` | degrees. ADQL `POINT` |
| `circle(ra, dec, radius)`<br>`circle(coordsys, ra, dec, radius)` | `skyregion` | radius in degrees. ADQL `CIRCLE` |
| `box(ra, dec, width, height)`<br>`box(coordsys, …)` | `skyregion` | centre and extent, as ADQL defines it. Errors if it reaches a pole — use a polygon there. ADQL `BOX` |
| `polygon(ra1, dec1, ra2, dec2, …)`<br>`polygon(coordsys, …)` | `skyregion` | ≥ 3 vertices, **convex**, smaller than a hemisphere. ADQL `POLYGON` |
| `contains(p skypos, r skyregion)`<br>`contains(a skyregion, b skyregion)` | `integer` (1/0) | ADQL `CONTAINS`. For an *indexable* test use the `<@` operator below |
| `intersects(p skypos, r skyregion)`<br>`intersects(a skyregion, b skyregion)` | `integer` (1/0) | ADQL `INTERSECTS`. For an *indexable* test use `<@` (point-region) or `&&` (region-region) below |
| `distance(a skypos, b skypos)`<br>`distance(ra1, dec1, ra2, dec2)` | `float8` | degrees. ADQL `DISTANCE`, both forms |
| `area(r skyregion)` | `float8` | square degrees. ADQL `AREA` |
| `coord1(p)` / `coord2(p)` | `float8` | right ascension / declination, degrees |
| `centroid(r skyregion)` | `skypos` | ADQL `CENTROID` |

If a bare name collides with `pg_catalog` (`point`, `circle`, `box`, `polygon`
all exist there), either install into a schema that precedes it in
`search_path`, or use the prefixed aliases: `skycell_point`, `skycell_circle`,
`skycell_box`, `skycell_polygon`.

### Operators — the indexable spelling

| Operator | Meaning |
|---|---|
| `skypos <@ skyregion` | position inside region — **the one the planner rewrites into index ranges**, against a constant region and an indexed point catalogue |
| `skyregion @> skypos` | the same, reversed — the same rewrite, or, when `skyregion` is itself a stored column with a GiST index on it, indexed that way instead (the region side need not be constant then); with a GIN index on `skycell_region_moc(region)` instead (or as well), rewritten into that array-overlap test automatically — see below |
| `skyregion && skyregion` | regions overlap — indexed by a plain `CREATE INDEX ON t USING gist (region)` on a `skyregion` column, no side table and no manual recipe needed (new; see the caveat below) |
| `skyregion @> skyregion` | region wholly contains region — indexed the same way, either spelling (new; see the caveat below) |
| `skyregion <@ skyregion` | the same, reversed — the same index, either spelling (new; see the caveat below) |

`<@`/`@>` against a constant region and a plain `ra`/`dec` (or `skypos`) catalogue
go through the B-tree cell rewrite described above. A `skyregion` column with its
own GiST index additionally indexes `&&` and both region-region containment
operators (`@>`/`<@`, in either argument order — they're commutators of each
other) directly against it, region side non-constant included — what a stored
footprint column (an ObsCore `s_region`, say) should carry. For a per-row region
cross-matched against a plain point catalogue with no `skyregion` column of its
own, use `skycell_cone()` or `skycell_cone_ranges()` instead.

**Two alternative index types for a `skypos` column itself**, instead of the
B-tree cell rewrite above, exist as opt-in opclasses — competitors to pgSphere's
native `spoint` GiST, not to the rewrite path, which stays the default:

```sql
CREATE INDEX ON t USING spgist (pos);                       -- skypos_spgist_ops, DEFAULT for spgist
CREATE INDEX ON t USING gist (pos skypos_cap_gist_ops);      -- opt-in
```

`skypos_spgist_ops` (`ext/src/spgist_region.c`) is a real SP-GiST descent of
the HEALPix NESTED pixel hierarchy, exact (recheck always false). `skypos_
cap_gist_ops` (`ext/src/gist_point_cap.c`) is a GiST opclass with a single
spherical-cap key per entry — leaves store the point exactly (recheck false
there too), internal nodes a bounding cap. Measured against pgSphere's native
GiST and against each other: `skypos_cap_gist_ops` beats `skypos_spgist_ops`
at every radius tested, and beats pgSphere on buffer counts from roughly 30
arcminutes on, but loses to pgSphere on median wall-clock at every radius
(the buffer-count win doesn't survive contact with pgSphere's cheaper native
per-candidate test below ~1 degree). Both are **EXPERIMENTAL**: correctness-
verified (brute-force comparisons in `ext/test/`), far less scrutiny than the
rewrite path's own years of hardening, and neither is a drop-in win over it —
see `GIST_REGION_DESIGN.md`'s "Round thirty-eight" onward and "Round
forty-three" for the numbers before relying on either for anything beyond
experimentation.

The `skyregion` GiST opclass is new and not yet stress-tested under concurrent
writes. Region-region `@>`/`<@` reuse `&&`'s own pruning test rather than a
tighter one purpose-built for containment (see `ext/src/gist_region.c`'s "round
six" for why a tighter test would risk silently dropping a true match): they
prune every row whose footprint doesn't even touch the query region, same as
`&&`, and let the exact test decide the rest, so they're sound but not as
selective as `&&` itself is for its own predicate. In exchange, unlike `@>`
(point) versus `<@` (point), which were already commutators from the start,
region-region `@>` predates `<@` by several versions and had no commutator of
its own until `<@` was added — both directions are indexed now, and either
spelling reaches the index regardless of which one you write.

**For a `skyregion` column you know holds small, catalogue-scale footprints**
(arcseconds to a few degrees — the common case for a source's own
footprint, as opposed to a survey tile or an all-sky query region), a
second, opt-in opclass is worth pointing at instead:

```sql
CREATE INDEX ON t USING gist (region skyregion_box_gist_ops);
```

`skyregion_box_gist_ops` (`ext/src/gist_region_box.c`) is a plain
axis-aligned 3D box key, no spherical caps — smaller and faster than the
default multi-cap opclass above at that scale (roughly a third of the
index size, and a clear win on `&&` and both containment strategies,
closing a gap to pgSphere the default opclass doesn't close), because a
box is cheaper to test per candidate than a multi-cap key is. That
advantage is real but not universal: it reverses once regions get large
— a circle's *multi-cap* key is the circle itself, exact at any radius,
while a box's looseness around a circle grows through the medium-to-large
radius range and doesn't recover until close to a hemisphere. Past
roughly 20° radius the default opclass wins back. So this is a deliberate
choice to make per column based on what it actually stores, not a
drop-in replacement — see `GIST_REGION_DESIGN.md`'s "Round forty-two"
(and its two addenda, on pole proximity and on large radii) for the
numbers behind both directions of that tradeoff. Same EXPERIMENTAL
caveat as the default opclass above applies, with less scrutiny behind
it (this opclass is new; the default one has had forty-plus rounds of
hardening).

Both opclasses can also coexist on the same column — `CREATE INDEX` both,
and PostgreSQL's ordinary cost-based planner picks between them per query,
the same way it already does for `skypos_spgist_ops` and `skypos_cap_gist_ops`
on a point column. That needs the region-region `&&`/`@>`/`<@` operators'
selectivity estimates to actually reflect the query's own region size,
which they didn't until `skycell` 0.16 (they used PostgreSQL's generic,
radius-blind `areasel`/`contsel` defaults until then — see
`GIST_REGION_DESIGN.md`'s "Round forty-six"); on 0.16 or later, letting
both opclasses compete per query is a real option, not just a per-column
choice.

For "which of my regions contain this point" specifically, a plain
`CREATE INDEX ON t USING gin (skycell_region_moc(region))` — an ordinary
PostgreSQL GIN index over the array `skycell_region_moc()` already returns, no
custom opclass — is usually the better choice over the GiST opclass above: the
planner rewrites `region @> pos`/`pos <@ region` into that index's own
array-overlap test automatically whenever the GIN index exists (`ext/src/adql.c`,
`region_support_simplify`), and it measures faster than the GiST strategy at
both benchmarked scales, matching or slightly beating pgSphere's own native
`<@` (see `GIST_REGION_DESIGN.md`'s "Round eight"). By default it caps the
lookup at whatever `max_order` the index itself was built with (safe: no
row's covering can use a finer cell than that); passing a smaller `max_order`
to `skycell_region_moc()` at index-creation time, if you know your own
corpus doesn't need the full range, narrows it further and gets closer
still. When both a GiST and this GIN index exist on the same column, the
GIN rewrite always takes over — deliberately, not a bug to report.

### The index key

| Function | Returns | Notes |
|---|---|---|
| `skycell_ang2cell(ra, dec)` | `int8` | the order-29 HEALPix cell — **index this** |
| `skycell_cell(p skypos)` | `int8` | same, from a `skypos` column |
| `skycell_ang2pix(order, ra, dec)` | `int8` | the cell at any order |
| `skycell_cell_corners(order, pix)` | `float8[]` | the four corners, as ra, dec, … |

### Indexable predicates (what the rewrite emits, and what you write by hand)

| Function | Notes |
|---|---|
| `skycell_cone(cell, ra, dec, ra0, dec0, radius)` | cone test; with a constant cone the planner turns it into index ranges, with a per-row cone into range slots (a cross-match) |
| `skycell_poly(cell, ra, dec, poly float8[])` | the same for a convex polygon |
| `skycell_radial_query(ra, dec, ra0, dec0, radius)` | Q3C's spelling of the cone test — no cell argument, the planner synthesises it |
| `skycell_join(ra1, dec1, ra2, dec2, radius)` | Q3C's spelling of the cross-match — likewise |
| `skycell_in_cone(ra, dec, ra0, dec0, radius [, sel])` | the exact test alone, no index |
| `skycell_in_poly(ra, dec, poly [, sel])` | likewise |
| `skycell_in_region(p skypos, r skyregion [, sel])` | likewise, on the types |

### Coming from Q3C

`skycell_join` and `skycell_radial_query` take Q3C's own argument lists, so
migrating is a prefix replacement — you do not pass the cell expression, the
planner synthesises it:

```sql
-- cross-match
WHERE q3c_join(a.ra, a.dec, b.ra, b.dec, 1./3600)
WHERE skycell_join(a.ra, a.dec, b.ra, b.dec, 1./3600)

-- cone search
WHERE q3c_radial_query(ra, dec, 266.4, -29.0, 0.5)
WHERE skycell_radial_query(ra, dec, 266.4, -29.0, 0.5)
```

| Q3C | skycell | note |
|---|---|---|
| `q3c_ang2ipix(ra, dec)` | `skycell_ang2cell(ra, dec)` | the index key; build the index on this |
| `q3c_radial_query(ra, dec, ra0, dec0, r)` | `skycell_radial_query(...)` | same arguments |
| `q3c_join(ra1, dec1, ra2, dec2, r)` | `skycell_join(...)` | same arguments |
| `q3c_poly_query(ra, dec, poly)` | `skycell_poly(cell, ra, dec, poly)` | no 3-arg form yet — pass the cell |
| `q3c_dist(ra1, dec1, ra2, dec2)` | `skycell_dist(ra1, dec1, ra2, dec2)` | degrees, as in Q3C |

Both need an index on `skycell_ang2cell(ra, dec)` to be rewritten into ranges,
exactly as Q3C needs one on `q3c_ang2ipix(ra, dec)`; without it you get a
correct sequential scan. `skycell_cone(cell, ra, dec, ra0, dec0, radius)`
remains the canonical spelling — one predicate covers both the cone search and
the cross-match, which is why there is no separate join function underneath.

### Coverings, cross-matches and stored regions

| Function | Returns | Notes |
|---|---|---|
| `skycell_cone_ranges(ra0, dec0, radius [, tbl, col])` | `SETOF (lo, hi)` | the covering as index ranges — the `LATERAL` cross-match form. Fastest at sub-arcsecond radii; **6× slower than `skycell_join` at 30″ on clustered targets** (see below) |
| `skycell_poly_ranges(poly [, tbl, col])` | `SETOF (lo, hi)` | the same for a polygon |
| `skycell_cone_moc(ra0, dec0, radius [, max_cells, max_order])` | `int8[]` | the covering as IVOA MOC `NUNIQ` cells — store these to index a region |
| `skycell_poly_moc(poly [, max_cells, max_order])` | `int8[]` | likewise |
| `skycell_ancestors(cell [, min_order, max_order])` | `int8[]` | a position's containing cells — join against stored MOC cells |
| `skycell_nuniq_order/lo/hi(nuniq)` | `int`/`int8` | decode a `NUNIQ` cell |

### Astrometry — IVOA UDF registry names

| Function | Returns | Notes |
|---|---|---|
| `ivo_epoch_prop(ra, dec, parallax, pmra, pmdec, rv, ref_epoch, out_epoch)` | `float8[6]` | all six parameters propagated, rigorously (ESA SP-1200 §1.5.5): parallax and radial velocity evolve too |
| `ivo_epoch_prop_pos(…8 args…)` | `skypos` | position only |
| `ivo_epoch_prop_pos(ra, dec, pmra, pmdec, ref_epoch, out_epoch)` | `skypos` | without parallax or radial velocity |
| `ivo_apply_pm(ra, dec, pmra, pmdec, epdiff)` | `skypos` | proper motion alone |
| `ivo_healpix_index(order, ra, dec)`<br>`ivo_healpix_index(order, p)` | `int8` | HEALPix cell |
| `ivo_healpix_center(order, hpxindex)` | `skypos` | and back |
| `icrs2gal` / `gal2icrs` / `icrs2ecl` / `ecl2icrs` | `skypos` | frame conversion |

Proper motions are in mas/yr, parallax in mas, radial velocity in km/s, epochs
in years. An index cannot know where a star has moved to, so write a
proper-motion-aware search as an indexable cone widened by the largest motion of
interest, with the exact test on the propagated position:

```sql
SELECT * FROM cat
WHERE point('ICRS', ra, dec) <@ circle('ICRS', 269.45, 4.69, 0.05 + 0.006)
  AND 1 = contains(ivo_epoch_prop_pos(ra, dec, plx, pmra, pmdec, rv, 2016, 2026),
                   circle('ICRS', 269.45, 4.69, 0.05));
```

### Diagnostics

| Function | Notes |
|---|---|
| `skycell_cover_info(ra0, dec0, radius [, tbl, col])` | what the covering looks like: ranges, cells examined, deepest order, expected rows, area ratio, the density used and the order chosen |
| `skycell_range_cost([tbl, col])` | the price of one index range the cost model derives for a relation |
| `skycell_density_build(tbl [, col, rows_per_cell, max_order])` | build a multi-order count map for a relation (**experimental**: no measurement yet shows it helps) |
| `skycell_density_drop(tbl [, col])` | remove it |

### Settings

| GUC | default | meaning |
|---|---|---|
| `skycell.range_cost` | -1 (derive) | price of one index range, in rows; -1 derives it from the relation's rows/page and the planner's cost factors |
| `skycell.max_ranges` | 64 | cap on ranges per covering |
| `skycell.max_area_ratio` | 64 | a partly covered cell may not exceed this × the region's area |
| `skycell.use_stats` | on | use the histogram as a density map |
| `skycell.cache_coverings` | on | memoise coverings per backend |
| `skycell.join_slots` | 4 | range slots emitted for a non-constant region |
| `skycell.exact_cells` | on | tight cell geometry; off falls back to the `max_pixrad` cap |
| `skycell.force_order` | -1 | diagnostics: cover cones at this order (-1 = let the model choose) |
| `skycell.split_cost`, `skycell.max_steps` | 1, 4000 | refinement guards for polygons and very large cones |

---

## Comparison with Q3C and pgSphere

Two 10M-row corpora, PostgreSQL 18.6, one core. The first is synthetic with
Gaia-like crowding; the second is resampled from the **real Gaia DR3 density
field** (source counts in all 3,145,728 order-9 cells, fetched from the ESA
archive), so the crowding structure is not one we invented.

Every query is a *trial*: all three methods answer it back to back in a
**randomized order**, repeated 5×, and the analysis is **paired** (ratio formed
per query, so its difficulty cancels) with 95% bootstrap intervals. Full tables
in [`bench/results-ab/`](bench/results-ab/); reproduce with `bench/run.sh` and
`bench/07_ab.sql`.

**Cone searches, skycell ÷ pgSphere** (below 1 = skycell faster; **bold** =
interval excludes 1):

| radius | designed, warm | designed, cold | Gaia, warm | Gaia, cold |
|---|---|---|---|---|
| 1″ | **0.90** | **0.72** | **0.89** | **0.78** |
| 10″ | **0.93** | **0.88** | **0.90** | **0.80** |
| 1′ | **0.94** | **0.95** | **0.93** | 0.95 |
| 6′ | 1.00 | **0.85** | *1.03* | **0.90** |
| 30′ | 0.98 | **0.68** | **0.92** | **0.57** |
| 1° | 0.92 | **0.57** | **0.85** | **0.53** |
| 3° | **0.63** | **0.39** | **0.69** | **0.40** |

Warm, the advantage is real but modest at small radii and **disappears between
6′ and 1°** (and is a 3% loss at 6′ on the Gaia field). Cold — server restarted,
page cache dropped — skycell wins everywhere, because it reads a third of the
pages. Against Q3C 2.0.5 skycell is 4–50× faster, mostly because that version
expands *every* `q3c_radial_query` into 100 key ranges whatever the radius.

**Does it get better with catalogue size? For big regions yes, for small cones
no.** Repeating the whole protocol at 50M rows — where pgSphere's index (3442 MB)
no longer fits in `shared_buffers` and skycell's (1072 MB) still does:

| radius | warm 10M → 50M | cold 10M → 50M |
|---|---|---|
| 1″ | 0.89 → 0.94 | 0.78 → **0.69** |
| 10″ | 0.90 → 1.00 | 0.80 → 0.90 |
| 1′ | 0.93 → *1.07* | 0.95 → 0.92 |
| 6′ | 1.03 → *1.13* | 0.90 → 0.81 |
| 30′ | 0.92 → **0.78** | 0.57 → **0.49** |
| 1° | 0.85 → **0.64** | 0.53 → **0.27** |
| 3° | 0.69 → **0.59** | 0.40 → **0.24** |

The reason is in the planning column: skycell's plan time **grows with source
density** (6′: 0.019 → 0.031 ms) because a denser sky makes the cost model cut
finer and emit more range arms, while pgSphere's planning stays flat at
0.009 ms. At 6′/50M skycell still *executes* faster (0.042 vs 0.049 ms) and
loses anyway. At 1° the execution gap (0.74 vs 1.26 ms, 153 vs 464 buffers)
dwarfs the planning penalty and widens with scale.

**How tight is the covering?** Planning is only half the story; the other half
is how much sky the covering actually scans. `skycell_cover_info()` reports it
directly, and it is **not monotonic in the radius** — measured on the
20.5M-row ObsCore relation at the galactic-centre density:

| radius | ranges | area scanned / area asked for |
|---|---|---|
| 0.1″ | 1 | 20.6 |
| **0.25–1″** | 2 | **~105** |
| 5–10″ | 1 | 33.8 |
| 50″ | 1 | 10.8 |
| 6′ | 1 | 4.6 |
| 1° | 13 | 1.34 |
| 3° | 24 | 1.25 |
| 30–90° | 64 | ~1.07 |

The worst point is **sub-arcsecond**, where the cost model refuses to cut finer
(the extra ranges would cost more than the area they save) and emits one or two
very coarse cells. That is exactly the cross-match radius, and it explains a
result further down that would otherwise look unmotivated: skycell only reaches
*parity* with `q3c_join` at 1″ rather than beating it, despite the cheaper
index — it is doing ~100× the area work and breaking even anyway. Above about
1° the covering is tight (within 7% of the region) and the advantage is
straightforward.

**Elongated regions — where the gap is widest.** A space-filling curve is
supposed to cover long thin shapes badly, and it does: a 1 deg² strip wastes 8×
at aspect 1 and 218× at aspect 14406. But pgSphere's bounding structure for a
long strip is looser still, so this is skycell's *best* case, not its worst.
Boxes of constant 1 deg² area at the galactic centre, 20.5M-row ObsCore
relation, randomized paired trials, 3 repetitions:

| aspect ratio | pgSphere | skycell | ratio | pgSphere buffers | skycell buffers |
|---|---|---|---|---|---|
| 1 | 3.5 ms | **0.7 ms** | 0.20 | 83 | 183 |
| 16 | 11.8 ms | **1.5 ms** | 0.12 | 354 | 533 |
| 100 | 47.2 ms | **4.5 ms** | 0.10 | 1447 | 1488 |
| 901 | 769 ms | **19.6 ms** | 0.03 | 21691 | **6147** |
| 3593 | 3439 ms | **21.4 ms** | **0.01** | 93136 | **8815** |

Scan tracks, slit spectra and survey stripes are the shapes this matters for.
Note also that cones at the pole are *not* a problem — the covering stays at
3–7 ranges at every declination from 0° to 89.9°, which is the point of an
equal-area scheme.

**So: on a Gaia-sized catalogue this index improves degree-scale selections,
polygons and cross-matches — not small cone searches.** Removing the plan-time
cost (an SP-GiST opclass, or the `= ANY` single-scan form) is what would change
that; making the sub-arcsecond covering tighter is a separate, independent
improvement.

**Everything else:**

| workload | Q3C 2.0.5 | pgSphere 1.5.2 | skycell |
|---|---|---|---|
| index size / build, 10M | 214 MB / 2.2 s | 685 MB / 42 s | **214 MB / 1.4 s** |
| index size / build, 50M | 1072 MB / 22 s | 3442 MB / 269 s | **1072 MB / 23 s** |
| buffers touched, 1° / 3° | 329 / 649 | 154 / 617 | **55 / 343** |
| convex polygons, median | 1.86 ms | 0.77 ms | **0.28 ms** |
| cross-match 200k probes, 1″ (steady state, 50M rows) | **1.23 s** | 2.95 s | **1.08 s** |
| 200k points in 20k footprints | not supported | 0.97 s, 1.1 MB | **0.76 s**, 9.5 MB |
| planner row-estimate error, 1° | 1.9× | 2.8× | **1.22×** |

**At Gaia DR3 scale.** Per-row index cost is flat (0.5% change between 10M
and 50M rows), so it extrapolates. For 1.81×10⁹ sources:

| index | size | build | server RAM to keep it warm |
|---|---|---|---|
| **skycell** | **38 GiB** | 14 min | **64 GiB** |
| Q3C | 38 GiB | 13 min | 64 GiB |
| pgSphere | 122 GiB | 2.7 h | 192 GiB |
| pgSphere + Q3C | 160 GiB | 2.9 h | 256 GiB |

The last row is the honest comparison: pgSphere answers cones and stores
regions, `q3c_join` is what archives use for cross-matching (2.4× faster than
pgSphere at it), so covering both means carrying both indexes. skycell answers
both from one. **That is 4× less memory.**

And on a machine sized between the two — holding skycell's index but not the
pair — the regimes shouldn't be matched: skycell runs warm while pgSphere runs
cold. Measured at 50M rows (ms):

| radius | skycell warm | pgSphere warm | pgSphere cold | warm sky / cold pgS |
|---|---|---|---|---|
| 1″ | 0.042 | 0.047 | 0.528 | **0.08** |
| 6′ | 0.092 | 0.085 | 1.168 | **0.08** |
| 1° | 0.875 | 1.450 | 18.401 | **0.05** |

Matched warm they are within 10%; with the residency the index sizes actually
buy, it is one to two orders of magnitude. Treat that as an **upper bound** —
our cold regime is a fully cold start, while a server that merely can't fit the
index thrashes at some hit rate in between. That in-between case is measured
below.

**The in-between regime, measured.** A container limited to 3 GiB with 768 MB of
`shared_buffers`, holding a 20,485,632-row ObsCore-shaped relation (22 GB heap).
pgSphere's GiST index is 1162 MB (59.5 B/row, 1.51× the pool); skycell's is
439 MB (22.5 B/row, 0.57×). Each index is measured **alone**, with the server
restarted between phases so the pool starts empty and the index not under test
holds zero buffers (checked every phase); both phase orderings were run
(`bench/13_pressure.sql`, raw output in
[`bench/results-pressure/`](bench/results-pressure/)).

| | pgSphere | skycell |
|---|---|---|
| index size | 1162 MB | **439 MB** |
| × buffer pool | 1.51 | **0.57** |
| median query | 10.4 / 13.4 ms | **2.5 / 2.8 ms** |
| buffer pages per query | 1108 | **729** |
| index accesses from disk | 90.6% | **47.4%** |
| index resident | 61 MB (5.2%) | 33 MB (7.4%) |

**4.2–4.8× faster** — between the ~1.0 of the matched-warm case and the
0.05–0.08 of the fully cold one, which is where it should land. Three things are
worth knowing before you expect to reproduce it:

- **The query stream has to sweep the sky before index size matters at all.**
  The stream above is 3500 cones of 2° drawn uniformly, covering the sphere
  about once. 20,000 cones of 0.15° cover 3.4% of it and read essentially
  nothing from disk *at either index size*. What has to fit is not the index,
  it is the part of it your queries touch.
- **Neither index is actually resident here.** The 22 GB heap takes 705–734 MB
  of the 768 MB pool in every phase. This is a smaller index winning a contested
  pool, not one index fitting while the other does not — the clean threshold the
  size arithmetic suggests is harder to reach than the arithmetic implies.
- **Measuring both translations in one interleaved pass halves the gap**, to
  1.6×. They answer the same cone from the same heap pages, so each leaves them
  warm for the other. Run one method per phase.

**Cross-match — Q3C's own speciality.** 200k probes against 50M sources, each
method warmed on its own block before timing, paired over 16 block-repetitions
(`bench/11_xmatch_ab.sql`). Identical row counts from all four:

| method | 1″ | 10″ | vs q3c_join | vs pgSphere |
|---|---|---|---|---|
| `q3c_join` | 1.23 s | 1.32 s | — | **0.42** |
| pgSphere | 2.95 s | 3.22 s | *2.39* | — |
| skycell, join form | 1.50 s | 1.85 s | 1.07 | **0.44** |
| skycell, `LATERAL` | **1.08 s** | **1.43 s** | 0.78 [0.62, 1.00] | **0.30** |

skycell is **level with `q3c_join`** (not faster — the intervals reach 1) and
2.5–3.3× faster than pgSphere. So one index is competitive with each of the two
established ones on the workload each was built for.

**At the radii cross-matching is actually done at, use the `LATERAL` form.**
Optical matching uses 1–1.5″ and radio up to ~5″. On **10M real Gaia DR3
positions**, geometric mean of the paired per-cell ratio over three upload sizes
(10³, 10⁴, 10⁵) × two target distributions (clustered and uniform), randomized
method order, each method warmed, 3 reps (`bench/18_xmatch_sweep.sql`):

| radius | `LATERAL` vs `q3c_join` | slots vs `q3c_join` | `LATERAL` vs pgSphere |
|---|---|---|---|
| 0.5″ | 0.96 | 1.26 | **0.35** |
| 1″ | **0.80** | 1.12 | **0.32** |
| 1.5″ | 1.12 | 1.66 | **0.28** |
| 5″ | 1.08 | 1.29 | **0.35** |
| **all** | **0.98** | 1.32 | **0.33** |

So over 0.5–5″ skycell is **level with `q3c_join`** (0.98, faster in 16 of 24
cells) and **3× faster than pgSphere** (faster in 23 of 24). The `LATERAL` form
is the better of skycell's two here; the fixed-slot form runs 1.32 of
`q3c_join`.

Above that range the `LATERAL` form degrades — at 30″ on clustered targets it
takes 5682 ms against the fixed-slot form's 896 ms. That is an association
radius rather than a cross-match one, so treat it as a bound on the `LATERAL`
form: use the fixed-slot `skycell_join` if you are matching at tens of
arcseconds.

**Fixing it: the `skycell_cell_ops` operator class (v0.7).** The cause is a
missing selectivity estimator, and selectivity comes from the operator, so the
fix is skycell's own comparison operators carrying their own estimator in their
own b-tree operator class. It is **opt-in and needs no migration** — indexes
built the ordinary way keep working:

```sql
CREATE INDEX t_cell ON t (skycell_ang2cell(ra, dec) skycell_cell_ops);

SELECT count(*) FROM probes p
CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, 1/3600.) g
JOIN t ON skycell_ang2cell(t.ra, t.dec) #>= g.lo
      AND skycell_ang2cell(t.ra, t.dec) #<= g.hi
WHERE skycell_in_cone(t.ra, t.dec, p.ra, p.dec, 1/3600.);
```

On the 20.5M-row ObsCore relation, 400 probes at 1″:

| | stock operators | `skycell_cell_ops` |
|---|---|---|
| inner scan estimate | 2,276,921 rows | **1 row** (true value) |
| total plan cost | 617,792,278 | **74.8** |
| plan chosen | Seq Scan on 20.5M rows | Index Scan + Memoize |
| runtime | **>120 s (timeout)** | **50 ms** |

Identical row counts against brute force. The estimator recovers the cone radius
at plan time — from `skycell_cone_bound`'s constant argument in the join form, or
by following the range variable to its function-scan entry in the `LATERAL` form
— and returns √(cap fraction) per side so the pair multiplies to the cone's sky
fraction. Where the bounds *are* constant it defers to the stock histogram
estimator, which does better. Worth it only on wide relations: narrow catalogues
showed no mis-planning in any of 36 cross-match measurements.

> **Caveat — check the plan on wide relations.** Those numbers are from a narrow
> 50M-row catalogue. Repeating the cross-match on the 20.5M-row *ObsCore-shaped*
> relation (23 columns, 22 GB heap), both the `LATERAL` and the join form were
> planned as a **sequential scan** and did not finish inside 90 s; with
> `enable_seqscan = off` the same query runs in **145 ms** for 400 probes. The
> cause is the row estimate: a range join with non-constant bounds
> (`cell BETWEEN r.lo AND r.hi`) has no selectivity estimator, so PostgreSQL
> falls back to ~11% per relation and predicted 2.7×10⁹ rows against a true
> 3642. A wide heap then makes the index path look expensive. Verify with
> `EXPLAIN` before trusting a cross-match plan on a wide table; a selectivity
> estimator for the range-join case is not implemented yet.

Cache residency dominates this measurement and has to be controlled: the same
block query takes 6.2 s cold and 0.32 s warm, so whichever method runs first on
a block pays for the others. An uncontrolled run of ours reported skycell at
2× Q3C — an artefact.

**Does it get better at scale?** Repeated at **50M rows**, where pgSphere's
index (3442 MB) no longer fits in `shared_buffers` (2048 MB) and skycell's
(1072 MB) still does. The answer splits by radius:

| radius | 10M warm | 50M warm | 10M cold | 50M cold |
|---|---|---|---|---|
| 1″ | **0.89** | **0.89** | **0.78** | **0.71** |
| 1′ | **0.93** | 1.02 | 0.95 | 0.95 |
| 6′ | *1.03* | 1.00 | **0.90** | **0.89** |
| 30′ | **0.92** | **0.72** | **0.57** | **0.49** |
| 1° | **0.85** | **0.60** | **0.53** | **0.31** |
| 3° | **0.69** | **0.58** | **0.40** | **0.23** |

For regions ≥30′ the advantage roughly doubles with 5× the rows — at 1° skycell
touches 387 pages against pgSphere's 1848. For cones ≤1′ nothing changes,
because both methods touch 5–8 pages at any table size and what is left is
skycell's ~0.02 ms of planning. **The rule is about pages, not rows.**

**Where skycell loses.** On a 500k-row ObsCore table inside a real TAP service
it is 1.2–1.45× *slower* per query, and all 18 end-to-end cells tie (the
database is 1–3% of a request). Measuring the same ObsCore-shaped relation at
three sizes puts the crossover between 0.5M and 2M rows for degree-scale cones —
and **beyond 10M for small cones on field-clustered data**, where a GiST bitmap
scan already reads exactly the pages it needs:

| class | 0.5M | 2M | 10M | buffers @10M (pgS/sky) |
|---|---|---|---|---|
| 0.15° cone | 1.84 | 1.81 | 2.16 † | 282 / **152** |
| 2° cone | **0.92** | **0.87** | **0.67** | 6052 / **830** |
| 0.5° cone + time/calib cuts | **0.84** | **0.76** | **0.71** | 712 / **182** |
| empty cone | 1.13 | 1.07 | 1.00 | 5 / 6 |

† The small-cone row is **regime-dependent and we don't claim it**: 2.16 measured
cold with disjoint intervals, 0.94 (level) on the same relation fully warm.
The deficit is in execution, not planning, and is *not* the exact predicate —
removing it changes ObsCore query time by ~1%, against 63% on the catalogue.

(An earlier version of this table was measured one method at a time and
reported the 0.5° class as a loss at every size; it is a win at every size.)

**The one parameter, derived not configured.** `range_cost` converts "one more
index range" into "false-positive rows worth avoiding" — a ratio of two costs
PostgreSQL already models. Since 0.4 it defaults to `-1`, meaning:

```
          (ceil(log2 N) + H + 1) * 50 * cpu_operator_cost
  ────────────────────────────────────────────────────────────────
  cpu_tuple_cost + cpu_index_tuple_cost + k*cpu_operator_cost
                 + random_page_cost / (reltuples/relpages)
```

`relpages` sits beside `reltuples` in the same catalogue tuple the density
model already reads. It adapts per relation: **116** for these catalogues
(120 rows/page), **93** for a wider ObsCore row (54 rows/page — each false
positive costs more page traffic, so cut finer), **67** with the stock
`random_page_cost` of 4.0.

Measured honestly, this buys **no speed**: derived (116) against the constant
it replaces (30), both in the same randomized trials, paired per query at 50M
rows, gives ratios 0.93–1.08 with every interval containing 1. The cost curve
is simply flat — at 1° every setting from 3 to 300 lies within 3%. What it
buys is that nobody has to pick the number, and it can't be left at a value
tuned for a different table. `skycell_range_cost('tbl')` reports it.

**The order the model picks** costs about **3%** against one order finer,
measured with the two interleaved per query. Getting that number took three
tries: 0.8–2% was measured through a covering-cache key that omitted the
forced order; 30–48% came from a sweep that visited orders in ascending
sequence, so each finer one ran on a warmer cache. Interleaving removes both.

`skycell.probe_orders` scores candidate orders on the covering they actually
produce instead of on the closed form. It now works — the probe does pick
finer coverings (area ratio 11.5 → 8.9 at 6′) — and query time does not move,
which is what 3% headroom predicts. **Off by default.**

`skycell_density_build()` builds a multi-order count map (leaf cells split
until each holds ≤ N rows) which the covering reads in preference to the
ANALYZE histogram. Built to fix an ObsCore density underestimate that turned
out to be my own diagnostic calling with the wrong statistics column — the
histogram was accurate all along (3701 estimated vs ~4155 scanned).
**Experimental: no measurement yet shows it helps.**

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
- Planning is the one cost that *grows* with source density, so the method's
  disadvantage on small cones gets worse, not better, as a catalogue grows:
  3% at 10M rows, 13% at 50M. An SP-GiST opclass is the fix.
- Tested to 50M rows on one machine, warm and with caches dropped. The Gaia
  corpus uses the real DR3 density field but not real positions, and has no
  structure below its 0.11° map cells.
- Cross-match plans are not robust on wide relations: the range join
  `cell BETWEEN r.lo AND r.hi` has no selectivity estimator, so the planner can
  pick a sequential scan that is orders of magnitude slower than the index path
  it rejects (measured: >90 s against 145 ms). Narrow catalogues are unaffected.
- The memory-residency advantage needs two things the index-size arithmetic
  does not mention: a query stream that sweeps enough sky to touch most of the
  index, and a heap small enough that it does not take the buffer pool by
  itself. Measured at 20M ObsCore rows with 768 MB of shared buffers, the 22 GB
  heap holds ~95% of the pool and *neither* index is resident — the 4.2–4.8×
  is a smaller index winning a contested pool, not one index fitting while the
  other does not.
- Better row estimates are demonstrated; better *plans* are not. On the join
  shapes measured, all three methods chose the same strategy — only one query
  in sixteen at 3° crossed a threshold into a hash join.

## License

MIT. See [LICENSE](LICENSE).
