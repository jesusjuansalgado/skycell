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

-- ADQL's own spelling, the operator a translator emits for CONTAINS, is the
-- same cone: a circle (constant, STC-S or built per row) goes to the custom
-- scan exactly as skycell_cone would, and returns what the rewrite returns;
-- a polygon, or a circle under an OR, keeps the rewrite
CREATE TABLE cs_spos AS SELECT id, skycell_point(ra, dec) AS pos FROM cs_cat;
CREATE INDEX cs_spos_cell ON cs_spos (skycell_cell(pos));
ANALYZE cs_spos;
SELECT t.name, tbl, (cs_join(replace(t.q, 'cs_cat', tbl))).*
FROM unnest(ARRAY['cs_cat', 'cs_sorted']) tbl, (VALUES
  ('adql cone',     'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE point(''ICRS'', ra, dec) <@ circle(''ICRS'', 10, 20, 1)'),
  ('adql @>',       'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE circle(''ICRS'', 10, 20, 1) @> point(''ICRS'', ra, dec)'),
  ('adql aliases',  'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE skycell_point(ra, dec) <@ skycell_circle(359.8, 1, 3)'),
  ('adql stc-s',    'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE point(''ICRS'', ra, dec) <@ ''CIRCLE ICRS 0 90 4''::skyregion'),
  ('adql join',     'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON point(''ICRS'', c.ra, c.dec) <@ circle(''ICRS'', p.ra, p.dec, 0.3)'),
  ('adql radius',   'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON circle(''ICRS'', p.ra, p.dec, p.r) @> point(''ICRS'', c.ra, c.dec)'),
  ('adql left',     'SELECT count(*) || '':'' || count(c.id) FROM cs_probe p LEFT JOIN cs_cat c ON point(''ICRS'', c.ra, c.dec) <@ circle(''ICRS'', p.ra, p.dec, 0.3)'),
  ('adql or',       'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE point(''ICRS'', ra, dec) <@ circle(''ICRS'', 10, 20, 1) OR id = 7'),
  ('adql polygon',  'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE point(''ICRS'', ra, dec) <@ polygon(''ICRS'', 10, 20, 12, 20, 11, 22)'),
  -- written the other way round, as the Gaia archive's examples do: a constant
  -- point inside a circle drawn around each row is the same cone
  ('adql reversed', 'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE point(''ICRS'', 10, 20) <@ circle(''ICRS'', ra, dec, 1)'),
  ('adql rev @>',   'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE circle(''ICRS'', ra, dec, 1) @> point(''ICRS'', 10, 20)'),
  ('adql rev join', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON point(''ICRS'', p.ra, p.dec) <@ circle(''ICRS'', c.ra, c.dec, 0.3)')
) t(name, q)
ORDER BY tbl, t.name;
-- the reversed cone returns exactly the rows of the cone it stands for
SELECT (SELECT count(*) FROM cs_cat WHERE point('ICRS', 10, 20) <@ circle('ICRS', ra, dec, 1))
     = (SELECT count(*) FROM cs_cat WHERE skycell_in_cone(ra, dec, 10, 20, 1)) AS reversed_same,
       (SELECT count(*) FROM cs_probe p JOIN cs_cat c ON point('ICRS', p.ra, p.dec) <@ circle('ICRS', c.ra, c.dec, 0.3))
     = (SELECT count(*) FROM cs_probe p JOIN cs_cat c ON skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 0.3)) AS rev_join_same;
SELECT t.name, (cs_join(t.q)).* FROM (VALUES
  ('stored skypos', 'SELECT count(*) || '':'' || sum(id) FROM cs_spos WHERE pos <@ circle(''ICRS'', 10, 20, 1)'),
  ('stored, join',  'SELECT count(*) || '':'' || sum(s.id) FROM cs_probe p JOIN cs_spos s ON s.pos <@ circle(''ICRS'', p.ra, p.dec, 0.3)'),
  ('stored, dist',  'SELECT count(*) || '':'' || sum(s.id) FROM (SELECT * FROM cs_probe WHERE pid % 5 = 0) p JOIN cs_spos s ON distance(s.pos, point(''ICRS'', p.ra, p.dec)) < 0.3'),
  ('stored, dist rev', 'SELECT count(*) || '':'' || sum(s.id) FROM (SELECT * FROM cs_probe WHERE pid % 5 = 0) p JOIN cs_spos s ON distance(point(''ICRS'', p.ra, p.dec), s.pos) < 0.3')
) t(name, q);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_probe p JOIN cs_cat c ON point('ICRS', c.ra, c.dec) <@ circle('ICRS', p.ra, p.dec, 0.3);
-- the coordinate system is still checked, at plan time
SELECT count(*) FROM cs_cat WHERE point('ICRS', ra, dec) <@ circle('GALACTIC', 10, 20, 1);
SELECT count(*) FROM cs_probe p JOIN cs_cat c ON point('FK4', c.ra, c.dec) <@ circle('ICRS', p.ra, p.dec, 0.3);
-- with both sides indexed, a cross-match probes the larger relation (cs_cat)
-- whichever side is written as the point and whichever as the circle
CREATE INDEX cs_probe_expr ON cs_probe (skycell_ang2cell(ra, dec));
ANALYZE cs_probe;
CREATE FUNCTION cs_probed(q text, OUT probed text, OUT result text)
LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    probed := coalesce(probed, substring(l from 'SkycellCone\) on (\w+)'));
  END LOOP;
  EXECUTE 'SELECT (' || q || ')::text' INTO result;
END $$;
SELECT t.name, (cs_probed(t.q)).* FROM (VALUES
  ('p(cat) <@ c(probe)', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON point(''ICRS'', c.ra, c.dec) <@ circle(''ICRS'', p.ra, p.dec, 0.3)'),
  ('p(probe) <@ c(cat)', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON point(''ICRS'', p.ra, p.dec) <@ circle(''ICRS'', c.ra, c.dec, 0.3)'),
  ('c(probe) @> p(cat)', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_cat c, cs_probe p WHERE circle(''ICRS'', p.ra, p.dec, 0.3) @> point(''ICRS'', c.ra, c.dec)'),
  ('c(cat) @> p(probe)', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_cat c, cs_probe p WHERE circle(''ICRS'', c.ra, c.dec, 0.3) @> point(''ICRS'', p.ra, p.dec)'),
  ('dist(probe, cat)',   'SELECT count(*) || '':'' || sum(c.id) FROM cs_probe p JOIN cs_cat c ON distance(p.ra, p.dec, c.ra, c.dec) < 0.3'),
  ('dist(cat, probe)',   'SELECT count(*) || '':'' || sum(c.id) FROM cs_cat c, cs_probe p WHERE distance(point(''ICRS'', c.ra, c.dec), point(''ICRS'', p.ra, p.dec)) < 0.3')
) t(name, q);
DROP FUNCTION cs_probed(text);
DROP INDEX cs_probe_expr;
ANALYZE cs_probe;
DROP TABLE cs_spos;

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

-- distance(p1, p2) < r, ADQL's other spelling of a cone or a cross-match, is
-- planned as the same cone, whatever the spelling and whichever way round,
-- and returns exactly what the comparison alone returns (custom scan off,
-- every pair compared: a fifth of the probes, and the odd ones, keep it short)
CREATE TABLE cs_dprobe AS SELECT * FROM cs_probe WHERE pid % 5 = 0 OR pid >= 200000;
ANALYZE cs_dprobe;
SELECT t.name, (cs_join(t.q)).* FROM (VALUES
  ('distance join',   'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p JOIN cs_cat c ON distance(point(''ICRS'', c.ra, c.dec), point(''ICRS'', p.ra, p.dec)) < 0.3'),
  ('distance rev',    'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p JOIN cs_cat c ON distance(point(''ICRS'', p.ra, p.dec), point(''ICRS'', c.ra, c.dec)) < 0.3'),
  ('distance 4 <=',   'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p, cs_cat c WHERE distance(p.ra, p.dec, c.ra, c.dec) <= 0.3'),
  ('skycell_dist >',  'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p, cs_cat c WHERE 0.3 > skycell_dist(c.ra, c.dec, p.ra, p.dec)'),
  ('distance radius', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p JOIN cs_cat c ON distance(c.ra, c.dec, p.ra, p.dec) < p.r'),
  ('distance left',   'SELECT count(*) || '':'' || count(c.id) FROM cs_dprobe p LEFT JOIN cs_cat c ON distance(p.ra, p.dec, c.ra, c.dec) < 0.3'),
  ('distance cone',   'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE distance(point(''ICRS'', 10, 20), point(''ICRS'', ra, dec)) < 1'),
  ('distance cone 4', 'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE distance(ra, dec, 10, 20) < 1'),
  ('distance, pole',  'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE distance(ra, dec, 0, 89.99) < 1')
) t(name, q);
-- and is estimated as the cone: the comparison next to it counts as always true
CREATE FUNCTION cs_rows(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE j json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  RETURN (j -> 0 -> 'Plan' ->> 'Plan Rows')::float8;
END $$;
SELECT cs_rows('SELECT c.id FROM cs_dprobe p JOIN cs_cat c ON distance(p.ra, p.dec, c.ra, c.dec) < 0.3')
     = cs_rows('SELECT c.id FROM cs_dprobe p JOIN cs_cat c ON point(''ICRS'', c.ra, c.dec) <@ circle(''ICRS'', p.ra, p.dec, 0.3)') AS join_rows_same,
       cs_rows('SELECT id FROM cs_cat WHERE distance(point(''ICRS'', 10, 20), point(''ICRS'', ra, dec)) < 1')
     = cs_rows('SELECT id FROM cs_cat WHERE point(''ICRS'', ra, dec) <@ circle(''ICRS'', 10, 20, 1)') AS cone_rows_same;
DROP FUNCTION cs_rows(text);
-- a centre off the sphere is the direction distance() takes it as, and a
-- non-finite one matches nothing, as its NaN distance does: neither is an error
CREATE TABLE cs_bad AS SELECT * FROM (VALUES
  (1, 10.0::float8, 95.0::float8), (2, 'NaN', 20), (3, 'Infinity', 20), (4, 10, -100),
  (5, 10, 'NaN'), (6, 10, 20), (7, 190, -85)) v(pid, ra, dec);
ANALYZE cs_bad;
SELECT (cs_join('SELECT string_agg(pid || ''='' || n, '','' ORDER BY pid) FROM (SELECT p.pid, count(c.id) n FROM cs_bad p LEFT JOIN cs_cat c ON distance(p.ra, p.dec, c.ra, c.dec) < 3 GROUP BY p.pid) s')).*;
SELECT string_agg(pid || '=' || n, ',' ORDER BY pid) FROM (SELECT p.pid, count(c.id) n
  FROM cs_bad p LEFT JOIN cs_cat c ON distance(p.ra, p.dec, c.ra, c.dec) < 3 GROUP BY p.pid) s;
DROP TABLE cs_bad;
-- ADQL's CONTAINS and INTERSECTS compared with 1 are their operator: the
-- same rows as the comparison evaluated as written (`+ 0` hides it), through
-- an index
CREATE FUNCTION cs_adql(q text, OUT same bool, OUT plan text)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; l text;
BEGIN
  EXECUTE 'SELECT (' || replace(replace(q, ') = 1', ') + 0 = 1'), '1 = contains(', '1 = 0 + contains(') || ')::text' INTO a;
  EXECUTE 'SELECT (' || q || ')::text' INTO b;
  same := a IS NOT DISTINCT FROM b;
  plan := 'seq';
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'Custom Scan \(SkycellCone\)' THEN plan := 'custom';
    ELSIF l ~ 'Index Scan' AND plan = 'seq' THEN plan := 'index'; END IF;
  END LOOP;
END $$;
CREATE TABLE cs_reg AS SELECT id, circle('ICRS', ra, dec, 0.5) AS r FROM cs_cat WHERE id % 20 = 0;
CREATE INDEX cs_reg_r ON cs_reg USING gist (r);
ANALYZE cs_reg;
SET enable_seqscan = off;
SELECT t.name, (cs_adql(t.q)).* FROM (VALUES
  ('contains cone',     'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE contains(point(''ICRS'', ra, dec), circle(''ICRS'', 10, 20, 1)) = 1'),
  ('contains reversed', 'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE 1 = contains(point(''ICRS'', 10, 20), circle(''ICRS'', ra, dec, 1))'),
  ('intersects cone',   'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE intersects(point(''ICRS'', ra, dec), circle(''ICRS'', 10, 20, 1)) = 1'),
  ('contains polygon',  'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE contains(point(''ICRS'', ra, dec), polygon(''ICRS'', 10, 20, 12, 20, 12, 22, 10, 22)) = 1'),
  ('intersects box',    'SELECT count(*) || '':'' || sum(id) FROM cs_cat WHERE intersects(point(''ICRS'', ra, dec), box(''ICRS'', 11, 21, 2, 2)) = 1'),
  ('contains join',     'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p JOIN cs_cat c ON contains(point(''ICRS'', c.ra, c.dec), circle(''ICRS'', p.ra, p.dec, 0.3)) = 1'),
  ('contains join rev', 'SELECT count(*) || '':'' || sum(c.id) FROM cs_dprobe p JOIN cs_cat c ON contains(point(''ICRS'', p.ra, p.dec), circle(''ICRS'', c.ra, c.dec, 0.3)) = 1'),
  ('region in region',  'SELECT count(*) || '':'' || sum(id) FROM cs_reg WHERE contains(circle(''ICRS'', 10, 20, 0.1), r) = 1'),
  ('regions intersect', 'SELECT count(*) || '':'' || sum(id) FROM cs_reg WHERE intersects(r, circle(''ICRS'', 10, 20, 3)) = 1')
) t(name, q);
RESET enable_seqscan;
DROP TABLE cs_reg;
DROP FUNCTION cs_adql(text);
-- the range rewrite's waste gate declines in favour of a GiST-family index on
-- the position; with none there is nothing to decline for, and the rewrite
-- stays (here the gate is forced to trigger)
SET skycell.rewrite_max_waste = 0;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_cat WHERE point('ICRS', ra, dec) <@ polygon('ICRS', 10, 20, 12, 20, 12, 22, 10, 22);
CREATE INDEX cs_cat_spg ON cs_cat USING spgist (point('ICRS', ra, dec) skypos_spgist_ops);
-- the index is what matters, not how it is read: PostgreSQL 16 reads it
-- through a bitmap where 18 scans it, so the plain index scan is pinned
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_cat WHERE point('ICRS', ra, dec) <@ polygon('ICRS', 10, 20, 12, 20, 12, 22, 10, 22);
RESET enable_bitmapscan;
DROP INDEX cs_cat_spg;
RESET skycell.rewrite_max_waste;
DROP TABLE cs_dprobe;
-- the first query of a session: distance()'s support function loads the
-- library while the comparison is planned, in time for the cone
\c
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM cs_probe p JOIN cs_cat c ON distance(p.ra, p.dec, c.ra, c.dec) < 0.3;
SET extra_float_digits = 0;

RESET skycell.custom_scan;
DROP TABLE cs_probe;
DROP FUNCTION cs_join(text);
DROP VIEW cs_view;
DROP FUNCTION cs_both(text);
DROP FUNCTION cs_plan(text);
DROP TABLE cs_cat, cs_sorted;

-- the index filter: with the position in the cell index (INCLUDE), the scan
-- tests the cone on each index entry and fetches only the rows inside it.
-- The rows are the same with the filter on and off, in both modes and for a
-- cross-match, NULL positions and an updated position included.
CREATE TABLE cs_fcat AS
SELECT i AS id,
       mod(i * 137.50776405003785, 360.0)::float8 AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 20000.0)) AS dec
FROM generate_series(0, 19999) i;
UPDATE cs_fcat SET ra = NULL WHERE id % 1000 = 7;
CREATE TABLE cs_fsorted AS SELECT * FROM cs_fcat ORDER BY skycell_ang2cell(ra, dec);
CREATE INDEX cs_fcat_cov ON cs_fcat ((skycell_ang2cell(ra, dec))) INCLUDE (ra, dec);
CREATE INDEX cs_fsorted_cov ON cs_fsorted ((skycell_ang2cell(ra, dec))) INCLUDE (ra, dec);
UPDATE cs_fcat SET ra = ra + 0.5 WHERE id % 50 = 3;
VACUUM ANALYZE cs_fcat;
VACUUM ANALYZE cs_fsorted;
CREATE TABLE cs_fprobe AS
SELECT id AS pid, ra + 0.0003 AS ra, dec FROM cs_fcat WHERE id % 97 = 0 AND ra IS NOT NULL;
ANALYZE cs_fprobe;
CREATE FUNCTION cs_filter(q text, OUT same bool, OUT filtered bool, OUT mode text)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; j json;
BEGIN
  PERFORM set_config('max_parallel_workers_per_gather', '0', true);
  PERFORM set_config('skycell.index_filter', 'off', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO a;
  PERFORM set_config('skycell.index_filter', 'on', true);
  EXECUTE 'SELECT (' || q || ')::text' INTO b;
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  same := a IS NOT DISTINCT FROM b;
  filtered := j::text ~ '"Index Filter": "ra, dec"';
  mode := substring(j::text from '"Mode": "([a-z]+)"');
END $$;
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(id)) FROM cs_fcat
  WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 10, 20, 3)$q$);
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(id)) FROM cs_fsorted
  WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 10, 20, 3)$q$);
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(id)) FROM cs_fcat
  WHERE contains(skycell_point(ra, dec), skycell_circle(359.9, 0, 4)) = 1$q$);
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(id)) FROM cs_fsorted
  WHERE contains(skycell_point(ra, dec), skycell_circle(0, 89.9, 5)) = 1$q$);
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(p.pid::int8 * 100000 + c.id)) FROM cs_fprobe p
  JOIN cs_fcat c ON contains(skycell_point(c.ra, c.dec), skycell_circle(p.ra, p.dec, 0.4)) = 1$q$);
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(p.pid::int8 * 100000 + c.id)) FROM cs_fprobe p
  JOIN cs_fsorted c ON contains(skycell_point(c.ra, c.dec), skycell_circle(p.ra, p.dec, 0.4)) = 1$q$);
-- a few probes share few pages, so the bitmap mode answers them: the filter
-- in it too, rescanned (the 206 probes above share theirs and run ordered)
SELECT * FROM cs_filter($q$SELECT row(count(*), sum(p.pid::int8 * 100000 + c.id))
  FROM (SELECT * FROM cs_fprobe WHERE pid < 3000) p
  JOIN cs_fcat c ON contains(skycell_point(c.ra, c.dec), skycell_circle(p.ra, p.dec, 0.4)) = 1$q$);
-- and against a sequential scan
SET skycell.custom_scan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*), sum(id) FROM cs_fcat WHERE skycell_in_cone(ra, dec, 10, 20, 3, -1);
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET skycell.custom_scan;
SELECT count(*), sum(id) FROM cs_fcat WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 10, 20, 3);
DROP FUNCTION cs_filter(text);
DROP TABLE cs_fprobe, cs_fcat, cs_fsorted;
