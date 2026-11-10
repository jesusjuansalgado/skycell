# 2. Regions and footprints: set-up and queries

Tables whose rows are regions rather than positions: observation footprints
(an ObsCore `s_region` column), survey tiles, fields of view. Which ones
contain a position, which overlap a search field, which sources lie inside
one. Every block runs as written, in order, on an empty database.

```sql
CREATE EXTENSION skycell;
```


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
  region's cells, and answer "which regions contain this position" only, by
  lookups of the position's cells. It is used when the column has no GiST;
  beside one, the GiST is used, being faster: on 50,000 footprints, 500
  positions take 4 ms through the default GiST, 12–17 ms through the GIN.

The two GiST classes can coexist on one column, and the planner picks per
query. A footprint table with only the GIN index:

```sql
CREATE TABLE obscore_g AS SELECT obs_id, s_region FROM obscore;
CREATE INDEX obscore_moc ON obscore_g USING gin (skycell_region_moc(s_region));
ANALYZE obscore_g;
EXPLAIN (COSTS OFF)
SELECT t.id, o.obs_id FROM targets t JOIN obscore_g o ON CONTAINS(POINT(t.ra, t.dec), o.s_region) = 1;
-- expect: obscore_moc
DROP TABLE obscore_g;
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

Join the footprints to the catalogue: each footprint row's region is covered
as the scan runs, and the catalogue is searched through its index, once per
footprint, as a cross-match searches it once per target:

```sql
EXPLAIN (COSTS OFF)
SELECT c.source_id FROM cat c JOIN obscore o ON CONTAINS(POINT(c.ra, c.dec), o.s_region) = 1
WHERE o.obs_id = 'obs42';
-- expect: Ranges: per outer row
-- the same with the footprint as a sub-query
EXPLAIN (COSTS OFF)
SELECT c.source_id FROM cat c
WHERE CONTAINS(POINT(c.ra, c.dec), (SELECT s_region FROM obscore WHERE obs_id = 'obs42')) = 1;
-- expect: cat_cell
```

Counts per footprint, over a few footprints or many; a footprint column kept
as STC-S text works too, read with `REGION()`:

```sql
SELECT o.obs_id, count(c.source_id) AS n
FROM obscore o LEFT JOIN cat c ON CONTAINS(POINT(c.ra, c.dec), o.s_region) = 1
WHERE o.obs_id IN ('obs1', 'obs2', 'obs3')
GROUP BY o.obs_id ORDER BY o.obs_id;
```

The union of several footprints, as one region, is a search too (MOCs are
covered in [tutorial 3](03-density-and-mocs.md)):

```sql
SELECT count(*) FROM cat c
WHERE CONTAINS(POINT(c.ra, c.dec),
               (SELECT skycell_union(VARIADIC array_agg(s_region)) FROM obscore WHERE obs_id IN ('obs1', 'obs2', 'obs3'))) = 1;
```
