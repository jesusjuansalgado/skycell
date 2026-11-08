-- Sources inside a stored footprint, by a join (0.54): the footprint is
-- another table's row, covered per outer row by the custom scan, against
-- what the planner chose before (skycell.custom_scan = off gives the same
-- plan for this query: the rewrite's range slots, or a scan of the table
-- when those look dearer).  On real36's gaia_realc, the 10M real Gaia DR3
-- positions.
--
-- 2000 footprints, 0.1-0.4 deg, centred on catalogue sources: circles, boxes
-- and two-detector mosaics (a UNION of two boxes).  30 of them, three
-- repetitions, each query planned and run after an untimed one.
\set ON_ERROR_STOP 1
SELECT setseed(0.6);
DROP TABLE IF EXISTS fp_bench;
CREATE TABLE fp_bench AS
SELECT row_number() OVER () AS obs_id,
  CASE (row_number() OVER ()) % 3
   WHEN 0 THEN circle(ra, dec, s / 2)
   WHEN 1 THEN box(ra, dec, s, s)
   ELSE skycell_union(box(ra - s / 4 / cos(radians(dec)), dec, s / 2.2, s),
                      box(ra + s / 4 / cos(radians(dec)), dec, s / 2.2, s)) END AS s_region
FROM (SELECT ra, greatest(-80, least(80, dec)) AS dec, 0.1 + 0.3 * random() AS s
      FROM gaia_realc TABLESAMPLE SYSTEM (0.02) LIMIT 2000) x;
CREATE INDEX ON fp_bench (obs_id);
ANALYZE fp_bench;
CREATE TEMP TABLE fpt (m text, k int, us float8, n bigint);
DO $$
DECLARE t0 timestamptz; n bigint; q text;
BEGIN
  FOR rep IN 1 .. 3 LOOP
  FOR k IN 1 .. 30 LOOP
    q := format('SELECT count(*) FROM gaia_realc c JOIN fp_bench o ON CONTAINS(POINT(''ICRS'', c.ra, c.dec), o.s_region) = 1 WHERE o.obs_id = %s', k * 37);
    PERFORM set_config('skycell.custom_scan', 'on', false);
    EXECUTE q INTO n;
    t0 := clock_timestamp(); EXECUTE q INTO n;
    INSERT INTO fpt VALUES ('new', k, extract(epoch FROM clock_timestamp() - t0) * 1e6, n);
    IF rep = 1 THEN
      PERFORM set_config('skycell.custom_scan', 'off', false);
      t0 := clock_timestamp(); EXECUTE q INTO n;
      INSERT INTO fpt VALUES ('old', k, extract(epoch FROM clock_timestamp() - t0) * 1e6, n);
    END IF;
  END LOOP;
  END LOOP;
END $$;
SELECT m, count(*), round(percentile_cont(0.5) WITHIN GROUP (ORDER BY us)::numeric / 1000, 2) AS median_ms,
       round(min(us)::numeric / 1000, 2) min_ms, round(max(us)::numeric / 1000, 2) max_ms
FROM fpt GROUP BY m;
SELECT count(*) AS disagree FROM (SELECT k FROM fpt GROUP BY k HAVING min(n) <> max(n)) x;
