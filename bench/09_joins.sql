-- Does a better row estimate produce a better plan? (referee experiment 4)
--
-- A spatial predicate is rarely alone: it is combined with catalogue cuts and
-- joined to other tables, and the planner decides the join order and method
-- from the estimated number of rows the cone returns.  Here the same logical
-- query is run through each method, and the plan it chose, its estimate and
-- its execution time are recorded together, so that "better estimate",
-- "different plan" and "faster" can be told apart.
\set ON_ERROR_STOP 1

-- a side table: three observations for every tenth source
DROP TABLE IF EXISTS obs;
CREATE TABLE obs AS
SELECT s.id, b.band, (10 + 5 * random())::real AS flux
FROM src s CROSS JOIN (VALUES ('g'), ('r'), ('i')) AS b(band)
WHERE s.id % 10 = 0;
CREATE INDEX obs_id_idx ON obs (id, band);
ANALYZE obs;

CREATE TABLE IF NOT EXISTS bench_join (corpus text, query text, label text, method text,
  qid bigint, est float8, act float8, ms float8, plan text);

/*
 * The three query shapes, per method.  `mag < 15.0` keeps about a third of the
 * catalogue, so the planner has a second selectivity to combine with the
 * spatial one; the joins give it a choice of strategy.
 */
CREATE OR REPLACE FUNCTION join_sql(shape text, method text, c bench_centers) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE shape
    WHEN 'filter' THEN
      format('SELECT count(*) FROM %s WHERE %s AND mag < 15.0', t.rel, t.pred)
    WHEN 'join' THEN
      format('SELECT count(*) FROM %s c JOIN obs o ON o.id = c.id '
             'WHERE %s AND o.band = ''r''', t.rel, replace(t.pred, 'ra,', 'c.ra,'))
    WHEN 'join3' THEN
      format('SELECT count(*) FROM %s c JOIN obs o ON o.id = c.id '
             'JOIN obs o2 ON o2.id = c.id AND o2.band = ''g'' '
             'WHERE %s AND o.band = ''r'' AND c.mag < 15.0',
             t.rel, replace(t.pred, 'ra,', 'c.ra,'))
  END
  FROM (SELECT CASE method
          WHEN 'q3c' THEN 'cat_q3c'
          WHEN 'pgsphere' THEN 'cat_sphere'
          ELSE 'cat_cell' END AS rel,
        CASE method
          WHEN 'q3c' THEN format('q3c_radial_query(ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r)
          WHEN 'pgsphere' THEN format('pos <@ scircle(spoint(radians(%s), radians(%s)), radians(%s))',
                                      c.ra0, c.dec0, c.r)
          ELSE format('skycell_cone(cell, ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r)
        END AS pred) t
$$;

/*
 * The strategy a plan chose: which join methods and which access paths it
 * used, as a set.  Repetitions are collapsed, because skycell's rewrite emits
 * one index scan per covering range and the number of ranges is a property of
 * the covering, not of the plan strategy -- counting them would report a
 * different "plan" for every query.
 */
CREATE OR REPLACE FUNCTION plan_shape(j json) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT string_agg(DISTINCT t, '+' ORDER BY t) FROM (
    SELECT jsonb_path_query(j::jsonb, 'strict $.**."Node Type"') #>> '{}' AS t) x
  WHERE t LIKE '%Join%' OR t LIKE '%Scan%' OR t = 'Nested Loop'
$$;

CREATE OR REPLACE FUNCTION bench_join_run(corpus text, shapes text[], labels text[],
                                          lim int DEFAULT 20) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE c bench_centers; lab text; shape text; m text; q text; j json; n bigint;
        t0 timestamptz; ms float8;
BEGIN
  DELETE FROM bench_join b WHERE b.corpus = bench_join_run.corpus;
  FOREACH lab IN ARRAY labels LOOP
    FOREACH shape IN ARRAY shapes LOOP
      FOR c IN SELECT * FROM bench_centers WHERE label = lab ORDER BY qid LIMIT lim LOOP
        FOREACH m IN ARRAY ARRAY['q3c', 'pgsphere', 'skycell'] LOOP
          q := join_sql(shape, m, c);
          EXECUTE q INTO n;                               -- warm
          t0 := clock_timestamp();
          EXECUTE q INTO n;
          ms := extract(epoch FROM clock_timestamp() - t0) * 1000;
          EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON) ' || q INTO j;
          -- the node under the aggregate: the rows the planner had to predict
          INSERT INTO bench_join VALUES (corpus, shape, lab, m, c.qid,
            (j -> 0 -> 'Plan' -> 'Plans' -> 0 ->> 'Plan Rows')::float8,
            (j -> 0 -> 'Plan' -> 'Plans' -> 0 ->> 'Actual Rows')::float8,
            ms, plan_shape(j -> 0 -> 'Plan'));
        END LOOP;
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
