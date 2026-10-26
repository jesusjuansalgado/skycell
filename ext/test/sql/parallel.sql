-- Parallel queries. The custom scan is parallel safe but not parallel aware:
-- under a Parallel Seq Scan of the probes, each worker covers and scans the
-- cones of its own probes, as it would an inner index scan, and under
-- debug_parallel_query a whole cone search runs in a worker. Every answer
-- must be the serial one.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;
SELECT setseed(0.31);

-- a clustered catalogue, its heap out of cell order (bitmap mode) and in it
-- (ordered mode), and a copy whose cell index holds the position (the index
-- filter)
CREATE TABLE par_cat AS
SELECT row_number() OVER () AS id, ra, dec, skycell_ang2cell(ra, dec) AS cell FROM (
  SELECT 360 * random() AS ra, degrees(asin(2 * random() - 1)) AS dec FROM generate_series(1, 20000)
  UNION ALL
  SELECT 10 + 3 * (random() - 0.5), 20 + 3 * (random() - 0.5) FROM generate_series(1, 20000)
  UNION ALL
  SELECT ((359.9 + (random() - 0.5))::numeric % 360)::float8, 87 + 2.9 * random() FROM generate_series(1, 5000)) s
ORDER BY random();
CREATE INDEX par_cat_cell ON par_cat (cell);
CREATE INDEX par_cat_expr ON par_cat (skycell_ang2cell(ra, dec));
CREATE TABLE par_sorted AS SELECT * FROM par_cat ORDER BY cell;
CREATE INDEX par_sorted_cell ON par_sorted (cell);
CREATE INDEX par_sorted_expr ON par_sorted (skycell_ang2cell(ra, dec));
CREATE TABLE par_fcat AS SELECT * FROM par_cat;
CREATE INDEX par_fcat_cov ON par_fcat ((skycell_ang2cell(ra, dec))) INCLUDE (ra, dec);
VACUUM ANALYZE par_cat;
VACUUM ANALYZE par_sorted;
VACUUM ANALYZE par_fcat;

-- probes on sources, in the clusters and over the pole, with a radius column,
-- a NULL centre and a cone across ra = 0
CREATE TABLE par_probe AS
SELECT id AS pid, ra + 0.0003 AS ra, dec, 0.01 + (id % 3) * 0.02 AS r FROM par_cat WHERE id % 13 = 0
UNION ALL SELECT 900000, NULL, 10, 0.1
UNION ALL SELECT 900001, 0, 89.99, 0.5
UNION ALL SELECT 900002, 359.99, 0, 0.5;
ANALYZE par_probe;

-- parallel as soon as it is possible at all: no setup or tuple cost, no
-- minimum table size
CREATE FUNCTION par_both(q text, OUT same bool, OUT plan text)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; j text;
BEGIN
  PERFORM set_config('max_parallel_workers_per_gather', '0', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO a;
  PERFORM set_config('max_parallel_workers_per_gather', '2', true);
  PERFORM set_config('parallel_setup_cost', '0', true);
  PERFORM set_config('parallel_tuple_cost', '0', true);
  PERFORM set_config('min_parallel_table_scan_size', '0', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO b;
  EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
  same := a IS NOT DISTINCT FROM b;
  -- the custom scan under a Gather, beside a Parallel Seq Scan of the probes
  plan := CASE WHEN j ~ '"Node Type": "Gather' AND j ~ '"Parallel Aware": true'
                    AND j ~ '"Custom Plan Provider": "SkycellCone"'
               THEN 'parallel/' || substring(j from '"Mode": "([a-z]+)"')
               WHEN j ~ 'SkycellCone' THEN 'serial custom' ELSE 'other' END;
END $$;
SELECT t.name, tbl, (par_both(replace(t.q, 'par_cat', tbl))).*
FROM unnest(ARRAY['par_cat', 'par_sorted']) tbl, (VALUES
  ('join',          'SELECT count(*) || '':'' || sum(c.id) FROM par_probe p JOIN par_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.05)'),
  ('radius column', 'SELECT count(*) || '':'' || sum(c.id) FROM par_probe p JOIN par_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, p.r)'),
  ('left join',     'SELECT count(*) || '':'' || count(c.id) FROM par_probe p LEFT JOIN par_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.05)'),
  ('exists',        'SELECT count(*) FROM par_probe p WHERE EXISTS (SELECT 1 FROM par_cat c WHERE skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.05))'),
  ('adql contains', 'SELECT count(*) || '':'' || sum(c.id) FROM par_probe p JOIN par_cat c ON contains(point(''ICRS'', c.ra, c.dec), circle(''ICRS'', p.ra, p.dec, 0.05)) = 1'),
  -- fewer and wider cones read pages no other probe shares: a bitmap on a
  -- heap out of cell order
  ('wide, few',     'SELECT count(*) || '':'' || sum(c.id) FROM (SELECT * FROM par_probe WHERE pid % 100 = 0) p JOIN par_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 1)')
) t(name, q)
ORDER BY tbl, t.name;
SELECT * FROM par_both($q$SELECT count(*) || ':' || sum(c.id) FROM par_probe p
  JOIN par_fcat c ON contains(skycell_point(c.ra, c.dec), skycell_circle(p.ra, p.dec, 0.05)) = 1$q$);

-- the workers did the scanning: the rows come out of the workers, and the
-- rows the index filter removed in them are counted (the node's own
-- instrumentation, which the workers send back); the leader's average of
-- ranges per covering says it is the leader's
CREATE FUNCTION par_analyze(q text, OUT launched bool, OUT removed_matches_serial bool,
                            OUT leader_label bool)
LANGUAGE plpgsql AS $$
DECLARE serial json; par json; l text; txt text := '';
BEGIN
  PERFORM set_config('max_parallel_workers_per_gather', '0', true);
  EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO serial;
  PERFORM set_config('max_parallel_workers_per_gather', '2', true);
  PERFORM set_config('parallel_setup_cost', '0', true);
  PERFORM set_config('parallel_tuple_cost', '0', true);
  PERFORM set_config('min_parallel_table_scan_size', '0', true);
  EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO par;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, COSTS OFF) ' || q LOOP
    txt := txt || l || E'\n';
  END LOOP;
  launched := substring(par::text from '"Workers Launched": ([0-9]+)')::int > 0;
  removed_matches_serial := substring(serial::text from '"Rows Removed by Index Filter": ([0-9.]+)')
                          = substring(par::text from '"Rows Removed by Index Filter": ([0-9.]+)');
  leader_label := txt !~ 'Ranges per Covering:' AND
                  (txt ~ 'Ranges per Covering \(leader\)' OR txt ~ 'Ranges: per outer row');
END $$;
SELECT * FROM par_analyze($q$SELECT count(*) FROM par_probe p
  JOIN par_fcat c ON contains(skycell_point(c.ra, c.dec), skycell_circle(p.ra, p.dec, 0.05)) = 1$q$);

-- a whole cone search in a worker: debug_parallel_query puts a single-copy
-- Gather over any parallel-safe plan, here the custom scan for a constant
-- cone in both modes and with the index filter, and for a cone of a generic
-- plan's parameters, covered at run time
CREATE FUNCTION par_worker(q text, OUT same bool, OUT in_worker bool)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; j text;
BEGIN
  PERFORM set_config('max_parallel_workers_per_gather', '0', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO a;
  -- parallel mode needs workers allowed, even for a single copy
  PERFORM set_config('max_parallel_workers_per_gather', '2', true);
  PERFORM set_config('debug_parallel_query', 'on', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO b;
  EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO j;
  same := a IS NOT DISTINCT FROM b;
  in_worker := j ~ '"Single Copy": true' AND j ~ '"Custom Plan Provider": "SkycellCone"';
END $$;
SELECT t.name, (par_worker(t.q)).* FROM (VALUES
  ('bitmap',       'SELECT count(*) || '':'' || sum(id) FROM par_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1)'),
  ('ordered',      'SELECT count(*) || '':'' || sum(id) FROM par_sorted WHERE skycell_cone(cell, ra, dec, 10, 20, 1)'),
  ('pole',         'SELECT count(*) || '':'' || sum(id) FROM par_sorted WHERE skycell_cone(cell, ra, dec, 0, 89.9, 2)'),
  ('index filter', 'SELECT count(*) || '':'' || sum(id) FROM par_fcat WHERE contains(skycell_point(ra, dec), skycell_circle(10, 20, 1)) = 1'),
  ('cross-match',  'SELECT count(*) || '':'' || sum(c.id) FROM par_probe p JOIN par_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.05)')
) t(name, q)
ORDER BY t.name;
PREPARE par_q(float8, float8, float8) AS
  SELECT count(*) || ':' || sum(id) FROM par_cat WHERE skycell_cone(cell, ra, dec, $1, $2, $3);
SET plan_cache_mode = force_generic_plan;
SET max_parallel_workers_per_gather = 0;
EXECUTE par_q(10, 20, 1);
RESET max_parallel_workers_per_gather;
-- planned again, now in parallel mode
DEALLOCATE par_q;
SET debug_parallel_query = on;
PREPARE par_q(float8, float8, float8) AS
  SELECT count(*) || ':' || sum(id) FROM par_cat WHERE skycell_cone(cell, ra, dec, $1, $2, $3);
EXECUTE par_q(10, 20, 1);
EXPLAIN (COSTS OFF) EXECUTE par_q(10, 20, 1);
RESET debug_parallel_query;
RESET plan_cache_mode;
DEALLOCATE par_q;

DROP FUNCTION par_both(text), par_analyze(text), par_worker(text);
DROP TABLE par_probe, par_cat, par_sorted, par_fcat;
