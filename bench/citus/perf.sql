-- One instance against Citus: the same 2M real Gaia DR3 positions as a plain
-- table on the coordinator (g_local), over 32 shards by id (g_id), and over
-- 32 shards by order-3 cell with the shard key (g_key).  Every query form
-- runs at 20 centres taken from the catalogue, once to warm up and once
-- timed; the median ms per query, planning included (EXECUTE).
\pset pager off
SET max_parallel_workers_per_gather = 0;

DROP TABLE IF EXISTS perf_centres, perf_probe_ref, perf_probe_local, perf_res;
-- every 100,000th row in id order: 20 centres spread over the sky as the catalogue is
CREATE TABLE perf_centres AS
SELECT row_number() OVER () AS i, ra, dec
FROM (SELECT ra, dec, row_number() OVER (ORDER BY id) AS rn FROM g_local) s WHERE rn % 100000 = 1;
-- 2,000 probes a hair off catalogue positions (every 1,000th row), for the cross-match
CREATE TABLE perf_probe_local AS
SELECT id AS pid, ra + 0.0002 AS ra, dec
FROM (SELECT id, ra, dec, row_number() OVER (ORDER BY id) AS rn FROM g_local) s WHERE rn % 1000 = 7;
CREATE TABLE perf_probe_ref AS SELECT * FROM perf_probe_local;
SELECT create_reference_table('perf_probe_ref');
ANALYZE perf_probe_local; ANALYZE perf_probe_ref;

CREATE OR REPLACE FUNCTION perf_time(q text, per_centre bool) RETURNS float8 LANGUAGE plpgsql AS $f$
DECLARE c record; t0 timestamptz; ms float8[] := '{}'; pass int; x text;
BEGIN
  FOR pass IN 1..2 LOOP
    FOR c IN SELECT * FROM perf_centres WHERE per_centre OR i = 1 ORDER BY i LOOP
      t0 := clock_timestamp();
      EXECUTE replace(replace(q, ':ra', c.ra::text), ':dec', c.dec::text) INTO x;
      IF pass = 2 THEN ms := ms || extract(epoch FROM clock_timestamp() - t0) * 1000; END IF;
    END LOOP;
  END LOOP;
  RETURN (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY m) FROM unnest(ms) m);
END $f$;

CREATE TABLE perf_res (workload text, tbl text, ms float8);
DO $d$
DECLARE w record; t text;
BEGIN
  FOR w IN SELECT * FROM (VALUES
    (1, 'cone 1"',             'SELECT count(*) FROM %s WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', :ra, :dec, 0.000278))', true),
    (2, 'cone 0.2 deg',        'SELECT count(*) FROM %s WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', :ra, :dec, 0.2))', true),
    (3, 'cone 2 deg',          'SELECT count(*) FROM %s WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', :ra, :dec, 2))', true),
    (4, 'cone 10 deg',         'SELECT count(*) FROM %s WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', :ra, :dec, 10))', true),
    (5, 'box 1 deg',           'SELECT count(*) FROM %s WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), BOX(''ICRS'', :ra, greatest(least(:dec, 80), -80), 1, 1))', true),
    (6, 'nearest 10',          'SELECT string_agg(id::text, '','') FROM (SELECT id FROM %s ORDER BY distance(ra, dec, :ra, :dec) LIMIT 10) s', true),
    (7, 'xmatch 2000 x 2"',    'SELECT count(*) FROM PROBE p JOIN %s c ON 1 = CONTAINS(POINT(''ICRS'', c.ra, c.dec), CIRCLE(''ICRS'', p.ra, p.dec, 0.000556))', false),
    (8, 'coverage order 8',    'SELECT skycell_moc_agg(8, skycell_ang2cell(ra, dec) >> 42) FROM %s', false),
    (9, 'full scan, dec > 0',  'SELECT count(*) FROM %s WHERE dec > 0', false),
    (10, 'estimate cone 5 deg', 'SELECT skycell_count_estimate(''%s'', CIRCLE(''ICRS'', :ra, :dec, 5))', true)
  ) v(n, name, q, per_centre) ORDER BY n LOOP
    FOREACH t IN ARRAY ARRAY['g_local', 'g_id', 'g_key'] LOOP
      INSERT INTO perf_res VALUES (w.name, t,
        perf_time(replace(format(w.q, t), 'PROBE', CASE WHEN t = 'g_local' THEN 'perf_probe_local' ELSE 'perf_probe_ref' END),
                  w.per_centre));
    END LOOP;
  END LOOP;
END $d$;

SELECT workload,
       round(max(ms) FILTER (WHERE tbl = 'g_local')::numeric, 2) AS one_instance_ms,
       round(max(ms) FILTER (WHERE tbl = 'g_id')::numeric, 2) AS citus_by_id_ms,
       round(max(ms) FILTER (WHERE tbl = 'g_key')::numeric, 2) AS citus_by_cell_ms
FROM perf_res GROUP BY workload ORDER BY min(ctid);
