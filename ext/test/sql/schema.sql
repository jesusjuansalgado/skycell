-- skycell in a schema that is not on the search_path (0.56): everything
-- named with that schema works, the functions written in SQL and PL/pgSQL
-- included, and the density map is found there.
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS skycell CASCADE;
CREATE SCHEMA "Astro 3";
CREATE EXTENSION skycell SCHEMA "Astro 3";
RESET client_min_messages;
SET search_path = public;
CREATE TABLE sc_cat AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra, degrees(asin(1 - 2 * (i + 0.5) / 20000.0)) AS dec
FROM generate_series(0, 19999) i;
CREATE INDEX sc_cat_cell ON sc_cat ("Astro 3".skycell_ang2cell(ra, dec));
ANALYZE sc_cat;
CREATE FUNCTION sc_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'SkycellCone' THEN p := p || 'cone '; END IF;
    IF l ~ 'SkycellNearest' THEN p := p || 'nearest '; END IF;
    IF l ~ 'Seq Scan' THEN p := p || 'seq '; END IF;
  END LOOP;
  RETURN rtrim(p);
END $$;
SELECT sc_plan($q$SELECT * FROM sc_cat WHERE "Astro 3".CONTAINS("Astro 3".POINT('ICRS', ra, dec), "Astro 3".CIRCLE('ICRS', 10, 10, 5)) = 1$q$) AS cone,
       sc_plan($q$SELECT * FROM sc_cat WHERE "Astro 3".DISTANCE("Astro 3".POINT('ICRS', ra, dec), "Astro 3".POINT('ICRS', 10, 10)) > 178$q$) AS beyond,
       sc_plan($q$SELECT id FROM sc_cat ORDER BY "Astro 3".skycell_dist(ra, dec, 10, 10) LIMIT 5$q$) AS nearest;
SELECT "Astro 3".skycell_density_build('sc_cat', 'skycell_ang2cell', 50, 10) > 0 AS map_built;
SELECT DISTINCT relname AS map_names FROM "Astro 3".skycell_density_map;
SELECT "Astro 3".skycell_coverage_moc('sc_cat', 1) AS coverage,
       (SELECT count(*) FROM "Astro 3".skycell_region_moc_ranges("Astro 3".circle(10, 10, 2))) > 0 AS moc_ranges,
       "Astro 3".skycell_density_view('sc_cat', 'sc_dens', 3, 0) AS density_view_rows,
       "Astro 3".skycell_density_drop('sc_cat') > 0 AS map_dropped;
DROP FUNCTION sc_plan(text);
DROP MATERIALIZED VIEW sc_dens;
DROP TABLE sc_cat;
RESET search_path;
SET client_min_messages = warning;
DROP EXTENSION skycell CASCADE;
DROP SCHEMA "Astro 3";
CREATE EXTENSION skycell;
RESET client_min_messages;
