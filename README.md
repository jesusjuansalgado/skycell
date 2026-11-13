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

-- a cone search, written as ADQL writes it
SELECT * FROM cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 266.4, -28.9, 0.05));

-- the ten nearest sources
SELECT * FROM cat ORDER BY DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', 266.4, -28.9)) LIMIT 10;
```

It **beats each established PostgreSQL sky index at its own speciality**:

- **Cone searches, pgSphere's speciality:** faster than pgSphere at every
  radius from 1″ to 3° warm, on every corpus: 19–31% on synthetic catalogues,
  5–34% on real Gaia DR3 positions. Cold it is faster or level everywhere, and
  up to 2.5× faster at 50M rows.
- **Cross-matching, Q3C's speciality:** 16–27% faster than `q3c_join`, and
  about 3× faster than pgSphere.

It also answers convex polygons 4.3–4.8× faster than pgSphere, non-convex ones
3.4–7.7× faster, and the k
nearest sources to a point (`ORDER BY DISTANCE(...) LIMIT k`) from the same
index, in about half of pgSphere's time on real Gaia DR3 positions for k up to
100. Its index is a third the size of pgSphere's and builds about 23× faster,
and its row estimates stay within 1.4× of the truth where pgSphere's are off
by up to 7.1×.

The interface is ADQL's. The functions carry ADQL's own names (`CONTAINS`,
`POINT`, `CIRCLE`, `POLYGON`, `DISTANCE`, …), and `1 = CONTAINS(…)` is indexed
as written: the planner turns it into its operator, `point <@ circle`, which
runs through the same scan as skycell's native functions, cross-matches
included, and so does a `DISTANCE(p1, p2) < r` comparison; `ORDER BY
DISTANCE(…)` with a `LIMIT` gets a nearest-neighbour scan of its own. It
works whichever way round the query is written: a constant point inside a
circle around each row is planned as the cone around that point, and a
cross-match probes the larger indexed table whichever side is the point and
whichever the circle. With pgSphere and Q3C the reversed form is a full table
scan, or a cross-match that probes the wrong table. Regions read and write as IVOA STC-S, including
`UNION`, `INTERSECTION`, `DIFFERENCE` and `NOT` of them, and one
column can hold circles, polygons and their combinations together: instrument
footprints with detector gaps and masks, stored as they are and searched with
a plain polygon ([Composite regions](#composite-regions-instrument-footprints)).

The numbers come from randomized paired trials on PostgreSQL 18.6, on 10M-row
catalogues (synthetic, resampled from the Gaia DR3 density field, and real DR3
positions) and a 50M-row one; see [Performance](#performance).

## Documentation

Tutorials, each a sequence of SQL that runs as written on an empty database
(and is checked to, by [`docs/tutorials/check.py`](docs/tutorials/check.py)):

1. [A source catalogue](docs/tutorials/01-source-catalogue.md): installing,
   indexing, cone, polygon and region searches, cross-matches, nearest
   neighbours, and what to check when a plan is a sequential scan.
2. [Regions and footprints](docs/tutorials/02-regions-and-footprints.md): the
   `skyregion` type and STC-S, an ObsCore `s_region` column, the region
   indexes and which to choose, footprints against positions, fields and
   catalogues.
3. [Density maps, counts and MOCs](docs/tutorials/03-density-and-mocs.md):
   counts without reading rows, exact per-cell counts, MOCs out and in, and
   their set operations.
4. [Large catalogues](docs/tutorials/04-scaling-parallel-partitions.md):
   parallel cross-matches and searches, partitioning by sky region, upkeep,
   and reading a plan.
5. [ADQL, TAP services and migration](docs/tutorials/05-adql-tap-and-migration.md):
   what a TAP service needs, the IVOA user-defined functions, and moving from
   Q3C or pgSphere.

The rest of this page is the reference: how skycell works, every function
and setting, and its limits. The measurements are in
[`docs/performance.md`](docs/performance.md) and the checks in
[`docs/testing.md`](docs/testing.md).

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
cone, box, polygon — is covered by cells, and each run of cells becomes
a key range. How finely to cut is a cost question, not a fixed depth:

```
cost(s) = ranges(s) · range_cost  +  ρ · (area covered − area of the region)
```

For cells of size *s* covering a region of radius *r*, the number of ranges
goes as *r/s* and the wasted area as perimeter·*s*. Minimising gives
**s\* = √(α · range_cost / ρ)** — independent of the region's size: the sky's
local density ρ and the price of one extra index range decide how finely it is
worth cutting. skycell evaluates that cost at all 30 orders and takes the
cheapest, so a crowded field is cut finer than empty sky. `range_cost` is
derived per relation from the planner's own cost factors and how much of the
relation `shared_buffers` holds, so there is nothing to tune.

**3. Cells are tested by their own geometry — but only where that is sound.**
Of the three possible verdicts on a cell, only *outside* can lose rows: inside
and straddling both keep it, and the exact predicate re-tests every row the
index returns. So *outside* is decided by a bound —
`min(max_pixrad(order), max corner distance)`, plus an explicit 2e-13 rad
margin — while *inside* may use the cell's own corners and edge midpoints
tightly, since being optimistic there costs false positives and never rows.

**4. Cones and cross-matches get a scan node of their own.** A cone —
`skycell_cone`, Q3C's `skycell_join`/`skycell_radial_query` spellings, or ADQL's
`CONTAINS(POINT, CIRCLE) = 1` and `DISTANCE(…) < r` — is left as one clause
whose selectivity skycell supplies, and a planner *custom scan* (`SkycellCone`
in `EXPLAIN`) walks the covering's ranges itself: one index scan per range in
cell order when the heap follows the cell order, or every range's TIDs into one
bitmap, each heap page read once, when it does not — chosen from the column's
correlation statistic, as PostgreSQL chooses between an index and a bitmap scan.
A **cross-match** cone, whose centre comes from another table's row, gets the
same node parameterized by that row on the inner side of a nested loop, each
probe covered with the ranges its own cone needs. Its row estimate is measured,
not modelled: at plan time skycell covers 32 sampled probes and counts the
catalogue rows their cones really hold (12,630 estimated against 12,390 actual).
A probe's rescan is costed as PostgreSQL costs its own index scan on the inner
side of a nested loop, the pages earlier probes have read counted as cached, so
a cross-match is weighed fairly against a parallel sequential scan. With enough
probes the cross-match runs in parallel workers, as a Gather over the nested
loop with a Parallel Seq Scan of the probes on its outer side, each worker
covering and scanning the cones of its own probes. A single wide cone,
polygon or composite search is parallel aware too: its covering is cut into
chunks that the leader and the workers claim from a shared counter, so a cone
over a crowded field is read by several processes at once. A query ordered by the distance to a point, with a `LIMIT`, gets a
second node, `SkycellNearest`, which reads widening rings round the point from
the same index ([Nearest neighbours](#nearest-neighbours)).

```
Nested Loop
  ->  Seq Scan on probe p
  ->  Custom Scan (SkycellCone) on cat c
        Filter: skycell_in_cone(ra, "dec", p.ra, p."dec", '0.0002777777777777778'::double precision, '-1'::double precision)
        Index: cat_cell_idx
        Ranges: per outer row
        Mode: ordered
```

**5. Polygons, boxes and composites take the same scan.** A constant polygon,
box or composite written as a top-level condition goes to the custom scan, as a
circle does, and so does a boolean of constant regions on one position, folded
into one composite:
covered once at plan time, its ranges read in cell order, each row tested from
its `ra` and `dec`, and split among parallel workers when it is large. Anywhere
else (under an `OR`, per row, or with the custom scan off) a support function
(`SupportRequestSimplify`, the mechanism PostGIS uses for `ST_DWithin`) turns
the predicate into B-tree range conditions plus the exact test before path
generation, so the planner estimates them from the same histogram.

**6. Regions are stored as regions.** `skyregion` holds circles and polygons
in one type, so one column carries both. A `skyregion` column takes its own
GiST index (a `float4` box key, pgSphere's own representation, by default), or
a footprint can be stored as MOC cells in a B-tree, where the footprints
containing a point are those whose cell is one of the point's ancestors.

Nothing in the covering is heuristic about correctness: every cell test is
conservative, so a covering never misses a row, and that is verified by brute
force over millions of points (see [Testing](#testing)).

---

## Installation

Needs PostgreSQL 16 or newer (18 recommended) and its server headers. CI tests
16, 17 and 18.

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

With Docker:

```bash
docker build -t skycell-pg:18 docker/
docker run -d --name skycell-pg -e POSTGRES_PASSWORD=skycell -e POSTGRES_DB=skycell \
  -v "$PWD":/work skycell-pg:18
docker exec -w /work/ext skycell-pg make with_llvm=no install
```

### Translating ADQL geometry

[`adql_geometry/`](adql_geometry/) holds reference translators in Python and Java
that turn the geometry in ADQL into SQL — for skycell, or for the pgSphere+Q3C
pair, so you can compare the two on your own queries. They rewrite only the
geometry (`CONTAINS`, `POINT`, `CIRCLE`, `DISTANCE`, `REGION`, …); the rest of ADQL, such
as `TOP n` → `LIMIT n`, is the job of the TAP service's own ADQL parser:

```bash
python3 adql_geometry/python/skycell_adql_geometry.py \
  "SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 0.5)) = 1"
# SELECT * FROM t WHERE (point('ICRS', ra, dec) <@ circle('ICRS', 10, 20, 0.5))
```

Both ADQL 2.0 and 2.1 are accepted; the two implementations are checked to
produce identical output. For skycell the translation is optional: the
planner indexes `1 = CONTAINS(…)`, `DISTANCE(…) < r` and `ORDER BY DISTANCE(…)`
as ADQL writes them, so a TAP service can pass the geometry through unchanged
([ADQL names](#adql-names)).

### ADQL names

The functions are named as ADQL names them: `POINT`, `CIRCLE`, `BOX`,
`POLYGON`, `REGION`, `CONTAINS`, `INTERSECTS`, `DISTANCE`, `AREA`,
`CENTROID`, `COORD1`, `COORD2`. PostgreSQL has a geometric
`point(float8, float8)` of its own, which a bare `POINT(ra, dec)` finds first
when skycell is installed in `public`; an implicit cast reads that point as the
sky position (ra, dec) in degrees and plans it as skycell's own, so the ADQL
spelling is indexed as written (PostgreSQL's geometry keeps its meaning:
`point <-> point` is still Euclidean). The forms carrying ADQL's
coordinate-system argument, `point('ICRS', ra, dec)` and `circle('ICRS', …)`,
and the prefixed `skycell_point`, `skycell_circle`, `skycell_box` and
`skycell_polygon` are the same functions. Installing into a schema of its own
works too (`CREATE EXTENSION skycell SCHEMA astro`), on the `search_path` or
off it with the functions named `astro.CONTAINS(...)`; the extension cannot be
moved afterwards (`ALTER EXTENSION ... SET SCHEMA`).

Positions and regions compare with `=` and `<>` (a region equals one stored
the same way), so `DISTINCT`, `GROUP BY` and `UNION` work on them. Their text
is exact, as `float8`'s is, so a dump and restore keeps every value, and both
types have a binary form, for `COPY ... (FORMAT binary)` and drivers that read
results in binary.

Through a security-barrier view (a view with `security_barrier`), a search
reaches the index of the table under the view: the region tests are
`LEAKPROOF`, and ADQL's `POINT()` and `CIRCLE()` on the view's columns, or on
an outer query's (a LATERAL or correlated subquery's), are read as
`skycell_point_or_null()` and `skycell_circle_or_null()`, which are too (they
return NULL where the constructors raise an error, so a row with impossible
coordinates matches nothing). Since PostgreSQL 16.16, 17.12 and 18.7 an outer
query's column counts as the view's here: a constructor that is not
`LEAKPROOF` on it would keep the search out of the view. PostgreSQL never pushes a join
condition into such a view, so skycell rewrites a cross-match against one,
`FROM t JOIN v ON CONTAINS(POINT('ICRS', v.ra, v.dec), CIRCLE('ICRS', t.ra,
t.dec, r)) = 1` (or the same condition in `WHERE`, or a `LEFT JOIN` with the
view on the right), into `t CROSS JOIN LATERAL (SELECT * FROM v WHERE ... OFFSET
0) v`, where each row of `t` searches the view's index. Only the conditions
that read the view and another table and call a skycell function move; the
view stays a security barrier, with its permissions checked as before.

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
so a cone reads fewer of them:

```sql
CREATE TABLE cat AS
  SELECT id, ra, dec, mag, skycell_ang2cell(ra, dec) AS cell
  FROM staging ORDER BY 5;                      -- heap in key order
CREATE INDEX ON cat (cell);
ALTER TABLE cat ALTER COLUMN cell SET STATISTICS 1000;
ANALYZE cat;
VACUUM (FREEZE) cat;
```

`skycell_cone(cell, ra, dec, ...)` names the column itself. The spellings that
do not — ADQL's `contains()` and `distance()`, `skycell_radial_query`,
`skycell_join` and the [nearest-neighbour scan](#nearest-neighbours) — work
from `ra` and `dec`, and use the column only when the planner is told what it
holds. Either declaration does it, and PostgreSQL then enforces it:

```sql
-- a new table: the column computed by PostgreSQL itself
CREATE TABLE cat (id bigint, ra float8, dec float8, mag float4,
                  cell int8 GENERATED ALWAYS AS (skycell_ang2cell(ra, dec)) STORED);
INSERT INTO cat (id, ra, dec, mag)
  SELECT id, ra, dec, mag FROM staging ORDER BY skycell_ang2cell(ra, dec);

-- an existing table: a constraint, checked once (no rewrite, the heap stays
-- in order); NOT NULL as well, since a CHECK lets a NULL through
ALTER TABLE cat ALTER COLUMN cell SET NOT NULL;
ALTER TABLE cat ADD CONSTRAINT cat_cell CHECK (cell = skycell_ang2cell(ra, dec)) NOT VALID;
ALTER TABLE cat VALIDATE CONSTRAINT cat_cell;
```

A row without a position needs a `cell` value under the `NOT NULL`, such as 0;
the constraint allows it, and no cone or distance selects the row. The same
works for a `skypos` column with `skycell_cell(pos)`. A constraint not
validated, or (PostgreSQL 18) not enforced, does not count.

**3. Regions rather than positions** (observation footprints, tiles, MOCs).
Store them in a `skyregion` column with a GiST index:
[Regions and footprints](docs/tutorials/02-regions-and-footprints.md).

**Settings worth knowing.** The statistics target governs how finely the
density map resolves: 1000 is a good default for 10⁷ rows and above, the
PostgreSQL default of 100 is coarse for a crowded sky. Nothing else needs
setting — `range_cost` derives itself from the relation (see
[Settings](#settings)).

**After bulk ingestion**, re-run `ANALYZE`: the covering is computed from the
statistics, so a table whose density structure has changed since the last one
is covered for the sky it used to have.

**Checking it worked.** A cone, polygon, box or composite search should plan as
`Custom Scan (SkycellCone)`, and a nearest-neighbour query as `Custom Scan
(SkycellNearest)`:

```sql
EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 266.4, -28.9, 0.05));
```

If you get a sequential scan, the usual causes are a missing `ANALYZE`, the
index being on a different expression than the query's position argument, or —
for the spellings that work from `ra` and `dec` (ADQL's, `skycell_join`,
`skycell_radial_query`) — neither an index on `skycell_ang2cell(ra, dec)` nor a
stored `cell` column declared to hold it. Per-row regions are indexed too: a
per-row cone (a cross-match) by the custom scan, one covering per outer row,
and a per-row region (a stored footprint joined to the catalogue) by the custom
scan too, each outer row's region covered as it runs.

## Composite regions: instrument footprints

Many instrument footprints are not one polygon:

- JWST's NIRCam records its short-wavelength light on eight detectors in two
  modules, with gaps between them.
- HST's WFC3/UVIS images on two CCDs, either side of a gap.
- Gaia's focal plane holds 106 CCDs.
- A coronagraphic observation is a field less the circle its mask blocks.
- A survey exposure is a field less the masks round its bright stars.
- A coordinated parallel observation is two fields arcminutes apart.
- A radio observation can be a mosaic of overlapping beams.

STC-S writes such shapes by combining regions:

- a `UNION` or `INTERSECTION` of two or more;
- the `DIFFERENCE` of two (the first less the second);
- the `NOT` of one (the rest of the sky).

They nest up to 32 deep, and a `skyregion` value can be any of them. An
ObsCore `s_region` column therefore holds the footprints as they are, next to
circles and polygons, under one index, and the archive asks its usual
questions of them unchanged. Neither Q3C nor pgSphere has such a type.

### Storing footprints

A footprint is written in STC-S, the frame once at the top and the operands
inside one pair of parentheses, or built in SQL with `skycell_union`, `skycell_intersection`,
`skycell_difference` and `skycell_not`:

```sql
CREATE TABLE obscore (obs_id text PRIMARY KEY, s_region skyregion);
INSERT INTO obscore VALUES
  -- two CCDs either side of a gap (WFC3/UVIS-like)
  ('uvis-1', 'UNION ICRS (POLYGON 53.10 -27.85 53.14 -27.85 53.14 -27.81 53.10 -27.81
                          POLYGON 53.15 -27.85 53.19 -27.85 53.19 -27.81 53.15 -27.81)'),
  -- a coronagraph: the field less the circle its mask blocks
  ('coro-1', 'DIFFERENCE ICRS (BOX 150.0 2.2 0.05 0.05 CIRCLE 150.0 2.2 0.004)'),
  -- coordinated parallels: two fields 7' apart
  ('par-1',  skycell_union(box(210.80, 54.35, 0.036, 0.036),
                           box(211.00, 54.35, 0.036, 0.036))),
  -- a plain circle (a radio beam) in the same column
  ('beam-1', circle(83.82, -5.39, 0.25));
CREATE INDEX ON obscore USING gist (s_region);
```

Every region index takes composites: the default GiST,
`skyregion_gist_ops` and the MOC GIN ([Operators](#operators)).

### Searching them

The archive's usual question, which observations meet a search field, is asked
with a plain polygon:

```sql
SELECT obs_id FROM obscore
WHERE INTERSECTS(s_region,
  POLYGON('ICRS', 53.142, -27.84, 53.148, -27.84, 53.145, -27.82)) = 1;
-- (0 rows)
```

This field falls in the gap between the two UVIS detectors, so the exposure
is not returned, as it would be against a single polygon round both. The
GiST index serves the query (`s_region && …`). The other questions are written
the same way, all indexed:

```sql
-- the observations that saw a source (not the coronagraph, whose mask hides it)
SELECT obs_id FROM obscore WHERE CONTAINS(POINT('ICRS', 150.0, 2.2), s_region) = 1;

-- the observations a search region lies wholly inside
SELECT obs_id FROM obscore WHERE CONTAINS(CIRCLE('ICRS', 53.12, -27.83, 0.005), s_region) = 1;

-- the sky an observation exposed, gaps and masks excluded
SELECT obs_id, AREA(s_region) * 3600 AS arcmin2 FROM obscore;

-- the catalogue sources that fell on an observation's detectors
SELECT g.* FROM gaia g JOIN obscore o ON CONTAINS(POINT('ICRS', g.ra, g.dec), o.s_region) = 1
WHERE o.obs_id = 'uvis-1';
```

The last one uses the stored footprint as the search region on the
catalogue's cell index. A constant composite works the same way, through
ADQL's `REGION`. For example, the sources in a field away from its bright
stars:

```sql
SELECT * FROM gaia WHERE CONTAINS(POINT('ICRS', ra, dec), REGION(
  'DIFFERENCE ICRS (BOX 266.4 -28.9 1 1 UNION (CIRCLE 266.3 -28.8 0.05 CIRCLE 266.55 -29.1 0.08))')) = 1;
```

### Against the alternatives

Without a composite type, a footprint is kept as one polygon per detector and
one circle per mask. A query then returns the distinct observations among
them, less those whose mask holds the whole field. pgSphere's MOC type,
`smoc`, has a union and an intersection, but it is approximate to its cells,
and no index serves a position test against it. So a query that needs an exact
answer spells the shape out as a boolean of predicates. On an ObsCore-like
table of 50,000 NIRCam-, UVIS-, parallel- and coronagraph-like footprints
([Performance](#performance)):

| workload | skycell, one `skyregion` per observation | pgSphere, one `spoly` per detector | skycell ÷ pgSphere |
|---|---|---|---|
| observations meeting a search polygon (1′–20′) | 0.20 ms | 1.40 ms | 0.04 |
| observations containing a position | 0.13 ms | 0.27 ms | 0.70 |

Both answered the same observations for all 900 queries. skycell's index is
2.9 MB against pgSphere's 10.8 MB. A composite used as the search region on a
catalogue (an annulus, two circles, a field less a hole) is at or below
pgSphere's boolean of predicates on every shape measured.

### How it works

A position lies in a composite when the boolean combination of its operands'
exact tests says so. The covering of the catalogue's cells comes from the
operands' own coverings where that is sound: the union of theirs for a
`UNION`, their intersection for an `INTERSECTION`, the first operand's for a
`DIFFERENCE` that takes away at most half of it. Otherwise it comes from the
operands' cell classes (inside, outside, straddling) combined by the operator:

- A cell is inside a union if it is inside one operand, and outside it if it is
  outside all of them; an intersection is the other way round.
- `A − B` is outside where `A` is outside or `B` inside.
- `NOT` exchanges inside and outside.

Each operand is classified with a test that is sound in the direction the
operators above it need, so a covering never misses a row. The exact
membership test runs on what it keeps. A constant composite written as a
top-level condition is answered by the custom scan, as a cone is.
It is covered once at plan time, and the scan reads the ranges in cell order,
testing each row from its `ra` and `dec`. Under an `OR` it is folded with the
other constant regions of the same position
([Booleans of regions](#booleans-of-regions-become-one-region)); otherwise, or
with `skycell.custom_scan` off, it is rewritten into range conditions, as a
polygon is.

Regions are compared with composites too, stored or constant: `&&`, `@>`
and `<@` between regions, and ADQL `INTERSECTS` and `CONTAINS` of two
regions, through every region index. Where the operator decides it the
answer comes from the operands, by identities of sets: a union meets a region
if one of its operands does, `NOT A` meets `B` unless `B` lies inside `A`,
`X` lies in `A − B` if it lies in `A` and misses `B`. Otherwise the combined
region (`X ∩ Y` for an overlap, `X ∩ NOT Y` for containment) is tested for a
point by refining cells: a cell soundly inside it, or a cell centre the exact
test puts inside, is a point, and cells soundly outside hold none. What is
still undecided at cells of 10⁻⁴ of the smallest circle or polygon involved
(never finer than about 0.01″) counts as empty. So two regions that meet in
less than that, or that only share a boundary, do not overlap. A composite
can be empty, such as an intersection of pieces that do not meet. As in
PostGIS for an empty geometry, it meets nothing, holds nothing and is held by
nothing; otherwise it would lie inside every region, which no index on the
regions could find. A comparison takes 0.25 ms on average against random
composites up to three deep, and up to about 10 ms where long boundaries
nearly coincide.

`AREA` and `CENTROID` of a composite integrate over cells. Cells inside it count
whole (HEALPix cells have equal areas), cells outside it not at all, and cells
on its boundary are refined to 3·10⁻³ of its size and then counted by their
centre. A composite larger than a hemisphere is integrated through the rest of
the sky. Against exact values (circles, annuli, inclusion–exclusion with
polygons), the area is within 2·10⁻⁴ of the true one and the centroid within
2·10⁻⁴ of the region's size, at about 3 ms per region. A `NOT` of a cone or
polygon is exact.

STC-S `BOX ICRS ra dec width height` is read as the four-corner polygon
`box()` makes, alone or inside a composite, and prints as that `POLYGON`.

### Booleans of regions become one region

A query need not write the composite itself. A position tested
against several constant regions, joined by `OR`, `AND` and `NOT`, is folded
at plan time into one test against their union, intersection or difference,
and answered by the custom scan with one covering of the whole shape:

```sql
-- planned as POINT <@ UNION(CIRCLE, CIRCLE), one SkycellCone scan
SELECT * FROM cat
WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10.0, 20, 0.5)) = 1
   OR CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10.6, 20, 0.5)) = 1;
```

| written as | folded into |
|---|---|
| `p <@ A OR p <@ B` | `p <@ UNION(A, B)` |
| `p <@ A AND p <@ B` | `p <@ INTERSECTION(A, B)` |
| `p <@ A AND NOT p <@ B` (or `CONTAINS(p, B) = 0`) | `p <@ DIFFERENCE(A, B)` |
| `p <@ A OR NOT p <@ B` | `p <@ UNION(A, NOT(B))` |

The answer cannot change: a composite's membership is the boolean of its
pieces' exact tests, and every function involved is strict, so a NULL
position gives NULL either way. Folding happens innermost first, so
`(p <@ A OR p <@ B) AND NOT p <@ C` becomes one difference, and other
conditions are kept beside the folded one. It applies to ADQL's
`CONTAINS(...) = 1` and `= 0`, `INTERSECTS` of a point, and the native `<@`
and `@>`, at every query level. What does not fold: regions that are not
constant (a column, a parameter of a generic plan, a sub-select), two different
position expressions (`pos` and `point(ra, dec)` are two), and an `OR` holding
a term on anything else (`... OR mag < 5`), which keeps the range rewrite.

On the composite benchmark's shapes (60 each, 0.05°–2°, the 10M real Gaia DR3
positions; [`bench/results-fold/`](bench/results-fold/)), the folded boolean
against the same boolean unfolded:

| shape | folded ÷ unfolded | folded ÷ one `REGION` | folded ÷ pgSphere |
|---|---|---|---|
| annulus | 0.84 [0.79, 0.91] | 1.05 [0.99, 1.15] | 0.89 [0.82, 0.98] |
| two circles | 0.32 [0.29, 0.36] | 1.04 [1.02, 1.07] | 0.90 [0.88, 0.93] |
| lens | 0.76 [0.73, 0.80] | 1.05 [1.01, 1.08] | 0.50 [0.43, 0.60] |
| square less circle | 0.84 [0.82, 0.86] | 1.01 [0.99, 1.04] | 0.15 [0.12, 0.20] |
| five circles | 0.29 [0.27, 0.32] | 1.17 [1.12, 1.22] | 0.71 [0.69, 0.73] |

All four spellings counted the same rows for all 300 shapes. What remains
against one `REGION` is parsing the longer query (five `CONTAINS`, `POINT` and
`CIRCLE` calls to resolve for the five circles): planning the folded boolean
takes about what planning the `REGION` does, and a third of the unfolded one.
`skycell.fold_regions = off` answers each region on its own, as written.

## Operators and functions

Every function skycell installs, one by one. Types are `skypos` (a position) and
`skyregion` (a cone, box or polygon, or a union, intersection,
difference or complement of regions). The `coordsys` argument is accepted
for ADQL compatibility and must be `'ICRS'`; convert other frames first with
`gal2icrs()` / `ecl2icrs()`.

### Geometry — ADQL's own spellings

| Function | Returns | Notes |
|---|---|---|
| `point(ra, dec)`<br>`point(coordsys, ra, dec)` | `skypos` | degrees. ADQL `POINT` |
| `circle(ra, dec, radius)`<br>`circle(coordsys, ra, dec, radius)` | `skyregion` | radius in degrees. ADQL `CIRCLE` |
| `box(ra, dec, width, height)`<br>`box(coordsys, …)` | `skyregion` | centre and extent, as ADQL defines it. Errors if it reaches a pole — use a polygon there. ADQL `BOX` |
| `polygon(ra1, dec1, ra2, dec2, …)`<br>`polygon(coordsys, …)` | `skyregion` | ≥ 3 vertices, convex or not, but simple: no two edges may cross or touch. It must fit in a hemisphere: some point within 90° of every vertex, not necessarily the vertices' mean, which bunched vertices pull aside. ADQL `POLYGON` |
| `contains(p skypos, r skyregion)`<br>`contains(a skyregion, b skyregion)` | `integer` (1/0) | ADQL `CONTAINS`. Compared with 1 (`contains(…) = 1`, or `1 = …`) in a `WHERE` or `JOIN ON` it is planned as the `<@` operator below, so it is indexed |
| `intersects(p skypos, r skyregion)`<br>`intersects(r skyregion, p skypos)`<br>`intersects(a skyregion, b skyregion)` | `integer` (1/0) | ADQL `INTERSECTS`, the point on either side. Compared with 1 it is planned as `<@` or `@>` (point-region) or `&&` (region-region) below, so it is indexed |
| `distance(a skypos, b skypos)`<br>`distance(ra1, dec1, ra2, dec2)` | `float8` | degrees. ADQL `DISTANCE`, both forms. `distance(…) < r` (or `<=`) in a `WHERE` or `JOIN ON` is planned as the cone, like `<@` below |
| `area(r skyregion)` | `float8` | square degrees. ADQL `AREA`. A composite region's is integrated over cells, to about 10⁻³ of its area (exact for a `NOT` of a cone or polygon) |
| `coord1(p)` / `coord2(p)` | `float8` | right ascension / declination, degrees |
| `centroid(r skyregion)` | `skypos` | ADQL `CENTROID`: a cone's centre, a polygon's centre of mass, which may lie outside a non-convex one, as a U's does. A composite region's is integrated over cells, to about 10⁻³ of its size; an empty one has none, and says so |
| `region(stcs text)` | `skyregion` | ADQL `REGION`: any region written in STC-S, e.g. `region('DIFFERENCE ICRS (CIRCLE 10 20 2 CIRCLE 10 20 1)')`. The same as casting the text to `skyregion` |
| `skycell_union(VARIADIC skyregion[])`<br>`skycell_intersection(VARIADIC skyregion[])` | `skyregion` | two regions or more; STC-S `UNION` / `INTERSECTION` |
| `skycell_difference(a, b)` | `skyregion` | `a` less `b`; STC-S `DIFFERENCE` |
| `skycell_not(r)` | `skyregion` | the rest of the sky; STC-S `NOT` |

`pg_catalog` has its own `point`, `circle`, `box` and `polygon`. Only `point`
takes the same arguments as ADQL's; a bare `POINT(ra, dec)` that resolves to it
is read as the sky position through an implicit cast ([ADQL names](#adql-names)).
The prefixed aliases `skycell_point`, `skycell_circle`, `skycell_box` and
`skycell_polygon` never collide.

Composite regions are described, with examples, in
[Composite regions: instrument footprints](#composite-regions-instrument-footprints).

### Operators

The ADQL functions above and these operators run the same tests, and both are
indexed. `CONTAINS(…) = 1` and `INTERSECTS(…) = 1` are planned as the operator
in the table below, wherever they appear: `WHERE`, `JOIN ON` and `LEFT JOIN`,
under `OR`, in views, CTEs, `EXISTS` and `LATERAL` sub-selects, with
an outer query's columns as the centre. So a TAP service can pass ADQL geometry
through as it is written. The operators return a boolean rather than 1 or 0,
read more naturally in plain SQL, and are what a pgSphere user already writes.

| Operator | ADQL spelling | Meaning |
|---|---|---|
| `skypos <@ skyregion` | `CONTAINS(p, r) = 1`, `INTERSECTS(p, r) = 1` | position inside region. Against a circle (constant, or built per row as in a cross-match) it is planned exactly as `skycell_cone`, through the custom scan; so is a constant polygon, box or composite. Constant regions of the same position joined by `OR`, `AND` and `NOT` are folded into one composite. A per-row polygon, or one under an `OR` with anything else, is rewritten into index ranges |
| `skyregion @> skypos` | `INTERSECTS(r, p) = 1` | the same, reversed. When `skyregion` is a stored column with a GiST (or MOC GIN) index, indexed through that instead |
| `skyregion && skyregion` | `INTERSECTS(a, b) = 1` | regions overlap. Indexed by `CREATE INDEX ON t USING gist (region)` |
| `skyregion @> skyregion` | `CONTAINS(b, a) = 1` | `a` wholly contains `b`. The same index |
| `skyregion <@ skyregion` | `CONTAINS(a, b) = 1` | `a` lies wholly inside `b`. The same index |

Both spellings take the same plans. `<@`/`@>` against a circle and a plain `ra`/`dec` (or `skypos`) catalogue go
through the custom scan, like `skycell_cone`, whether the circle is a constant or
comes from another table's row (`JOIN cat c ON point('ICRS', c.ra, c.dec) <@
circle('ICRS', p.ra, p.dec, r)` is a cross-match). Either side can be the one
indexed: a constant point inside a circle drawn around each row, as the Gaia
archive's examples write cone searches (`point('ICRS', 81.28, -69.78) <@
circle('ICRS', ra, dec, r)`), is planned as the cone around that point, and a
cross-match probes whichever table has the index (the larger one if both do).
`distance(p1, p2) < r` (also `<=`, `r > distance(…)`, the four-coordinate form
and `skycell_dist`) gets the same plan: the planner adds the cone next to the
comparison, which still decides, so the answer is exactly the comparison's,
and the row estimate is the cone's.
Against a constant polygon or box they go through the custom scan (the B-tree
cell rewrite described above under an `OR` with other conditions). A `skyregion` column with its
own GiST index additionally indexes `&&` and both region-region containment
operators (`@>`/`<@`, in either argument order — they're commutators of each
other) directly against it, region side non-constant included — what a stored
footprint column (an ObsCore `s_region`, say) should carry. A per-row region
matched against a plain point catalogue needs no `skyregion` column of its own:
a per-row circle, or a per-row `skyregion` (a footprint of another table, or
`REGION()` of its STC-S text), goes to the custom scan, covered per outer row;
a per-row polygon given as an array, to the rewrite's run-time range slots.

**Region opclasses.** `CREATE INDEX ON t USING gist (region)` picks
`skyregion_box4_gist_ops`: an axis-aligned 3D box rounded outward to `float4`
(24 bytes, pgSphere's own representation). It beats pgSphere's native GiST on
every region operator (see [Performance](#performance)). Another can be named
explicitly:

```sql
CREATE INDEX ON t USING gist (region skyregion_gist_ops);      -- a few spherical caps per entry
```

`skyregion_gist_ops` keys each entry by spherical caps, exact for a circle at
any radius; it is the better choice for a column mixing very different region
scales (survey tiles alongside catalogue footprints), where a box is loose
around large circles. The opclasses can coexist on one column and the planner
picks per query; an expression index on `area(region)` lets it estimate the
column's region sizes. Region-region containment prunes with the `&&` test, so
it is sound but less selective than `&&` itself.

**"Which of my regions contain this point"** can also be served by a plain
GIN index over the region's MOC cells, which `region @> pos` / `pos <@ region`
reach automatically, for one position or a join of many:

```sql
CREATE INDEX ON t USING gin (skycell_region_moc(region));
```

It is used where the column has no GiST; beside one, the GiST is used, being
faster (500 positions against 50,000 footprints: 4 ms through the default
GiST, 12–17 ms through the GIN, 17 ms through `skyregion_gist_ops`).

Three operator classes deprecated in 0.36 (`skyregion_box_gist_ops`,
`skypos_spgist_ops`, `skypos_cap_gist_ops`) were removed in 0.38; an index on
one must be dropped before upgrading past 0.37, and the upgrade says so.

### The index key

| Function | Returns | Notes |
|---|---|---|
| `skycell_ang2cell(ra, dec)` | `int8` | the order-29 HEALPix cell — **index this** |
| `skycell_cell(p skypos)` | `int8` | same, from a `skypos` column |
| `skycell_ang2pix(order, ra, dec)` | `int8` | the cell at any order |
| `skycell_cell_corners(order, pix)` | `float8[]` | the four corners, as ra, dec, … |
| `skycell_cell_range(order, pix)` | `(lo int8, hi int8)` | the first and last order-29 ids inside a cell: a partition holding it is `FOR VALUES FROM (lo) TO (hi + 1)` |
| `skycell_create_partitions(parent regclass, order int [, with_default bool])` | `int` | cut a table partitioned by `RANGE` on the cell into the 12·4^order cells of an order (0–4), one partition each (`<table>_<order>_<cell>`), and a default partition for rows without a position; returns the partitions made ([Large catalogues](docs/tutorials/04-scaling-parallel-partitions.md#partitioning-by-sky-region)) |

### Indexable predicates

| Function | Notes |
|---|---|
| `skycell_cone(cell, ra, dec, ra0, dec0, radius)` | cone test, answered by the custom scan: a constant cone covered at plan time; a per-row cone (a cross-match, `JOIN cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, r)`) covered per outer row; a generic plan or correlated sub-select covered per rescan |
| `skycell_poly(cell, ra, dec, poly float8[])` | the same for a polygon; a constant one goes to the custom scan |
| `skycell_radial_query(ra, dec, ra0, dec0, radius)` | Q3C's spelling of the cone test — no cell argument, the planner synthesises `skycell_ang2cell(ra, dec)`; the custom scan answers it through an expression index on that, or a stored column declared to hold it ([setup 2](#setting-up-a-table)) |
| `skycell_join(ra1, dec1, ra2, dec2, radius)` | Q3C's spelling of the cross-match — likewise; the first pair is the indexed side, as in `q3c_join` |
| `skycell_in_cone(ra, dec, ra0, dec0, radius [, sel])` | the exact test alone, no index |
| `skycell_in_poly(ra, dec, poly [, sel])` | likewise |
| `skycell_in_region(p skypos, r skyregion [, sel])` | likewise, on the types |

### Nearest neighbours

`ORDER BY` the distance to a point, with a `LIMIT`, is answered from the cell
index by the `SkycellNearest` scan: the k nearest rows, without sorting the
relation.

```sql
-- ADQL's SELECT TOP 10 ... ORDER BY DISTANCE(...)
SELECT source_id, DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', 266.4, -29.0)) AS d
FROM cat ORDER BY d LIMIT 10;

-- the nearest neighbour of every row of another table
SELECT t.id, n.source_id, n.d
FROM targets t CROSS JOIN LATERAL (
  SELECT source_id, skycell_dist(ra, dec, t.ra, t.dec) AS d
  FROM cat ORDER BY 2 LIMIT 1) n;
```

The distance can be written `skycell_dist(ra, dec, ra0, dec0)`,
`distance(ra, dec, ra0, dec0)`, `distance(p, q)` or `p <-> q` (ADQL's
`DISTANCE(POINT(...), POINT(...))`), with either point first. The relation's
side is its columns, as `ra, dec`, `point([coordsys,] ra, dec)` or a `skypos`
column, and needs the index the cone searches use:
`skycell_ang2cell(ra, dec)`, or `skycell_cell(pos)` for a `skypos` column — or
a stored column declared to hold it ([setup 2](#setting-up-a-table)). The
other side may be constants or parameters: a generic plan's, or a `LATERAL`
sub-select's outer columns, as above.

The scan reads widening rings round the point, each the cone of the cell index
less the cells the previous ring's cone wholly holds, and sorts each ring by
the `ORDER BY` expression itself, so the order is the one a sort would give;
the `LIMIT` stops it after the ring that completes the answer. The first ring
is sized from the density round the point for the rows the `LIMIT` asks for,
over the share the query's other conditions keep; those conditions are tested
as a ring is read. A radius the conditions bound the answer to — the
distance below `r`, ADQL's `CONTAINS(..., CIRCLE(...))` or a `skycell_cone`
round the same point — is as far as the rings go, so "the 10 nearest within
1′" of an empty patch reads that patch, not the relation. No cone is needed
for speed, though: `ORDER BY` the distance with a `LIMIT` is the fast form on
its own. Rows with a NULL distance come last, as `ASC NULLS LAST` puts them.
Without a `LIMIT`, or ordered `DESC`, the query keeps PostgreSQL's sort.

On a heap out of cell order (the bitmap mode) the first ring is found best
first: HEALPix cells are visited nearest first, by the least distance a
point of each can have from the point. A cell the statistics expect to hold
many rows is split into its four children, at no cost to the index, since a
cell at any order is one key range; the others are read in batches through one
TID bitmap. Once k rows are held, the k-th distance bounds the rest: a cell
crossing it is split finer, a farther row is not kept, and the walk stops when
the nearest cell left is farther. The rows read come close to the rows
returned, as with a GiST ordering. `skycell.nearest_bestfirst = off` sizes the
first ring from the density instead; the walk is also left aside when the
rows it would hold do not fit in `work_mem`.

When a ring holds no row at all — empty sky round the point, or a partition
holding another part of the sky — the scan finds where the rows start from the
index itself: a best-first walk over HEALPix cells, nearest first, each probed
for an index entry, until an occupied cell is small next to its distance.
Every nearer cell was found empty, so no row is nearer than that cell's
closest point, and the next ring starts there.

A partitioned table gets the scan on each partition, and PostgreSQL
merges their ordered rows under the `LIMIT` (Merge Append), however the
partitions are laid out: by declination, by cell range, sub-partitioned, with
their columns in another order, or with a partition left without the index
(sorted instead). On 3M uniform rows, the 10 nearest take 2.3 ms with four
partitions by cell range and 32 ms with four declination bands, against
0.35 ms on the table unpartitioned.

On 10M real Gaia DR3 positions it takes 0.48–0.61 of the time of pgSphere's
GiST ordering for k from 10 to 1000 (warm, a new point every query, planning
included); on a clustered ObsCore table with its heap out of cell order, 0.94
at k = 100 and 0.95 at k = 1000
([`bench/results-nearest/`](bench/results-nearest/)).

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
| `q3c_poly_query(ra, dec, poly)` | `skycell_poly_join(ra, dec, poly)` | same arguments; the polygon may be a constant or another table's column |
| `q3c_dist(ra1, dec1, ra2, dec2)` | `skycell_dist(ra1, dec1, ra2, dec2)` | degrees, as in Q3C |

They need an index on `skycell_ang2cell(ra, dec)` to be indexed at all, exactly
as Q3C needs one on `q3c_ang2ipix(ra, dec)` — or a stored `cell` column declared
to hold it ([setup 2](#setting-up-a-table)); without either you get a correct
sequential scan. With it the cone and the cross-match take the custom scan, the
same plan as `skycell_cone` and ADQL's `CONTAINS`, and cross-match 16–27% faster
than `q3c_join`. Q3C has no nearest-neighbour ordering; here `ORDER BY
skycell_dist(…)` with a `LIMIT` is indexed ([Nearest neighbours](#nearest-neighbours)).
`skycell_cone(cell, ra, dec, ra0, dec0, radius)` remains the canonical spelling
— one predicate covers both the cone search and the cross-match.

### Coverings, cross-matches and stored regions

| Function | Returns | Notes |
|---|---|---|
| `skycell_cone_ranges(ra0, dec0, radius [, tbl, col])` | `SETOF (lo, hi)` | the covering as index ranges, for hand-built plans (a `LATERAL` cross-match); the plain join through the custom scan is faster. Index with `skycell_cell_ops` (`CREATE INDEX ON t (skycell_ang2cell(ra, dec) skycell_cell_ops)`) so the planner can estimate such a range join |
| `skycell_cone_cell_lo(ra0, dec0, radius)`<br>`skycell_cone_cell_hi(ra0, dec0, radius)` | `int8` | the lowest and highest order-29 cell the cone can hold. The planner adds `cell >= lo AND cell <= hi` next to a cone on a catalogue range-partitioned by its cell, so PostgreSQL prunes the partitions the cone cannot reach; the custom scan leaves them out of its filter. The whole sky for a cone over 90° or an invalid centre; cones over 10° are bounded more loosely. Constant polygons and composites get the same comparisons, as constants, from their bounding circle |
| `skycell_poly_ranges(poly [, tbl, col])` | `SETOF (lo, hi)` | the same for a polygon |
| `skycell_cone_moc(ra0, dec0, radius [, max_cells, max_order])` | `int8[]` | the covering as IVOA MOC `NUNIQ` cells — store these to index a region |
| `skycell_poly_moc(poly [, max_cells, max_order])` | `int8[]` | likewise |
| `skycell_ancestors(cell [, min_order, max_order])` | `int8[]` | a position's containing cells — join against stored MOC cells |
| `skycell_nuniq_order/lo/hi(nuniq)` | `int`/`int8` | decode a `NUNIQ` cell |

### Astrometry — IVOA user-defined function names

The names are those of the IVOA [catalogue of ADQL user-defined
functions](https://github.com/ivoa-std/udf-catalogue) (Endorsed Note 1.2) or,
where it has none, of GAVO DaCHS, so ADQL written for DaCHS or the Gaia
archives runs unchanged.

| Function | Returns | Source | Notes |
|---|---|---|---|
| `ivo_epoch_prop_pos(ra, dec, parallax, pmra, pmdec, rv, ref_epoch, out_epoch)` | `skypos` | catalogue | the position, propagated rigorously (ESA SP-1200 §1.5.5, the catalogue's recommended formalism) |
| `ivo_epoch_prop_pos(ra, dec, pmra, pmdec, ref_epoch, out_epoch)` | `skypos` | DaCHS | as if parallax and radial velocity were NULL |
| `ivo_epoch_prop(…the same 8…)` | `float8[6]` | DaCHS | all six parameters: parallax and radial velocity evolve too; NULL there without a parallax |
| `ivo_apply_pm(ra, dec, pmra, pmdec, epdiff)` | `skypos` | skycell | proper motion alone, in mas/yr. Neither the catalogue (which dropped its `ivo_apply_pm`) nor DaCHS (`gavo_apply_pm`, in degrees/yr) has this one |
| `ivo_healpix_index(hpxOrder, ra, dec)`<br>`ivo_healpix_index(hpxOrder, p)` | `int8` | catalogue | NESTED HEALPix cell |
| `ivo_healpix_center(hpxOrder, hpxIndex)` | `skypos` | catalogue | and back |
| `icrs2gal` / `gal2icrs` / `icrs2ecl` / `ecl2icrs` | `skypos` | skycell | frame conversion |

NULL arguments to the epoch functions are taken as the catalogue says: a missing parallax means infinitely remote, a missing proper motion or
radial velocity is 0, and a missing position or epoch is an error — not a NULL
result, which would drop every star without a measured radial velocity.

Proper motions are in mas/yr, parallax in mas, radial velocity in km/s, epochs
in years. An index cannot know where a star has moved to, so write a
proper-motion-aware search as an indexable cone widened by the largest motion of
interest, with the exact test on the propagated position:

```sql
SELECT * FROM cat
WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 269.45, 4.69, 0.05 + 0.006))
  AND 1 = CONTAINS(ivo_epoch_prop_pos(ra, dec, plx, pmra, pmdec, rv, 2016, 2026),
                   CIRCLE('ICRS', 269.45, 4.69, 0.05));
```

### Estimates for sky views

A zoomed-out sky view has no use for millions of points: it wants how many
sources it holds, and a coarse density map by HiPS tile. skycell reads both
from the statistics the planner already keeps (the `ANALYZE` histogram of
the table's cell index), without reading a row:

| Function | Returns | Notes |
|---|---|---|
| `skycell_count_estimate(tbl text, r skyregion)` | `float8` | the rows the table is expected to hold in region `r` (any region: cone, polygon, box, composite) |
| `skycell_count_estimate(tbl text, hpx_order int, ipix bigint)` | `float8` | the same for one NESTED HEALPix cell, a HiPS tile |
| `skycell_density_estimate(tbl text, r skyregion, hpx_order int)` | `SETOF (ipix, estimate, inside)` | the cells of order `hpx_order` (0–20) that `r` meets, with each one's estimate and whether it lies wholly inside `r`; at most 200,000 cells. For SQL: ADQL has no table functions |

The table is named by a string, resolved as `regclass` would (schema,
`search_path`, quoting); it needs an index on `skycell_ang2cell(ra, dec)` or
`skycell_cell(pos)`, or a stored `cell` column, and a partitioned table is
summed over its partitions. The functions are scalar so that ADQL can call
them, on a TAP service that declares them as user-defined functions; the
geometry translators pass them through:

```sql
-- the number of sources in the view
SELECT skycell_count_estimate('gaia.gaia_source', CIRCLE('ICRS', 266.4, -28.9, 10))
FROM tap_schema.schemas;
-- a density map: the client knows its visible HiPS tiles, and uploads them
SELECT t.ipix, skycell_count_estimate('gaia.gaia_source', 6, t.ipix) AS n
FROM TAP_UPLOAD.tiles AS t;
```

A region's estimate integrates the density model over the region (its cells
refined along the edge to a 16th of its size, each counted whole once
inside). Against exact counts on the 10M real Gaia DR3 positions (statistics
target 1000, so 10,000 rows a bucket), 150 cones of 0.5°–30°:

| rows in the region | median factor off | worst |
|---|---|---|
| over 1M | 1.03 | 1.04 |
| 100k–1M | 1.06 | 1.20 |
| 10k–100k | 1.18 | 1.41 |
| under 10k | 1.25 | 4.2 |

HiPS tiles of orders 2–6 (300 to 44,000 rows each) come within 1.10–1.28 at
the median. Below the size of a histogram bucket (the table's rows over its
statistics target) an estimate is only indicative: raise the target
(`ALTER INDEX … ALTER COLUMN 1 SET STATISTICS 1000`, on each partition of a
partitioned table, whose default of 100 leaves coarse buckets). A region's
estimate takes 0.1–0.3 ms, a tile's about 2 µs (14 µs over 16 partitions),
a 40° × 30° map of 1,713 order-6 cells 1.7 ms; an exact count takes a few
milliseconds in sparse sky and hundreds where the catalogue is crowded. The
estimates are as current as the last `ANALYZE`: run it after loading rows.

### Density maps in the database

For counts exact per cell, at HiPS orders, kept beside the catalogue and
rebuilt after each load, `skycell_density_view` makes a materialised view of
the rows in every occupied NESTED HEALPix cell:

```sql
SELECT skycell_density_view('gaia.gaia_source', 'gaia.gaia_density', 9, 0);  -- orders 0 to 9
REFRESH MATERIALIZED VIEW CONCURRENTLY gaia.gaia_density;                     -- after a load
```

| Function | Returns | Notes |
|---|---|---|
| `skycell_density_view(tbl text, target text, max_order int DEFAULT 8, min_order int DEFAULT 0)` | `bigint` | (re)creates `target` as a materialised view `(hpx_order, ipix, n)` with a unique index on `(hpx_order, ipix)`; returns its row count. Orders 0–20 |
| `skycell_cell_overlaps(hpx_order int, ipix bigint, r skyregion)` | `integer` (1/0) | whether the cell meets region `r`: every cell it meets, and perhaps one it misses by less than 1/16 of the cell's size. ADQL |

The table's cells come from its skycell index (or a stored `cell` column);
the finest order is counted in one pass over the table and each coarser one
from the order below it. On the 10M real Gaia DR3 positions, orders 0–9
(2.7M occupied cells) take 9 s to build, the same to `REFRESH`; `REFRESH …
CONCURRENTLY`, which leaves the view readable meanwhile, 42 s. ADQL reads the
view as a table, a sky view's cells through `skycell_cell_overlaps`, which
the planner turns into ranges of `ipix` on the view's index for a constant
order and region:

```sql
SELECT ipix, n FROM gaia.gaia_density
WHERE hpx_order = 6 AND 1 = skycell_cell_overlaps(6, ipix, CIRCLE('ICRS', 266.4, -28.9, 5));
```

120 order-6 cells in 1.8 ms, 276 order-9 cells round a 1° view in 4 ms. Each
cell's count is exact as of the last refresh; a view's sum counts its edge
cells whole (398k rows for the 5° view above, against 294k inside it, which
`skycell_count_estimate` gives within 0.2%). A `skycell_density_view` call
replaces the view it names; anything built on it must be dropped first.
(`skycell_density_build`, below, is another thing: a finer density model
the planner reads instead of the histogram.)

### MOC export

A region's or a table's coverage as an IVOA MOC 2.0, in its ASCII or JSON
serialisation, for MOCPy, Aladin, the CDS MOC server or any other tool that
reads MOCs:

```sql
SELECT skycell_moc(CIRCLE('ICRS', 10, 20, 2), 6);
-- 5/4965 4967 4976 6/19771 19774-19775 19818-19819 19857 19859 19865 19912-19914
SELECT skycell_moc(CIRCLE('ICRS', 10, 20, 2), 6, 'json');
-- {"5": [4965, 4967, 4976], "6": [19771, 19774, 19775, ...]}
SELECT skycell_coverage_moc('gaia.gaia_source', 10);               -- where the table has rows
SELECT skycell_moc_agg(10, skycell_ang2cell(ra, dec) >> 38)        -- where a query's rows are
FROM gaia.gaia_source WHERE phot_g_mean_mag < 12;
```

| Function | Returns | Notes |
|---|---|---|
| `skycell_moc(r skyregion, max_order int DEFAULT 10, fmt text DEFAULT 'ascii')` | `text` | the cells of `max_order` that meet `r`, as `skycell_cell_overlaps` counts them; cells inside it whole at a coarser order. Orders 0–29 |
| `skycell_moc_agg(hpx_order int, ipix bigint [, fmt text])` | `text` | aggregate: the MOC of the cells of one order, duplicates allowed. Orders 0–12; no rows, or no row with a cell, give NULL. Runs in parallel |
| `skycell_coverage_moc(tbl text, max_order int DEFAULT 10, fmt text DEFAULT 'ascii')` | `text` | the cells of `max_order` that hold a row of `tbl`, from its skycell index or stored `cell` column. Orders 0–12; an empty table gives an empty MOC |

`fmt` is `'ascii'` or `'json'`. The output is normalised as MOCPy writes it:
each cell at the coarsest order it can be written at (four sibling cells are
their parent), orders ascending, runs of cells `a-b` in ASCII, and the MOC's
depth declared by an empty max order written last (`10/`, `"10": []`) when
no cell is that fine. MOCPy 0.21 reads every MOC above back and writes it out
the same, for 200 random cones, boxes, unions and differences at orders 0–12;
a table's coverage equals `MOC.from_lonlat` on the same positions at orders
6, 10 and 12.

A region's MOC holds every cell the region meets, and may hold a cell it
misses by less than 1/16 of the cell's size. That is tighter than
`MOC.from_cone`: on 50 random cones, MOCPy's MOC held every cell of skycell's,
and 21 of the 50 held cells more, which by dense sampling lie outside the cone
(up to 0.49° beyond a 5° cone at order 2). A 20° cone at order 10 takes
18 ms; the coverage of the 10M-row Gaia table at order 10, 1.2 s, one pass
over its index. FITS, the third MOC 2.0 serialisation, is not written.

`skycell_moc_agg`, and so `skycell_coverage_moc`, runs in parallel:
each worker marks its own rows' cells and the leader combines them. On a
sharded database such as Citus, each shard does the same. Its state is a type
of its own, `skycell_mocstate`, so that it can travel between processes and
nodes. On the 10M-row Gaia table with four workers: order 10 takes 0.68 s
against 1.5 s in one process, order 12 takes 1.8 s, and order 3 takes 0.43 s.

### MOCs as search regions

A MOC from MOCPy, Aladin, the CDS MOC server or skycell itself is a region
like any other: the sources inside a survey's coverage, an observation's
footprint MOC, or where two catalogues overlap.

```sql
-- the Gaia sources inside a footprint MOC
SELECT source_id FROM gaia.gaia_source
WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('9/1312345-1312360 10/5249461'));

-- the same in STC-S style, and in a composite
SELECT ... WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), REGION('MOC 9/1312345-1312360'));
SELECT ... WHERE 1 = CONTAINS(POINT('ICRS', ra, dec),
                              REGION('DIFFERENCE ICRS (MOC 6/1200-1300 CIRCLE 266.4 -28.9 0.5)'));

-- the Gaia sources inside the area another table covers
SELECT count(*) FROM gaia.gaia_source
WHERE 1 = CONTAINS(POINT('ICRS', ra, dec),
                   skycell_moc_region(skycell_coverage_moc('ivoa.obscore', 10)));
```

| Function | Returns | Notes |
|---|---|---|
| `skycell_moc_region(moc text)` | `skyregion` | a MOC in its ASCII form (MOC 2.0 `5/4965 4967 6/19771-19775`, or MOC 1.0 with commas) or its JSON form (`{"5": [4965, 4967]}`). Cells may overlap and come in any order; they are merged |

`REGION('MOC ...')`, a cast from text, and a `MOC` inside `UNION`,
`INTERSECTION`, `DIFFERENCE` and `NOT` read the ASCII form. A MOC region
prints as `MOC` followed by its normalised ASCII, so it can be stored in a
`skyregion` column and read back.

A position is in a MOC when the order-29 cell holding it is. That is the cell
the index stores, so a MOC's answer is exact to its own cells. A MOC made from a
shape at order k holds the shape and some sky around it: 121, 87 and 83 Gaia
sources for a 0.5°×0.4° polygon's MOC at orders 10, 12 and 14, against 81 in
the polygon. A MOC is its own covering: the custom scan reads its ranges,
merging gaps where the density says they hold few rows. Everything else a
region does works for MOCs too:

- `AREA` is exact;
- `CENTROID` is the centre of area of the MOC's cells;
- `INTERSECTS` and `CONTAINS` between two MOCs are exact range tests;
  - between a MOC and another region, they use the test composites use, to
    within the size of the MOC's finest cell;
- stored MOCs take a GiST index;
- a MOC prunes cell-range partitions to its first and last cell;
- `skycell_count_estimate` and `skycell_cell_overlaps` accept a MOC;
- `skycell_moc` writes a MOC again at another order.

**Validation** ([`bench/results-moc/`](bench/results-moc/)): 60 random MOCs,
on the 500,000 positions of the ObsCore-shaped table. Half were written by
skycell (cones, polygons and annuli at orders 2–12), half by MOCPy (cones,
coverages of the table's own positions, and random cells of orders 0–9).
Each gave exactly the rows MOCPy's `contains_lonlat` gives, 3.7 million in
all, through the custom scan, the range rewrite and a sequential scan. A MOC
less a cone gave the expected rows in 20 cases. On 5,000 stored MOC
footprints against 25 cones, `INTERSECTS` and both `CONTAINS` agreed with the
cones' own order-12 MOCs.

**Speed:** a footprint MOC at order 12 or 14 takes 0.2–0.4 ms on the 10M-row
Gaia table, as the polygon does. The coverage of the ObsCore table at order 6
(22 KB of MOC, a ninth of the sky) holds 5M of those rows. A sequential scan
reads them in 1.9 s; the row estimate is within 5% of the count. A constant
MOC is decoded once a query, not once a row: before that, the same scan took
50 s.

### MOC set operations

Footprints kept as MOCs, one per observation, can be combined in the database:
the area a collection covers, where two surveys overlap, or what one covers
and another does not.

```sql
-- each observation's footprint as an order-10 MOC, under a GiST index
ALTER TABLE obs ADD COLUMN fp skyregion;
UPDATE obs SET fp = skycell_moc_region(s_region, 10);
CREATE INDEX ON obs USING gist (fp);

-- the observations whose footprint holds a source
SELECT obs_id FROM obs WHERE 1 = CONTAINS(POINT('ICRS', 120.3, 30.2), fp);

-- the coverage of a collection, as one MOC, and its area
SELECT area(skycell_moc_union_agg(fp)) FROM obs WHERE obs_collection = 'HST';

-- where two collections overlap, and what the first covers that the second does not
SELECT skycell_moc_intersection(a, b), skycell_moc_difference(a, b)
FROM (SELECT skycell_moc_union_agg(fp) FILTER (WHERE obs_collection = 'HST') AS a,
             skycell_moc_union_agg(fp) FILTER (WHERE obs_collection = 'JWST') AS b FROM obs) s;
```

| Function | Returns | Notes |
|---|---|---|
| `skycell_moc_region(r skyregion, max_order int)` | `skyregion` | any region as a MOC: the cells of `max_order` it meets, as `skycell_moc` writes them; a MOC finer than `max_order` is degraded to it |
| `skycell_moc_union(a, b)`, `skycell_moc_intersection(a, b)`, `skycell_moc_difference(a, b)` | `skyregion` | exact, on the two MOCs' cells; the result's depth is the finer of the two |
| `skycell_moc_complement(a)` | `skyregion` | the rest of the sky |
| `skycell_moc_degrade(a, hpx_order int)` | `skyregion` | at a coarser order: every cell of that order it meets, whole (a superset) |
| `skycell_moc_union_agg(r skyregion [, max_order int])` | `skyregion` | aggregate: the union of the MOCs aggregated, or with `max_order` of any regions, each as `skycell_moc_region(r, max_order)` makes it. Runs in parallel; NULL over no rows |

The operations take MOCs (`skycell_moc_region(r, order)` makes one of any
region). The result is a MOC region like any other: it can be stored, indexed,
searched, measured with `AREA`, or written out with `skycell_moc(m, order,
'json')`.

**Validation** ([`bench/results-moc/setops.py`](bench/results-moc/)): 60
pairs of random MOCs, written by skycell and by MOCPy 0.21.0. Union,
intersection, difference, complement and degrade equal MOCPy's, cell for cell,
in every case, and so does the union of 200 MOCs at once. Difference is
compared with MOCPy's A ∩ (complement of B), because MOCPy 0.21.0's own
`difference()` returns an empty MOC when the two do not overlap
(`MOC("1/1").difference(MOC("1/5"))`), as it did for 21 of the 60 pairs.

**Speed:** 50,000 footprints of 0.1°–0.4°, stored as order-10 MOCs:
- the union of all of them takes 0.6 s;
- the footprints that hold a source take 0.14 ms under the GiST index (0.2 s
  without);
- making the MOC of a footprint takes 0.3 ms at order 10, 1 ms at order 12.

### Diagnostics

| Function | Notes |
|---|---|
| `skycell_cover_info(ra0, dec0, radius [, tbl, col])` | what the covering looks like: ranges, cells examined, deepest order, expected rows, area ratio, the density used and the order chosen |
| `skycell_range_cost([tbl, col])` | the price of one index range the cost model derives for a relation |
| `skycell_density_build(tbl [, col, rows_per_cell, max_order])` | build a multi-order count map the covering reads instead of the histogram (**experimental**) |
| `skycell_density_drop(tbl [, col])` | remove it |

### Settings

| GUC | default | meaning |
|---|---|---|
| `skycell.fold_regions` | on | answer a position tested against several constant regions, joined by `OR`, `AND` and `NOT`, as one test against their union, intersection or difference ([Booleans of regions](#booleans-of-regions-become-one-region)). Off answers each region on its own, as written |
| `skycell.custom_scan` | on | answer `skycell_cone` (and `skycell_join`, `skycell_radial_query`, and ADQL's `CONTAINS(POINT, CIRCLE) = 1`, `point <@ circle` and `DISTANCE(…) < r`) with the `SkycellCone` scan node instead of rewriting it into B-tree range conditions — constant cones, cross-match cones (a parameterized scan, one covering per outer row) and cones over parameters alone (generic plans, correlated sub-selects). A cone that is not a top-level `AND` term of a `WHERE`/`JOIN ON` (one under an `OR`, say) keeps the rewrite, so it stays indexable. Off restores the rewrite everywhere |
| `skycell.index_filter` | on | when the cell index also holds the position — `CREATE INDEX ON t ((skycell_ang2cell(ra, dec))) INCLUDE (ra, dec)` — the `SkycellCone` scan tests the cone on each index entry and fetches from the table only the rows inside it, instead of every row of the covering cells (both modes, constant cones and cross-matches). The plan's exact test still applies to every row, so the answer is the same; off reads every row of the covering cells |
| `skycell.nearest_bestfirst` | on | on a heap out of cell order, find a nearest-neighbour search's first rows by a best-first walk over HEALPix cells instead of a first ring sized from the density ([Nearest neighbours](#nearest-neighbours)) |
| `skycell.nearest_scan` | on | answer `ORDER BY` a distance to a point, with a `LIMIT`, with the `SkycellNearest` scan ([Nearest neighbours](#nearest-neighbours)); needs `skycell.custom_scan`. Off leaves the query to a sort of the relation |
| `skycell.range_cost` | -1 (derive) | price of one index range, in rows; -1 derives it from the relation's rows/page and the planner's cost factors (for the custom scan, also from how much of the relation `shared_buffers` holds) |
| `skycell.max_ranges` | 64 | cap on ranges per covering |
| `skycell.max_area_ratio` | 64 | a partly covered cell may not exceed this × the region's area |
| `skycell.use_stats` | on | use the histogram as a density map |
| `skycell.cache_coverings` | on | memoise coverings per backend |
| `skycell.join_slots` | 4 | range slots the rewrite emits for a non-constant region (used when the custom scan does not apply: `custom_scan` off, a region under an `OR`, per-row polygon arrays) |
| `skycell.exact_cells` | on | tight cell geometry; off falls back to the `max_pixrad` cap |
| `skycell.force_order` | -1 | diagnostics: cover cones at this order (-1 = let the model choose) |
| `skycell.split_cost`, `skycell.max_steps` | 1, 4000 | refinement guards for polygons and very large cones |
| `skycell.probe_orders` | 0 (off) | experimental: try this many orders past the closed-form choice and keep whichever scores lowest |
| `skycell.probe_split_cost` | 1.0 | experimental: cost per cell the order probe charges itself |
| `skycell.rewrite_max_waste` | 100 | decline the range rewrite for a constant region when the covering expects more than this many false-positive rows, scaled by how cache-resident the relation is — only when the relation also has a GiST or SP-GiST index on the position for the planner to use instead (without one, declining would leave a sequential scan) |
| `skycell.rewrite_waste_scale_cap` | 10 | upper bound, as a multiple of `rewrite_max_waste`, on how far that scaling may go for a relation far bigger than `shared_buffers` |

---

## Performance

Randomized paired trials on PostgreSQL 18.6, on 10M-row catalogues
(synthetic, resampled from the Gaia DR3 density field, and real DR3 positions)
and a 50M-row one, each method's table in its own key order; ratios are
skycell's time over the other's, with 95% bootstrap intervals:

| workload | against | skycell's time |
|---|---|---|
| cone searches, 1″–3°, warm | pgSphere | 0.66–0.95 (faster at every radius) |
| cone searches, cold, 50M rows | pgSphere | up to 2.5× faster from 30′ |
| cross-matches, 10³–10⁵ targets, 0.5″–5″ | `q3c_join` / pgSphere | 0.73–0.84 / 0.26–0.33 |
| convex / non-convex polygons | pgSphere | 0.21–0.23 / 0.13–0.29 |
| stored regions: overlap, containment, point-in-footprint | pgSphere | 0.11–0.70 |
| composite footprints meeting a field | pgSphere, one row per detector | 0.04 |
| nearest neighbours, k = 10–1000 | pgSphere GiST ordering | 0.48–0.61 (Gaia), 0.94–1.01 (clustered ObsCore table) |

Its index is a third the size of pgSphere's (214 MB against 687 MB at 10M
rows) and builds about 23× faster; at Gaia DR3 scale, one 38 GiB index replaces
the 160 GiB an archive keeps for pgSphere plus Q3C. The full measurements, the
protocol and how each figure moved between versions are in
[`docs/performance.md`](docs/performance.md).

## Testing

The coverings are checked by brute force against points sampled in, out and
on the edge of every region kind: adversarial cones and polygons at the poles,
zone boundaries and face edges, non-convex polygons against an independent
reference, and random composites up to four deep, with zero missed rows. The
SQL regression suite, run on PostgreSQL 16, 17 and 18 for every change,
requires every indexed answer to equal a sequential scan with the exact
predicate. `make perfcheck` guards against slowdowns before a release. Details
in [`docs/testing.md`](docs/testing.md); the tutorials' own SQL is checked by
[`docs/tutorials/check.py`](docs/tutorials/check.py).

## Limitations

- **Geometry.** A polygon must be simple, with no crossing or touching edges,
  and it must fit in a hemisphere: some point within 90° of every vertex. A
  larger region is written as a composite, the `NOT` of the rest of the sky or
  a `UNION` of pieces. A `BOX` may not reach a pole. A composite region (`UNION`, `INTERSECTION`,
  `DIFFERENCE`, `NOT`) has its `AREA` and `CENTROID` integrated to about
  10⁻³, not computed exactly. Comparing it with another region is decided to
  10⁻⁴ of its smallest piece where its operands do not decide it, and an empty
  composite is contained in nothing. Its
  covering is as tight as its pieces' cell classes allow, so a deep
  `DIFFERENCE` of nearly equal regions reads more candidate rows than its
  area suggests. A non-convex polygon costs more than a
  convex one. Parsing a U or a comb takes 2–6 µs against 0.6 µs for a convex
  quadrilateral, and each step of its covering is 2–3× dearer. The ranges it
  yields are as few, so the rows read do not change. Setting one up is
  quadratic in its vertices: 10 ms for 1,000 vertices, about 1 s for 10,000.
  A constant polygon is set up once per query, a stored one once per row.
- **Nearest neighbours.** The `SkycellNearest` scan needs the relation's
  position to be the one its cell index is built on: an expression index on
  `skycell_ang2cell(ra, dec)` or `skycell_cell(pos)`, or a stored `cell`
  column declared to hold it (setup 2); an undeclared one leaves the query to
  a sort, as it leaves ADQL's `distance()` and `contains()` unindexed. A query
  that reads a system column of the relation
  (`ctid`, `FOR UPDATE`) keeps the sort too, since the scan returns rows from
  its own buffer. The best-first walk that finds the first rows on a heap out
  of cell order holds them in memory, so a query wanting more rows than about
  a third of `work_mem` holds uses the rings, which spill to disk, instead.
- **Where the custom scan applies.** It answers cones, cross-matches and
  constant polygons, boxes and composites; per-row polygons and stored regions
  use the range rewrite and the region indexes. The search must be a top-level
  `AND` term of a `WHERE` or `JOIN ON`, or a boolean of constant regions of
  one position, which is folded into one; one under an `OR` with
  anything else is rewritten instead and stays indexable through a `BitmapOr`. It reads heap
  tables only. On a partitioned table each partition gets the scan, and a
  partition without the cell index a sequential scan. A cross-match against a
  partitioned catalogue rescans the partitioned table for every probe.
  Partitioned by its cell (`skycell_ang2cell(ra, dec)` or a stored cell
  column, range partitioning), the partitions a cone cannot reach are pruned
  at each probe, and 16 partitions cost 1.2–1.7 times the unpartitioned
  table. Partitioned otherwise (by declination band, say)
  every partition is rescanned, each one whose keys cannot hold the cone
  answering in about 0.3 µs; bands hold cells all over the key space, so
  there it is the covering that costs. A partition left without the index
  stops a cross-match from using the others' scans at all. Constant polygons
  and composites are pruned by their bounding circle; a cone or
  circle over 90°, or a `NOT` region, prunes nothing.
  Nearest neighbours on a partitioned table are answered partition by
  partition and merged, so every partition finds its own nearest
  row before the first is returned: one far from the point costs a cell walk
  and a few rings, about 30 ms a query on 3M rows in four declination bands,
  where partitions by cell range, each holding its own part of the sky, take
  2.3 ms. Before PostgreSQL 18 its bitmap mode does not prefetch.
- **Parallel queries.** A cross-match's probes are shared out among workers,
  and a single cone, polygon, box or composite search with a fixed region is
  split among them (its covering in chunks). A cone whose region is known only
  at run time (a generic plan's parameters) and a nearest-neighbour query are
  not split. PostgreSQL gives
  the probe table workers by its size: none below 8 MB
  (`min_parallel_table_scan_size`), one at 10 MB, two at 50 MB. A smaller target
  list runs serially unless told otherwise: `ALTER TABLE targets SET
  (parallel_workers = 3)`. In a parallel `EXPLAIN ANALYZE` the average of ranges
  per covering is the leader's own, labelled so, since a node that is not
  parallel aware gets no shared memory to collect the workers' figures; the rows
  removed by the index filter are counted in every process.
- **Planning costs.** A covering is computed per query: skycell plans a 1″
  cone in 0.054 ms, against 0.040 ms for pgSphere, so on a small table (0.5M
  rows) a small cone at a new position every query can still favour pgSphere.
  skycell's win at small radii comes from execution.
- **Plan-time sampling for cross-matches.** To estimate a cross-match's rows,
  the planner reads up to 32 probe rows and up to 4000 catalogue rows through
  the index — about 2–5 ms of planning, more on a cold cache. It applies when
  the probe position is a plain column and the cones are small; otherwise the
  estimate falls back to the density model, which can be far too low for
  probes that sit on catalogue sources.
- **The density estimate is coarse.** An equi-depth histogram cannot see a
  cluster smaller than one of its buckets; `max_area_ratio` bounds the damage
  rather than fixing it, and the experimental count map
  (`skycell_density_build`) replaces the histogram where it is too coarse.

## License

MIT. See [LICENSE](LICENSE).
