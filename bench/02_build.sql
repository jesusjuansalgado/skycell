-- Build one table per method, all physically ordered along a space-filling
-- curve (so every method gets the same heap locality), and record build
-- times and sizes in bench_build.
\set ON_ERROR_STOP 1
CREATE EXTENSION IF NOT EXISTS q3c;
CREATE EXTENSION IF NOT EXISTS pg_sphere;
CREATE EXTENSION IF NOT EXISTS skycell;

CREATE TABLE IF NOT EXISTS bench_build (method text, step text, seconds float8, mb float8);
TRUNCATE bench_build;

CREATE OR REPLACE PROCEDURE timed(method text, step text, sql text, rel text DEFAULT NULL)
LANGUAGE plpgsql AS $$
DECLARE t0 timestamptz := clock_timestamp();
BEGIN
  EXECUTE sql;
  INSERT INTO bench_build VALUES (method, step, extract(epoch FROM clock_timestamp() - t0),
         CASE WHEN rel IS NULL THEN NULL ELSE pg_relation_size(rel::regclass) / 1048576.0 END);
  COMMIT;
END $$;

DROP TABLE IF EXISTS cat_q3c, cat_sphere, cat_cell;

-- Freeze each new table and checkpoint before timing its index build.  A table
-- just made by CREATE TABLE AS has no hint bits; the index build is its first
-- full scan and sets them, and with data checksums on (the default from
-- PostgreSQL 18) every such page is WAL-logged as a full-page image: ~650 MB of
-- WAL per 10M-row table, about half the build time, and checkpoints running
-- back to back for whichever build comes last.  The freeze is timed as its own
-- step so the total cost stays visible.  VACUUM cannot run inside a procedure,
-- hence the psql variables.

-- Q3C: expression B-tree on q3c_ang2ipix, table sorted by ipix (== CLUSTER)
CALL timed('q3c', 'table', 'CREATE TABLE cat_q3c AS SELECT id, ra, dec, mag FROM src ORDER BY q3c_ang2ipix(ra, dec)', 'cat_q3c');
SELECT clock_timestamp() AS t0 \gset
VACUUM (FREEZE) cat_q3c;
INSERT INTO bench_build VALUES ('q3c', 'freeze', extract(epoch FROM clock_timestamp() - :'t0'::timestamptz), NULL);
CHECKPOINT;
CALL timed('q3c', 'index', 'CREATE INDEX cat_q3c_idx ON cat_q3c (q3c_ang2ipix(ra, dec))', 'cat_q3c_idx');
CALL timed('q3c', 'analyze', 'ANALYZE cat_q3c');

-- pgSphere: spoint column + GiST, table sorted along the HEALPix curve
CALL timed('pgsphere', 'table', 'CREATE TABLE cat_sphere AS SELECT id, ra, dec, mag, spoint(radians(ra), radians(dec)) AS pos FROM src ORDER BY skycell_ang2cell(ra, dec)', 'cat_sphere');
SELECT clock_timestamp() AS t0 \gset
VACUUM (FREEZE) cat_sphere;
INSERT INTO bench_build VALUES ('pgsphere', 'freeze', extract(epoch FROM clock_timestamp() - :'t0'::timestamptz), NULL);
CHECKPOINT;
CALL timed('pgsphere', 'index', 'CREATE INDEX cat_sphere_idx ON cat_sphere USING gist (pos)', 'cat_sphere_idx');
CALL timed('pgsphere', 'analyze', 'ANALYZE cat_sphere');

-- skycell: plain int8 column + B-tree, histogram with 1000 buckets
CALL timed('skycell', 'table', 'CREATE TABLE cat_cell AS SELECT id, ra, dec, mag, skycell_ang2cell(ra, dec) AS cell FROM src ORDER BY 5', 'cat_cell');
SELECT clock_timestamp() AS t0 \gset
VACUUM (FREEZE) cat_cell;
INSERT INTO bench_build VALUES ('skycell', 'freeze', extract(epoch FROM clock_timestamp() - :'t0'::timestamptz), NULL);
CHECKPOINT;
CALL timed('skycell', 'index', 'CREATE INDEX cat_cell_idx ON cat_cell (cell)', 'cat_cell_idx');
ALTER TABLE cat_cell ALTER COLUMN cell SET STATISTICS 1000;
CALL timed('skycell', 'analyze', 'ANALYZE cat_cell');

-- already frozen before their index builds; this only picks up ANALYZE's changes
VACUUM (FREEZE) cat_q3c, cat_sphere, cat_cell;
SELECT method, step, round(seconds::numeric, 1) AS s, round(mb::numeric, 1) AS mb FROM bench_build ORDER BY 1, 2;
