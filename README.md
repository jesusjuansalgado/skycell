<p align="center">
  <img src="docs/skycell-logo.png" alt="skycell" width="220">
</p>

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

It **beats each established PostgreSQL sky index at its own speciality**:

- **Cone searches, pgSphere's speciality:** faster than pgSphere at every
  radius from 1″ to 3° warm, on every corpus: 17–33% on synthetic catalogues,
  4–31% on real Gaia DR3 positions. Cold it is up to 2.7× faster.
- **Cross-matching, Q3C's speciality:** 20–33% faster than `q3c_join` on both
  resampled and real positions, and 2.9–3.8× faster than pgSphere.

It also answers convex polygons 2.7–4.4× faster than pgSphere. Its index is a
third the size of pgSphere's and builds 11–30× faster, and its row estimates are
1.2× off instead of 2–3×.

The interface follows ADQL. The functions carry ADQL's own names (`CONTAINS`,
`POINT`, `CIRCLE`, `POLYGON`, …), and the indexable `<@` operator is a
one-to-one translation of `1 = CONTAINS(…)`. Stored regions read and write as
IVOA STC-S, and one column can hold circles and polygons together.

These numbers come from randomized paired trials on PostgreSQL 18.6. The
corpora are 10M-row catalogues (synthetic, resampled from the Gaia DR3 density
field, and real DR3 positions) and a 50M-row one. The details are in
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

**5. Cones get a scan node of their own.** The rewrite above hands the planner
one `OR` arm per range, and PostgreSQL builds, estimates and costs an index path
for each — about 10 µs a range, more than computing the covering. So a cone is
instead left as one opaque clause whose selectivity skycell supplies, and a
planner *custom scan* (`SkycellCone` in `EXPLAIN`) walks the covering's ranges
itself: one index scan per range in cell order when the heap follows the cell
order, or every range's TIDs into one bitmap and each heap page read once when it
does not (an ObsCore table clustered by collection, say) — chosen from the
column's correlation statistic, as PostgreSQL chooses between an index and a
bitmap scan. A **cross-match** cone, whose centre comes from another table's row,
gets the same node parameterized by that row on the inner side of a nested loop:
each probe is covered with as many ranges as its cone needs. The planner's row
estimate for such a join is measured, not modelled: at plan time skycell reads 32
probe rows, covers each and counts the catalogue rows its cone really holds,
because a density model cannot know that cross-match targets sit on catalogue
sources (it was 9 rows against 12,390 before; now 12,630).

```
Nested Loop
  ->  Seq Scan on probe p
  ->  Custom Scan (SkycellCone) on cat c
        Filter: skycell_in_cone(ra, "dec", p.ra, p."dec", '0.0002777777777777778'::double precision, '-1'::double precision)
        Index: cat_cell_idx
        Ranges: per outer row
        Mode: ordered
```

**6. Regions are stored as MOC cells.** A footprint is covered by a handful of
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

If you get a sequential scan, the usual causes are a missing `ANALYZE`, the
index being on a different expression than the query's position argument, or —
for `skycell_join` and `skycell_radial_query` — no index on
`skycell_ang2cell(ra, dec)` at all. Per-row regions are indexed too: a per-row
cone (a cross-match) by the custom scan, one covering per outer row, and a
per-row polygon or stored region by the rewrite's run-time range slots.

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
| `skypos <@ skyregion` | position inside region — **the indexable ADQL `CONTAINS`**: against a circle (constant, or built per row as in a cross-match) it is planned exactly as `skycell_cone`, through the custom scan; against a polygon it is rewritten into index ranges |
| `skyregion @> skypos` | the same, reversed — the same rewrite, or, when `skyregion` is itself a stored column with a GiST index on it, indexed that way instead (the region side need not be constant then); with a GIN index on `skycell_region_moc(region)` instead (or as well), rewritten into that array-overlap test automatically — see below |
| `skyregion && skyregion` | regions overlap — indexed by a plain `CREATE INDEX ON t USING gist (region)` on a `skyregion` column, no side table and no manual recipe needed (new; see the caveat below) |
| `skyregion @> skyregion` | region wholly contains region — indexed the same way, either spelling (new; see the caveat below) |
| `skyregion <@ skyregion` | the same, reversed — the same index, either spelling (new; see the caveat below) |

`<@`/`@>` against a circle and a plain `ra`/`dec` (or `skypos`) catalogue go
through the custom scan, like `skycell_cone`, whether the circle is a constant or
comes from another table's row (`JOIN cat c ON point('ICRS', c.ra, c.dec) <@
circle('ICRS', p.ra, p.dec, r)` is a cross-match); against a constant polygon or
box they go through the B-tree cell rewrite described above. A `skyregion` column with its
own GiST index additionally indexes `&&` and both region-region containment
operators (`@>`/`<@`, in either argument order — they're commutators of each
other) directly against it, region side non-constant included — what a stored
footprint column (an ObsCore `s_region`, say) should carry. A per-row region
matched against a plain point catalogue needs no `skyregion` column of its own:
a per-row circle goes to the custom scan and a per-row polygon to the rewrite's
run-time range slots.

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

**`CREATE INDEX ON t USING gist (region)` with no opclass named picks
`skyregion_box4_gist_ops`** (`ext/src/gist_region_box4.c`, DEFAULT as of
`skycell` 0.26) — a plain axis-aligned 3D box key, no spherical caps,
keyed exactly like pgSphere's own `spherekey` (six `float4`s, a fixed
24-byte type, bounds rounded strictly outward on the `float4` cast for
soundness). It beat every other opclass tried — the multi-cap opclass
below, its own `float8` predecessor (`skyregion_box_gist_ops`), and
pgSphere's native GiST — on every corpus measured: isolated single-scale
bands, a mixed circle/polygon corpus, and all four strategies (`&&`,
`@>(region,point)`, `@>(region,region)`, `<@(region,region)`) on the real
50,000-row `fpr` corpus, including `@>(region,point)`, the one strategy
its `float8` predecessor itself loses to pgSphere on. See `GIST_REGION_
DESIGN.md`'s "Round fifty-seven" through "Round sixty-one" (promotion
scrutiny) for the numbers.

**For a `skyregion` column mixing very different region scales in one
index** (a survey-tile table alongside catalogue-scale footprints, say),
a second, opt-in opclass is worth pointing at instead:

```sql
CREATE INDEX ON t USING gist (region skyregion_gist_ops);
```

`skyregion_gist_ops` (`ext/src/gist_region.c`) keys each entry with a
small number of spherical caps rather than a box — a circle's own
multi-cap key is the circle itself, exact at any radius, so it doesn't
share the box opclasses' large-radius looseness (see below) and wins
specifically when a single index must serve both ends of a wide scale
range. That key is larger and costs more per candidate than the box
opclasses' fixed 24/48 bytes, which is why it loses on every single-
scale corpus measured — the box opclass above is the better default
precisely because most columns don't mix scales.

**A third opclass, `skyregion_box_gist_ops`** (`ext/src/gist_region_box.c`)
is the same axis-aligned-box geometry as the default, but `float8` in a
`bytea` (48 bytes) instead of `float4` in a fixed-length type (24 bytes).
It is EXPERIMENTAL and non-default as of 0.26 (it was DEFAULT from 0.21
through 0.25) — superseded by `skyregion_box4_gist_ops`, which beat it on
every corpus measured with no regression found in any regime (round
sixty-one's own promotion scrutiny). Kept available for comparison and as
a fallback; select it explicitly if needed:

```sql
CREATE INDEX ON t USING gist (region skyregion_box_gist_ops);
```

A box opclass's looseness around a circle grows through the medium-to-
large radius range and doesn't recover until close to a hemisphere; past
roughly 20° radius `skyregion_gist_ops` (multi-cap) wins back regardless
of which box opclass it's compared against — see `GIST_REGION_DESIGN.md`'s
"Round forty-two" (and its two addenda, on pole proximity and on large
radii) for the numbers behind that tradeoff, measured originally against
`skyregion_box_gist_ops` and not yet confirmed to reproduce at the exact
same crossover radius for `skyregion_box4_gist_ops` (round fifty-seven
onward found the known round-fifty-four crossover didn't reproduce
through 50° for either box opclass in a fresh, plan-verified
re-measurement — an open discrepancy, not yet resolved, so treat any
specific large-radius crossover claim for either box opclass as unsettled
rather than relying on the round forty-two number).

All three opclasses can also coexist on the same column — `CREATE INDEX`
each — and PostgreSQL's ordinary cost-based planner will pick between
them per query, the same way it already does for `skypos_spgist_ops` and
`skypos_cap_gist_ops` on a point column. That needs the region-region
`&&`/`@>`/`<@` operators' selectivity estimates to actually reflect the
query's own region size, which they didn't until `skycell` 0.16 (they used
PostgreSQL's generic, radius-blind `areasel`/`contsel` defaults until then
— see `GIST_REGION_DESIGN.md`'s "Round forty-six"). **Measured directly**
(`GIST_REGION_DESIGN.md`'s "Round forty-seven"), and 0.16's estimate turned
out to have a real gap of its own: it assumed the *other* side of the
clause was point-like, zero-area, which badly underestimated selectivity
— and led to a severe planner regression — whenever the indexed column's
own stored regions have real area (not the small-catalogue-footprint case
0.16 was scoped for). A `CREATE INDEX ... (area(region_col))` expression
index, when present, lets the planner use a real, data-driven estimate of
the column's typical region size instead of assuming zero — first with a
single representative value (0.17, `GIST_REGION_DESIGN.md`'s "Round
forty-eight"), then, once that left a smaller residual on mixed-scale
columns, averaged properly over the index's whole size distribution
instead of collapsed to one number (0.18, "Round forty-nine"). **As of
0.18**, measured directly on the same corpus that found the original
regression: all four radius bands land on the cheapest available plan
with zero exceptions across 240 probes — no residual left. The one thing
0.18 does not change: the planner's own plain-Index-Scan-vs-Bitmap-Heap-
Scan cost model is still a generic PostgreSQL limitation this extension
cannot override, so a sufficiently adversarial corpus could in principle
still land close enough to that boundary to trip it; `SET enable_indexscan
= off` remains available for anyone who wants a guarantee independent of
estimate quality. Picking one opclass explicitly per column, per the
guidance above, is still the simplest default — but running both
together, with a `CREATE INDEX ... (area(region_col))` alongside them, is
now a genuinely solid option, not just a measured-safe-enough one.

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
| `skycell_cone(cell, ra, dec, ra0, dec0, radius)` | cone test, answered by the custom scan: a constant cone covered at plan time; a per-row cone (a cross-match, `JOIN cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, r)`) covered per outer row; a generic plan or correlated sub-select covered per rescan. With `skycell.custom_scan` off, index ranges and range slots instead |
| `skycell_poly(cell, ra, dec, poly float8[])` | the same for a convex polygon |
| `skycell_radial_query(ra, dec, ra0, dec0, radius)` | Q3C's spelling of the cone test — no cell argument, the planner synthesises `skycell_ang2cell(ra, dec)`; the custom scan answers it through an expression index on that |
| `skycell_join(ra1, dec1, ra2, dec2, radius)` | Q3C's spelling of the cross-match — likewise; the first pair is the indexed side, as in `q3c_join` |
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

Both need an index on `skycell_ang2cell(ra, dec)` to be indexed at all, exactly
as Q3C needs one on `q3c_ang2ipix(ra, dec)`; without it you get a correct
sequential scan. With it they take the custom scan: `skycell_join` is the same
node at the same speed as the `skycell_cone` join (0.96–0.97 of it, not
distinguishable), 27–33% faster than `q3c_join` on the 10M Gaia-density corpus. `skycell_cone(cell, ra, dec, ra0, dec0, radius)`
remains the canonical spelling — one predicate covers both the cone search and
the cross-match, which is why there is no separate join function underneath.

### Coverings, cross-matches and stored regions

| Function | Returns | Notes |
|---|---|---|
| `skycell_cone_ranges(ra0, dec0, radius [, tbl, col])` | `SETOF (lo, hi)` | the covering as index ranges, for hand-built plans — the old `LATERAL` cross-match form. Superseded: the plain join through the custom scan does the same per probe without the function scan and is 8–11% faster |
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
| `skycell.custom_scan` | on | answer `skycell_cone` (and `skycell_join`, `skycell_radial_query`, and ADQL's `point <@ circle`) with the `SkycellCone` scan node instead of rewriting it into B-tree range conditions — constant cones, cross-match cones (a parameterized scan, one covering per outer row) and cones over parameters alone (generic plans, correlated sub-selects). A cone that is not a top-level `AND` term of a `WHERE`/`JOIN ON` (one under an `OR`, say) keeps the rewrite, so it stays indexable. Off restores the rewrite everywhere |
| `skycell.range_cost` | -1 (derive) | price of one index range, in rows; -1 derives it from the relation's rows/page and the planner's cost factors (for the custom scan, also from how much of the relation `shared_buffers` holds) |
| `skycell.max_ranges` | 64 | cap on ranges per covering |
| `skycell.max_area_ratio` | 64 | a partly covered cell may not exceed this × the region's area |
| `skycell.use_stats` | on | use the histogram as a density map |
| `skycell.cache_coverings` | on | memoise coverings per backend |
| `skycell.join_slots` | 4 | range slots the rewrite emits for a non-constant region (used when the custom scan does not apply: `custom_scan` off, polygons and stored regions) |
| `skycell.exact_cells` | on | tight cell geometry; off falls back to the `max_pixrad` cap |
| `skycell.force_order` | -1 | diagnostics: cover cones at this order (-1 = let the model choose) |
| `skycell.split_cost`, `skycell.max_steps` | 1, 4000 | refinement guards for polygons and very large cones |
| `skycell.probe_orders` | 0 (off) | try this many orders past the closed-form choice, keep whichever scores lowest; off by default — real but inconsistent gain once planning time is counted honestly for queries that don't repeat, see `GIST_REGION_DESIGN.md`'s "Round sixty-five" |
| `skycell.probe_split_cost` | 1.0 (inert while probing is off) | cost per cell the order-probe loop charges itself; kept separate from `skycell.split_cost` precisely so tuning this doesn't also detune `split_cost`'s own, unrelated guard — see "Round sixty-five" |
| `skycell.rewrite_max_waste` | 100 | decline the B-tree range rewrite for a constant region when the covering's own cost model expects more than this many false-positive rows, scaled by how cache-resident the relation is; set very high to force the always-rewrite behaviour every version before 0.2x had |
| `skycell.rewrite_waste_scale_cap` | 10 | upper bound, as a multiple of `rewrite_max_waste`, on how far that scaling may go for a relation far bigger than `shared_buffers` |

---

## Comparison with Q3C and pgSphere

Every number below is skycell as installed, with its defaults: cones and
cross-matches go through the `SkycellCone` custom scan, polygons and boxes
through the range rewrite, stored regions through the GiST opclass.

**Protocol.** Every query is a *trial*: all three methods answer it back to
back in a **randomized order**, and the analysis is **paired** (the ratio is
formed per query, so its difficulty cancels), with 95% bootstrap intervals.
Warm means one untimed pass and then five timed repetitions. Cold means the
server is restarted and the page cache dropped before each timed pass. On the
second host a cold page read costs 0.06–0.1 ms (local flash storage); on the
first host's laptop disk it cost about 16 ms, so the two hosts' cold results are
different regimes. Cache
residency has to be controlled: an uncontrolled run of ours once reported
skycell at 2× Q3C on a cross-match, an artefact of which method ran first.

**Corpora.** There are three 10M-row catalogues:

- **designed:** synthetic, with Gaia-like crowding;
- **Gaia:** resampled from the real Gaia DR3 density field (source counts in
  all 3,145,728 order-9 cells), so the crowding structure is not one we
  invented;
- **real:** 10M real Gaia DR3 positions, with every scale of clustering.

There is also a 50M-row resampled catalogue, and an ObsCore-shaped relation in
three sizes.

**Host.** A 4-vCPU x86-64 cloud VM running PostgreSQL 18.6, Q3C 2.0.5 and
pgSphere 1.5.2, all built from source, with the server settings of
[REPRODUCING.md](REPRODUCING.md) §2. Raw results, drivers and reports are in
[`bench/results-pg18/`](bench/results-pg18/). Rows marked *first host* come
from an Apple M3 Pro under Docker
([`bench/results-ab/`](bench/results-ab/)), measured before the custom scan
existed. They cover polygons, regions and index sizes.

### Cone searches — pgSphere's speciality

skycell ÷ pgSphere (below 1 = skycell faster; **bold** = 95% interval excludes 1):

| radius | designed, warm | designed, cold | Gaia, warm | Gaia, cold | real, warm | real, cold |
|---|---|---|---|---|---|---|
| 1″ | **0.75** | **0.85** | **0.70** | **0.79** | **0.83** | **0.87** |
| 10″ | **0.76** | **0.88** | **0.71** | **0.88** | **0.85** | 1.01 |
| 1′ | **0.80** | 0.99 | **0.74** | 0.96 | **0.90** | 0.98 |
| 6′ | **0.83** | **0.86** | **0.80** | **0.86** | **0.96** | **0.61** |
| 30′ | **0.79** | **0.75** | **0.75** | **0.69** | **0.81** | **0.67** |
| 1° | **0.75** | **0.61** | **0.71** | **0.51** | **0.80** | **0.53** |
| 3° | **0.73** | **0.61** | **0.67** | **0.51** | **0.69** | **0.48** |

**Warm, skycell is faster at every radius on all three corpora.** Cold, it is
faster or level everywhere: the level cells are 1′ on all three corpora and
10″ on real positions. At 1″ skycell plans in 0.036 ms (pgSphere: 0.029 ms)
and executes in 0.022 ms against pgSphere's 0.040 ms. At 1° and above it reads
a fraction of pgSphere's pages, so the cold advantage reaches 2×.

On real positions the custom scan plans in at most 0.004 ms more than pgSphere
and executes in 0.52–0.82 of its time. The older range rewrite, run in the
same trials, lost there (1.25–1.56 of pgSphere below 3°) because it planned one
index path per range. Removing that planning cost is what the custom scan is
for.

Against Q3C 2.0.5 skycell is 4–50× faster at cone searches. Most of that gap
is because Q3C 2.0.5 expands *every* `q3c_radial_query` into about 100 bitmap
index scans, whatever the radius, and spends ~2.7 ms planning them.

### Cross-matching — Q3C's speciality

The test runs 200,000 probes against the 10M Gaia-density catalogue. Half of
the probes are catalogue sources displaced by ~0.3″, and half are uniform. The
analysis pairs 16 trials (`bench/11_xmatch_ab.sql`). Every skycell form returns
exactly `q3c_join`'s rows:

| ratio | 1″ | 10″ |
|---|---|---|
| `skycell_join` ÷ `q3c_join` | **0.67** | **0.73** |
| `skycell_cone` join ÷ `q3c_join` | **0.71** | **0.75** |
| `skycell_join` ÷ pgSphere | **0.26** | **0.31** |

**skycell beats `q3c_join` by 25–33% at Q3C's own workload, and pgSphere by
3.2–3.8×.**

On **real Gaia DR3 positions** the sweep covers three upload sizes (10³–10⁵
probes), clustered and uniform targets, and the radii cross-matching is
actually done at. Each figure below is the geometric mean of per-cell ratios
(`bench/18_xmatch_sweep.sql`):

| radius | `skycell_cone` ÷ `q3c_join` | `skycell_join` ÷ `q3c_join` | `skycell_join` ÷ pgSphere |
|---|---|---|---|
| 0.5″ | 0.81 | 0.77 | 0.30 |
| 1″ | 0.77 | 0.76 | 0.30 |
| 1.5″ | 0.77 | 0.78 | 0.34 |
| 5″ | 0.84 | 0.83 | 0.35 |
| **all** | **0.80** | **0.79** | **0.32** |

skycell is faster than both rivals in all 24 cells. For each probe, the scan node walks that probe's own covering:
1.2 ranges and 4.3 buffer pages on average at 1″. The planner's row estimate
for the join is measured rather than modelled. At plan time skycell samples 32
probes, covers each one, and counts the catalogue rows its cone really holds:
12,630 estimated against 12,390 actual.

### Polygons, regions, size

| workload | Q3C 2.0.5 | pgSphere 1.5.2 | skycell |
|---|---|---|---|
| index size / build, 10M | 214 MB / 2.2 s | 685 MB / 42 s | **214 MB / 1.4 s** |
| index size / build, 50M | 1072 MB / 22 s | 3442 MB / 269 s | **1072 MB / 23 s** |
| buffers touched, 1° / 3° cone | 329 / 649 | 154 / 617 | **55 / 343** |
| convex polygons, median | 1.86 ms | 0.77 ms | **0.28 ms** |
| cross-match, 1″, 10M rows, median per 25k-probe block | 226 ms | 595 ms | **153 ms** |
| 200k points in 20k stored footprints | not supported | 0.97 s, 1.1 MB | **0.76 s**, 9.5 MB |
| planner row-estimate error, 1° | 1.9× | 2.8× | **1.22×** |

All rows come from the first host except the cross-match row.

**Elongated regions are skycell's best case.** A space-filling curve covers long
thin shapes badly: a 1 deg² strip wastes 8× at aspect 1 and 218× at aspect
14406. pgSphere's bounding structure for a long strip is looser still. The test
uses boxes of constant 1 deg² area at the galactic centre, on the 20.5M-row
ObsCore relation (first host):

| aspect ratio | pgSphere | skycell | ratio | pgSphere buffers | skycell buffers |
|---|---|---|---|---|---|
| 1 | 3.5 ms | **0.7 ms** | 0.20 | 83 | 183 |
| 16 | 11.8 ms | **1.5 ms** | 0.12 | 354 | 533 |
| 100 | 47.2 ms | **4.5 ms** | 0.10 | 1447 | 1488 |
| 901 | 769 ms | **19.6 ms** | 0.03 | 21691 | **6147** |
| 3593 | 3439 ms | **21.4 ms** | **0.01** | 93136 | **8815** |

This matters for scan tracks, slit spectra and survey stripes. Cones at the
pole are not a problem either: the covering stays at 3–7 ranges at every
declination from 0° to 89.9°, which is the point of an equal-area scheme.

### ObsCore-shaped tables

Observations come in groups of 128 per field, clustered by collection and
field. There are three sizes, built from a stored set of field and query
centres (`bench/10_crossover.sql`, `bench/data/`). The table shows total time,
skycell ÷ pgSphere, with two runs pooled:

| class | 0.5M | 2M | 10M |
|---|---|---|---|
| 0.15° cone | 1.14 | 1.07 | 0.97 |
| 2° cone | 0.92 | 0.93 | 0.75 |
| 0.5° cone + catalogue cuts | 0.94 | 0.99 | 0.80 |
| empty 0.7″ cone | 0.71 | 0.68 | 0.47 |

skycell's lead grows with the table. By 10M rows it is level on the 0.15°
cone and faster on every other class. The one cell pgSphere wins is the small
cone on the smallest table, where its bitmap scan on field-clustered data
already reads exactly the pages it needs.

### At Gaia DR3 scale

Per-row index cost is flat (a 0.5% change between 10M and 50M rows), so it
extrapolates. For 1.81×10⁹ sources:

| index | size | build | server RAM to keep it warm |
|---|---|---|---|
| **skycell** | **38 GiB** | 14 min | **64 GiB** |
| Q3C | 38 GiB | 13 min | 64 GiB |
| pgSphere | 122 GiB | 2.7 h | 192 GiB |
| pgSphere + Q3C | 160 GiB | 2.9 h | 256 GiB |

The last row is the comparison an archive faces today. It runs pgSphere for
cones and stored regions and `q3c_join` for cross-matching, so it carries both
indexes. skycell beats each of them at its own workload from one index, which
needs **4× less memory**.

**The advantage survives scale.** At 50M rows, pgSphere's index (3439 MB) no
longer fits in `shared_buffers` (2 GB), while skycell's (1071 MB, built in
17 s against pgSphere's 387 s) still does:

| radius | warm 10M → 50M | cold 10M → 50M |
|---|---|---|
| 1″ | **0.70** → **0.78** | **0.79** → **0.82** |
| 10″ | **0.71** → **0.78** | **0.88** → **0.86** |
| 1′ | **0.74** → **0.86** | 0.96 → 0.98 |
| 6′ | **0.80** → **0.86** | **0.86** → **0.81** |
| 30′ | **0.75** → **0.75** | **0.69** → **0.53** |
| 1° | **0.71** → **0.69** | **0.51** → **0.37** |
| 3° | **0.67** → **0.73** | **0.51** → **0.42** |

- **Warm**, skycell stays 14–31% faster at every radius.
- **Cold**, its lead for regions of 30′ and above widens to 2.7× at 1°. At 1°
  skycell touches 188 buffer pages against pgSphere's 642.

On a machine sized between the two (holding skycell's index but not the pair),
skycell runs warm while pgSphere runs cold. On the first host, at 50M rows,
that is 0.05–0.08 of pgSphere's time. That is an upper bound. In the measured in-between regime, a 3 GiB
container with 768 MB of `shared_buffers` and a 20.5M-row ObsCore relation,
skycell is **4.2–4.8× faster** (`bench/13_pressure.sql`,
[`bench/results-pressure/`](bench/results-pressure/)). Two conditions matter
for reproducing this:

- The query stream must sweep enough sky to touch most of the index.
- Each method must be measured in its own phase. Interleaving the methods halves
  the gap, because they read the same heap pages.

### The cost model

**The one parameter is derived, not configured.** `range_cost` converts "one
more index range" into "false-positive rows worth avoiding", a ratio of two
costs PostgreSQL already models. Both sides are read from the relation's own
statistics:

- **Range** = the descent into the index, plus the leaf and heap pages a new
  range starts on.
- **False row** = its index entry, its heap tuple, the exact test and its share
  of a heap page.

The custom scan also prices a page by where it is expected to come from,
blending a buffer hit with `random_page_cost` according to how much of the
relation `shared_buffers` holds. Typical values:

| relation | rows per range |
|---|---|
| 10M-row catalogue, warm | ~34 |
| relation far larger than the buffer pool | ~87 |
| range rewrite, these catalogues | ~110–116 |

A wide ObsCore row packs fewer rows to a page, so each false positive costs
more page traffic and the covering is cut finer. Nobody has to pick the number,
and it can't be left at a value tuned for a different table.
`skycell_range_cost('tbl')` reports it. On the first host the derived value
gave the same speed as the constant it replaced: paired at 50M rows, every
ratio was 0.93–1.08 and every interval contained 1. The cost curve is flat:
at 1°, every setting from 3 to 300 lies within 3%.

**The order the model picks** costs about **3%** against one order finer,
measured with the two interleaved per query.

**How tight is the covering?** `skycell_cover_info()` reports it directly. On
the 20.5M-row ObsCore relation at galactic-centre density:

| radius | ranges | area scanned / area asked for |
|---|---|---|
| 0.1″ | 1 | 20.6 |
| 0.25–1″ | 2 | ~105 |
| 5–10″ | 1 | 33.8 |
| 50″ | 1 | 10.8 |
| 6′ | 1 | 4.6 |
| 1° | 13 | 1.34 |
| 3° | 24 | 1.25 |
| 30–90° | 64 | ~1.07 |

At sub-arcsecond radii the model emits one or two coarse cells, because finer
ones would cost more ranges than the area they save. Above about 1° the
covering is within 7% of the region. skycell beats `q3c_join` at 1″ while
scanning ~100× the cone's area, so a tighter sub-arcsecond covering is
headroom still to take.

Two experimental settings are off by default:

- **`skycell.probe_orders`** scores candidate orders on the covering they
  actually produce. It picks tighter coverings, but in total time the gain does
  not survive the extra planning for queries that don't repeat. See
  `GIST_REGION_DESIGN.md`, rounds 63–65.
- **`skycell_density_build()`** builds a multi-order count map, which the
  covering then reads instead of the histogram. No measurement yet shows that
  it helps.

### The `LATERAL` form and `skycell_cell_ops`

The custom scan answers cross-matches written as a join on `skycell_cone` or
`skycell_join`. The older `LATERAL skycell_cone_ranges(…)` form is a range join
with non-constant bounds, and PostgreSQL has no estimator for that. On wide
relations it can choose a sequential scan: one test ran more than 90 s, against
145 ms on the index path. Building the index with the opt-in
`skycell_cell_ops` operator class supplies the estimate:

```sql
CREATE INDEX t_cell ON t (skycell_ang2cell(ra, dec) skycell_cell_ops);
```

On the 20.5M-row ObsCore relation, with 400 probes at 1″, that changes the
inner-scan estimate from 2,276,921 rows to 1, the plan from a sequential scan
(>120 s) to an index scan with Memoize, and the runtime to **50 ms**.

**Measurements of the range rewrite alone** (`skycell.custom_scan = off`) are
in [`bench/results-ab/`](bench/results-ab/) (first host). Every
`bench/results-pg18/` run also measures the rewrite in the same trials, which
isolates the custom scan's contribution.

---

## Testing

Two layers, both in [`ext/test/`](ext/test/).

**Geometry, standalone** — `make selftest` in `ext/`, no PostgreSQL needed
(about two minutes):

- `healpix_selftest`: order-0 pixel centres, the pixel → centre → pixel round
  trip at every order, nesting across orders, every point within `max_pixrad`
  of its pixel's centre (the bound the covering's *outside* test relies on), and
  equal area (χ²/dof 1.14 for uniform points over order-3 pixels).
- `cover_selftest`: brute force on the covering itself. Two million catalogue
  points; for cones from 1″ to 120° and for convex polygons, under a uniform
  and a histogram density model, every point inside the region must have its
  order-29 cell inside one of the ranges. Then the awkward places: 7,266
  polygons at the poles, zone boundaries, RA wrap-around and face edges (869,117
  points inside, half within a part per billion of an edge), and 2 × 30,000 cones
  with 200 points each. It also prints ranges, cells examined, false-positive
  fraction and µs per covering. Current run: **0 false negatives**.

**SQL regression** — `make installcheck` in `ext/`, against a running server:

- `skycell`: every indexed query must return exactly what the exact predicate
  returns on a sequential scan — no false negatives, no false positives — on a
  clustered catalogue with clusters at both poles and across RA = 0: cones,
  polygons, the Q3C-shaped spellings, cross-matches, MOC-stored regions and the
  GiST region operator classes.
- `adql`: the ADQL surface — the standard's spelling, the indexable operator
  form, and the IVOA UDF astronomy functions.
- `cone_scan`: the custom scan. Every query returns the same rows with
  `skycell.custom_scan` on and off, in both of its modes (a heap in cell order
  and one that is not), rescanned in a nested loop, under a generic plan and
  through a scrollable cursor; each placement of a cone (under `OR`, in a view,
  a `JOIN ON`, a sub-select, a target list) gets the plan it should; cross-matches
  (join, per-row radius, `WHERE` join, `LEFT JOIN`, `EXISTS`, expression index,
  correlated nearest-neighbour, `skycell_join`), with a NULL centre, a cone round
  the pole and one across RA = 0, match the rewrite; an invalid centre errors as
  the rewrite does; and the cross-match row estimate is within 3× of the truth.

All three pass on PostgreSQL 16 and 18. The benchmark harness records each
method's row count in every trial too; the phase-grid (`bench/report.py`),
cross-match and crossover reports compare them across methods.

## Limitations

- **Geometry.** Polygons must be convex. There is no `REGION`, no union of
  regions and no non-convex support.
- **No KNN ordering yet.** `ORDER BY pos <-> point(…) LIMIT k` is not indexed.
  A correlated sub-select with `ORDER BY skycell_dist(…) LIMIT 1` over a cone
  is indexed, because the custom scan covers the cone per rescan, so use that
  instead.
- **Where the custom scan applies.** It answers cones and cross-matches only;
  polygons and stored regions use the range rewrite, which is fast for them.
  The cone must be a top-level `AND` term of a `WHERE` or `JOIN ON`. A cone
  under an `OR` is rewritten instead and stays indexable through a `BitmapOr`.
  The custom scan reads heap tables only. On a partitioned table each partition
  keeps the rewrite, because PostgreSQL re-simplifies each partition's copy of
  the condition. It is not parallel-aware, and before PostgreSQL 18 its bitmap
  mode does not prefetch.
- **One host type.** Every custom-scan number comes from one 4-vCPU cloud
  host type, on fast flash storage (0.06–0.1 ms per cold page read), so the
  cold margins are those of fast storage. On a slow disk, where each page
  saved costs more, they would likely be wider. On real positions, small cold cones are level rather than won.
- **Small tables.** On a 0.5M-row field-clustered table, pgSphere still wins
  the small cone (skycell takes 1.14× its time). Inside a TAP service whose own
  code dominates each request, the index choice is immaterial: in our
  measurement the database was 1–3% of a request.
- **Planning is the remaining cost.** A covering is computed at plan time:
  0.036 ms at 1″, against 0.029 ms for pgSphere's whole plan. skycell's win at
  small radii comes from execution.
- **Plan-time sampling for cross-matches.** To estimate a cross-match's rows,
  the planner reads up to 32 probe rows and up to 4000 catalogue rows through
  the index, as PostgreSQL itself reads index endpoints for range estimates.
  That costs about 2–5 ms of planning, more on a cold cache. Sampling applies
  when the probe position is a plain column of one table and the cones are
  small. Otherwise the estimate falls back to the density model, which ignores
  that probes sit on sources and can be orders of magnitude low.
- **The density model is a coarse synopsis.** It reads the histogram of a plain
  column or expression index. A dense cluster split by the space-filling curve
  can be underestimated; `max_area_ratio` bounds the damage rather than fixing
  it.
- **Better estimates, not yet better plans.** Row estimates are better, but on
  the join shapes measured all three methods chose the same plan strategy.

## License

MIT. See [LICENSE](LICENSE).
