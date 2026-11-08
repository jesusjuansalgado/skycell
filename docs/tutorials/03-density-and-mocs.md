# 3. Density maps, counts and MOCs

How many sources lie in a field without counting them; exact counts per
HEALPix cell kept in the database; coverage maps (IVOA MOC 2.0) out of the
database for MOCPy, Aladin or the CDS MOC server, and back in as search
regions. Every block runs as written, in order, on an empty database.

```sql
CREATE SCHEMA sky;
CREATE EXTENSION skycell SCHEMA sky;
ALTER DATABASE skycell_tutorial SET search_path = sky, public, pg_catalog;
ALTER DATABASE skycell_tutorial SET session_preload_libraries = 'skycell';
```

```sql
SELECT setseed(0.3);
CREATE TABLE cat AS
SELECT i AS source_id,
       CASE WHEN i % 10 < 6 THEN 360 * random()
            ELSE mod(((i % 300) * 47.3 + (random() - 0.5) * 0.6 + 360)::numeric, 360)::float8 END AS ra,
       CASE WHEN i % 10 < 6 THEN degrees(asin(2 * random() - 1))
            ELSE greatest(-89.9, least(89.9, (i % 300) * 0.57 - 85 + (random() - 0.5) * 0.6)) END AS dec,
       10 + 10 * random() AS mag
FROM generate_series(1, 200000) i;
CREATE INDEX cat_cell ON cat (skycell_ang2cell(ra, dec));
ALTER INDEX cat_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE cat;
```

## Estimates without reading a row

A sky viewer zoomed out over millions of sources needs how many lie in the
view, not the sources. `skycell_count_estimate` integrates the density map
`ANALYZE` keeps for the cell index over a region, in a fraction of a
millisecond, without touching the table:

```sql
SELECT skycell_count_estimate('cat', CIRCLE(47.3, -84.43, 1)) AS estimate,
       (SELECT count(*) FROM cat WHERE DISTANCE(POINT(ra, dec), POINT(47.3, -84.43)) < 1) AS exact;
```

The table is named as text (a schema-qualified name works too), so the call
fits in ADQL. The estimate is good where a region holds many histogram
buckets' worth of rows (with a statistics target of 1000, a bucket is a
thousandth of the table) and only indicative below that.

Per HEALPix cell, for a map: one cell, or every cell of an order that a
region meets, with its estimate and whether it lies wholly inside:

```sql
SELECT skycell_count_estimate('cat', 5, 4885) AS one_cell;
SELECT ipix, round(estimate) AS n, inside
FROM skycell_density_estimate('cat', CIRCLE(47.3, -84.43, 3), 5) ORDER BY ipix LIMIT 5;
```

## Exact counts per cell, kept in the database

Where the counts must be exact, `skycell_density_view` makes a materialised
view of the rows in every occupied cell at each order, from one pass over
the table (each coarser order is summed from the finer one):

```sql
SELECT skycell_density_view('cat', 'cat_density', 8, 0);   -- orders 0 to 8; returns its rows
SELECT hpx_order, count(*) AS cells, sum(n) AS sources
FROM cat_density GROUP BY 1 ORDER BY 1 LIMIT 3;
```

Refresh it after a load (`REFRESH MATERIALIZED VIEW cat_density`). The cells
of a view are selected with `skycell_cell_overlaps`, which the planner turns
into ranges on the view's index:

```sql
EXPLAIN (COSTS OFF)
SELECT ipix, n FROM cat_density
WHERE hpx_order = 6 AND skycell_cell_overlaps(6, ipix, CIRCLE(47.3, -84.43, 2)) = 1;
-- expect: Index Scan on cat_density_cell
```

Each cell's count is exact; a view's total counts its edge cells whole, so
for the number of sources in the view itself the estimate above is the
better figure.

## MOCs out

A region's MOC, a whole table's coverage (read from its cell index, without
the table), or the coverage of any query's rows (an aggregate, which runs in
parallel on a large table), in MOC 2.0's ASCII or JSON form:

```sql
SELECT skycell_moc(CIRCLE(10, 10, 1), 6) AS region_moc;
SELECT skycell_moc(CIRCLE(10, 10, 1), 5, 'json') AS region_moc_json;
SELECT left(skycell_coverage_moc('cat', 6), 60) AS table_coverage;
SELECT left(skycell_moc_agg(7, skycell_ang2pix(7, ra, dec)), 60) AS bright_coverage
FROM cat WHERE mag < 10.1;
```

A region's MOC holds every cell the region touches, and few others; a
table's or a query's, every cell that holds a row. MOCPy reads them back
unchanged.

## MOCs in, as search regions

A MOC, from another survey or an archive, is a search region like any other,
in ADQL or as text in either form:

```sql
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cat WHERE CONTAINS(POINT(ra, dec), REGION('MOC 5/4885 6/19548-19549')) = 1;
-- expect: Custom Scan (SkycellCone) on cat
SELECT count(*) FROM cat
WHERE CONTAINS(POINT(ra, dec), skycell_moc_region('{"5": [4885, 4887, 4928, 4930]}')) = 1;
```

A position is inside when its cell is: the answer is exact to the MOC's
cells, which is what a MOC means.

## Combining MOCs and footprints

Union, intersection, difference and complement of MOCs, exact along their
cells; a MOC degraded to a coarser order; and any region as a MOC at an
order:

```sql
SELECT skycell_moc_union(skycell_moc_region('5/1-3'), skycell_moc_region('5/3-6')) AS u,
       skycell_moc_intersection(skycell_moc_region('5/1-3'), skycell_moc_region('5/3-6')) AS i,
       skycell_moc_difference(skycell_moc_region('5/1-6'), skycell_moc_region('5/3')) AS d,
       skycell_moc_degrade(skycell_moc_region('6/19548-19549'), 4) AS coarser,
       skycell_moc_region(CIRCLE(10, 10, 0.5), 7) AS circle_as_moc;
```

Over a table of footprints, `skycell_moc_union_agg` gives a collection's
coverage in one query, in parallel; the result is a region, so it can be
stored, indexed or searched with:

```sql
CREATE TABLE fields AS
SELECT i AS field_id, circle(40 + 10 * random(), -85 + 2 * random(), 0.2) AS footprint
FROM generate_series(1, 200) i;
SELECT left(skycell_moc(skycell_moc_union_agg(footprint, 8), 8), 60) AS coverage FROM fields;
-- the sources inside the survey's coverage
SELECT count(*) FROM cat
WHERE CONTAINS(POINT(ra, dec), (SELECT skycell_moc_union_agg(footprint, 8) FROM fields)) = 1;
```
