-- skycell.custom_scan: a constant cone answered by the SkycellCone custom scan
-- must return exactly what the range rewrite returns.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;
SET max_parallel_workers_per_gather = 0;

-- a deterministic Fibonacci-sphere catalogue, small enough (20,000 rows) that
-- ANALYZE samples all of it, so the histogram and the coverings are stable
CREATE TABLE cs_cat AS
SELECT i AS id,
       mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 20000.0)) AS dec
FROM generate_series(0, 19999) i;
ALTER TABLE cs_cat ADD COLUMN cell int8;
UPDATE cs_cat SET cell = skycell_ang2cell(ra, dec);
CREATE INDEX cs_cat_cell ON cs_cat (cell);
CREATE INDEX cs_cat_expr ON cs_cat (skycell_ang2cell(ra, dec));
VACUUM ANALYZE cs_cat;
-- the same rows stored in cell order: cs_cat's heap does not follow the cell
-- order, so the scan runs in bitmap mode there and in ordered mode here
CREATE TABLE cs_sorted AS SELECT * FROM cs_cat ORDER BY cell;
CREATE INDEX cs_sorted_cell ON cs_sorted (cell);
CREATE INDEX cs_sorted_expr ON cs_sorted (skycell_ang2cell(ra, dec));
VACUUM ANALYZE cs_sorted;

-- result with the custom scan off and on, and how "on" planned it
CREATE FUNCTION cs_both(q text, OUT same bool, OUT custom text)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; j json;
BEGIN
  PERFORM set_config('skycell.custom_scan', 'off', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO a;
  PERFORM set_config('skycell.custom_scan', 'on', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO b;
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  same := a IS NOT DISTINCT FROM b;
  custom := CASE WHEN j::text LIKE '%"Mode": "bitmap"%' THEN 'bitmap'
                 WHEN j::text LIKE '%SkycellCone%' THEN 'ordered' ELSE 'no' END;
END $$;

SELECT t.name, tbl, (cs_both(replace(t.q, 'cs_cat', tbl))).*
FROM unnest(ARRAY['cs_cat', 'cs_sorted']) tbl, (VALUES
  ('1 deg',           'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1)'),
  ('3 arcmin',        'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 200, -45, 0.05)'),
  ('zero radius',     'SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 0)'),
  ('north pole',      'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 0, 90, 4)'),
  ('south pole',      'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 123, -89.5, 2)'),
  ('ra wrap',         'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 359.8, 1, 3)'),
  ('10 deg',          'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 300, 30, 10)'),
  ('whole sky',       'SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 0, 0, 180)'),
  ('expression',      'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 10, 20, 1)'),
  ('two cones',       'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 3) AND skycell_cone(cell, ra, dec, 11, 20, 2)'),
  ('cone and filter', 'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 3) AND id % 3 = 0'),
  ('cone or id',      'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 3) OR id = 7'),
  ('ordered, limit',  'SELECT string_agg(id::text, '','') FROM (SELECT id FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 3) ORDER BY id LIMIT 4) s'),
  ('non-constant',    'SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, (SELECT 10.0::float8), 20, 1)')
) t(name, q)
ORDER BY tbl, t.name;

SET skycell.custom_scan = on;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_sorted WHERE skycell_cone(cell, ra, dec, 10, 20, 1);

-- rescanned as the inner side of a nested loop: three times the single count,
-- in both modes
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT count(*) = 3 * (SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 3))
       AS rescan_bitmap_ok
FROM (VALUES (1), (2), (3)) v(x) JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, 10, 20, 3) AND v.x > 0;
SELECT count(*) = 3 * (SELECT count(*) FROM cs_sorted WHERE skycell_cone(cell, ra, dec, 10, 20, 3))
       AS rescan_ordered_ok
FROM (VALUES (1), (2), (3)) v(x) JOIN cs_sorted c ON skycell_cone(c.cell, c.ra, c.dec, 10, 20, 3) AND v.x > 0;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- a generic plan (parameters, not constants) keeps the run-time range slots
PREPARE cs_p(float8, float8, float8) AS
  SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, $1, $2, $3);
SET plan_cache_mode = force_custom_plan;
EXECUTE cs_p(10, 20, 1);
SET plan_cache_mode = force_generic_plan;
EXECUTE cs_p(10, 20, 1);
-- a generic plan covers the cone when it runs
EXPLAIN (COSTS OFF) EXECUTE cs_p(10, 20, 1);
RESET plan_cache_mode;

-- a scrollable cursor reads backwards (the planner adds a Material)
BEGIN;
DECLARE cs_cur SCROLL CURSOR FOR
  SELECT id FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1) ORDER BY cell;
FETCH FIRST FROM cs_cur;
MOVE LAST IN cs_cur;
FETCH ABSOLUTE 1 FROM cs_cur;
COMMIT;

-- where the cone sits decides the plan: only a top-level AND term of a WHERE
-- or JOIN ON becomes the custom scan; under an OR it is rewritten, so the
-- BitmapOr over the cell ranges and the id index still serves it
CREATE INDEX cs_cat_id ON cs_cat (id);
ANALYZE cs_cat;
CREATE VIEW cs_view AS SELECT * FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1);
CREATE FUNCTION cs_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'SkycellCone' THEN RETURN 'custom'; END IF;
    p := p || l;
  END LOOP;
  RETURN CASE WHEN p ~ 'Bitmap Index Scan on cs_cat_cell' THEN 'rewrite, indexed'
              WHEN p ~ 'Seq Scan' THEN 'seq scan' ELSE 'other' END;
END $$;
SELECT t.name, (cs_both(t.q)).same, cs_plan(t.q) AS plan FROM (VALUES
  ('or',            'SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1) OR id = 7'),
  ('or, and-ed',    'SELECT count(*) FROM cs_cat WHERE (skycell_cone(cell, ra, dec, 10, 20, 1) OR id = 7) AND id >= 0'),
  ('and',           'SELECT count(*) FROM cs_cat WHERE id >= 0 AND skycell_cone(cell, ra, dec, 10, 20, 1)'),
  ('view',          'SELECT count(*) FROM cs_view'),
  ('join on',       'SELECT count(*) FROM cs_cat c JOIN (VALUES (1)) x(k) ON skycell_cone(c.cell, c.ra, c.dec, 10, 20, 1)'),
  ('subquery',      'SELECT count(*) FROM (SELECT * FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1) OFFSET 0) s'),
  ('folded radius', 'SELECT count(*) FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 2.0 / 2)'),
  ('target list',   'SELECT count(*) FILTER (WHERE skycell_cone(cell, ra, dec, 10, 20, 1)) FROM cs_cat')
) t(name, q);

-- cross-match: a cone whose centre (or radius) comes from another relation is
-- answered by the scan parameterized by the outer row, one covering per row;
-- it must return what the rewrite (run-time slots) returns, in both modes
CREATE TABLE cs_probe AS
SELECT id AS pid, ra + 0.0003 AS ra, dec, 0.05 + (id % 3) * 0.1 AS r
FROM cs_cat WHERE id % 97 = 0
UNION ALL SELECT 100000 + i, mod(i * 7.3, 360), -80 + mod(i * 11, 160), 0.2
FROM generate_series(1, 120) i
UNION ALL SELECT 200000, NULL, 10, 0.1         -- a NULL centre matches nothing
UNION ALL SELECT 200001, 0, 89.99, 1           -- a cone around the pole
UNION ALL SELECT 200002, 359.99, 0, 0.5;       -- and across ra = 0
ANALYZE cs_probe;
CREATE FUNCTION cs_join(q text, OUT same bool, OUT plan text)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; l text;
BEGIN
  PERFORM set_config('skycell.custom_scan', 'off', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO a;
  PERFORM set_config('skycell.custom_scan', 'on', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO b;
  same := a IS NOT DISTINCT FROM b;
  plan := 'other';
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'Custom Scan \(SkycellCone\)' THEN plan := 'custom'; END IF;
  END LOOP;
END $$;
SELECT t.name, tbl, (cs_join(replace(t.q, 'cs_cat', tbl))).*
FROM unnest(ARRAY['cs_cat', 'cs_sorted']) tbl, (VALUES
  ('join',          'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.3)'),
  ('radius column', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, p.r)'),
  ('where',         'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p, cs_cat c WHERE skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.3) AND c.id >= 0'),
  ('left join',     'SELECT count(*) || '':'' || count(c.id) FROM cs_probe p LEFT JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.3)'),
  ('exists',        'SELECT count(*) FROM cs_probe p WHERE EXISTS (SELECT 1 FROM cs_cat c WHERE skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.3))'),
  ('expression',    'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON skycell_cone(skycell_ang2cell(c.ra, c.dec), c.ra, c.dec, p.ra, p.dec, 0.3)'),
  ('skycell_join',  'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON skycell_join(c.ra, c.dec, p.ra, p.dec, 0.3)'),
  ('join, left',    'SELECT count(*) || '':'' || count(c.id) FROM cs_probe p LEFT JOIN cs_cat c ON skycell_join(c.ra, c.dec, p.ra, p.dec, 0.3)'),
  ('join, radius',  'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON skycell_join(c.ra, c.dec, p.ra, p.dec, p.r)'),
  ('radial query',  'SELECT count(*) || '':'' || sum(c.id) FROM cs_cat c WHERE skycell_radial_query(c.ra, c.dec, 10, 20, 1)'),
  ('nearest',       'SELECT string_agg(pid || ''='' || n, '','' ORDER BY pid) FROM (SELECT p.pid, (SELECT c.id FROM cs_cat c WHERE skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.3) ORDER BY skycell_dist(c.ra, c.dec, p.ra, p.dec), c.id LIMIT 1) n FROM cs_probe p WHERE p.pid < 2000) s')
) t(name, q)
ORDER BY tbl, t.name;

-- the join's row estimate: half the probes sit on catalogue sources, which a
-- density model cannot know; sampled probes' cones measure it (within 3x)
CREATE FUNCTION cs_est(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE j json; est float8; act float8;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  est := (j -> 0 -> 'Plan' ->> 'Plan Rows')::float8;
  EXECUTE 'SELECT count(*) FROM (' || q || ') s' INTO act;
  RETURN CASE WHEN est BETWEEN act / 3 AND act * 3 THEN 'within 3x' ELSE est || ' vs ' || act END;
END $$;
SELECT r, tbl, cs_est(format('SELECT c.id FROM cs_probe p JOIN %s c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, %s)', tbl, r))
FROM unnest(ARRAY['cs_cat', 'cs_sorted']) tbl, unnest(ARRAY[0.01, 0.3, 3]) r ORDER BY tbl, r;
DROP FUNCTION cs_est(text);

-- an invalid centre is an error, as it is for the rewrite
SELECT count(*) FROM (VALUES (10.0::float8, 95.0::float8)) p(ra, dec)
  JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.1);

-- the plan, and a generic plan for a prepared cross-match
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_probe p JOIN cs_sorted c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.3);
SET plan_cache_mode = force_generic_plan;
PREPARE cs_xm(float8) AS
  SELECT count(*) FROM cs_probe p JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, $1);
EXECUTE cs_xm(0.3);
EXPLAIN (COSTS OFF) EXECUTE cs_xm(0.3);
SET skycell.custom_scan = off;
PREPARE cs_xm_rw(float8) AS
  SELECT count(*) FROM cs_probe p JOIN cs_cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, $1);
EXECUTE cs_xm_rw(0.3);
SET skycell.custom_scan = on;
RESET plan_cache_mode;

RESET skycell.custom_scan;
DROP TABLE cs_probe;
DROP FUNCTION cs_join(text);
DROP VIEW cs_view;
DROP FUNCTION cs_both(text);
DROP FUNCTION cs_plan(text);
DROP TABLE cs_cat, cs_sorted;
