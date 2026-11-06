-- Nearest neighbours on the ObsCore-shaped table (real36.perf_oc, a copy of
-- oc05.oc), skycell against pgSphere, with micro_knn.sql's protocol: per
-- centre, k and method, n EXECUTEs (plan + run), each at a new centre 1e-9
-- deg away; methods in random order.  mko_report(): per k, the ratio of
-- skycell's to pgSphere's summed per-centre medians.
CREATE TEMP TABLE IF NOT EXISTS mko (k int, qid int, method text, us float8, n bigint);
CREATE OR REPLACE FUNCTION mko_sql(m text, ra0 float8, de0 float8, k int) RETURNS text LANGUAGE sql AS $$
  SELECT CASE m
    WHEN 'pgsphere' THEN format('SELECT count(*) FROM (SELECT 1 FROM perf_oc ORDER BY pos <-> spoint(radians(%s), radians(%s)) LIMIT %s) x', ra0, de0, k)
    ELSE format('SELECT count(*) FROM (SELECT 1 FROM perf_oc ORDER BY skycell_dist(s_ra, s_dec, %s, %s) LIMIT %s) x', ra0, de0, k) END $$;
CREATE OR REPLACE FUNCTION mko_run(n int, reps int, ks int[]) RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record; m text; k int; t0 timestamptz; x bigint;
BEGIN
  FOR rep IN 1 .. reps LOOP
   FOR c IN SELECT * FROM mk_oc_centres ORDER BY random() LOOP
    FOREACH k IN ARRAY ks LOOP
     FOR m IN SELECT unnest(ARRAY['pgsphere', 'skycell']) ORDER BY random() LOOP
      EXECUTE mko_sql(m, c.ra, c.dec, k) INTO x;
      t0 := clock_timestamp();
      FOR i IN 1 .. n LOOP
        EXECUTE mko_sql(m, c.ra + 1e-9 * (i + n * rep), c.dec, k) INTO x;
      END LOOP;
      INSERT INTO mko VALUES (k, c.qid, m, extract(epoch FROM clock_timestamp() - t0) * 1e6 / n, x);
     END LOOP;
    END LOOP;
   END LOOP;
  END LOOP;
END $$;
CREATE OR REPLACE FUNCTION mko_report() RETURNS TABLE (k int, skycell_us float8, pgsphere_us float8, ratio numeric) LANGUAGE sql AS $$
  WITH med AS (SELECT k, qid, method, percentile_cont(0.5) WITHIN GROUP (ORDER BY us) us FROM mko GROUP BY 1, 2, 3)
  SELECT k, round(sum(us) FILTER (WHERE method = 'skycell')::numeric / count(*) FILTER (WHERE method = 'skycell'), 1)::float8,
            round(sum(us) FILTER (WHERE method = 'pgsphere')::numeric / count(*) FILTER (WHERE method = 'pgsphere'), 1)::float8,
         round((sum(us) FILTER (WHERE method = 'skycell') / sum(us) FILTER (WHERE method = 'pgsphere'))::numeric, 2)
  FROM med GROUP BY k ORDER BY k $$;
