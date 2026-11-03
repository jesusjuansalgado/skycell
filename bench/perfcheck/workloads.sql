-- perfcheck workloads: per workload and centre, the median of 5 executions,
-- each planned anew at a centre 1e-9 deg away, after one untimed execution.
-- Rows: workload, centre, microseconds, the count it returned.
\set ON_ERROR_STOP 1
SET max_parallel_workers_per_gather = 0;
CREATE TEMP TABLE perf_out (workload text, c int, us float8, n bigint);
CREATE OR REPLACE FUNCTION pg_temp.perf_one(w text, q text, centres text DEFAULT 'perf_centre')
RETURNS void LANGUAGE plpgsql AS $$
DECLARE p record; t0 timestamptz; x bigint; ts float8[];
BEGIN
  IF to_regclass(centres) IS NULL THEN
    RETURN;					-- a table this database lacks: the workload is skipped
  END IF;
  FOR p IN EXECUTE format('SELECT * FROM %I ORDER BY c', centres) LOOP
    EXECUTE format(q, p.ra, p.dec, p.c) INTO x;
    ts := '{}';
    FOR i IN 1 .. 5 LOOP
      t0 := clock_timestamp();
      EXECUTE format(q, p.ra + i * 1e-9, p.dec, p.c) INTO x;
      ts := ts || extract(epoch FROM clock_timestamp() - t0) * 1e6;
    END LOOP;
    INSERT INTO perf_out SELECT w, p.c, percentile_cont(0.5) WITHIN GROUP (ORDER BY t), x FROM unnest(ts) t;
  END LOOP;
END $$;
\o /dev/null
SELECT pg_temp.perf_one(w, q, coalesce(ct, 'perf_centre')) FROM (VALUES
  ('cone 1"',          'SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', %s, %s, 1.0/3600))', NULL),
  ('cone 1 deg',       'SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', %s, %s, 1))', NULL),
  ('native cone 6''',  'SELECT count(*) FROM gaia_realc WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, %s, %s, 0.1)', NULL),
  ('polygon 0.5 deg',  'SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), POLYGON(''ICRS'', %1$s - 0.25, %2$s - 0.2, %1$s + 0.25, %2$s - 0.2, %1$s, %2$s + 0.25))', NULL),
  ('annulus 0.3 deg',  'SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), REGION(''DIFFERENCE ICRS (CIRCLE %1$s %2$s 0.3 CIRCLE %1$s %2$s 0.15)''))', NULL),
  ('xmatch 300 x 1"',  'SELECT count(*) + 0 * %s * %s FROM perf_probe p JOIN gaia_realc g ON 1 = CONTAINS(POINT(''ICRS'', g.ra, g.dec), CIRCLE(''ICRS'', p.ra, p.dec, 1.0/3600)) WHERE p.c = %s', NULL),
  ('nearest k=1',      'SELECT count(*) FROM (SELECT 1 FROM gaia_realc ORDER BY skycell_dist(ra, dec, %s, %s) LIMIT 1) s', NULL),
  ('nearest k=100',    'SELECT count(*) FROM (SELECT 1 FROM gaia_realc ORDER BY skycell_dist(ra, dec, %s, %s) LIMIT 100) s', NULL),
  ('partition cone 0.5 deg', 'SELECT count(*) FROM perf_part WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', %s, %s, 0.5))', NULL),
  ('partition xmatch 300 x 5"', 'SELECT count(*) + 0 * %s * %s FROM perf_probe p JOIN perf_part g ON 1 = CONTAINS(POINT(''ICRS'', g.ra, g.dec), CIRCLE(''ICRS'', p.ra, p.dec, 5.0/3600)) WHERE p.c = %s', NULL),
  ('ObsCore nearest k=100', 'SELECT count(*) FROM (SELECT 1 FROM perf_oc ORDER BY skycell_dist(s_ra, s_dec, %s, %s) LIMIT 100) s', 'perf_oc_centre'),
  ('ObsCore nearest k=1000', 'SELECT count(*) FROM (SELECT 1 FROM perf_oc ORDER BY skycell_dist(s_ra, s_dec, %s, %s) LIMIT 1000) s', 'perf_oc_centre')
) v(w, q, ct);
\o
\copy perf_out TO STDOUT CSV
