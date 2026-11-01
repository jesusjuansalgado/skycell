-- Composite regions (0.38) against what a query writes without them: the same
-- shape as a boolean of predicates, in skycell and in pgSphere.  Run on
-- gaia_realc (the 10M real Gaia DR3 positions, with skycell's cell index and
-- pgSphere's GiST on pos).
--
--   region   skycell, 1 = CONTAINS(POINT, REGION('<STC-S composite>'))
--   boolean  skycell, the pieces as ADQL predicates joined by AND / OR / NOT
--   pgsphere pgSphere, the pieces as pos <@ scircle / spoly, AND / OR / NOT
--
-- Shapes around a catalogue source, size s (0.05..2 deg, log-uniform):
--   annulus   DIFFERENCE of circles s and s/2
--   pair      UNION of two circles s/2, centres 0.75 s apart
--   lens      INTERSECTION of two circles s/2, centres 0.5 s apart
--   holed     DIFFERENCE of a square s across and a circle s/4
--   mosaic    UNION of five circles s/4 in a cross
-- Per shape, the three methods in random order, :n executions each (planned
-- and run), each shifted 1e-9 deg so that nothing planned or covered is reused.
-- skycell's boolean OR (pair, mosaic) is a sequential scan of the table, 2-5 s:
-- it is timed once per shape, in the first round only.
\set ON_ERROR_STOP 1

SELECT setseed(0.34);
CREATE TEMP TABLE bc_centre AS
SELECT row_number() OVER ()::int AS c, ra, greatest(-80, least(80, dec)) AS dec,
       power(10, -1.3 + 1.6 * random()) AS s
FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (0.01) REPEATABLE (34) LIMIT :ncentres) x;
CREATE TEMP TABLE bc_shape (shape text);
INSERT INTO bc_shape VALUES ('annulus'), ('pair'), ('lens'), ('holed'), ('mosaic');

-- a circle at offset (x, y) * s from the centre, radius r * s, in each spelling
CREATE OR REPLACE FUNCTION bc_circle(m text, ra float8, de float8, s float8, x float8, y float8, r float8)
RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE m
    WHEN 'region'   THEN format('CIRCLE %s %s %s', ra + x * s / cos(radians(de)), de + y * s, r * s)
    WHEN 'boolean'  THEN format('CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', %s, %s, %s))',
                                ra + x * s / cos(radians(de)), de + y * s, r * s)
    ELSE format('pos <@ scircle(spoint(radians(%s), radians(%s)), radians(%s))',
                ra + x * s / cos(radians(de)), de + y * s, r * s) END $$;
CREATE OR REPLACE FUNCTION bc_square(m text, ra float8, de float8, s float8) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE m
    WHEN 'region'  THEN format('POLYGON %s %s %s %s %s %s %s %s', a, b, c, b, c, d, a, d)
    WHEN 'boolean' THEN format('CONTAINS(POINT(''ICRS'', ra, dec), POLYGON(''ICRS'', %s, %s, %s, %s, %s, %s, %s, %s))',
                               a, b, c, b, c, d, a, d)
    ELSE format('pos <@ spoly_deg(ARRAY[%s, %s, %s, %s, %s, %s, %s, %s]::float8[])', a, b, c, b, c, d, a, d) END
  FROM (SELECT ra - s / 2 / cos(radians(de)) a, de - s / 2 b, ra + s / 2 / cos(radians(de)) c, de + s / 2 d) q $$;

CREATE OR REPLACE FUNCTION bc_sql(m text, shape text, ra float8, de float8, s float8) RETURNS text
LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
  w text;
  bp text := CASE m WHEN 'boolean' THEN ' = 1' ELSE '' END;     -- ADQL predicate true
  bn text := CASE m WHEN 'boolean' THEN ' = 0' ELSE '' END;     -- ADQL predicate false
  np text := CASE m WHEN 'pgsphere' THEN 'NOT ' ELSE '' END;
BEGIN
  IF m = 'region' THEN
    w := CASE shape
      WHEN 'annulus' THEN 'DIFFERENCE ICRS (' || bc_circle(m, ra, de, s, 0, 0, 1) || ' ' || bc_circle(m, ra, de, s, 0, 0, 0.5) || ')'
      WHEN 'pair'    THEN 'UNION ICRS (' || bc_circle(m, ra, de, s, -0.375, 0, 0.5) || ' ' || bc_circle(m, ra, de, s, 0.375, 0, 0.5) || ')'
      WHEN 'lens'    THEN 'INTERSECTION ICRS (' || bc_circle(m, ra, de, s, -0.25, 0, 0.5) || ' ' || bc_circle(m, ra, de, s, 0.25, 0, 0.5) || ')'
      WHEN 'holed'   THEN 'DIFFERENCE ICRS (' || bc_square(m, ra, de, s) || ' ' || bc_circle(m, ra, de, s, 0, 0, 0.25) || ')'
      WHEN 'mosaic'  THEN 'UNION ICRS (' || bc_circle(m, ra, de, s, 0, 0, 0.25) || ' ' || bc_circle(m, ra, de, s, 0.4, 0, 0.25)
                          || ' ' || bc_circle(m, ra, de, s, -0.4, 0, 0.25) || ' ' || bc_circle(m, ra, de, s, 0, 0.4, 0.25)
                          || ' ' || bc_circle(m, ra, de, s, 0, -0.4, 0.25) || ')' END;
    RETURN format('SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), REGION(%L))', w);
  END IF;
  w := CASE shape
    WHEN 'annulus' THEN bc_circle(m, ra, de, s, 0, 0, 1) || bp || ' AND ' || np || bc_circle(m, ra, de, s, 0, 0, 0.5) || bn
    WHEN 'pair'    THEN bc_circle(m, ra, de, s, -0.375, 0, 0.5) || bp || ' OR ' || bc_circle(m, ra, de, s, 0.375, 0, 0.5) || bp
    WHEN 'lens'    THEN bc_circle(m, ra, de, s, -0.25, 0, 0.5) || bp || ' AND ' || bc_circle(m, ra, de, s, 0.25, 0, 0.5) || bp
    WHEN 'holed'   THEN bc_square(m, ra, de, s) || bp || ' AND ' || np || bc_circle(m, ra, de, s, 0, 0, 0.25) || bn
    WHEN 'mosaic'  THEN bc_circle(m, ra, de, s, 0, 0, 0.25) || bp || ' OR ' || bc_circle(m, ra, de, s, 0.4, 0, 0.25) || bp
                        || ' OR ' || bc_circle(m, ra, de, s, -0.4, 0, 0.25) || bp || ' OR ' || bc_circle(m, ra, de, s, 0, 0.4, 0.25) || bp
                        || ' OR ' || bc_circle(m, ra, de, s, 0, -0.4, 0.25) || bp END;
  RETURN 'SELECT count(*) FROM gaia_realc WHERE ' || w;
END $$;

CREATE TEMP TABLE bc_time (rep int, c int, shape text, s float8, method text, us float8, n bigint);
CREATE OR REPLACE FUNCTION bc_run(n int, reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE p record; m text; t0 timestamptz; x bigint; k int;
BEGIN
  FOR rep IN 1 .. reps LOOP
    FOR p IN SELECT * FROM bc_centre, bc_shape ORDER BY random() LOOP
      FOR m IN SELECT unnest(ARRAY['region', 'boolean', 'pgsphere']) ORDER BY random() LOOP
        k := n;
        IF m = 'boolean' AND p.shape IN ('pair', 'mosaic') THEN
          CONTINUE WHEN rep > 1;
          k := 1;
        ELSE
          EXECUTE bc_sql(m, p.shape, p.ra, p.dec, p.s) INTO x;        -- warm its pages
        END IF;
        t0 := clock_timestamp();
        FOR i IN 1 .. k LOOP
          EXECUTE bc_sql(m, p.shape, p.ra + 1e-9 * (i + n * rep), p.dec, p.s) INTO x;
        END LOOP;
        INSERT INTO bc_time VALUES (rep, p.c, p.shape, p.s, m,
          extract(epoch FROM clock_timestamp() - t0) * 1e6 / k, x);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
SELECT setseed(0.5);
SELECT bc_run(:n, :reps);
\copy bc_time TO 'composite.csv' WITH CSV HEADER

-- the three methods must count the same rows
SELECT count(DISTINCT (c, shape)) AS shapes,
       count(*) FILTER (WHERE n_reg <> n_bool OR n_reg <> n_pgs) AS methods_disagree
FROM (SELECT c, shape, max(n) FILTER (WHERE method = 'region') n_reg,
             max(n) FILTER (WHERE method = 'boolean') n_bool,
             max(n) FILTER (WHERE method = 'pgsphere') n_pgs FROM bc_time GROUP BY 1, 2) a;

-- the plans, for one centre
SELECT c, ra, dec, s FROM bc_centre WHERE c = 1;
DO $$
DECLARE p record; m text; l text;
BEGIN
  FOR p IN SELECT * FROM bc_centre, bc_shape WHERE c = 1 LOOP
    FOR m IN SELECT unnest(ARRAY['region', 'boolean', 'pgsphere']) LOOP
      RAISE NOTICE '--- % %', p.shape, m;
      FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || bc_sql(m, p.shape, p.ra, p.dec, p.s) LOOP
        RAISE NOTICE '%', left(l, 140);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
