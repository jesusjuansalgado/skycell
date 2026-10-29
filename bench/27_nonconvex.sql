-- Non-convex polygons against pgSphere: U, L, a four-tooth comb and a
-- five-pointed star, with the convex quadrilateral of 05_poly.sql on the same
-- centres and sizes for reference.  Run on gaia_realc (the 10M real Gaia DR3
-- positions, with both skycell's cell index and pgSphere's GiST on pos).
--
-- skycell is written as ADQL writes it, 1 = CONTAINS(POINT, POLYGON), and
-- goes through the polygon rewrite; pgSphere as pos <@ spoly.  Per polygon,
-- both methods in random order, :n executions each (planned and run), each
-- shifted 1e-9 deg so that nothing planned or covered is reused.
\set ON_ERROR_STOP 1

-- unit shapes, x and y in [-0.5, 0.5], counter-clockwise
CREATE TEMP TABLE nc_shape (shape text, v float8[]);
INSERT INTO nc_shape VALUES
  ('quad', '{-0.5,-0.5, 0.5,-0.5, 0.25,0.5, -0.25,0.5}'),
  ('U',    '{-0.5,-0.5, 0.5,-0.5, 0.5,0.5, 0.25,0.5, 0.25,-0.25, -0.25,-0.25, -0.25,0.5, -0.5,0.5}'),
  ('L',    '{-0.5,-0.5, 0.5,-0.5, 0.5,-0.2, -0.2,-0.2, -0.2,0.5, -0.5,0.5}'),
  ('comb', '{-0.5,-0.5, 0.5,-0.5, 0.5,0.5, 0.4,0.5, 0.4,-0.2, 0.2,-0.2, 0.2,0.5, 0.1,0.5, 0.1,-0.2,
             -0.1,-0.2, -0.1,0.5, -0.2,0.5, -0.2,-0.2, -0.4,-0.2, -0.4,0.5, -0.5,0.5}'),
  ('star', (SELECT array_agg(c ORDER BY k, j) FROM (
              SELECT k, j, CASE j WHEN 0 THEN r * cos(radians(90 + 36 * k)) ELSE r * sin(radians(90 + 36 * k)) END c
              FROM generate_series(0, 9) k, LATERAL (SELECT CASE WHEN k % 2 = 0 THEN 0.5 ELSE 0.2 END r) rr,
                   generate_series(0, 1) j) s));

-- centres on catalogue sources, sizes 0.05..2 deg across, log-uniform
SELECT setseed(0.27);
CREATE TEMP TABLE nc_centre AS
SELECT row_number() OVER ()::int AS c, ra, greatest(-80, least(80, dec)) AS dec,
       power(10, -1.3 + 1.6 * random()) AS s
FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (0.01) REPEATABLE (11) LIMIT :ncentres) x;

-- each shape at each centre: vertex (x, y) -> (ra + x s / cos dec, dec + y s)
CREATE TEMP TABLE nc_poly AS
SELECT c, shape, s, (SELECT array_agg(CASE WHEN i % 2 = 1 THEN n.ra + sh.v[i] * n.s / cos(radians(n.dec))
                                          ELSE n.dec + sh.v[i] * n.s END ORDER BY i)
                     FROM generate_subscripts(sh.v, 1) i) AS v
FROM nc_centre n, nc_shape sh;

CREATE OR REPLACE FUNCTION nc_sql(m text, v float8[]) RETURNS text LANGUAGE sql AS $$
  SELECT CASE m
    WHEN 'pgsphere' THEN format('SELECT count(*) FROM gaia_realc WHERE pos <@ spoly_deg(%L::float8[])', v)
    ELSE format('SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), POLYGON(''ICRS'', VARIADIC %L::float8[]))', v)
  END $$;

CREATE TEMP TABLE nc_time (rep int, c int, shape text, s float8, method text, us float8, n bigint);
CREATE OR REPLACE FUNCTION nc_run(n int, reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE p record; m text; t0 timestamptz; x bigint; w float8[];
BEGIN
  FOR rep IN 1 .. reps LOOP
    FOR p IN SELECT * FROM nc_poly ORDER BY random() LOOP
      FOR m IN SELECT unnest(ARRAY['pgsphere', 'skycell']) ORDER BY random() LOOP
        EXECUTE nc_sql(m, p.v) INTO x;                  -- warm its pages
        t0 := clock_timestamp();
        FOR i IN 1 .. n LOOP
          -- every ra moved by the same 1e-9 deg step: a new polygon each time
          SELECT array_agg(CASE WHEN k % 2 = 1 THEN p.v[k] + 1e-9 * (i + n * rep) ELSE p.v[k] END ORDER BY k)
            INTO w FROM generate_subscripts(p.v, 1) k;
          EXECUTE nc_sql(m, w) INTO x;
        END LOOP;
        INSERT INTO nc_time VALUES (rep, p.c, p.shape, p.s, m,
          extract(epoch FROM clock_timestamp() - t0) * 1e6 / n, x);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
SELECT setseed(0.5);
SELECT nc_run(:n, :reps);
\copy nc_time TO 'nonconvex.csv' WITH CSV HEADER

-- the answers: both methods, and the exact test alone (a sequential scan),
-- must count the same rows
SET enable_indexscan = off; SET enable_bitmapscan = off; SET skycell.custom_scan = off;
CREATE TEMP TABLE nc_truth AS
SELECT c, shape, (SELECT count(*) FROM gaia_realc g WHERE skycell_in_poly(g.ra, g.dec, p.v)) AS n
FROM nc_poly p WHERE c <= :ntruth;
RESET enable_indexscan; RESET enable_bitmapscan; RESET skycell.custom_scan;
SELECT count(DISTINCT (c, shape)) AS polygons,
       count(*) FILTER (WHERE n_sky <> n_pgs) AS methods_disagree
FROM (SELECT c, shape, max(n) FILTER (WHERE method = 'skycell') n_sky,
             max(n) FILTER (WHERE method = 'pgsphere') n_pgs FROM nc_time GROUP BY 1, 2) a;
SELECT count(*) AS checked, count(*) FILTER (WHERE t.n <> a.n) AS differ_from_exact
FROM nc_truth t JOIN (SELECT DISTINCT c, shape, n FROM nc_time WHERE method = 'skycell') a USING (c, shape);
