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
  4–31% on real Gaia DR3 positions. Cold it is faster or level everywhere, and
  up to 2.7× faster at 50M rows.
- **Cross-matching, Q3C's speciality:** 20–33% faster than `q3c_join`, and
  about 3× faster than pgSphere.

It also answers convex polygons 2.7–4.4× faster than pgSphere. Its index is a
third the size of pgSphere's and builds 11–23× faster, and its row estimates
stay within 1.3× of the truth where pgSphere's are off by up to 8.7×.

The interface is ADQL's. The functions carry ADQL's own names (`CONTAINS`,
`POINT`, `CIRCLE`, `POLYGON`, …), and `1 = CONTAINS(…)` is indexed as written:
the planner turns it into its operator, `point <@ circle`, which runs through
the same scan as skycell's native functions, cross-matches included, and so
does a `DISTANCE(p1, p2) < r` comparison. It
works whichever way round the query is written: a constant point inside a
circle around each row is planned as the cone around that point, and a
cross-match probes the larger indexed table whichever side is the point and
whichever the circle. With pgSphere and Q3C the reversed form is a full table
scan, or a cross-match that probes the wrong table. Stored regions read and write as IVOA STC-S, and one
column can hold circles and polygons together.

The numbers come from randomized paired trials on PostgreSQL 18.6, on 10M-row
catalogues (synthetic, resampled from the Gaia DR3 density field, and real DR3
positions) and a 50M-row one; see [Performance](#performance).

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
`point <@ circle` — is left as one clause whose selectivity skycell supplies,
and a planner *custom scan* (`SkycellCone` in `EXPLAIN`) walks the covering's
ranges itself: one index scan per range in cell order when the heap follows the
cell order, or every range's TIDs into one bitmap, each heap page read once,
when it does not — chosen from the column's correlation statistic, as
PostgreSQL chooses between an index and a bitmap scan. A **cross-match** cone,
whose centre comes from another table's row, gets the same node parameterized
by that row on the inner side of a nested loop, each probe covered with the
ranges its own cone needs. Its row estimate is measured, not modelled: at plan
time skycell covers 32 sampled probes and counts the catalogue rows their cones
really hold (12,630 estimated against 12,390 actual). A probe's rescan is
costed as PostgreSQL costs its own index scan on the inner side of a nested
loop, the pages earlier probes have read counted as cached, so a cross-match
is weighed fairly against a parallel sequential scan. Since 0.36 the node is
parallel safe: with enough probes the cross-match runs in parallel workers, as a Gather
over the nested loop with a Parallel Seq Scan of the probes on its outer side,
each worker covering and scanning the cones of its own probes.

```
Nested Loop
  ->  Seq Scan on probe p
  ->  Custom Scan (SkycellCone) on cat c
        Filter: skycell_in_cone(ra, "dec", p.ra, p."dec", '0.0002777777777777778'::double precision, '-1'::double precision)
        Index: cat_cell_idx
        Ranges: per outer row
        Mode: ordered
```

**5. Polygons are rewritten by the planner.** A support function
(`SupportRequestSimplify`, the mechanism PostGIS uses for `ST_DWithin`) turns a
polygon or box predicate into B-tree range conditions plus the exact test
before path generation, so the planner estimates them from the same histogram.

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
geometry (`CONTAINS`, `POINT`, `CIRCLE`, `DISTANCE`, …); the rest of ADQL, such
as `TOP n` → `LIMIT n`, is the job of the TAP service's own ADQL parser:

```bash
python3 adql_geometry/python/skycell_adql_geometry.py \
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

**Checking it worked.** A cone should plan as `Custom Scan (SkycellCone)`
with a `skycell_in_cone` filter (a polygon as an index or bitmap scan with a
`skycell_in_*` filter):

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
`skyregion` (a cone, box or polygon). The `coordsys` argument is accepted
for ADQL compatibility and must be `'ICRS'`; convert other frames first with
`gal2icrs()` / `ecl2icrs()`.

### Geometry — ADQL's own spellings

| Function | Returns | Notes |
|---|---|---|
| `point(ra, dec)`<br>`point(coordsys, ra, dec)` | `skypos` | degrees. ADQL `POINT` |
| `circle(ra, dec, radius)`<br>`circle(coordsys, ra, dec, radius)` | `skyregion` | radius in degrees. ADQL `CIRCLE` |
| `box(ra, dec, width, height)`<br>`box(coordsys, …)` | `skyregion` | centre and extent, as ADQL defines it. Errors if it reaches a pole — use a polygon there. ADQL `BOX` |
| `polygon(ra1, dec1, ra2, dec2, …)`<br>`polygon(coordsys, …)` | `skyregion` | ≥ 3 vertices, convex or not (non-convex since 0.35), but simple: no two edges may cross or touch. Every vertex must lie within 90° of the vertices' mean. ADQL `POLYGON` |
| `contains(p skypos, r skyregion)`<br>`contains(a skyregion, b skyregion)` | `integer` (1/0) | ADQL `CONTAINS`. Compared with 1 (`contains(…) = 1`, or `1 = …`) in a `WHERE` or `JOIN ON` it is planned as the `<@` operator below, so it is indexed |
| `intersects(p skypos, r skyregion)`<br>`intersects(a skyregion, b skyregion)` | `integer` (1/0) | ADQL `INTERSECTS`. Compared with 1 it is planned as `<@` (point-region) or `&&` (region-region) below, so it is indexed |
| `distance(a skypos, b skypos)`<br>`distance(ra1, dec1, ra2, dec2)` | `float8` | degrees. ADQL `DISTANCE`, both forms. `distance(…) < r` (or `<=`) in a `WHERE` or `JOIN ON` is planned as the cone, like `<@` below |
| `area(r skyregion)` | `float8` | square degrees. ADQL `AREA` |
| `coord1(p)` / `coord2(p)` | `float8` | right ascension / declination, degrees |
| `centroid(r skyregion)` | `skypos` | ADQL `CENTROID`: a cone's centre, a polygon's centre of mass, which may lie outside a non-convex one, as a U's does. Before 0.35 a polygon's was the mean of its vertices |

If a bare name collides with `pg_catalog` (`point`, `circle`, `box`, `polygon`
all exist there), either install into a schema that precedes it in
`search_path`, or use the prefixed aliases: `skycell_point`, `skycell_circle`,
`skycell_box`, `skycell_polygon`.

### Operators — the indexable spelling

| Operator | Meaning |
|---|---|
| `skypos <@ skyregion` | position inside region — **the indexable ADQL `CONTAINS`**: against a circle (constant, or built per row as in a cross-match) it is planned exactly as `skycell_cone`, through the custom scan; against a polygon it is rewritten into index ranges |
| `skyregion @> skypos` | the same, reversed; when `skyregion` is a stored column with a GiST (or MOC GIN) index, indexed through that instead |
| `skyregion && skyregion` | regions overlap — indexed by `CREATE INDEX ON t USING gist (region)` |
| `skyregion @> skyregion` | region wholly contains region — the same index, either spelling |
| `skyregion <@ skyregion` | the same, reversed |

`<@`/`@>` against a circle and a plain `ra`/`dec` (or `skypos`) catalogue go
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
and the row estimate is the cone's (as of 0.28).
Against a constant polygon or box they go through the B-tree cell rewrite
described above. A `skyregion` column with its
own GiST index additionally indexes `&&` and both region-region containment
operators (`@>`/`<@`, in either argument order — they're commutators of each
other) directly against it, region side non-constant included — what a stored
footprint column (an ObsCore `s_region`, say) should carry. A per-row region
matched against a plain point catalogue needs no `skyregion` column of its own:
a per-row circle goes to the custom scan and a per-row polygon to the rewrite's
run-time range slots.

**Region opclasses.** `CREATE INDEX ON t USING gist (region)` picks
`skyregion_box4_gist_ops`: an axis-aligned 3D box rounded outward to `float4`
(24 bytes, pgSphere's own representation). It beats pgSphere's native GiST on
every region operator (see [Performance](#performance)). Two others can be
named explicitly:

```sql
CREATE INDEX ON t USING gist (region skyregion_gist_ops);      -- a few spherical caps per entry
CREATE INDEX ON t USING gist (region skyregion_box_gist_ops);  -- the float8 box, kept for comparison
```

`skyregion_gist_ops` keys each entry by spherical caps, exact for a circle at
any radius; it is the better choice for a column mixing very different region
scales (survey tiles alongside catalogue footprints), where a box is loose
around large circles. The opclasses can coexist on one column and the planner
picks per query; an expression index on `area(region)` lets it estimate the
column's region sizes. Region-region containment prunes with the `&&` test, so
it is sound but less selective than `&&` itself.

**"Which of my regions contain this point"** is often best served by a plain
GIN index over the region's MOC cells, which the planner uses automatically for
`region @> pos` / `pos <@ region`:

```sql
CREATE INDEX ON t USING gin (skycell_region_moc(region));
```

When both a GiST and this GIN index exist on the column, the GIN rewrite wins.

**Experimental point opclasses.** `skypos_spgist_ops` (SP-GiST over the HEALPix
hierarchy) and `skypos_cap_gist_ops` (GiST with a spherical-cap key) index a
`skypos` column directly. Neither beats the default cell B-tree; they are kept
for comparison.

### The index key

| Function | Returns | Notes |
|---|---|---|
| `skycell_ang2cell(ra, dec)` | `int8` | the order-29 HEALPix cell — **index this** |
| `skycell_cell(p skypos)` | `int8` | same, from a `skypos` column |
| `skycell_ang2pix(order, ra, dec)` | `int8` | the cell at any order |
| `skycell_cell_corners(order, pix)` | `float8[]` | the four corners, as ra, dec, … |

### Indexable predicates

| Function | Notes |
|---|---|
| `skycell_cone(cell, ra, dec, ra0, dec0, radius)` | cone test, answered by the custom scan: a constant cone covered at plan time; a per-row cone (a cross-match, `JOIN cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, r)`) covered per outer row; a generic plan or correlated sub-select covered per rescan |
| `skycell_poly(cell, ra, dec, poly float8[])` | the same for a polygon |
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
sequential scan. With it they take the custom scan, the same plan as `skycell_cone` and ADQL's
`point <@ circle`, and cross-match 20–33% faster than `q3c_join`.
`skycell_cone(cell, ra, dec, ra0, dec0, radius)` remains the canonical spelling
— one predicate covers both the cone search and the cross-match.

### Coverings, cross-matches and stored regions

| Function | Returns | Notes |
|---|---|---|
| `skycell_cone_ranges(ra0, dec0, radius [, tbl, col])` | `SETOF (lo, hi)` | the covering as index ranges, for hand-built plans (a `LATERAL` cross-match); the plain join through the custom scan is faster. Index with `skycell_cell_ops` (`CREATE INDEX ON t (skycell_ang2cell(ra, dec) skycell_cell_ops)`) so the planner can estimate such a range join |
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

NULL arguments to the epoch functions are taken as the catalogue says (since
0.32): a missing parallax means infinitely remote, a missing proper motion or
radial velocity is 0, and a missing position or epoch is an error — not a NULL
result, which would drop every star without a measured radial velocity.

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
| `skycell_density_build(tbl [, col, rows_per_cell, max_order])` | build a multi-order count map the covering reads instead of the histogram (**experimental**) |
| `skycell_density_drop(tbl [, col])` | remove it |

### Settings

| GUC | default | meaning |
|---|---|---|
| `skycell.custom_scan` | on | answer `skycell_cone` (and `skycell_join`, `skycell_radial_query`, and ADQL's `point <@ circle`) with the `SkycellCone` scan node instead of rewriting it into B-tree range conditions — constant cones, cross-match cones (a parameterized scan, one covering per outer row) and cones over parameters alone (generic plans, correlated sub-selects). A cone that is not a top-level `AND` term of a `WHERE`/`JOIN ON` (one under an `OR`, say) keeps the rewrite, so it stays indexable. Off restores the rewrite everywhere |
| `skycell.index_filter` | on | when the cell index also holds the position — `CREATE INDEX ON t ((skycell_ang2cell(ra, dec))) INCLUDE (ra, dec)` — the `SkycellCone` scan tests the cone on each index entry and fetches from the table only the rows inside it, instead of every row of the covering cells (both modes, constant cones and cross-matches). The plan's exact test still applies to every row, so the answer is the same; off reads every row of the covering cells |
| `skycell.range_cost` | -1 (derive) | price of one index range, in rows; -1 derives it from the relation's rows/page and the planner's cost factors (for the custom scan, also from how much of the relation `shared_buffers` holds) |
| `skycell.max_ranges` | 64 | cap on ranges per covering |
| `skycell.max_area_ratio` | 64 | a partly covered cell may not exceed this × the region's area |
| `skycell.use_stats` | on | use the histogram as a density map |
| `skycell.cache_coverings` | on | memoise coverings per backend |
| `skycell.join_slots` | 4 | range slots the rewrite emits for a non-constant region (used when the custom scan does not apply: `custom_scan` off, polygons and stored regions) |
| `skycell.exact_cells` | on | tight cell geometry; off falls back to the `max_pixrad` cap |
| `skycell.force_order` | -1 | diagnostics: cover cones at this order (-1 = let the model choose) |
| `skycell.split_cost`, `skycell.max_steps` | 1, 4000 | refinement guards for polygons and very large cones |
| `skycell.probe_orders` | 0 (off) | experimental: try this many orders past the closed-form choice and keep whichever scores lowest |
| `skycell.probe_split_cost` | 1.0 | experimental: cost per cell the order probe charges itself |
| `skycell.rewrite_max_waste` | 100 | decline the range rewrite for a constant region when the covering expects more than this many false-positive rows, scaled by how cache-resident the relation is — only when the relation also has a GiST or SP-GiST index on the position for the planner to use instead (without one, declining would leave a sequential scan) |
| `skycell.rewrite_waste_scale_cap` | 10 | upper bound, as a multiple of `rewrite_max_waste`, on how far that scaling may go for a relation far bigger than `shared_buffers` |

---

## Performance

Against Q3C 2.0.5 and pgSphere 1.5.2 on PostgreSQL 18.6, with skycell's
defaults throughout (ratio below 1 = skycell faster):

| workload | skycell ÷ pgSphere | skycell ÷ Q3C |
|---|---|---|
| cone searches, 1″–3°, warm | 0.67–0.96 | 0.02–0.58 |
| cone searches, 1″–3°, cold | 0.48–1.01 | |
| cross-matching, 0.5″–5″, real Gaia DR3 positions | 0.30–0.35 | 0.76–0.84 |
| convex polygons, 0.05°–2° | 0.23–0.37 | |
| stored regions: overlap and region-in-region (`&&`, `@>`, `<@` between regions) | 0.12–0.70 | |
| stored regions: footprints that contain a position (`@>` a point) | 0.57 on x86; 0.75–0.93 on an Apple M3 with 0.6 | |

The stored-region rows were re-measured on the x86 cloud host with skycell 0.34,
0.35 and 0.36, 8 runs each ([`bench/results-regions/`](bench/results-regions/); 0.36 as
`main` at b3bb7be, the same library code). Overlap and containment between regions kept their
margins there (0.11, 0.55, 0.22), and the v0.35 GiST key changed none of them.
Point-in-footprint was within 5% of pgSphere with 0.34 and 0.35 (0.98 and
0.95): the index converted the query point to a unit vector at every entry it
visited. Since 0.36 the point is converted once per index scan, and it takes
0.57 (interval 0.55–0.60) of pgSphere's time.

Its index is a third the size of pgSphere's (214 MB against 685 MB at 10M
rows, about the size of Q3C's) and builds 11–23× faster, and its row estimates
stay within 1.3× of the truth where pgSphere's are off by up to 8.7×. At Gaia
DR3 scale that is one 38 GiB index where an archive running pgSphere for cones
and Q3C for cross-matching keeps 160 GiB.

**Parallel cross-matching.** The comparisons above, like the paper's, are
serial. They run with `max_parallel_workers_per_gather = 0` for every method.
With parallelism on, a cross-match with enough probes runs in parallel
workers. The setup:

- PostgreSQL 18.6 with its default settings, 4 cores.
- 5M catalogue rows, the heap stored once in cell order and once at random.
- 200k and 1M probes on sources.
- Median of 3 warm runs, against v0.35, which cannot run a cross-match in
  parallel. Every result is identical to the serial one.

| probes | cone | serial | default workers | 3 workers |
|---|---|---|---|---|
| 200k (10 MB) | 1″–30″ | 1.4–18.2 s | 1 worker: 1.8–2.2× faster | — |
| 1M (50 MB) | 1″, cell order | 8.4 s | 2 workers: 2.9× | 3.7× |
| 1M | 30″, cell order | 20.9 s | 3.3× | 3.9× |
| 1M | 1″, random order | 6.9 s | 2.7× | 3.6× |
| 1M | 30″, random order | 92.0 s | 3.4× | 4.6× |

"3 workers" sets the probe table's `parallel_workers`, so the leader and three
workers use the four cores. Q3C's and pgSphere's cross-matches can run under a
parallel nested loop too. They were not measured that way, so these figures
compare skycell with itself only.

The benchmark scripts behind the paper describing skycell, and the
documentation of every measurement it quotes, are in [`bench/`](bench/):
[`bench/README.md`](bench/README.md) maps each table to its directory, and
[`bench/REPRODUCING.md`](bench/REPRODUCING.md) gives the hosts, settings and run
order. The paper itself, the raw per-query results, the runs of earlier versions
and the design notebook (`GIST_REGION_DESIGN.md`) that the "Round …" references
in the source comments point to are in
[skycell-paper](https://github.com/jesusjuansalgado/skycell-paper).

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
  order-29 cell inside one of the ranges. Then the awkward places: 7,552
  polygons at the poles, zone boundaries, RA wrap-around and face edges (904,809
  points inside, half within a part per billion of an edge), and 2 × 30,000 cones
  with 200 points each. Then 1,200 non-convex polygons, from 0.2″ to 55°: L, U,
  comb and random star shapes, in both orientations, at the same places. Each
  is checked against a reference that shares none of its code: a crossing-number
  test in a plane of the test's own choosing, plus a fan sum for the area. That
  covers 444,683 points, half of them a hair either side of an edge, and each
  point's membership and covering, the area, the centroid (against brute-force
  quadrature, every quarter of the shapes) and CONTAINS and INTERSECTS for
  14,400 small triangles around the notches. It also prints ranges, cells examined, false-positive
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
- `polygon`: non-convex polygons, checked against things that do not share
  their code: a U is written again as three convex pieces, and must hold
  exactly the pieces' rows, with the pieces' total area. Points in the notch,
  a cone in the notch and a bridge across it (every vertex inside, an edge
  outside) are tested, along with self-crossing, self-touching and
  doubled-back input. The exact test, the range rewrite and the indexed and
  sequential operator must agree for a U, a comb across RA = 0, a pac-man
  over the pole, a star at the zone boundary and an arcsecond-sized U. Every
  GiST region operator class must give the sequential answer for regions
  that are non-convex polygons.
- `parallel`: parallel queries, with parallelism made free so that the
  planner uses it wherever it can.
  - A cross-match is planned as a Gather over a Parallel Seq Scan of the
    probes, with the custom scan in ordered and bitmap mode and with the index
    filter. It must give the serial answer as a join, with a radius column, as
    a `LEFT JOIN`, an `EXISTS` and an ADQL `CONTAINS`.
  - Workers are launched, and the rows the index filter removes in them add up
    to the serial count.
  - Under `debug_parallel_query` a whole cone search runs inside a worker, in
    both modes, with the index filter, as a cross-match and as a generic plan's
    run-time cone, and returns the serial answer.

**Reference translators** — [`adql_geometry/tests/`](adql_geometry/tests/): the
Python translator's tests (pytest), and `cross_check.py`, which requires the
Python and Java translators to produce identical SQL for every case.

[CI](.github/workflows/ci.yml) runs all of it on every push to `main` and every
pull request: the SQL regression on PostgreSQL 16, 17 and 18 (packages from
apt.postgresql.org, a fresh cluster with default settings; the steps are
[`.github/ci/pg-regress.sh`](.github/ci/pg-regress.sh), which runs as well
locally), the standalone selftests, and the reference translators.

## Limitations

- **Geometry.** A polygon must be simple, with no crossing or touching edges,
  and every vertex must lie within 90° of the vertices' mean. There is no
  `REGION` and no union of regions. A non-convex polygon costs more than a
  convex one. Parsing a U or a comb takes 2–6 µs against 0.6 µs for a convex
  quadrilateral, and each step of its covering is 2–3× dearer. The ranges it
  yields are as few, so the rows read do not change. Setting one up is
  quadratic in its vertices: 10 ms for 1,000 vertices, about 1 s for 10,000.
  A constant polygon is set up once per query, a stored one once per row.
- **No KNN ordering yet.** `ORDER BY pos <-> point(…) LIMIT k` is not indexed.
  A correlated sub-select with `ORDER BY skycell_dist(…) LIMIT 1` over a cone
  is indexed (the custom scan covers the cone per rescan), so use that instead.
- **Where the custom scan applies.** It answers cones and cross-matches; polygons
  and stored regions use the range rewrite and the region indexes. The cone must
  be a top-level `AND` term of a `WHERE` or `JOIN ON`: a cone under an `OR` is
  rewritten instead and stays indexable through a `BitmapOr`. It reads heap
  tables only; on a partitioned table each partition keeps the rewrite. Before
  PostgreSQL 18 its bitmap mode does not prefetch.
- **Parallel queries.** The custom scan is parallel safe but not parallel
  aware. A cross-match's probes are shared out among workers, but a single
  cone search is not split among them. PostgreSQL gives the probe table workers
  by its size: none below 8 MB (`min_parallel_table_scan_size`), one at 10 MB,
  two at 50 MB. A smaller target list runs serially unless told otherwise:
  `ALTER TABLE targets SET (parallel_workers = 3)`. In a parallel `EXPLAIN
  ANALYZE` the average of ranges per covering is the leader's own, labelled
  so, since a node that is not parallel aware gets no shared memory to collect
  the workers' figures; the rows removed by the index filter are counted in
  every process.
- **Planning costs.** A covering is computed per query: 0.036 ms at 1″, against
  0.029 ms for pgSphere's whole plan, so on a small table (0.5M rows) a small
  cone can still favour pgSphere. skycell's win at small radii comes from
  execution.
- **Plan-time sampling for cross-matches.** To estimate a cross-match's rows,
  the planner reads up to 32 probe rows and up to 4000 catalogue rows through
  the index — about 2–5 ms of planning, more on a cold cache. It applies when
  the probe position is a plain column and the cones are small; otherwise the
  estimate falls back to the density model, which can be far too low for
  probes that sit on catalogue sources.
- **The density estimate is coarse.** An equi-depth histogram cannot see a
  cluster smaller than one of its buckets; `max_area_ratio` bounds the damage
  rather than fixing it.

## License

MIT. See [LICENSE](LICENSE).
