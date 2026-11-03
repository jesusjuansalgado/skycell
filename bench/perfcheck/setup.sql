-- perfcheck tables, built once in the benchmark database (real36: the 10M
-- real Gaia DR3 positions in gaia_realc, with its cell index).  Kept between
-- runs; drop them to rebuild.
\set ON_ERROR_STOP 1
SET max_parallel_workers_per_gather = 0;
DO $$
BEGIN
  IF to_regclass('perf_centre') IS NULL THEN
    PERFORM setseed(0.47);
    -- 30 centres on catalogue sources, as a TAP user's queries mostly are
    CREATE TABLE perf_centre AS
      SELECT row_number() OVER (ORDER BY random())::int AS c, ra + 0.0001 AS ra, dec
      FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (0.01) REPEATABLE (47) LIMIT 30) s;
    -- probes for the cross-matches: 30 sets of 300 sources, nudged by 0.5"
    CREATE TABLE perf_probe AS
      SELECT (row_number() OVER (ORDER BY random()) - 1)::int / 300 + 1 AS c,
             ra + 0.5 / 3600 AS ra, dec
      FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (1) REPEATABLE (48) LIMIT 9000) s;
    CREATE INDEX ON perf_probe (c);
    ANALYZE perf_centre;
    ANALYZE perf_probe;
  END IF;
  -- an ObsCore-shaped table, observations grouped in fields, its heap out of
  -- cell order (copied from oc05 by hand: pg_dump -t oc -t oc_queries, both
  -- renamed perf_oc, perf_oc_queries); its first 30 query centres
  IF to_regclass('perf_oc_queries') IS NOT NULL AND to_regclass('perf_oc_centre') IS NULL THEN
    CREATE TABLE perf_oc_centre AS
      SELECT qid::int AS c, ra, dec FROM perf_oc_queries WHERE qid <= 30;
    ANALYZE perf_oc_centre;
  END IF;
  IF to_regclass('perf_part') IS NULL THEN
    -- 2M of the positions in 16 partitions by cell range
    CREATE TABLE perf_part (source_id bigint, ra float8, dec float8)
      PARTITION BY RANGE (skycell_ang2cell(ra, dec));
    FOR i IN 0 .. 15 LOOP
      EXECUTE format('CREATE TABLE perf_part%s PARTITION OF perf_part FOR VALUES FROM (%s) TO (%s)', i,
                     CASE WHEN i = 0 THEN 'MINVALUE' ELSE (i * (3458764513820540928 / 16))::text END,
                     CASE WHEN i = 15 THEN 'MAXVALUE' ELSE ((i + 1) * (3458764513820540928 / 16))::text END);
    END LOOP;
    INSERT INTO perf_part SELECT source_id, ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (20) REPEATABLE (49)
      ORDER BY skycell_ang2cell(ra, dec);
    CREATE INDEX ON perf_part (skycell_ang2cell(ra, dec));
    ANALYZE perf_part;
  END IF;
END $$;
