-- Fresh, never-before-queried probe centres for a genuine cold-cache
-- benchmark -- GIST_REGION_DESIGN.md's "Round forty-four" found that
-- reusing 03_cone.sql's own bench_centers rows for a cold run is invalid
-- regardless of how carefully shared_buffers is cleared: this sandbox's
-- /proc/sys/vm/drop_caches write silently succeeds without actually
-- evicting the OS page cache (a known container limitation, confirmed
-- directly in that round), so a "cold" probe against an already-queried
-- coordinate is not cold at all -- only a coordinate whose matching pages
-- have genuinely never been read since the last real restart is. Hence:
-- entirely new centres, reseeded, inserted alongside (not replacing)
-- 03_cone.sql's own qid 1-1456, at the same per-label sample sizes and
-- kind mix, for a driver script to run through once each (with a real
-- `service postgresql restart` before every radius label -- see Round
-- forty-four's own driver for that half of the methodology, not captured
-- here since it's a shell loop, not SQL).
--
-- Two disjoint batches, not one: round forty-four's own qid 10001-11456
-- (one batch is enough there -- its two methods, 'pgsphere' and
-- 'skycell', query different tables, cat_sphere and cat_cell, so probing
-- the same centre with both never cross-contaminates); round forty-five's
-- qid 20001-21456 ('adaptive') and 30001-31456 ('purebtree') are disjoint
-- from each other because both methods there query the *same* table and
-- column (cat_pos.pos) -- probing one centre with both modes back to back
-- would warm the second probe's own read.
--
-- Depends on `src` (01_data.sql) and `bench_centers` already existing
-- (03_cone.sql) -- this inserts into it, not replaces it.
\set ON_ERROR_STOP 1

-- Round forty-four: cat_cell/cat_sphere, qid 10001-11456.
SELECT setseed(0.419);
DROP TABLE IF EXISTS bench_pool_fresh;
CREATE TABLE bench_pool_fresh AS
SELECT row_number() OVER () AS rn, ra, dec
FROM (SELECT ra, dec FROM src TABLESAMPLE BERNOULLI (0.1) REPEATABLE (41) ORDER BY random() LIMIT 2000) s;

DO $$ BEGIN
  IF (SELECT count(*) FROM bench_pool_fresh) < 100 THEN
    RAISE EXCEPTION 'bench_pool_fresh has too few rows: is src populated?';
  END IF;
END $$;

INSERT INTO bench_centers
WITH radii(r, label, nq) AS (
  VALUES (1/3600.0, '1"', 400), (10/3600.0, '10"', 400), (1/60.0, '1''', 300),
         (0.1, '6''', 200), (0.5, '30''', 100), (1.0, '1deg', 40), (3.0, '3deg', 16))
SELECT 10000 + row_number() OVER (ORDER BY r, j) AS qid, label, r,
       CASE WHEN j % 2 = 0 THEN 'data' ELSE 'uniform' END AS kind,
       CASE WHEN j % 2 = 0 THEN p.ra ELSE 360 * random() END AS ra0,
       CASE WHEN j % 2 = 0 THEN p.dec ELSE degrees(asin(2 * random() - 1)) END AS dec0
FROM radii CROSS JOIN LATERAL generate_series(1, nq) j
JOIN bench_pool_fresh p ON p.rn = 1 + ((j * 7 + (r * 3600)::int) % 2000);

-- Round forty-five: cat_pos adaptive-gate vs forced-rewrite, qid
-- 20001-21456 ('adaptive') and 30001-31456 ('purebtree') -- two disjoint
-- draws from the same fresh pool (different modulus offset, j*11 vs
-- j*13) so the two batches' own sky positions differ too, not just their
-- qid ranges.
SELECT setseed(0.531);
DROP TABLE IF EXISTS bench_pool_fresh2;
CREATE TABLE bench_pool_fresh2 AS
SELECT row_number() OVER () AS rn, ra, dec
FROM (SELECT ra, dec FROM src TABLESAMPLE BERNOULLI (0.1) REPEATABLE (53) ORDER BY random() LIMIT 2000) s;

DO $$ BEGIN
  IF (SELECT count(*) FROM bench_pool_fresh2) < 100 THEN
    RAISE EXCEPTION 'bench_pool_fresh2 has too few rows: is src populated?';
  END IF;
END $$;

INSERT INTO bench_centers
WITH radii(r, label, nq) AS (
  VALUES (1/3600.0, '1"', 400), (10/3600.0, '10"', 400), (1/60.0, '1''', 300),
         (0.1, '6''', 200), (0.5, '30''', 100), (1.0, '1deg', 40), (3.0, '3deg', 16))
SELECT 20000 + row_number() OVER (ORDER BY r, j) AS qid, label, r,
       CASE WHEN j % 2 = 0 THEN 'data' ELSE 'uniform' END AS kind,
       CASE WHEN j % 2 = 0 THEN p.ra ELSE 360 * random() END AS ra0,
       CASE WHEN j % 2 = 0 THEN p.dec ELSE degrees(asin(2 * random() - 1)) END AS dec0
FROM radii CROSS JOIN LATERAL generate_series(1, nq) j
JOIN bench_pool_fresh2 p ON p.rn = 1 + ((j * 11 + (r * 3600)::int) % 2000);

INSERT INTO bench_centers
WITH radii(r, label, nq) AS (
  VALUES (1/3600.0, '1"', 400), (10/3600.0, '10"', 400), (1/60.0, '1''', 300),
         (0.1, '6''', 200), (0.5, '30''', 100), (1.0, '1deg', 40), (3.0, '3deg', 16))
SELECT 30000 + row_number() OVER (ORDER BY r, j) AS qid, label, r,
       CASE WHEN j % 2 = 0 THEN 'data' ELSE 'uniform' END AS kind,
       CASE WHEN j % 2 = 0 THEN p.ra ELSE 360 * random() END AS ra0,
       CASE WHEN j % 2 = 0 THEN p.dec ELSE degrees(asin(2 * random() - 1)) END AS dec0
FROM radii CROSS JOIN LATERAL generate_series(1, nq) j
JOIN bench_pool_fresh2 p ON p.rn = 1 + ((j * 13 + (r * 3600)::int) % 2000);

SELECT (CASE WHEN qid < 20000 THEN 'round44 (10001+)'
             WHEN qid < 30000 THEN 'round45 adaptive (20001+)'
             ELSE 'round45 purebtree (30001+)' END) AS batch,
       label, count(*)
FROM bench_centers WHERE qid >= 10000
GROUP BY 1, label ORDER BY 1, label;
