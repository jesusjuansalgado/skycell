-- Stored regions: which footprints contain each point?
--   pgSphere: scircle column + GiST, operator @>
--   skycell : MOC (<= 8 NUNIQ cells per footprint) in a B-tree; a point's
--             candidates are cells equal to one of its 30 ancestors.
-- Q3C indexes points only, so it has no equivalent here.
\set ON_ERROR_STOP 1
SELECT setseed(0.55);
DROP TABLE IF EXISTS fp, fp_cells, fp_meta;
CREATE TABLE fp AS
SELECT f AS fid, p.ra AS ra0, p.dec AS dec0, power(10, -1.3 + 1.3 * random()) AS r
FROM generate_series(1, 20000) f
JOIN bench_pool p ON p.rn = 1 + (f * 13) % 2000;
UPDATE fp SET ra0 = (ra0 + 2 * random() - 1 + 360)::numeric % 360, dec0 = greatest(-89, least(89, dec0 + 2 * random() - 1));
ALTER TABLE fp ADD PRIMARY KEY (fid);
ALTER TABLE fp ADD COLUMN reg scircle;
UPDATE fp SET reg = scircle(spoint(radians(ra0), radians(dec0)), radians(r));

CREATE TABLE IF NOT EXISTS bench_fp (method text, pass int, n bigint, ms float8, build_ms float8, index_mb float8, plan text);
TRUNCATE bench_fp;



CREATE OR REPLACE FUNCTION bench_fp_run(method text, pass int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE n bigint; s numeric; t0 timestamptz; plan text := ''; l text;
BEGIN
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || fp_sql(method) LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE fp_sql(method) INTO n, s;
  INSERT INTO bench_fp (method, pass, n, ms, plan)
  VALUES (method, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;

DO $$ DECLARE t0 timestamptz := clock_timestamp(); BEGIN
  CREATE INDEX fp_reg_idx ON fp USING gist (reg);
  INSERT INTO bench_fp (method, pass, build_ms, index_mb)
  VALUES ('pgsphere', 0, extract(epoch FROM clock_timestamp() - t0) * 1000, pg_relation_size('fp_reg_idx') / 1048576.0);
END $$;
DO $$ DECLARE t0 timestamptz := clock_timestamp(); BEGIN
  CREATE TABLE fp_cells AS SELECT fid, unnest(skycell_cone_moc(ra0, dec0, r, 8)) AS nuniq FROM fp;
  IF NOT EXISTS (SELECT 1 FROM fp_cells) THEN
    RAISE EXCEPTION 'fp_cells is empty: the footprint corpus was not built';
  END IF;
  CREATE INDEX fp_cells_idx ON fp_cells (nuniq);
  DROP TABLE IF EXISTS fp_meta;
  CREATE TABLE fp_meta AS SELECT min(skycell_nuniq_order(nuniq)) AS min_order, max(skycell_nuniq_order(nuniq)) AS max_order FROM fp_cells;
  INSERT INTO bench_fp (method, pass, build_ms, index_mb)
  VALUES ('skycell', 0, extract(epoch FROM clock_timestamp() - t0) * 1000,
          (pg_relation_size('fp_cells') + pg_relation_size('fp_cells_idx')) / 1048576.0);
END $$;
ANALYZE fp;
ANALYZE fp_cells;

CREATE OR REPLACE FUNCTION fp_sql(method text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'pgsphere' THEN 'SELECT count(*), sum(f.fid) FROM probe p JOIN fp f ON f.reg @> p.pos'
    WHEN 'skycell' THEN format('SELECT count(*), sum(f.fid) FROM probe p JOIN fp_cells fc ON fc.nuniq = ANY (skycell_ancestors(p.cell, %s, %s)) JOIN fp f ON f.fid = fc.fid WHERE skycell_in_cone(p.ra, p.dec, f.ra0, f.dec0, f.r)',
                               (SELECT coalesce(min_order, 0) FROM fp_meta),
                               (SELECT coalesce(max_order, 29) FROM fp_meta))
  END
$$;
