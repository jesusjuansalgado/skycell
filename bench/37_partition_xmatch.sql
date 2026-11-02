-- Cross-matching against a partitioned catalogue (0.44) and against the same
-- rows unpartitioned. Run on real36: 5M of the 10M real Gaia DR3 positions,
-- once in one table (bp_flat) and once in 16 partitions by cell range
-- (bp_part), the way an archive partitions by HEALPix index or by Gaia's
-- source_id, both with the same columns and a cell index each.
--
-- Probes on catalogue sources (moved 0.5"), 10^3, 10^4 and 10^5 of them, at
-- 1", 5" and 30". Per (probes, radius) the two tables in random order, :reps
-- rounds, warm (one untimed run first). Run once per library version; the
-- tables are built by the first run and kept.
\set ON_ERROR_STOP 1
SET max_parallel_workers_per_gather = 0;

DO $$
BEGIN
  IF to_regclass('bp_flat') IS NULL THEN
    CREATE TABLE bp_flat AS
      SELECT source_id, ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (50) REPEATABLE (37);
    CREATE INDEX bp_flat_cell ON bp_flat (skycell_ang2cell(ra, dec));
    ALTER INDEX bp_flat_cell ALTER COLUMN 1 SET STATISTICS 1000;
    ANALYZE bp_flat;
    CREATE TABLE bp_part (source_id bigint, ra float8, dec float8)
      PARTITION BY RANGE (skycell_ang2cell(ra, dec));
    FOR i IN 0 .. 15 LOOP
      EXECUTE format('CREATE TABLE bp_part%s PARTITION OF bp_part FOR VALUES FROM (%s) TO (%s)', i,
                     CASE WHEN i = 0 THEN 'MINVALUE' ELSE (i * (3458764513820540928 / 16))::text END,
                     CASE WHEN i = 15 THEN 'MAXVALUE' ELSE ((i + 1) * (3458764513820540928 / 16))::text END);
    END LOOP;
    INSERT INTO bp_part SELECT * FROM bp_flat ORDER BY skycell_ang2cell(ra, dec);
    CREATE INDEX ON bp_part (skycell_ang2cell(ra, dec));
    FOR i IN 0 .. 15 LOOP
      EXECUTE format('ALTER INDEX bp_part%s_skycell_ang2cell_idx ALTER COLUMN 1 SET STATISTICS 1000', i);
    END LOOP;
    ANALYZE bp_part;
    -- the flat table in the same (cell) order as the partitions
    CLUSTER bp_flat USING bp_flat_cell;
    ANALYZE bp_flat;
    CREATE TABLE bp_probe AS
      SELECT row_number() OVER (ORDER BY random())::int AS pid, ra + 0.5 / 3600 AS ra, dec
      FROM (SELECT ra, dec FROM bp_flat TABLESAMPLE SYSTEM (5) REPEATABLE (7) LIMIT 100000) s;
    ANALYZE bp_probe;
  END IF;
END $$;

CREATE TEMP TABLE bp_time (rep int, nprobe int, radius_as float8, tbl text, ms float8, n bigint);
CREATE OR REPLACE FUNCTION bp_run(reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record; t text; q text; t0 timestamptz; x bigint;
BEGIN
  FOR rep IN 1 .. reps LOOP
    FOR c IN SELECT * FROM unnest(ARRAY[1000, 10000, 100000]) np, unnest(ARRAY[1.0, 5.0, 30.0]) ras ORDER BY random() LOOP
      FOR t IN SELECT unnest(ARRAY['bp_flat', 'bp_part']) ORDER BY random() LOOP
        q := format('SELECT count(*) FROM (SELECT * FROM bp_probe WHERE pid <= %s) p JOIN %s c
                     ON 1 = CONTAINS(POINT(''ICRS'', c.ra, c.dec), CIRCLE(''ICRS'', p.ra, p.dec, %s))',
                    c.np, t, c.ras / 3600);
        EXECUTE q INTO x;                              -- warm
        t0 := clock_timestamp();
        EXECUTE q INTO x;
        INSERT INTO bp_time VALUES (rep, c.np, c.ras, t,
                                    extract(epoch FROM clock_timestamp() - t0) * 1000, x);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
SELECT setseed(0.37);
SELECT bp_run(:reps);
\copy bp_time TO 'partition.csv' WITH CSV HEADER

-- both tables must match the same rows
SELECT count(*) AS cases, count(*) FILTER (WHERE nmin <> nmax) AS tables_disagree
FROM (SELECT rep, nprobe, radius_as, min(n) nmin, max(n) nmax FROM bp_time GROUP BY 1, 2, 3) a;

-- median ms per table, and partitioned over flat
SELECT nprobe, radius_as,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) FILTER (WHERE tbl = 'bp_flat'))::numeric, 1) AS flat_ms,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) FILTER (WHERE tbl = 'bp_part'))::numeric, 1) AS part_ms,
       round(((percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) FILTER (WHERE tbl = 'bp_part'))
            / (percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) FILTER (WHERE tbl = 'bp_flat')))::numeric, 2) AS part_over_flat
FROM bp_time GROUP BY 1, 2 ORDER BY 1, 2;

-- the plan, for one case
EXPLAIN (COSTS OFF)
SELECT count(*) FROM (SELECT * FROM bp_probe WHERE pid <= 1000) p JOIN bp_part c
ON 1 = CONTAINS(POINT('ICRS', c.ra, c.dec), CIRCLE('ICRS', p.ra, p.dec, 5.0 / 3600));
