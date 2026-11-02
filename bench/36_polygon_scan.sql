-- Constant polygons through the custom scan (0.43) against the range rewrite
-- they took before, pgSphere and Q3C. Run on gaia_realc (the 10M real Gaia
-- DR3 positions, with skycell's cell index, pgSphere's GiST on pos and Q3C's
-- index).
--
--   scan      skycell_poly(skycell_ang2cell(ra, dec), ra, dec, poly), the custom scan (0.43)
--   rewrite   the same with skycell.custom_scan off: the OR of index ranges
--             and per-row exact test that every polygon took up to 0.42
--   adql      1 = CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', ...)),
--             the custom scan through the ADQL spelling
--   pgsphere  pos <@ spoly_deg(poly)
--   q3c       q3c_poly_query(ra, dec, poly), convex shapes only
--
-- Shapes round a catalogue source, size s (0.05..2 deg, log-uniform):
--   quad   a convex quadrilateral (bench 05's shape)
--   box    an axis-aligned box s by s/2 (ADQL BOX's four corners)
--   ushape a non-convex U, 8 vertices, s across
-- Per shape the methods in random order, :n executions each (planned and
-- run), each shifted 1e-9 deg so that nothing planned or covered is reused,
-- and each method by its own 1e-12 deg more, so that one method never finds
-- the covering another planned for the same polygon (the scan and the ADQL
-- spelling share skycell's covering cache).
\set ON_ERROR_STOP 1

SELECT setseed(0.36);
CREATE TEMP TABLE pc_centre AS
SELECT row_number() OVER ()::int AS c, ra, greatest(-80, least(80, dec)) AS dec,
       power(10, -1.3 + 1.6 * random()) AS s
FROM (SELECT ra, dec FROM gaia_realc TABLESAMPLE SYSTEM (0.01) REPEATABLE (36) LIMIT :ncentres) x;
CREATE TEMP TABLE pc_shape (shape text);
INSERT INTO pc_shape VALUES ('quad'), ('box'), ('ushape');

-- vertices (ra, dec pairs) of a shape round (ra, de), size s
CREATE OR REPLACE FUNCTION pc_poly(shape text, ra float8, de float8, s float8) RETURNS float8[]
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE shape
    WHEN 'quad' THEN ARRAY[ra - s * f, de - s / 2, ra + s * f, de - s / 2,
                           ra + s * f / 2, de + s / 2, ra - s * f / 2, de + s / 2]
    WHEN 'box'  THEN ARRAY[ra - s / 2 * f, de - s / 4, ra + s / 2 * f, de - s / 4,
                           ra + s / 2 * f, de + s / 4, ra - s / 2 * f, de + s / 4]
    ELSE ARRAY[ra - s / 2 * f, de - s / 2, ra + s / 2 * f, de - s / 2,
               ra + s / 2 * f, de + s / 2, ra + s / 6 * f, de + s / 2,
               ra + s / 6 * f, de - s / 6, ra - s / 6 * f, de - s / 6,
               ra - s / 6 * f, de + s / 2, ra - s / 2 * f, de + s / 2] END
  FROM (SELECT 1 / cos(radians(de)) AS f) x $$;

CREATE OR REPLACE FUNCTION pc_sql(m text, p float8[]) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE m
    WHEN 'scan'     THEN format('SELECT count(*) FROM gaia_realc WHERE skycell_poly(skycell_ang2cell(ra, dec), ra, dec, %L::float8[])', p)
    WHEN 'rewrite'  THEN format('SELECT count(*) FROM gaia_realc WHERE skycell_poly(skycell_ang2cell(ra, dec), ra, dec, %L::float8[])', p)
    WHEN 'adql'     THEN format('SELECT count(*) FROM gaia_realc WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), POLYGON(''ICRS'', VARIADIC %L::float8[]))', p)
    WHEN 'pgsphere' THEN format('SELECT count(*) FROM gaia_realc WHERE pos <@ spoly_deg(%L::float8[])', p)
    ELSE format('SELECT count(*) FROM gaia_realc WHERE q3c_poly_query(ra, dec, %L::float8[])', p) END $$;

CREATE TEMP TABLE pc_time (rep int, c int, shape text, s float8, method text, us float8, n bigint);
CREATE OR REPLACE FUNCTION pc_run(n int, reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE p record; m text; t0 timestamptz; x bigint;
BEGIN
  FOR rep IN 1 .. reps LOOP
    FOR p IN SELECT * FROM pc_centre, pc_shape ORDER BY random() LOOP
      FOR m IN SELECT unnest(ARRAY['scan', 'rewrite', 'adql', 'pgsphere', 'q3c']) ORDER BY random() LOOP
        CONTINUE WHEN m = 'q3c' AND p.shape = 'ushape';
        PERFORM set_config('skycell.custom_scan', (m <> 'rewrite')::text, true);
        EXECUTE pc_sql(m, pc_poly(p.shape, p.ra, p.dec, p.s)) INTO x;      -- warm its pages
        t0 := clock_timestamp();
        FOR i IN 1 .. n LOOP
          EXECUTE pc_sql(m, pc_poly(p.shape, p.ra + 1e-9 * (i + n * rep)
                                    + 1e-12 * array_position(ARRAY['scan', 'rewrite', 'adql', 'pgsphere', 'q3c'], m),
                                    p.dec, p.s)) INTO x;
        END LOOP;
        INSERT INTO pc_time VALUES (rep, p.c, p.shape, p.s, m,
          extract(epoch FROM clock_timestamp() - t0) * 1e6 / n, x);
      END LOOP;
    END LOOP;
  END LOOP;
  PERFORM set_config('skycell.custom_scan', 'on', true);
END $$;
SELECT setseed(0.5);
SELECT pc_run(:n, :reps);
\copy pc_time TO 'polyscan.csv' WITH CSV HEADER

-- every method must count the same rows
SELECT count(*) AS shapes,
       count(*) FILTER (WHERE nmin <> nmax) AS methods_disagree
FROM (SELECT c, shape, min(n) nmin, max(n) nmax FROM pc_time GROUP BY 1, 2) a;

-- median us per query, and the summed time of each method over the scan's
SELECT shape, method, count(*) AS runs,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY us))::numeric, 0) AS median_us,
       round((sum(us) / sum(sum(us)) FILTER (WHERE method = 'scan') OVER (PARTITION BY shape))::numeric, 2) AS sum_over_scan
FROM pc_time GROUP BY shape, method ORDER BY shape, sum_over_scan;

-- the plans, for one centre
DO $$
DECLARE p record; m text; l text;
BEGIN
  FOR p IN SELECT * FROM pc_centre, pc_shape WHERE c = 1 LOOP
    FOREACH m IN ARRAY ARRAY['scan', 'rewrite', 'pgsphere'] LOOP
      PERFORM set_config('skycell.custom_scan', (m <> 'rewrite')::text, true);
      RAISE NOTICE '--- % %', p.shape, m;
      FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || pc_sql(m, pc_poly(p.shape, p.ra, p.dec, p.s)) LOOP
        RAISE NOTICE '%', left(l, 140);
      END LOOP;
    END LOOP;
  END LOOP;
  PERFORM set_config('skycell.custom_scan', 'on', true);
END $$;
