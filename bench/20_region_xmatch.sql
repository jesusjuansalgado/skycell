-- Cross-match against a per-row stored footprint (a "s_region"-style column),
-- mixing CIRCLE and POLYGON footprints in the same corpus -- the capability
-- skycell_region_bound (skycell 0.9) adds to <@'s non-constant fallback.
--
-- Q3C has no region type at all: it can only join the circle subset, via its
-- own q3c_join on the circle's plain (ra, dec, r) scalars -- polygon
-- footprints have no q3c join form (q3c_poly_query takes a literal array, not
-- a per-row column), so the polygon and mixed rows are simply out of reach
-- for it, not just slower.
--
-- pgSphere has no single region supertype either: scircle and spoly are
-- distinct types with their own GiST opclasses (both indexable against a
-- plain spoint column, so pgSphere is fully competitive on speed), so a
-- corpus mixing both kinds needs two typed columns and a query that UNIONs
-- one join per column. skycell's point here is that one skyregion column and
-- one <@ predicate cover both kinds in the same query.
\set ON_ERROR_STOP 1
\if :{?nfp}
\else
  \set nfp 5000
\endif

SELECT setseed(0.77);
DROP TABLE IF EXISTS fpr;
CREATE TABLE fpr AS
SELECT f AS fid, s.ra AS ra0, greatest(-85, least(85, s.dec)) AS dec0,
       power(10, -1.7 + 1.2 * random()) AS r,
       (f % 2 = 0) AS is_poly
FROM generate_series(1, :nfp) f
JOIN src s ON s.id = 1 + (f * 97) % (SELECT count(*) FROM src);

ALTER TABLE fpr ADD COLUMN s_region skyregion;
ALTER TABLE fpr ADD COLUMN reg_circ scircle;
ALTER TABLE fpr ADD COLUMN reg_poly spoly;

UPDATE fpr SET s_region = circle('ICRS', ra0, dec0, r),
               reg_circ = scircle(spoint(radians(ra0), radians(dec0)), radians(r))
WHERE NOT is_poly;
UPDATE fpr SET s_region = polygon('ICRS', ra0 - r, dec0 - r, ra0 + r, dec0 - r, ra0 + r, dec0 + r, ra0 - r, dec0 + r),
               reg_poly = spoly(format('{(%1$sd,%2$sd),(%3$sd,%2$sd),(%3$sd,%4$sd),(%1$sd,%4$sd)}',
                                       ra0 - r, dec0 - r, ra0 + r, dec0 + r))
WHERE is_poly;
ANALYZE fpr;

CREATE TABLE IF NOT EXISTS bench_region_xmatch (method text, scope text, pass int, n bigint, ms float8, plan text);
TRUNCATE bench_region_xmatch;

CREATE OR REPLACE FUNCTION region_xmatch_sql(method text, scope text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method || ':' || scope
    WHEN 'skycell:circle'  THEN $q$SELECT count(*) FROM fpr f, cat_cell c WHERE NOT f.is_poly AND point('ICRS', c.ra, c.dec) <@ f.s_region$q$
    WHEN 'skycell:poly'    THEN $q$SELECT count(*) FROM fpr f, cat_cell c WHERE f.is_poly AND point('ICRS', c.ra, c.dec) <@ f.s_region$q$
    WHEN 'skycell:mixed'   THEN $q$SELECT count(*) FROM fpr f, cat_cell c WHERE point('ICRS', c.ra, c.dec) <@ f.s_region$q$
    WHEN 'pgsphere:circle' THEN $q$SELECT count(*) FROM fpr f, cat_sphere c WHERE NOT f.is_poly AND c.pos <@ f.reg_circ$q$
    WHEN 'pgsphere:poly'   THEN $q$SELECT count(*) FROM fpr f, cat_sphere c WHERE f.is_poly AND c.pos <@ f.reg_poly$q$
    WHEN 'pgsphere:mixed'  THEN $q$SELECT (SELECT count(*) FROM fpr f, cat_sphere c WHERE NOT f.is_poly AND c.pos <@ f.reg_circ)
                                        + (SELECT count(*) FROM fpr f, cat_sphere c WHERE f.is_poly AND c.pos <@ f.reg_poly)$q$
    WHEN 'q3c:circle'      THEN $q$SELECT count(*) FROM fpr f, cat_q3c c WHERE NOT f.is_poly AND q3c_join(f.ra0, f.dec0, c.ra, c.dec, f.r)$q$
  END
$$;

CREATE OR REPLACE FUNCTION bench_region_xmatch_run(method text, scope text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; t0 timestamptz; plan text := ''; l text; q text;
BEGIN
  q := region_xmatch_sql(method, scope);
  IF q IS NULL THEN
    INSERT INTO bench_region_xmatch (method, scope, pass, n, ms, plan) VALUES (method, scope, pass, NULL, NULL, 'n/a: no region type / no per-row join form');
    RETURN;
  END IF;
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE q INTO n;
  INSERT INTO bench_region_xmatch (method, scope, pass, n, ms, plan)
  VALUES (method, scope, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;
