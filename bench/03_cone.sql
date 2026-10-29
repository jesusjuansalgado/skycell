-- Cone-search benchmark: identical query centres for every method.
-- Half the centres are catalogue positions (density-weighted: mostly disk,
-- bulge and clusters), half are uniform on the sky.
\set ON_ERROR_STOP 1

SELECT setseed(0.77);
DROP TABLE IF EXISTS bench_pool, bench_centers CASCADE;
CREATE TABLE bench_pool AS
SELECT row_number() OVER () AS rn, ra, dec
FROM (SELECT ra, dec FROM src TABLESAMPLE BERNOULLI (0.1) REPEATABLE (7) ORDER BY random() LIMIT 2000) s;

DO $$ BEGIN
  IF (SELECT count(*) FROM bench_pool) < 100 THEN
    RAISE EXCEPTION 'bench_pool has too few rows: is src populated?';
  END IF;
END $$;

CREATE TABLE bench_centers AS
WITH radii(r, label, nq) AS (
  VALUES (1/3600.0, '1"', 400), (10/3600.0, '10"', 400), (1/60.0, '1''', 300),
         (0.1, '6''', 200), (0.5, '30''', 100), (1.0, '1deg', 40), (3.0, '3deg', 16))
SELECT row_number() OVER (ORDER BY r, j) AS qid, label, r,
       CASE WHEN j % 2 = 0 THEN 'data' ELSE 'uniform' END AS kind,
       CASE WHEN j % 2 = 0 THEN p.ra ELSE 360 * random() END AS ra0,
       CASE WHEN j % 2 = 0 THEN p.dec ELSE degrees(asin(2 * random() - 1)) END AS dec0
FROM radii CROSS JOIN LATERAL generate_series(1, nq) j
JOIN bench_pool p ON p.rn = 1 + ((j * 7 + (r * 3600)::int) % 2000);

CREATE TABLE IF NOT EXISTS bench_cone (method text, variant text, pass int, qid bigint, n bigint, ms float8);
CREATE TABLE IF NOT EXISTS bench_cone_x (method text, variant text, qid bigint, est_rows float8,
                                         act_rows float8, buffers bigint, plan_ms float8, exec_ms float8);

-- variant: '' or 'guc=value,guc=value' (skycell settings for ablations)
CREATE OR REPLACE FUNCTION cone_sql(method text, c bench_centers) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'q3c' THEN format('SELECT count(*), sum(mag) FROM cat_q3c WHERE q3c_radial_query(ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r)
    WHEN 'pgsphere' THEN format('SELECT count(*), sum(mag) FROM cat_sphere WHERE pos <@ scircle(spoint(radians(%s), radians(%s)), radians(%s))', c.ra0, c.dec0, c.r)
    WHEN 'skycell' THEN format('SELECT count(*), sum(mag) FROM cat_cell WHERE skycell_cone(cell, ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r)
    ELSE CASE WHEN method LIKE 'skycell%' THEN
      format('SELECT count(*), sum(mag) FROM cat_cell WHERE skycell_cone(cell, ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r)
    END
  END
$$;

CREATE OR REPLACE FUNCTION apply_variant(variant text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE kv text;
BEGIN
  IF variant = '' THEN RETURN; END IF;
  FOREACH kv IN ARRAY string_to_array(variant, ',') LOOP
    PERFORM set_config('skycell.' || split_part(kv, '=', 1), split_part(kv, '=', 2), true);
  END LOOP;
END $$;

CREATE OR REPLACE FUNCTION bench_cone_run(method text, variant text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE c bench_centers; n bigint; s float8; t0 timestamptz;
BEGIN
  PERFORM apply_variant(variant);
  DELETE FROM bench_cone b WHERE b.method = bench_cone_run.method AND b.variant = bench_cone_run.variant AND b.pass = bench_cone_run.pass;
  FOR c IN SELECT * FROM bench_centers ORDER BY qid LOOP
    t0 := clock_timestamp();
    EXECUTE cone_sql(method, c) INTO n, s;
    INSERT INTO bench_cone VALUES (method, variant, pass, c.qid, n, extract(epoch FROM clock_timestamp() - t0) * 1000);
  END LOOP;
END $$;

CREATE OR REPLACE FUNCTION bench_cone_explain(method text, variant text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE c bench_centers; j json; p json; scan json;
BEGIN
  PERFORM apply_variant(variant);
  DELETE FROM bench_cone_x b WHERE b.method = bench_cone_explain.method AND b.variant = bench_cone_explain.variant;
  FOR c IN SELECT * FROM bench_centers ORDER BY qid LOOP
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) ' || cone_sql(method, c) INTO j;
    p := j -> 0 -> 'Plan';
    scan := p -> 'Plans' -> 0;
    INSERT INTO bench_cone_x VALUES (method, variant, c.qid,
      (scan ->> 'Plan Rows')::float8, (scan ->> 'Actual Rows')::float8,
      (p ->> 'Shared Hit Blocks')::bigint + (p ->> 'Shared Read Blocks')::bigint,
      (j -> 0 ->> 'Planning Time')::float8, (j -> 0 ->> 'Execution Time')::float8);
  END LOOP;
END $$;
