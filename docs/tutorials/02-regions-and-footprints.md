# 2. Regions and footprints: set-up and queries

Tables whose rows are regions rather than positions: observation footprints
(an ObsCore `s_region` column), survey tiles, fields of view. Which ones
contain a position, which overlap a search field, which sources lie inside
one. Every block runs as written, in order, on an empty database.

```sql
CREATE SCHEMA sky;
CREATE EXTENSION skycell SCHEMA sky;
ALTER DATABASE skycell_tutorial SET search_path = sky, public, pg_catalog;
ALTER DATABASE skycell_tutorial SET session_preload_libraries = 'skycell';
```

(Why the schema and the preload: [tutorial 1](01-source-catalogue.md#installing-into-a-schema-of-its-own).)

## The `skyregion` type

One type holds every kind of region, so one column can mix them: circles,
polygons (convex or not), boxes, MOCs, and unions, intersections,
differences and complements of any of these, nested. They are written and
read as IVOA STC-S, which is what `s_region` columns already hold:

```sql
SELECT REGION('CIRCLE ICRS 150.0 2.2 0.05') AS circle,
       REGION('POLYGON ICRS 53.10 -27.85 53.14 -27.85 53.14 -27.81 53.10 -27.81') AS polygon,
       -- two detectors either side of a gap
       REGION('UNION ICRS (POLYGON 53.10 -27.85 53.14 -27.85 53.14 -27.81 53.10 -27.81
                           POLYGON 53.15 -27.85 53.19 -27.85 53.19 -27.81 53.15 -27.81)') AS mosaic,
       -- a coronagraph: the field less its mask
       REGION('DIFFERENCE ICRS (BOX 150.0 2.2 0.05 0.05 CIRCLE 150.0 2.2 0.004)') AS coronagraph;
```

or built in SQL, from ADQL's constructors and skycell's combinators:

```sql
SELECT skycell_union(box(210.80, 54.35, 0.036, 0.036), box(211.00, 54.35, 0.036, 0.036)) AS parallels,
       skycell_difference(circle(10, 20, 2), circle(10, 20, 1)) AS annulus,
       skycell_not(circle(266.4, -28.9, 10)) AS away_from_the_centre,
       AREA(skycell_difference(circle(10, 20, 2), circle(10, 20, 1))) AS annulus_sq_deg,
       CENTROID(polygon(10, 10, 12, 10, 11, 11, 12, 12, 10, 12)) AS centre_of_mass;
```

## A footprint table

Five thousand observations, as an archive gets them: STC-S text in
`s_region`, a mix of circles, single fields and two-detector mosaics.

```sql
SELECT setseed(0.2);
CREATE TABLE obscore AS
SELECT 'obs' || i AS obs_id, ra0, dec0,
  CASE i % 3
   WHEN 0 THEN format('CIRCLE ICRS %s %s %s', ra0, dec0, 0.05 + 0.2 * random())
   WHEN 1 THEN format('POLYGON ICRS %s %s %s %s %s %s %s %s',
                      ra0 - 0.1, dec0 - 0.1, ra0 + 0.1, dec0 - 0.1, ra0 + 0.1, dec0 + 0.1, ra0 - 0.1, dec0 + 0.1)
   ELSE format('UNION ICRS (POLYGON %s %s %s %s %s %s %s %s POLYGON %s %s %s %s %s %s %s %s)',
               ra0 - 0.1, dec0 - 0.05, ra0 - 0.005, dec0 - 0.05, ra0 - 0.005, dec0 + 0.05, ra0 - 0.1, dec0 + 0.05,
               ra0 + 0.005, dec0 - 0.05, ra0 + 0.1, dec0 - 0.05, ra0 + 0.1, dec0 + 0.05, ra0 + 0.005, dec0 + 0.05)
  END AS s_region
FROM (SELECT i, 10 + 20 * random() AS ra0, -5 + 10 * random() AS dec0
      FROM generate_series(1, 5000) i) x;
```

Convert the column to `skyregion` (each value is parsed once, and a malformed
one is reported here rather than at query time) and index it:

```sql
ALTER TABLE obscore ALTER COLUMN s_region TYPE skyregion USING REGION(s_region);
CREATE INDEX obscore_region ON obscore USING gist (s_region);
CREATE INDEX ON obscore (obs_id);
ANALYZE obscore;
```

A table that must keep `s_region` as text for other software can add a
`skyregion` column beside it, or a generated one:
`ALTER TABLE obscore ADD COLUMN region skyregion GENERATED ALWAYS AS (REGION(s_region)) STORED`.

## Searching the footprints

The four questions an archive asks of its footprints, in ADQL, each through
the GiST index:

```sql
-- which observations cover this position
EXPLAIN (COSTS OFF) SELECT obs_id FROM obscore WHERE CONTAINS(POINT(20, 0), s_region) = 1;
-- expect: obscore_region
-- which observations overlap this field
EXPLAIN (COSTS OFF) SELECT obs_id FROM obscore WHERE INTERSECTS(s_region, CIRCLE(20, 0, 0.3)) = 1;
-- expect: obscore_region
-- which lie wholly inside it
EXPLAIN (COSTS OFF) SELECT obs_id FROM obscore WHERE CONTAINS(s_region, CIRCLE(20, 0, 1)) = 1;
-- expect: obscore_region
-- which contain the whole field
EXPLAIN (COSTS OFF) SELECT obs_id FROM obscore WHERE CONTAINS(CIRCLE(20, 0, 0.01), s_region) = 1;
-- expect: obscore_region
```

The detector gaps count: a position in the gap of a mosaic is not covered by
it, and a field that falls in the gap does not overlap it.

```sql
SELECT count(*) FROM obscore
WHERE INTERSECTS(s_region, REGION('UNION ICRS (CIRCLE 20 0 0.5 CIRCLE 25 0 0.5)')) = 1;
```

With other tables: the observations covering each of a list of targets, and
the observations that overlap one of them:

```sql
CREATE TABLE targets AS
SELECT i AS id, 10 + 20 * random() AS ra, -5 + 10 * random() AS dec FROM generate_series(1, 2000) i;
ANALYZE targets;
EXPLAIN (COSTS OFF)
SELECT t.id, o.obs_id FROM targets t JOIN obscore o ON CONTAINS(POINT(t.ra, t.dec), o.s_region) = 1;
-- expect: Index Scan using obscore_region
SELECT b.obs_id FROM obscore a JOIN obscore b ON INTERSECTS(a.s_region, b.s_region) = 1
WHERE a.obs_id = 'obs7' AND b.obs_id <> a.obs_id;
```

## Choosing the region index

- **The default GiST** (`USING gist (s_region)`, operator class
  `skyregion_box4_gist_ops`) keys each region by a small 3D box. It serves
  every operator above and suits a column of regions of similar size.
- **Spherical caps** (`USING gist (s_region skyregion_gist_ops)`) key each
  region by a few caps, exact for a circle of any size. Prefer it where very
  different scales share a column: survey tiles of degrees beside footprints of
  arcseconds. An index on `area(s_region)` lets the planner see that spread.
- **MOC cells** (`USING gin (skycell_region_moc(s_region))`) index each
  region's cells. It answers "which regions contain this position" by exact
  lookups of the position's cells, and is often the fastest for that question
  alone, joins of many targets in particular.

They can coexist on one column, and the planner picks per query:

```sql
CREATE INDEX obscore_moc ON obscore USING gin (skycell_region_moc(s_region));
ANALYZE obscore;
EXPLAIN (COSTS OFF)
SELECT t.id, o.obs_id FROM targets t JOIN obscore o ON CONTAINS(POINT(t.ra, t.dec), o.s_region) = 1;
-- expect: obscore_moc
```

## Sources inside a footprint

With a catalogue indexed as in [tutorial 1](01-source-catalogue.md), the
sources inside a stored footprint are a search of the catalogue with that
region:

```sql
CREATE TABLE cat AS
SELECT i AS source_id, 10 + 20 * random() AS ra, -5 + 10 * random() AS dec, 10 + 10 * random() AS mag
FROM generate_series(1, 200000) i;
CREATE INDEX cat_cell ON cat (skycell_ang2cell(ra, dec));
ANALYZE cat;
```

Write the footprint as a scalar sub-query: it is fetched once, and the
catalogue is searched with it through its index.

```sql
EXPLAIN (COSTS OFF)
SELECT c.source_id FROM cat c
WHERE CONTAINS(POINT(c.ra, c.dec), (SELECT s_region FROM obscore WHERE obs_id = 'obs42')) = 1;
-- expect: Bitmap Index Scan on cat_cell
```

Written as a join instead (`FROM cat c JOIN obscore o ON CONTAINS(POINT(c.ra,
c.dec), o.s_region) = 1 WHERE o.obs_id = 'obs42'`), the planner currently
underestimates the indexed plan and may read the whole catalogue: prefer the
sub-query form for one footprint. For counts per footprint over many
footprints, the GiST join above, from the catalogue's side, is the one to
use:

```sql
SELECT o.obs_id, count(*) AS n
FROM cat c JOIN obscore o ON CONTAINS(POINT(c.ra, c.dec), o.s_region) = 1
WHERE c.mag < 10.05
GROUP BY o.obs_id ORDER BY n DESC LIMIT 5;
```

The union of several footprints, as one region, is a search too (MOCs are
covered in [tutorial 3](03-density-and-mocs.md)):

```sql
SELECT count(*) FROM cat c
WHERE CONTAINS(POINT(c.ra, c.dec),
               (SELECT skycell_union(VARIADIC array_agg(s_region)) FROM obscore WHERE obs_id IN ('obs1', 'obs2', 'obs3'))) = 1;
```
