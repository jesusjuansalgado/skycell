-- Every warm centre's plan, without costs, under the current database's
-- skycell: skycell (custom scan) and skycell-rw (custom_scan off).  Run in
-- real26 under 0.26 and in real36 under 0.36; plans_026.csv and plans_036.csv.
-- Needs 30_real_cones.sql's realcone_centers and realcone_sql.
CREATE OR REPLACE FUNCTION realcone_plan(method text, c realcone_centers) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE l text; s text := '';
BEGIN
  PERFORM set_config('skycell.custom_scan', CASE method WHEN 'skycell-rw' THEN 'off' ELSE 'on' END, true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || realcone_sql('skycell', 'gaia_realc', c.ra0, c.dec0, c.r) LOOP
    s := s || l || E'\n';
  END LOOP;
  RETURN s;
END $$;
\copy (SELECT c.qid, c.label, m, realcone_plan(m, c) AS plan FROM realcone_centers c, unnest(ARRAY['skycell','skycell-rw']) m ORDER BY 1, 3) TO STDOUT WITH CSV HEADER
DROP FUNCTION realcone_plan(text, realcone_centers);
