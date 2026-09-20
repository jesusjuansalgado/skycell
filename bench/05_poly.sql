-- Polygon benchmark: random convex quadrilaterals, 0.05 .. 2 deg across.
\set ON_ERROR_STOP 1
SELECT setseed(0.31);
DROP TABLE IF EXISTS bench_polys CASCADE;
CREATE TABLE bench_polys AS
SELECT q, ARRAY[cra - s * f, cdec - s / 2, cra + s * f, cdec - s / 2, cra + s * f / 2, cdec + s / 2, cra - s * f / 2, cdec + s / 2]::float8[] AS poly, s
FROM (SELECT q, p.ra AS cra, greatest(-80, least(80, p.dec)) AS cdec, power(10, -1.3 + 1.6 * random()) AS s,
             1 / cos(radians(greatest(-80, least(80, p.dec)))) AS f
      FROM generate_series(1, 120) q JOIN bench_pool p ON p.rn = q * 11) x;

CREATE TABLE IF NOT EXISTS bench_poly (method text, pass int, q int, n bigint, ms float8);
CREATE OR REPLACE FUNCTION poly_sql(method text, poly float8[]) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'q3c' THEN format('SELECT count(*), sum(mag) FROM cat_q3c WHERE q3c_poly_query(ra, dec, %L::float8[])', poly)
    WHEN 'pgsphere' THEN format('SELECT count(*), sum(mag) FROM cat_sphere WHERE pos <@ spoly_deg(%L::float8[])', poly)
    WHEN 'skycell' THEN format('SELECT count(*), sum(mag) FROM cat_cell WHERE skycell_poly(cell, ra, dec, %L::float8[])', poly)
  END
$$;
CREATE OR REPLACE FUNCTION bench_poly_run(method text, pass int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE b record; n bigint; s float8; t0 timestamptz;
BEGIN
  DELETE FROM bench_poly x WHERE x.method = bench_poly_run.method AND x.pass = bench_poly_run.pass;
  FOR b IN SELECT * FROM bench_polys ORDER BY q LOOP
    t0 := clock_timestamp();
    EXECUTE poly_sql(method, b.poly) INTO n, s;
    INSERT INTO bench_poly VALUES (method, pass, b.q, n, extract(epoch FROM clock_timestamp() - t0) * 1000);
  END LOOP;
END $$;
