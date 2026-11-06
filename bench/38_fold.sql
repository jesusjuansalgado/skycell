-- Booleans of constant regions folded into one composite (0.53), on the
-- shapes of 34_composite.sql (the paper's Table 9), at its centres: the same
-- shape written as
--   region     one REGION('<STC-S composite>')
--   boolean    the pieces as ADQL predicates joined by AND / OR / NOT, folded
--              by skycell into the composite (skycell.fold_regions = on)
--   unfolded   the same boolean with the fold off, as 0.52 answered it
--   pgsphere   pgSphere's boolean of pos <@ scircle / spoly
-- Per shape, the methods in random order, :n executions each (planned and
-- run), each shifted 1e-9 deg so that nothing planned or covered is reused.
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
CREATE OR REPLACE FUNCTION bf_run(n int, reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE p record; m text; sm text; t0 timestamptz; x bigint;
BEGIN
  FOR rep IN 1 .. reps LOOP
    FOR p IN SELECT * FROM bc_centre, bc_shape ORDER BY random() LOOP
      FOR m IN SELECT unnest(ARRAY['region', 'boolean', 'unfolded', 'pgsphere']) ORDER BY random() LOOP
        PERFORM set_config('skycell.fold_regions', CASE WHEN m = 'unfolded' THEN 'off' ELSE 'on' END, true);
        sm := CASE WHEN m = 'unfolded' THEN 'boolean' ELSE m END;
        EXECUTE bc_sql(sm, p.shape, p.ra, p.dec, p.s) INTO x;        -- warm its pages
        t0 := clock_timestamp();
        FOR i IN 1 .. n LOOP
          EXECUTE bc_sql(sm, p.shape, p.ra + 1e-9 * (i + n * rep), p.dec, p.s) INTO x;
        END LOOP;
        INSERT INTO bc_time VALUES (rep, p.c, p.shape, p.s, m,
          extract(epoch FROM clock_timestamp() - t0) * 1e6 / n, x);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
SELECT setseed(0.5);
SELECT bf_run(:n, :reps);
\copy bc_time TO 'fold.csv' WITH CSV HEADER

-- every method must count the same rows
SELECT count(DISTINCT (c, shape)) AS shapes,
       count(*) FILTER (WHERE lo <> hi) AS methods_disagree
FROM (SELECT c, shape, min(n) lo, max(n) hi FROM bc_time GROUP BY 1, 2) a;
