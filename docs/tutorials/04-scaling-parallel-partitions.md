# 4. Large catalogues: parallel queries, partitions and upkeep

Running skycell on a production catalogue: parallel cross-matches and wide
searches, partitioning a large table by sky region, and what to do after
loads and upgrades. Every block runs as written, in order, on an empty
database.

```sql
CREATE SCHEMA sky;
CREATE EXTENSION skycell SCHEMA sky;
ALTER DATABASE skycell_tutorial SET search_path = sky, public, pg_catalog;
ALTER DATABASE skycell_tutorial SET session_preload_libraries = 'skycell';
```

```sql
SELECT setseed(0.4);
CREATE TABLE cat AS
SELECT * FROM (
  SELECT i AS source_id, 360 * random() AS ra, degrees(asin(2 * random() - 1)) AS dec, 10 + 10 * random() AS mag
  FROM generate_series(1, 300000) i) s
ORDER BY skycell_ang2cell(ra, dec);          -- stored in sky order
CREATE INDEX cat_cell ON cat (skycell_ang2cell(ra, dec));
ALTER INDEX cat_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE cat;
CREATE TABLE targets AS
SELECT row_number() OVER () AS id, ra + 0.3 / 3600 AS ra, dec FROM cat TABLESAMPLE SYSTEM (10);
ANALYZE targets;
```

## Parallel cross-matches

A cross-match of a large target list runs in PostgreSQL's parallel workers:
the targets are shared out among them, and each searches the catalogue's
index for its own. PostgreSQL decides from the size of the target table:
none below `min_parallel_table_scan_size` (8 MB), then one worker and more as
it grows, up to `max_parallel_workers_per_gather` (2 by default). Answers are
the same as serially. For a table that should always be shared out, name the
number of workers:

```sql
ALTER TABLE targets SET (parallel_workers = 2);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM targets t JOIN cat c ON DISTANCE(POINT(c.ra, c.dec), POINT(t.ra, t.dec)) < 1.0 / 3600;
-- expect: Parallel Seq Scan on targets
```

On a four-core server, three workers (so that four processes share the four
cores) gain another 20–40% over two; set `max_parallel_workers_per_gather`
to match the cores you want one query to use. An upload of a few thousand
targets stays serial, and is fast that way.

## Parallel searches of one region

A single wide search over a crowded field is split too: its index ranges
are cut into chunks, which the processes claim in turn. The planner offers it
when the search is expected to read enough of the table to be worth workers,
so small cones stay serial. The settings below only make this small sample
qualify:

```sql
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cat WHERE CONTAINS(POINT(ra, dec), CIRCLE(0, 0, 20)) = 1;
-- expect: Parallel Custom Scan (SkycellCone) on cat
```

On the 10 million real Gaia DR3 positions, cones round the Galactic centre
holding 3·10⁵ rows or more run 2.1–2.7 times faster with two workers.

## Partitioning by sky region

A catalogue too large to load, vacuum or archive in one piece can be split by
sky region. Partition by ranges of the cell itself, so that PostgreSQL drops
the partitions a search cannot reach. The cell of HEALPix order *k* number
*p* holds the order-29 ids from *p*·4^(29−*k*) to (*p*+1)·4^(29−*k*) − 1;
the twelve order-0 cells, for instance:

```sql
CREATE TABLE catp (source_id bigint, ra float8, dec float8, mag float8)
  PARTITION BY RANGE (skycell_ang2cell(ra, dec));
SELECT format('CREATE TABLE catp_%s PARTITION OF catp FOR VALUES FROM (%s) TO (%s)',
              p, p::bigint << 58, (p + 1)::bigint << 58)
FROM generate_series(0, 11) p \gexec
INSERT INTO catp SELECT * FROM cat;
CREATE INDEX ON catp (skycell_ang2cell(ra, dec));
ANALYZE catp;
```

(`\gexec` is psql's: it runs each row of the result as a statement. Order 1
gives 48 partitions, order 2 gives 192.) A constant search is pruned at plan
time to the partitions it touches; a cross-match tests each partition's key
range once per target, and skips those that cannot hold the cone; nearest
neighbours merge one ordered scan per partition:

```sql
EXPLAIN (COSTS OFF)
SELECT * FROM catp WHERE CONTAINS(POINT(ra, dec), CIRCLE(10, 10, 0.5)) = 1;
-- expect: Subplans Removed: 11
EXPLAIN (COSTS OFF)
SELECT source_id FROM catp ORDER BY DISTANCE(POINT(ra, dec), POINT(10, 10)) LIMIT 5;
-- expect: Merge Append
```

Partitioning is for upkeep, not speed: on 5 million real positions in 16
partitions, cross-matches take 1.2–1.7 times as long as on the same rows
unpartitioned, and a partitioned table brings no parallel gain of its own.
Partitions by declination band (or anything else) work too, without the
pruning by cell.

## Keeping it fast

- **`ANALYZE` after every bulk load.** Every search reads the density map
  from the cell index's statistics; one taken before a load covers the sky as
  it was. Autovacuum's analyze catches up eventually, but not at once after a
  large load.
- **Statistics target 1000** on the cell index (or column) for 10⁷ rows and
  more: `ALTER INDEX cat_cell ALTER COLUMN 1 SET STATISTICS 1000`.
- **Sky order.** A table loaded in cell order, or `CLUSTER`ed on the cell
  index after large loads, reads fewer pages per search (`Mode: ordered` in
  `EXPLAIN`); new rows appended out of order slowly turn it back to
  `Mode: bitmap`, which is correct, only slower on disk.
- **Memory.** The index is one 8-byte key per row: about 21 GiB of index per
  billion sources. A nearest-neighbour search holds its candidate rows in
  `work_mem` and spills to disk beyond it.
- **Session preload.** `session_preload_libraries` (per database) or
  `shared_preload_libraries` (server), as in the set-up block above, so the
  first query of each session is planned like the others.
- **Upgrades.** `make install` copies the new version; each database then
  needs `ALTER EXTENSION skycell UPDATE` (or `make upgrade` for all of them).

```sql
SELECT extversion FROM pg_extension WHERE extname = 'skycell';
```

## Reading a plan

`EXPLAIN` shows skycell's scans and what they did:

| line | meaning |
|---|---|
| `Custom Scan (SkycellCone)` | a cone, polygon, region or cross-match search on the cell index |
| `Custom Scan (SkycellNearest)` | an `ORDER BY` distance `LIMIT k` search |
| `Ranges: n` | the index ranges the region's covering used; `per outer row` in a cross-match |
| `Mode: ordered` / `bitmap` | rows read in index order (table in sky order), or collected first and each page read once |
| `Rings`, `Rows Read by Rings` (with `ANALYZE`) | for nearest neighbours: how far the search had to go |

```sql
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)
SELECT source_id FROM cat ORDER BY DISTANCE(POINT(ra, dec), POINT(10, 10)) LIMIT 100;
-- expect: Rows Read by Rings
```
