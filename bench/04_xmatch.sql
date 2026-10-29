-- Cross-match benchmark: probe catalogue joined to the big catalogue.
-- 50% probes are catalogue sources displaced by ~0.3" (real matches),
-- 50% are uniform on the sky.  Probes are sorted along the sky curve, as a
-- real pipeline would do, which helps all methods equally.
\set ON_ERROR_STOP 1
\if :{?nprobe}
\else
  \set nprobe 200000
\endif

SELECT setseed(0.99);
DROP TABLE IF EXISTS probe;
CREATE TABLE probe AS
WITH d AS (
  SELECT ra, dec FROM src TABLESAMPLE BERNOULLI (100.0 * :nprobe / 2 / (SELECT reltuples FROM pg_class WHERE relname = 'src')) REPEATABLE (3)
  LIMIT :nprobe / 2
), pts AS (
  SELECT ((ra + 0.3 / 3600 * sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random()) / greatest(cos(radians(dec)), 1e-3)) + 360)::numeric % 360 AS ra,
         greatest(-90, least(90, dec + 0.3 / 3600 * sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random()))) AS dec
  FROM d
  UNION ALL
  SELECT 360 * random(), degrees(asin(2 * random() - 1)) FROM generate_series(1, :nprobe / 2)
)
SELECT row_number() OVER (ORDER BY skycell_ang2cell(ra::float8, dec)) AS pid, ra::float8 AS ra, dec,
       skycell_ang2cell(ra::float8, dec) AS cell, spoint(radians(ra::float8), radians(dec)) AS pos
FROM pts ORDER BY cell;
ANALYZE probe;

CREATE TABLE IF NOT EXISTS bench_xmatch (method text, radius_arcsec float8, pass int, n bigint, ms float8, plan text);

CREATE OR REPLACE FUNCTION xmatch_sql(method text, r float8) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'q3c' THEN format('SELECT count(*), sum(c.id) FROM probe p JOIN cat_q3c c ON q3c_join(p.ra, p.dec, c.ra, c.dec, %s)', r)
    WHEN 'q3c_ios' THEN format('SELECT count(*), sum(c.id) FROM probe p JOIN cat_q3c c ON q3c_join(p.ra, p.dec, c.ra, c.dec, %s)', r)
    WHEN 'pgsphere' THEN format('SELECT count(*), sum(c.id) FROM probe p JOIN cat_sphere c ON c.pos <@ scircle(p.pos, radians(%s))', r)
    WHEN 'skycell_slots' THEN format('SELECT count(*), sum(c.id) FROM probe p JOIN cat_cell c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, %s)', r)
    WHEN 'skycell_lateral' THEN format('SELECT count(*), sum(c.id) FROM probe p CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, %s, ''cat_cell'') g JOIN cat_cell c ON c.cell BETWEEN g.lo AND g.hi AND skycell_in_cone(c.ra, c.dec, p.ra, p.dec, %s)', r, r)
    WHEN 'skycell_lateral_ios' THEN format('SELECT count(*), sum(c.id) FROM probe p CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, %s, ''cat_cell'') g JOIN cat_cell c ON c.cell BETWEEN g.lo AND g.hi AND skycell_in_cone(c.ra, c.dec, p.ra, p.dec, %s)', r, r)
  END
$$;

CREATE OR REPLACE FUNCTION bench_xmatch_run(method text, r_arcsec float8, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; s numeric; t0 timestamptz; plan text := ''; l text;
BEGIN
  -- Memoize keyed on (lo, hi) never hits for distinct probes; it only adds overhead
  IF method LIKE 'skycell_lateral%' THEN PERFORM set_config('enable_memoize', 'off', true); END IF;
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || xmatch_sql(method, r_arcsec / 3600) LOOP
      plan := plan || regexp_replace(l, '\{[0-9,]{100,}\}', '{...}', 'g') || E'\n';
    END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE xmatch_sql(method, r_arcsec / 3600) INTO n, s;
  INSERT INTO bench_xmatch VALUES (method, r_arcsec, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;
