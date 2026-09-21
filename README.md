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

**So: on a Gaia-sized catalogue this index improves degree-scale selections,
polygons and cross-matches — not small cone searches.** Removing the plan-time
cost (an SP-GiST opclass, or the `= ANY` single-scan form) is what would change
that.

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
both from one. **That is 4× less memory** — and residency is what decides
whether you get the warm or the cold numbers, which differ by 2–3×.

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
- Better row estimates are demonstrated; better *plans* are not. On the join
  shapes measured, all three methods chose the same strategy — only one query
  in sixteen at 3° crossed a threshold into a hash join.

## License

MIT. See [LICENSE](LICENSE).
