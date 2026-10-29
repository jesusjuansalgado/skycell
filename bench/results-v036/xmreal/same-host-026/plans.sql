-- One fixed probe set (every 1000th source_id-ordered row of gaia_realc: 10,000
-- probes, the same in both databases) and each method's 1" cross-match plan
-- with buffers and time, warmed then measured three times.  Run in real26
-- under 0.26 and in real36 under 0.36 (plans.sh).
\set ON_ERROR_STOP 1
DROP TABLE IF EXISTS xp;
CREATE TABLE xp AS SELECT ra, dec FROM (SELECT ra, dec, row_number() OVER (ORDER BY source_id) rn FROM gaia_realc) s WHERE rn % 1000 = 0;
ANALYZE xp;
CREATE OR REPLACE FUNCTION xms_plan(m text, r float8) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE q text := xms_sql(m, r / 3600.0); l text; i int;
BEGIN
  PERFORM set_config('skycell.custom_scan', CASE WHEN m = 'skycell_slots' THEN 'off' ELSE 'on' END, true);
  EXECUTE q;
  FOR i IN 1 .. 3 LOOP
    FOR l IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF) ' || q LOOP
      RETURN NEXT m || ' ' || i || ' | ' || l;
    END LOOP;
  END LOOP;
END $$;
SELECT xms_plan(m, 1) FROM unnest(ARRAY['q3c_join','skycell_cs','skycell_join','skycell_slots','pgsphere']) m;
DROP FUNCTION xms_plan(text, float8);
