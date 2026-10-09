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
RESET plan_cache_mode;

-- a scrollable cursor reads backwards (the planner adds a Material)
BEGIN;
DECLARE cs_cur SCROLL CURSOR FOR
  SELECT id FROM cs_cat WHERE skycell_cone(cell, ra, dec, 10, 20, 1) ORDER BY cell;
FETCH FIRST FROM cs_cur;
MOVE LAST IN cs_cur;
FETCH ABSOLUTE 1 FROM cs_cur;
COMMIT;

RESET skycell.custom_scan;
DROP FUNCTION cs_both(text);
DROP TABLE cs_cat, cs_sorted;
