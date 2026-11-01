-- UNION, INTERSECTION, DIFFERENCE and NOT regions: STC-S in and out, the
-- constructors, the exact point test, and index use for a constant composite
-- and for stored ones.  Every indexed answer is checked against the
-- sequential scan, and the composites' own membership against the boolean
-- combination of their pieces.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;
SELECT setseed(0.38);

-- STC-S ---------------------------------------------------------------
SELECT 'UNION ICRS (CIRCLE 10 20 1 CIRCLE 12 20 0.5 POLYGON 1 1 2 1 2 2)'::skyregion AS u;
SELECT 'INTERSECTION ICRS (CIRCLE 10 20 1 CIRCLE 10.5 20 1)'::skyregion AS i;
SELECT 'DIFFERENCE ICRS (CIRCLE 10 20 1 CIRCLE 10 20 0.5)'::skyregion AS d;
SELECT 'NOT ICRS (UNION ICRS (CIRCLE 1 2 3 NOT (CIRCLE 4 5 1)))'::skyregion AS nested;
SELECT '  union  icrs ( circle 1 2 3   circle 4 5 6 ) '::skyregion AS spacing_and_case;
-- the text form reads back as itself
SELECT r::text::skyregion::text = r::text AS roundtrip
FROM (VALUES ('UNION ICRS (CIRCLE 10 20 1 POLYGON 1 1 2 1 2 2)'::skyregion),
             ('NOT ICRS (DIFFERENCE (CIRCLE 1 2 3 INTERSECTION (CIRCLE 1 2 1 CIRCLE 1.5 2 1)))'::skyregion)) v(r);
SELECT 'UNION ICRS (CIRCLE 1 2 3) (CIRCLE 1 2 3)'::skyregion;  -- error: one pair of parentheses
SELECT 'UNION ICRS (CIRCLE 1 2 3)'::skyregion;                 -- error: one region
SELECT 'DIFFERENCE ICRS (CIRCLE 1 2 3 CIRCLE 1 2 1 CIRCLE 1 2 2)'::skyregion;  -- error: three
SELECT 'NOT ICRS (CIRCLE 1 2 3 CIRCLE 1 2 3)'::skyregion;      -- error: two
SELECT 'UNION (CIRCLE 1 2 3 CIRCLE 1 2 3)'::skyregion;         -- error: no frame
SELECT 'UNION ICRS (CIRCLE 1 2 3 CIRCLE 1 2 3'::skyregion;     -- error: unbalanced
SELECT 'UNION ICRS (CIRCLE 1 2 3 CIRCLE 1 2 3) x'::skyregion;  -- error: trailing text
SELECT 'UNION ICRS CIRCLE 1 2 3'::skyregion;                   -- error: no parentheses
SELECT (repeat('NOT ICRS (', 33) || 'CIRCLE 1 2 3' || repeat(')', 33))::skyregion;  -- error: depth
SELECT length((repeat('NOT ICRS (', 32) || 'CIRCLE 1 2 3' || repeat(')', 32))::skyregion::text) > 0 AS depth_32_ok;

-- constructors ----------------------------------------------------------
SELECT region('DIFFERENCE ICRS (CIRCLE 10 20 1 CIRCLE 10 20 0.5)')::text
     = skycell_difference(circle('ICRS', 10, 20, 1), circle('ICRS', 10, 20, 0.5))::text AS region_is_difference,
       skycell_union(circle('ICRS', 1, 2, 3), circle('ICRS', 4, 5, 6), polygon('ICRS', 7, 8, 8, 8, 8, 9)) AS union3,
       skycell_intersection(circle('ICRS', 1, 2, 3), polygon('ICRS', 0, 0, 3, 0, 3, 3)) AS inter,
       skycell_not(circle('ICRS', 1, 2, 3)) AS complement;
SELECT skycell_union(circle('ICRS', 1, 2, 3));                       -- error: one region
SELECT skycell_union(VARIADIC ARRAY[circle('ICRS', 1, 2, 3), NULL]); -- error: NULL member
SELECT skycell_union(VARIADIC NULL::skyregion[]) IS NULL AS null_array;
SELECT region('CIRCLE ICRS 1 2 3') AS plain_stcs;

-- membership: each composite against its pieces --------------------------
CREATE TABLE cpts AS
SELECT i AS id, point('ICRS', 8 + 4 * random(), 18 + 4 * random()) AS p FROM generate_series(1, 5000) i;
CREATE TABLE cparts AS SELECT circle('ICRS', 10, 20, 1) AS a, circle('ICRS', 10.6, 20, 0.8) AS b,
                              polygon('ICRS', 9, 19, 11, 19, 10, 21.5) AS c;
SELECT bool_and(contains(p, skycell_union(a, b, c)) = (contains(p, a) | contains(p, b) | contains(p, c))) AS u_ok,
       bool_and(contains(p, skycell_intersection(a, b, c)) = (contains(p, a) & contains(p, b) & contains(p, c))) AS i_ok,
       bool_and(contains(p, skycell_difference(a, b)) = (contains(p, a) & (1 - contains(p, b)))) AS d_ok,
       bool_and(contains(p, skycell_not(c)) = 1 - contains(p, c)) AS n_ok,
       bool_and((p <@ skycell_difference(skycell_union(a, c), skycell_not(b)))
                = ((p <@ a OR p <@ c) AND p <@ b)) AS nested_ok,
       count(*) FILTER (WHERE p <@ skycell_difference(a, b)) > 0 AS d_nonempty,
       count(*) FILTER (WHERE p <@ skycell_intersection(a, b, c)) > 0 AS i_nonempty
FROM cpts, cparts;
-- an annulus: in the ring, not in the hole, not outside
SELECT contains(point('ICRS', 10, 20.75), r) AS ring, contains(point('ICRS', 10, 20), r) AS hole,
       contains(point('ICRS', 10, 21.5), r) AS outside
FROM (SELECT region('DIFFERENCE ICRS (CIRCLE 10 20 1 CIRCLE 10 20 0.5)') AS r) t;

-- what is not computed for a composite says so
SELECT area(skycell_union(circle('ICRS', 1, 2, 3), circle('ICRS', 4, 5, 6)));
SELECT centroid(skycell_not(circle('ICRS', 1, 2, 3)));
SELECT skycell_not(circle('ICRS', 1, 2, 3)) && circle('ICRS', 1, 2, 3);
-- except the area of a complement, which is exact
SELECT round(area(skycell_not(circle('ICRS', 1, 2, 3)))::numeric, 6)
     = round((41252.961249419271 - area(circle('ICRS', 1, 2, 3)))::numeric, 6) AS not_area;

-- a constant composite on the catalogue index -------------------------
CREATE TABLE ccat AS
SELECT i AS id, ra, dec, skycell_point(ra, dec) AS pos FROM (
  SELECT i, CASE WHEN i % 2 = 0 THEN 6 + 8 * random() ELSE 360 * random() END AS ra,
            CASE WHEN i % 2 = 0 THEN 16 + 8 * random() ELSE degrees(asin(2 * random() - 1)) END AS dec
  FROM generate_series(1, 100000) i) s;
CREATE INDEX ccat_pos ON ccat (skycell_cell(pos));
ANALYZE ccat;

CREATE TABLE cregs(name text, r skyregion);
INSERT INTO cregs VALUES
  ('annulus',      region('DIFFERENCE ICRS (CIRCLE 10 20 2 CIRCLE 10 20 1)')),
  ('two circles',  region('UNION ICRS (CIRCLE 8 18 1 CIRCLE 12 22 1.5)')),
  ('lens',         region('INTERSECTION ICRS (CIRCLE 10 20 2 CIRCLE 11.5 20 2)')),
  ('box less U',   skycell_difference(box('ICRS', 10, 20, 6, 6),
                     polygon('ICRS', 8,18, 12,18, 12,22, 11,22, 11,19, 9,19, 9,22, 8,22))),
  ('three-way',    region('INTERSECTION ICRS (CIRCLE 10 20 3 NOT (CIRCLE 9 19 1) POLYGON 6 16 14 16 10 24)')),
  ('not a cap',    region('NOT ICRS (CIRCLE 10 20 60)')),
  ('nested',       region('UNION ICRS (DIFFERENCE (CIRCLE 10 20 3 CIRCLE 10 20 2) INTERSECTION (CIRCLE 10 20 1 NOT (CIRCLE 10 20 0.3)))')),
  ('empty',        region('INTERSECTION ICRS (CIRCLE 0 0 1 CIRCLE 90 0 1)'));

CREATE FUNCTION comp_plan_uses_index(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; found bool := false;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'Index (Only )?Scan|Bitmap Index Scan|SkycellCone' THEN found := true; END IF;
  END LOOP;
  RETURN found;
END $$;

CREATE FUNCTION comp_counts(reg skyregion, OUT idx text, OUT seq text, OUT exact text, OUT indexed bool)
LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM ccat WHERE pos <@ %L::skyregion', reg::text);
BEGIN
  EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM ccat WHERE skycell_in_region(pos, %L::skyregion)', reg::text) INTO exact;
  SET LOCAL enable_seqscan = off;
  EXECUTE q INTO idx;
  indexed := comp_plan_uses_index(q);
  SET LOCAL enable_seqscan = on;
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  EXECUTE q INTO seq;
END $$;
-- (an empty composite needs no index: the planner folds it to false)
SELECT name, c.idx = c.seq AND c.idx = c.exact AS agree, split_part(c.seq, ':', 1)::int > 0 AS nonempty, c.indexed
FROM cregs, comp_counts(r) c ORDER BY name;
-- the ADQL spelling too
SELECT count(*) = (SELECT count(*) FROM ccat, cregs WHERE name = 'annulus' AND skycell_in_region(pos, r)) AS adql_agrees
FROM ccat WHERE contains(pos, region('DIFFERENCE ICRS (CIRCLE 10 20 2 CIRCLE 10 20 1)')) = 1;

-- coverings keep every row, for many random composites
CREATE FUNCTION rand_leaf() RETURNS skyregion LANGUAGE sql VOLATILE AS $$
  SELECT CASE WHEN random() < 0.6 THEN circle('ICRS', 6 + 8 * random(), 16 + 8 * random(), 0.2 + 2 * random())
              ELSE box('ICRS', 6 + 8 * random(), 16 + 8 * random(), 0.5 + 3 * random(), 0.5 + 3 * random()) END $$;
CREATE FUNCTION rand_region(depth int) RETURNS skyregion LANGUAGE plpgsql VOLATILE AS $$
DECLARE k float8 := random();
BEGIN
  IF depth = 0 OR k < 0.25 THEN RETURN rand_leaf(); END IF;
  IF k < 0.45 THEN RETURN skycell_union(rand_region(depth - 1), rand_region(depth - 1)); END IF;
  IF k < 0.65 THEN RETURN skycell_intersection(rand_region(depth - 1), rand_region(depth - 1)); END IF;
  IF k < 0.85 THEN RETURN skycell_difference(rand_region(depth - 1), rand_region(depth - 1)); END IF;
  RETURN skycell_not(rand_region(depth - 1));
END $$;
CREATE TABLE crand AS SELECT i AS n, rand_region(3) AS r FROM generate_series(1, 40) i;
SELECT count(*) AS composites, count(*) FILTER (WHERE c.idx = c.seq AND c.idx = c.exact) AS agree,
       count(*) FILTER (WHERE split_part(c.seq, ':', 1)::int > 0) > 20 AS mostly_nonempty
FROM crand, comp_counts(r) c;

-- stored composites under each region index ------------------------------
CREATE TABLE cstore AS
SELECT n AS fid, r FROM crand
UNION ALL SELECT 100 + n, rand_leaf() FROM generate_series(1, 40) n
UNION ALL SELECT 200 + row_number() OVER (), r FROM cregs WHERE name <> 'empty';
CREATE TABLE cprobe AS
SELECT i AS pid, point('ICRS', 6 + 8 * random(), 16 + 8 * random()) AS p FROM generate_series(1, 300) i
UNION ALL SELECT 1000 + i, point('ICRS', 360 * random(), degrees(asin(2 * random() - 1))) FROM generate_series(1, 100) i;
ANALYZE cprobe;

CREATE FUNCTION store_counts(OUT name text, OUT idx text, OUT seq text) RETURNS SETOF record
LANGUAGE plpgsql AS $$
DECLARE q text;
BEGIN
  FOR name, q IN VALUES
    ('point join', 'SELECT count(*) || '':'' || coalesce(sum(fid * 10000 + pid), 0) FROM cprobe JOIN cstore ON r @> p'),
    ('one point',  'SELECT count(*) || '':'' || coalesce(sum(fid), 0) FROM cstore WHERE r @> point(''ICRS'', 10, 20)'),
    ('ADQL',       'SELECT count(*) || '':'' || coalesce(sum(fid), 0) FROM cstore WHERE contains(point(''ICRS'', 10.5, 19.5), r) = 1'),
    ('correlated', 'SELECT sum((SELECT count(*) FROM cstore WHERE r @> pr.p)) || '''' FROM cprobe pr')
  LOOP
    SET LOCAL enable_seqscan = off;
    SET LOCAL enable_indexscan = on;
    SET LOCAL enable_bitmapscan = on;
    SET LOCAL enable_hashjoin = off;
    SET LOCAL enable_mergejoin = off;
    EXECUTE q INTO idx;
    SET LOCAL enable_seqscan = on;
    SET LOCAL enable_indexscan = off;
    SET LOCAL enable_bitmapscan = off;
    EXECUTE q INTO seq;
    RETURN NEXT;
  END LOOP;
END $$;

CREATE INDEX cstore_r ON cstore USING gist (r);
ANALYZE cstore;
SELECT 'gist' AS idx, name, idx = seq AS agree, split_part(seq, ':', 1)::bigint > 0 AS nonempty
FROM store_counts() ORDER BY name;
DROP INDEX cstore_r;
CREATE INDEX cstore_box4 ON cstore USING gist (r skyregion_box4_gist_ops);
SELECT 'box4' AS idx, name, idx = seq AS agree FROM store_counts() ORDER BY name;
DROP INDEX cstore_box4;
CREATE INDEX cstore_moc ON cstore USING gin (skycell_region_moc(r));
ANALYZE cstore;
SELECT 'moc' AS idx, name, idx = seq AS agree FROM store_counts() ORDER BY name;
-- comparing two stored regions is not computed for a composite
SELECT count(*) FROM cstore a JOIN cstore b ON a.r && b.r;

DROP TABLE cpts, cparts, ccat, cregs, crand, cstore, cprobe;
DROP FUNCTION store_counts(), comp_counts(skyregion), comp_plan_uses_index(text),
              rand_region(int), rand_leaf();
