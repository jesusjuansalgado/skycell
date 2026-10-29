-- Cone searches on real Gaia DR3 positions: tab:cones' "real corpus" columns
-- (GIST_REGION_DESIGN.md rounds sixty-eight and sixty-nine), skycell's B-tree
-- rewrite against pgSphere's GiST, every skycell.* GUC at its default.
--
-- WARM (round sixty-eight) runs on the shared heap gaia_realc
-- (19_gaia_load.sh): 1,456 centres at tab:cones' radii and counts, half drawn
-- from the catalogue and half uniform.  The two methods' working sets together
-- can exceed shared_buffers, so each method gets its own block: three untimed
-- passes, then five measured EXPLAIN (ANALYZE, BUFFERS) passes, before the
-- other method runs.  31_real_cones.sh runs the blocks in both orders.
--
-- COLD (round sixty-nine) runs on one table per method, gaia_real_cell and
-- gaia_real_sphere (19_gaia_load_split.sh), so neither warms the other's
-- pages.  1,456 *new* centres, placed so that no two cones come within
-- r1 + r2 + 2 deg of each other (no centre's first touch warms another's heap
-- or index-leaf pages).  The driver restarts the server and drops the OS page
-- cache before each method's pass at each radius, and each centre is run once.
--
-- Results: bench_realcone (warm) and bench_realcone_cold (cold), one row per
-- query and measurement; bench/realcone_report.py turns them into the paired
-- ratios with bootstrap intervals.  Every query's row count is kept so the
-- report can check that the methods agree.
--
-- psql variables: -v scale=F multiplies every count (default 1; a small value
-- such as 0.05 gives a quick smoke test).
\set ON_ERROR_STOP 1
\if :{?scale}
\else
  \set scale 1
\endif

CREATE TABLE IF NOT EXISTS bench_realcone (blk text, method text, pass int, qid int,
  n bigint, buffers bigint, plan_ms float8, exec_ms float8);
CREATE TABLE IF NOT EXISTS bench_realcone_cold (method text, label text, qid int, seq int,
  n bigint, hit bigint, rd bigint, io_ms float8, plan_ms float8, exec_ms float8);

CREATE TEMP TABLE realcone_radii AS
SELECT * FROM (VALUES (1/3600.0, '1"', 400), (10/3600.0, '10"', 400), (1/60.0, '1''', 300),
                      (0.1, '6''', 200), (0.5, '30''', 100), (1.0, '1deg', 40), (3.0, '3deg', 16)) v(r, label, nq);
UPDATE realcone_radii SET nq = greatest(2, 2 * round(nq * :scale / 2.0)::int);

-- ------------------------------------------------------------------
-- warm: centres and harness on gaia_realc
-- ------------------------------------------------------------------
SELECT setseed(0.68);
DROP TABLE IF EXISTS realcone_centers;
CREATE TABLE realcone_centers AS
WITH pool AS (SELECT row_number() OVER () AS rn, ra, dec
              FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE BERNOULLI (0.05) REPEATABLE (68)
                    ORDER BY random() LIMIT 2000) s)
SELECT (row_number() OVER (ORDER BY r, j))::int AS qid, label, r,
       CASE WHEN j % 2 = 0 THEN 'data' ELSE 'uniform' END AS kind,
       CASE WHEN j % 2 = 0 THEN p.ra ELSE 360 * random() END AS ra0,
       CASE WHEN j % 2 = 0 THEN p.dec ELSE degrees(asin(2 * random() - 1)) END AS dec0
FROM realcone_radii CROSS JOIN LATERAL generate_series(1, nq) j
JOIN pool p ON p.rn = 1 + ((j * 7 + (r * 3600)::int) % 2000);

-- sum(random_index) stands in for 03_cone.sql's sum(mag): it forces the heap visit
CREATE OR REPLACE FUNCTION realcone_sql(method text, tbl text, ra0 float8, dec0 float8, r float8)
RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'skycell' THEN CASE WHEN tbl = 'gaia_real_cell'
      THEN format('SELECT count(*), sum(random_index) FROM gaia_real_cell WHERE skycell_cone(cell, ra, dec, %s, %s, %s)', ra0, dec0, r)
      ELSE format('SELECT count(*), sum(random_index) FROM %I WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, %s, %s, %s)', tbl, ra0, dec0, r) END
    WHEN 'pgsphere' THEN format('SELECT count(*), sum(random_index) FROM %I WHERE pos <@ scircle(spoint(radians(%s), radians(%s)), radians(%s))', tbl, ra0, dec0, r)
  END $$;

-- one method's block: `warm` untimed passes, then `meas` measured ones.
-- 'skycell' is skycell's default plan (the SkycellCone custom scan where the
-- build has one); 'skycell-rw' is the range rewrite, skycell.custom_scan off
-- for the block.
CREATE OR REPLACE FUNCTION realcone_block(method text, blk text, warm int DEFAULT 3, meas int DEFAULT 5)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE c realcone_centers; j json; p json; n bigint; s numeric; i int; m text := method;
BEGIN
  DELETE FROM bench_realcone b WHERE b.method = realcone_block.method AND b.blk = realcone_block.blk;
  IF method = 'skycell-rw' THEN
    PERFORM set_config('skycell.custom_scan', 'off', true);
    m := 'skycell';
  END IF;
  FOR i IN 1 .. warm LOOP
    FOR c IN SELECT * FROM realcone_centers ORDER BY qid LOOP
      EXECUTE realcone_sql(m, 'gaia_realc', c.ra0, c.dec0, c.r) INTO n, s;
    END LOOP;
  END LOOP;
  FOR i IN 1 .. meas LOOP
    FOR c IN SELECT * FROM realcone_centers ORDER BY qid LOOP
      EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) '
              || realcone_sql(m, 'gaia_realc', c.ra0, c.dec0, c.r) INTO j;
      p := j -> 0 -> 'Plan';
      INSERT INTO bench_realcone VALUES (blk, method, i, c.qid,
        (p -> 'Plans' -> 0 ->> 'Actual Rows')::float8::bigint,
        (p ->> 'Shared Hit Blocks')::bigint + (p ->> 'Shared Read Blocks')::bigint,
        (j -> 0 ->> 'Planning Time')::float8, (j -> 0 ->> 'Execution Time')::float8);
    END LOOP;
  END LOOP;
END $$;

-- ------------------------------------------------------------------
-- cold: fresh, mutually separated centres and the per-label pass
-- ------------------------------------------------------------------
SELECT setseed(0.69);
DROP TABLE IF EXISTS realcone_cold_centers;
CREATE TABLE realcone_cold_centers (qid serial, label text, r float8, kind text, ra0 float8, dec0 float8);
CREATE TEMP TABLE realcone_pool AS
SELECT ra, dec FROM gaia_realc TABLESAMPLE BERNOULLI (0.1) REPEATABLE (69) ORDER BY random() LIMIT 20000;
CREATE TEMP TABLE realcone_cand AS
SELECT label, r, nq, kind, ra0, dec0, random() AS o FROM (
  SELECT label, r, nq, 'data' AS kind, p.ra AS ra0, p.dec AS dec0
  FROM realcone_radii, LATERAL (SELECT ra, dec FROM realcone_pool ORDER BY random() LIMIT nq * 6) p
  UNION ALL
  SELECT label, r, nq, 'uniform', 360 * random(), degrees(asin(2 * random() - 1))
  FROM realcone_radii, LATERAL generate_series(1, nq * 6) g) c;
DO $$
DECLARE c record; got int;
BEGIN
  -- largest radii first, so the big cones get room
  FOR c IN SELECT * FROM realcone_cand ORDER BY r DESC, o LOOP
    SELECT count(*) INTO got FROM realcone_cold_centers WHERE label = c.label AND kind = c.kind;
    CONTINUE WHEN got >= c.nq / 2;
    IF NOT EXISTS (SELECT 1 FROM realcone_cold_centers a
                   WHERE degrees(spoint(radians(a.ra0), radians(a.dec0)) <-> spoint(radians(c.ra0), radians(c.dec0)))
                         < a.r + c.r + 2.0) THEN
      INSERT INTO realcone_cold_centers (label, r, kind, ra0, dec0) VALUES (c.label, c.r, c.kind, c.ra0, c.dec0);
    END IF;
  END LOOP;
END $$;

-- one cold pass: every centre of one label, once, first touch.  The driver must
-- restart the server (and drop the OS page cache) immediately before each call.
CREATE OR REPLACE FUNCTION realcone_cold_pass(method text, lbl text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE c realcone_cold_centers; j json; p json; k int := 0;
BEGIN
  DELETE FROM bench_realcone_cold b WHERE b.method = realcone_cold_pass.method AND b.label = lbl;
  FOR c IN SELECT * FROM realcone_cold_centers WHERE label = lbl ORDER BY md5(qid::text) LOOP
    k := k + 1;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) '
            || realcone_sql(method, CASE method WHEN 'skycell' THEN 'gaia_real_cell' ELSE 'gaia_real_sphere' END,
                            c.ra0, c.dec0, c.r) INTO j;
    p := j -> 0 -> 'Plan';
    INSERT INTO bench_realcone_cold VALUES (method, lbl, c.qid, k, (p -> 'Plans' -> 0 ->> 'Actual Rows')::float8::bigint,
      (p ->> 'Shared Hit Blocks')::bigint, (p ->> 'Shared Read Blocks')::bigint,
      coalesce((p ->> 'I/O Read Time')::float8, (p ->> 'Shared I/O Read Time')::float8),
      (j -> 0 ->> 'Planning Time')::float8, (j -> 0 ->> 'Execution Time')::float8);
  END LOOP;
END $$;

SELECT label, count(*) AS warm_centres FROM realcone_centers GROUP BY label ORDER BY min(r);
SELECT label, count(*) AS cold_centres FROM realcone_cold_centers GROUP BY label ORDER BY min(r);
