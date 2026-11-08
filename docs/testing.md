# Testing

*How skycell is checked: the brute-force self-tests of the coverings, the SQL regression suite, and the cross-checks against reference implementations.*


Two layers, both in [`ext/test/`](../ext/test/), and a performance check.

**Performance, against the last release** — `make perfcheck` in `ext/`, on a
development server with the benchmark database (about three minutes;
[`bench/perfcheck/`](../bench/perfcheck/)). The tests below check answers, not
speed: 0.44 shipped a nearest-neighbour search a third slower at k = 1 that
none of them could see. The check installs the library just built and the
latest release tag in turn, over 20 alternating rounds, times ten short
workloads (cones, a polygon, an annulus, a cross-match, nearest neighbours,
a partitioned cone and cross-match) at 30 centres, and fails when one is more
than 10% slower with 95% confidence; both libraries must return the same
rows. Run against v0.42, it flags v0.44's regression (1.21 [1.13, 1.30]).

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
  14,400 small triangles around the notches. Then 300 wide polygons, the same
  shapes stretched to about 70° from their centre with one edge cut into 60
  more vertices, so that their vertex mean no longer holds them: membership,
  covering and area against the same reference. It also prints ranges, cells examined, false-positive
  fraction and µs per covering. Then 1,500 random composites (unions,
  intersections, differences and NOTs, up to four deep, of cones and convex and
  non-convex polygons): every catalogue point inside must be covered, and every
  cell class must be honest for 128 sampled points per cell, half on cell edges.
  Then 1,200 pairs of a composite and a cone, polygon or composite nearby,
  half of them built as SQL's membership path builds them (without the
  covering tables). Sampled points check every answer: a point in both
  regions means they overlap, a point of the first outside the second means
  it is not contained, and every non-empty region holds itself
  (`SC_SELFTEST_PAIRS=1 SC_SELFTEST_NPAIR=20000` runs only these, more of
  them: 0 wrong in 20,000). Then AREA and CENTROID of 300 composites against
  what shares none of their code: a circle's area and moment in closed form,
  annuli, inclusion–exclusion (A + B = A ∪ B + A ∩ B) for circles and for a
  polygon with its exact area, and a union against its complement; each within
  the bounds the integration reports (`SC_SELFTEST_INT=1`, with
  `SC_SELFTEST_NINT`, runs only these). Current run: **0 false negatives**.

**SQL regression** — `make installcheck` in `ext/`, against a running server:

- `skycell`: every indexed query must return exactly what the exact predicate
  returns on a sequential scan — no false negatives, no false positives — on a
  clustered catalogue with clusters at both poles and across RA = 0: cones,
  polygons, the Q3C-shaped spellings, cross-matches, MOC-stored regions and the
  GiST region operator classes.
- `adql`: the ADQL surface — the standard's spelling and the operator form,
  each indexed (under `OR` and in correlated sub-selects too), and the IVOA
  UDF astronomy functions.
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
  - A single cone, a 40° cone and a composite are planned parallel aware
    under a Gather, in both modes, and give the serial answer, also when a
    nested loop rescans the Gather.
- `nearest`: the nearest-neighbour scan. Every query must return what
  PostgreSQL's sort returns, the same distances in the same order and the same
  rows (ties aside): on a catalogue with duplicated positions, NULL
  coordinates and a knot past RA = 0 at the pole, its heap in and out of cell
  order, for centres at both poles and either side of RA = 0, k from one row
  to past the relation (NULL distances last), every spelling of the distance,
  with conditions that take several rings, a `work_mem` small enough that a
  ring spills to a tuplesort, a NULL or invalid centre, a generic plan whose
  centre and `LIMIT` are parameters, `LATERAL` per outer row, and inside a
  parallel worker. The spellings it does not take (no `LIMIT`, `DESC`, a
  system column, a centre that reads the row) must keep the sort. A stored
  `cell` column serves the nearest scan, ADQL's `contains()` and `distance()`,
  `skycell_radial_query` and `skycell_join` when it is generated or carries a
  validated `CHECK` with `NOT NULL`, with the plain predicates' answers, and
  not when the `CHECK` lacks `NOT NULL`, is not validated, not enforced or
  names another expression, or once it is dropped. A partitioned catalogue
  (declination bands, one attached with its columns in another order, one
  split again by right ascension, a default partition for NULL declinations,
  one band without the index) must give the sort's answer through a Merge
  Append of the partitions' nearest scans, pruned or not.

- `partition`: catalogues partitioned by cell range (with an empty partition)
  and by declination band (sub-partitioned, a default partition, a partition
  without the index), and a table holding one patch of sky. Constant cones,
  polygons, boxes, composites, distance comparisons and cross-matches must
  give the unpartitioned table's answers (the patch's, with the custom scan
  off), and every indexed partition must offer the custom scan. On the
  catalogue partitioned by cell range (and on one partitioned by a stored cell
  column), a constant cone must be pruned at plan time to its partition and a
  cross-match must run only the partition holding its probes; every cell of a
  cone must lie within `skycell_cone_cell_lo/hi`.
- `estimate`: row-count and density estimates against exact counts on a
  catalogue ANALYZE reads whole; density views at several orders against
  `GROUP BY` counts, their refresh, and `skycell_cell_overlaps` with its
  index rewrite against testing every cell.
- `moc`: MOCs of cones, polygons, composites, the whole sky and nothing, in
  ASCII and JSON; the max-order cells of a region's MOC must be exactly those
  `skycell_cell_overlaps` counts; the aggregate's merging of duplicates and
  siblings; a table's coverage by expression index and by stored cell
  column, and of an empty table; out-of-range orders and formats. The
  aggregate in parallel (workers only) must give the one-process MOC at
  orders 0–12, also when workers see no row with a cell.
- `moc_region`: MOCs read from ASCII, MOC 1.0 commas and JSON, normalised;
  in STC-S and composites; AREA and CENTROID; malformed MOCs refused. On a
  catalogue, nine MOCs (single cells, mixed orders, a pole, the whole sky,
  nothing) must give the rows whose cell is in them, counted independently,
  by the custom scan, the range rewrite and a sequential scan, and in a
  DIFFERENCE, INTERSECTION and NOT the boolean combination of the tests.
  Relations between MOCs, and with cones as the cones' own MOCs answer them;
  stored MOCs under GiST against a sequential scan; estimates; partition
  pruning. The set operations (0.46) on chosen MOCs, the identities they must
  keep (a split by b and joined again is a; a with its complement is the sky;
  inclusion–exclusion of areas), the union aggregate over 3,000 footprints as
  MOCs and as circles, in parallel and over no rows; operations refused on a
  region that is not a MOC.
- `composite`: unions, intersections, differences and NOTs. STC-S in and
  out (every malformed form errors with a reason), the constructors, each
  composite's membership against the boolean combination of its pieces,
  region-to-region answers that can be checked by hand (a circle in an
  annulus's hole, one across the gap between a union's pieces, nested NOTs),
  AREA and CENTROID against exact values, and STC-S `BOX`. A constant composite on the catalogue index (eight fixed
  shapes and 40 random ones) must give the exact test's and the sequential
  scan's rows, and take the custom scan as a top-level condition (from a
  `skypos` column or from `ra`/`dec`, alone or with another condition) and the
  range rewrite under an `OR` or with the custom scan off; stored composites must give the sequential answer under each
  region index, for a point, a join, ADQL `CONTAINS` and a correlated
  sub-select, and for overlap and containment against cones, polygons and
  composites (`&&`, `@>`, `<@`, ADQL `INTERSECTS` and `CONTAINS`). Every pair
  of stored regions must overlap symmetrically, and each non-empty one must
  hold and meet itself.
- Correlated sub-selects (in `adql`): an outer query's column used as the
  point, the centre or a polygon vertex in a sub-select must plan, use the
  index and give the join's answer.

**Reference translators** — [`adql_geometry/tests/`](../adql_geometry/tests/): the
Python translator's tests (pytest), and `cross_check.py`, which requires the
Python and Java translators to produce identical SQL for every case.

[CI](../.github/workflows/ci.yml) runs all of it on every push to `main` and every
pull request: the SQL regression on PostgreSQL 16, 17 and 18 (packages from
apt.postgresql.org, a fresh cluster with default settings; the steps are
[`.github/ci/pg-regress.sh`](../.github/ci/pg-regress.sh), which runs as well
locally), the standalone selftests, and the reference translators.

