# 1. A source catalogue: set-up and queries

From a table of positions to indexed cone searches, polygon and region
searches, cross-matches and nearest neighbours. Every block below runs as
written, in order, on an empty database (`check.py` in this directory runs
them).

## Installing into a schema of its own

ADQL's names (`POINT`, `CIRCLE`, `BOX`, `POLYGON`) collide with PostgreSQL's
own geometric `point`, `circle`, `box` and `polygon`. Install skycell into a
schema and put it before `pg_catalog`, so that the bare ADQL spellings resolve
to skycell's:

```sql
CREATE SCHEMA sky;
CREATE EXTENSION skycell SCHEMA sky;
-- your database's name here; new sessions pick these up
ALTER DATABASE skycell_tutorial SET search_path = sky, public, pg_catalog;
ALTER DATABASE skycell_tutorial SET session_preload_libraries = 'skycell';
```

The second setting (a superuser's) loads skycell's library when a session
starts. Without it the library loads during the first query that uses it,
after that query's planning has begun, and that one query misses part of
skycell's planning: a boolean of several regions is not joined into one
(below). With a connection pool, or many short sessions, set it, or put
`shared_preload_libraries = 'skycell'` in `postgresql.conf` for every
database at once.

Without it, `POINT(ra, dec)` is PostgreSQL's point, and
`CONTAINS(POINT(ra, dec), CIRCLE(...)) = 1` fails with *function
contains(point, skyregion) does not exist*. The forms with ADQL's
coordinate system, `POINT('ICRS', ra, dec)` and `CIRCLE('ICRS', ...)`, work
either way, as do the prefixed aliases `skycell_point`, `skycell_circle`,
`skycell_box` and `skycell_polygon`.

## A sample catalogue

Two hundred thousand sources, two fifths of them in 300 compact clusters,
and a list of targets to match against it. Your own catalogue only needs
`ra` and `dec` columns in degrees (ICRS).

```sql
SELECT setseed(0.1);
CREATE TABLE cat AS
SELECT i AS source_id,
       CASE WHEN i % 10 < 6 THEN 360 * random()
            ELSE mod(((i % 300) * 47.3 + (random() - 0.5) * 0.6 + 360)::numeric, 360)::float8 END AS ra,
       CASE WHEN i % 10 < 6 THEN degrees(asin(2 * random() - 1))
            ELSE greatest(-89.9, least(89.9, (i % 300) * 0.57 - 85 + (random() - 0.5) * 0.6)) END AS dec,
       10 + 10 * random() AS mag
FROM generate_series(1, 200000) i;

-- targets: a sample of the sources, shifted by 0.3"
CREATE TABLE targets AS
SELECT row_number() OVER () AS id, ra + 0.3 / 3600 AS ra, dec
FROM cat TABLESAMPLE SYSTEM (2);
ANALYZE targets;
```

## The index

The index is an ordinary B-tree on the position's order-29 HEALPix cell. Index
the expression; the table needs no new column:

```sql
CREATE INDEX cat_cell ON cat (skycell_ang2cell(ra, dec));
ALTER INDEX cat_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE cat;
```

`ANALYZE` is not optional. Its histogram of the cell index is the map of how
crowded each part of the sky is, from which every search chooses how finely
to cut its region into index ranges. Run it again after a bulk load. A
statistics target of 1000 resolves a crowded sky well for 10⁷ rows and more;
PostgreSQL's default, 100, is coarse.

Check that a cone search uses it: the plan shows skycell's own scan,
`Custom Scan (SkycellCone)`.

```sql
EXPLAIN (COSTS OFF)
SELECT * FROM cat WHERE CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.1)) = 1;
-- expect: Custom Scan (SkycellCone) on cat
```

`Mode: bitmap` says the table's rows are not stored in sky order, so each
search collects its rows' locations first and reads each table page once.

### Storing the table in sky order

For a large catalogue, storing the rows in cell order puts neighbouring sky on
neighbouring pages, so a search reads fewer of them. Either order the table by
the index once (and again after large loads; `CLUSTER` locks the table while it
runs):

```sql
CLUSTER cat USING cat_cell;
ANALYZE cat;
EXPLAIN (COSTS OFF)
SELECT * FROM cat WHERE CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.1)) = 1;
-- expect: Mode: ordered
```

or load it in that order, with the cell as a column PostgreSQL computes
itself:

```sql
CREATE TABLE cat_sorted (
  source_id bigint, ra float8, dec float8, mag float8,
  cell bigint GENERATED ALWAYS AS (skycell_ang2cell(ra, dec)) STORED);
INSERT INTO cat_sorted (source_id, ra, dec, mag)
  SELECT source_id, ra, dec, mag FROM cat ORDER BY skycell_ang2cell(ra, dec);
CREATE INDEX cat_sorted_cell ON cat_sorted (cell);
ALTER TABLE cat_sorted ALTER COLUMN cell SET STATISTICS 1000;
ANALYZE cat_sorted;
EXPLAIN (COSTS OFF)
SELECT * FROM cat_sorted WHERE CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.1)) = 1;
-- expect: Index: cat_sorted_cell
```

The queries keep naming `ra` and `dec`: the generated column tells the planner
what `cell` holds. An existing `cell` column can say so with a validated
`CHECK (cell = skycell_ang2cell(ra, dec))` constraint and `NOT NULL` instead.

A table that stores positions as skycell's `skypos` type is indexed the same
way, on `skycell_cell(pos)`:

```sql
CREATE TABLE cat_pos AS SELECT source_id, point(ra, dec) AS pos, mag FROM cat;
CREATE INDEX cat_pos_cell ON cat_pos (skycell_cell(pos));
ANALYZE cat_pos;
EXPLAIN (COSTS OFF) SELECT * FROM cat_pos WHERE CONTAINS(pos, CIRCLE(10, 10, 0.5)) = 1;
-- expect: Custom Scan (SkycellCone) on cat_pos
DROP TABLE cat_sorted, cat_pos;
```

## Searching a region

Every spelling below is the same indexed search. ADQL's form is passed
through as a TAP service receives it:

```sql
-- a cone, in ADQL
SELECT count(*) FROM cat WHERE CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.2)) = 1;
-- the same as a distance
SELECT count(*) FROM cat WHERE DISTANCE(POINT(ra, dec), POINT(47.3, -84.43)) < 0.2;
-- the same with skycell's operator
SELECT count(*) FROM cat WHERE point(ra, dec) <@ circle(47.3, -84.43, 0.2);
```

Other conditions sit beside it, and are tested on the rows the search reads:

```sql
EXPLAIN (COSTS OFF)
SELECT source_id, mag FROM cat
WHERE CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.2)) = 1 AND mag < 12;
-- expect: Custom Scan (SkycellCone) on cat
```

Polygons (convex or not, as long as the edges do not cross), boxes and any
region written in STC-S take the same scan:

```sql
EXPLAIN (COSTS OFF)
SELECT * FROM cat WHERE CONTAINS(POINT(ra, dec), POLYGON(10, 10, 12, 10, 11, 11, 12, 12, 10, 12)) = 1;
-- expect: Custom Scan (SkycellCone) on cat
SELECT count(*) FROM cat WHERE CONTAINS(POINT(ra, dec), BOX(11, 11, 2, 2)) = 1;
SELECT count(*) FROM cat
WHERE CONTAINS(POINT(ra, dec), REGION('DIFFERENCE ICRS (CIRCLE 47.3 -84.43 0.3 CIRCLE 47.3 -84.43 0.1)')) = 1;
```

A shape spelled out as several conditions on the same position is joined into
one region by the planner, so it is one search, not one per piece:

```sql
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cat
WHERE CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.3)) = 1
  AND CONTAINS(POINT(ra, dec), CIRCLE(47.3, -84.43, 0.1)) = 0;
-- expect: DIFFERENCE ICRS
```

(It applies to `OR`, `AND` and `NOT` of constant regions on the same position;
an `OR` with a condition on another column is left as written.)

## Nearest neighbours

`ORDER BY` the distance with a `LIMIT` reads the nearest rows from the index,
without sorting the table: ADQL's `SELECT TOP 10 ... ORDER BY DISTANCE(...)`.

```sql
EXPLAIN (COSTS OFF)
SELECT source_id, DISTANCE(POINT(ra, dec), POINT(47.3, -84.43)) AS d
FROM cat ORDER BY d LIMIT 10;
-- expect: Custom Scan (SkycellNearest) on cat
```

The nearest beyond a distance (the ten nearest more than 1° away) starts the
search at that distance:

```sql
SELECT source_id FROM cat
WHERE DISTANCE(POINT(ra, dec), POINT(47.3, -84.43)) > 1
ORDER BY DISTANCE(POINT(ra, dec), POINT(47.3, -84.43)) LIMIT 10;
```

## Cross-matching a target list

A join on a distance, or on a circle round each target, is a cross-match: the
catalogue's index is searched once per target, each with the ranges that
target's cone needs.

```sql
EXPLAIN (COSTS OFF)
SELECT t.id, c.source_id
FROM targets t JOIN cat c ON DISTANCE(POINT(c.ra, c.dec), POINT(t.ra, t.dec)) < 1.0 / 3600;
-- expect: Ranges: per outer row
```

The usual variants:

```sql
-- every target, with its best match within 1" (NULL if none)
SELECT t.id, m.source_id, m.d * 3600 AS sep_arcsec
FROM targets t LEFT JOIN LATERAL (
  SELECT source_id, DISTANCE(POINT(c.ra, c.dec), POINT(t.ra, t.dec)) AS d
  FROM cat c
  WHERE DISTANCE(POINT(c.ra, c.dec), POINT(t.ra, t.dec)) < 1.0 / 3600
  ORDER BY d LIMIT 1) m ON true
LIMIT 5;

-- every target's nearest source, however far
SELECT t.id, n.source_id
FROM targets t CROSS JOIN LATERAL (
  SELECT source_id FROM cat c
  ORDER BY DISTANCE(POINT(c.ra, c.dec), POINT(t.ra, t.dec)) LIMIT 1) n
LIMIT 5;

-- Q3C's spelling, for code moving from Q3C
SELECT count(*) FROM targets t JOIN cat c ON skycell_join(c.ra, c.dec, t.ra, t.dec, 1.0 / 3600);
```

It does not matter which way round the query is written: a point of `cat`
inside a circle round each target, or a target inside a circle round each
source, is planned as the search of the indexed table.

## When the plan is a sequential scan

- **No `ANALYZE`** since the index was built or the table loaded.
- **A different expression**: the index is on `skycell_ang2cell(ra, dec)`,
  but the query's position is another pair of columns, or `skycell_cell(pos)`
  of a `skypos` column the index does not cover.
- **PostgreSQL's `point`**: `search_path` does not put skycell's schema first
  (above), and a bare `POINT(ra, dec)` is the geometric type.
- **A huge region**: a cone over most of the sky reads most of the table
  anyway, and a sequential scan is cheaper. That is the planner being right.

`EXPLAIN (ANALYZE, BUFFERS)` shows how many index ranges a search used
(`Ranges`) and the rows it read and kept.
