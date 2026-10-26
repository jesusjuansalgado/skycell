-- Non-convex polygons: cut into convex pieces in the gnomonic plane at the
-- vertex mean (cover.c), with the boundary ring kept whole.  Every answer
-- here is checked against something that does not share that code: the same
-- polygon written as a union of convex pieces, the sequential scan, or plain
-- geometry.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;
SELECT setseed(0.29);

-- A U opening north, 4 x 4 degrees at (180, 0), with a 2 x 3 notch, and the
-- same U as three convex pieces that share its vertices (their union is the
-- U exactly: the pieces' extra edges are diagonals of the U).
CREATE TABLE shape AS SELECT
  polygon('ICRS', 178,-2, 182,-2, 182,2, 181,2, 181,-1, 179,-1, 179,2, 178,2) AS u,
  polygon('ICRS', 178,-2, 182,-2, 181,-1, 179,-1) AS u1,
  polygon('ICRS', 178,-2, 179,-1, 179,2, 178,2) AS u2,
  polygon('ICRS', 182,-2, 182,2, 181,2, 181,-1) AS u3;

-- input -------------------------------------------------------------
SELECT 'POLYGON ICRS 0 0 10 0 1 1 0 10'::skyregion AS arrowhead,
       polygon('ICRS', 178,2, 179,2, 179,-1, 181,-1, 181,2, 182,2, 182,-2, 178,-2)::text
         = polygon('ICRS', 178,2, 179,2, 179,-1, 181,-1, 181,2, 182,2, 182,-2, 178,-2)::text AS clockwise_accepted;
SELECT polygon('ICRS', 0,0, 10,10, 10,0, 0,10);           -- error: a bow tie
SELECT polygon('ICRS', 0,0, 4,0, 4,4, 2,0.0);             -- error: touches its own edge
SELECT polygon('ICRS', 0,0, 4,0, 2,0, 2,3);               -- error: doubles back
SELECT polygon('ICRS', 0,0, 120,0, 120,10, 60,-5, 0,10, 240,0);  -- error: hemisphere

-- points, area, centroid --------------------------------------------
SELECT contains(point('ICRS', 178.5, 1.5), u) AS left_arm,
       contains(point('ICRS', 181.5, 1.5), u) AS right_arm,
       contains(point('ICRS', 180, -1.5), u) AS base,
       contains(point('ICRS', 180, 1), u) AS notch,
       contains(point('ICRS', 180, -0.999), u) AS notch_floor_above,
       contains(point('ICRS', 180, -1.001), u) AS notch_floor_below,
       contains(point('ICRS', 179, 0.5), u) AS on_inner_edge,
       contains(point('ICRS', 181, 2), u) AS at_inner_corner
FROM shape;
SELECT abs(area(u) - (area(u1) + area(u2) + area(u3))) < 1e-9 * area(u) AS area_is_pieces,
       round(area(u)::numeric, 4) AS sqdeg,
       round(coord1(centroid(u))::numeric, 9) AS c1,
       round(coord2(centroid(u))::numeric, 2) AS c2,
       contains(centroid(u), u) AS centroid_in_notch_is_outside
FROM shape;
-- a convex polygon's centroid is its centre of mass too (the mean of the
-- vertices until 0.35): this lopsided quadrilateral is small enough to be
-- flat to 1e-5, where the planar answer is (10.16667, 0.10833) and the
-- vertex mean (10.2, 0.1).  The same polygon 0.4 arcsec and 4 microarcsec
-- across: the centre of mass keeps its place in the shape.
SELECT round(coord1(c)::numeric, 4) AS c1, round(coord2(c)::numeric, 4) AS c2
FROM (SELECT centroid(polygon('ICRS', 10,0, 10.4,0, 10.4,0.1, 10,0.3)) AS c) t;
SELECT s AS size_deg,
       round(((coord1(c) - 10) / s)::numeric, 5) AS x_in_shape,
       round(((coord2(c) - 0) / s)::numeric, 5) AS y_in_shape
FROM (SELECT s, centroid(polygon('ICRS', 10,0, 10+4*s,0, 10+4*s,s, 10,3*s)) AS c
      FROM (VALUES (1/36000.0::float8), (1/3.6e9::float8)) v(s)) t;
-- from the middle of the notch the U is asin(cos 1 sin 1) = 0.99985 degrees
-- away, across the meridian arc of either arm: a cone meets it just past that
SELECT degrees(asin(cos(radians(1)) * sin(radians(1)))) AS d,
       intersects(circle('ICRS', 180, 1, 0.99984), u) AS short_of_arms,
       intersects(circle('ICRS', 180, 1, 0.99986), u) AS reaches_arms
FROM shape;

-- regions -----------------------------------------------------------
-- the bridge has every vertex in an arm and spans the notch: the case a
-- vertex-only containment test gets wrong
SELECT contains(circle('ICRS', 180, 1, 0.2), u) AS circle_in_notch,
       intersects(circle('ICRS', 180, 1, 0.2), u) AS notch_meets,
       contains(circle('ICRS', 178.5, 0, 0.2), u) AS circle_in_arm,
       contains(polygon('ICRS', 178.5,1, 181.5,1, 181.5,1.5, 178.5,1.5), u) AS bridge_inside,
       intersects(polygon('ICRS', 178.5,1, 181.5,1, 181.5,1.5, 178.5,1.5), u) AS bridge_meets,
       contains(polygon('ICRS', 178.2,-1.8, 181.8,-1.8, 181.8,-1.5, 178.2,-1.5), u) AS strip_in_base,
       contains(u, u) AS self,
       contains(u1, u) + contains(u2, u) + contains(u3, u) = 3 AS pieces_inside,
       contains(u, polygon('ICRS', 178,-2, 182,-2, 182,2, 178,2)) AS in_hull,
       contains(polygon('ICRS', 178,-2, 182,-2, 182,2, 178,2), u) AS hull_inside,
       contains(u, circle('ICRS', 180, 0, 3)) AS in_circle,
       intersects(u, polygon('ICRS', 179.5,0, 180.5,0, 180.5,1, 179.5,1)) AS notch_square_meets,
       intersects(u, polygon('ICRS', 179,0, 181,0, 181,1, 179,1)) AS notch_fill_touches,
       intersects(u, u) AS self_meets
FROM shape;

-- the index ---------------------------------------------------------
-- positions dense around the shapes below: the U, a pac-man over the north pole,
-- a comb across RA 0, a star at the zone boundary, a tiny U (arcseconds)
CREATE TABLE pcat AS
SELECT i AS id, ra, dec, skycell_point(ra, dec) AS pos FROM (
  SELECT i,
         CASE i % 4 WHEN 0 THEN 175 + 10 * random()
                    WHEN 1 THEN 360 * random()
                    WHEN 2 THEN (360 + 10 * random() - 5)::numeric % 360
                    ELSE 40 + 10 * random() END::float8 AS ra,
         CASE i % 4 WHEN 0 THEN 5 * (2 * random() - 1)
                    WHEN 1 THEN 85 + 5 * random()
                    WHEN 2 THEN 5 * (2 * random() - 1)
                    ELSE 41.8103148958 + 5 * (2 * random() - 1) END::float8 AS dec
  FROM generate_series(1, 120000) i) s;
INSERT INTO pcat SELECT 200000 + i, 180 + (random() - 0.5) * 0.01, (random() - 0.5) * 0.01, NULL
FROM generate_series(1, 20000) i;
UPDATE pcat SET pos = skycell_point(ra, dec) WHERE pos IS NULL;
CREATE INDEX pcat_pos ON pcat (skycell_cell(pos));
CREATE INDEX pcat_radec ON pcat (skycell_ang2cell(ra, dec));
ANALYZE pcat;

CREATE TABLE polys(name text, v float8[]);
INSERT INTO polys VALUES
  ('U',        ARRAY[178,-2, 182,-2, 182,2, 181,2, 181,-1, 179,-1, 179,2, 178,2]),
  ('pac-man',  ARRAY[60,86, 120,86, 180,86, 240,86, 300,86, 0,86, 30,88]),
  ('comb RA0', ARRAY[356,-3, 4,-3, 4,3, 3,3, 3,-1, 1,-1, 1,3, 0,3, 0,-1, 358,-1, 358,3, 357,3,
                     357,-1, 356.5,-1, 356.5,3, 356,3]),
  ('star',     ARRAY[45,45, 45.6,43, 48,42.5, 46,41.4, 47,39.5, 45,40.8, 43,39.5, 44,41.4, 42,42.5, 44.4,43]),
  ('tiny U',   ARRAY[179.998,-0.002, 180.002,-0.002, 180.002,0.002, 180.001,0.002, 180.001,-0.001,
                     179.999,-0.001, 179.999,0.002, 179.998,0.002]);

CREATE FUNCTION poly_plan_uses_index(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; found bool := false;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'Index (Only )?Scan|Bitmap Index Scan|SkycellCone' THEN found := true; END IF;
  END LOOP;
  RETURN found;
END $$;

-- the exact test row by row, the B-tree range rewrite (skycell_poly, and
-- pos <@ polygon with the index forced) and pos <@ polygon with the index
-- forbidden must all agree
CREATE FUNCTION poly_counts(v float8[], OUT exact text, OUT ranges text, OUT op text,
                            OUT op_seq text, OUT indexed bool) LANGUAGE plpgsql AS $$
DECLARE reg text := format('skycell_polygon(VARIADIC %L::float8[])', v);
BEGIN
  EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM pcat WHERE skycell_in_poly(ra, dec, %L::float8[])', v) INTO exact;
  EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM pcat WHERE skycell_poly(skycell_ang2cell(ra, dec), ra, dec, %L::float8[])', v) INTO ranges;
  SET LOCAL enable_seqscan = off;
  EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM pcat WHERE pos <@ %s', reg) INTO op;
  indexed := poly_plan_uses_index(format('SELECT * FROM pcat WHERE pos <@ %s', reg));
  SET LOCAL enable_seqscan = on;
  SET LOCAL enable_indexscan = off;
  SET LOCAL enable_bitmapscan = off;
  EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM pcat WHERE pos <@ %s', reg) INTO op_seq;
END $$;
SELECT name, c.exact = c.ranges AND c.exact = c.op AND c.exact = c.op_seq AS agree,
       split_part(c.exact, ':', 1)::int > 0 AS nonempty, c.indexed
FROM polys, poly_counts(v) c ORDER BY name;

SELECT contains(point('ICRS', 0, 90), p) AS pole, contains(point('ICRS', 30, 87), p) AS mouth,
       contains(point('ICRS', 30, 89), p) AS behind_mouth, contains(point('ICRS', 210, 87), p) AS back
FROM (SELECT skycell_polygon(VARIADIC v) AS p FROM polys WHERE name = 'pac-man') t;

-- and the U's rows are exactly its three pieces' rows
SELECT (SELECT count(*) FROM pcat, shape WHERE pos <@ u)
     = (SELECT count(*) FROM pcat, shape WHERE pos <@ u1 OR pos <@ u2 OR pos <@ u3) AS u_is_pieces,
       (SELECT count(*) FROM pcat, shape WHERE pos <@ u AND ra BETWEEN 179.01 AND 180.99 AND dec > -0.99) AS in_notch;

-- coverings of a non-convex polygon keep every row
SELECT bool_and((SELECT count(*) FROM pcat WHERE skycell_in_poly(ra, dec, v)
                 AND NOT EXISTS (SELECT 1 FROM skycell_poly_ranges(v) r
                                 WHERE skycell_ang2cell(ra, dec) BETWEEN r.lo AND r.hi)) = 0) AS ranges_cover
FROM polys;

-- region indexes ----------------------------------------------------
-- U shapes and circles scattered over the dense patch, under each GiST
-- opclass for regions; indexed and sequential answers must match
CREATE TABLE preg AS
SELECT i AS id,
       CASE WHEN i % 100 = 0 THEN circle('ICRS', ra, dec, 4)
            WHEN i % 3 = 0 THEN circle('ICRS', ra, dec, 0.3)
            ELSE skycell_polygon(VARIADIC ARRAY[ra-s, dec-s, ra+s, dec-s, ra+s, dec+s, ra+s/4, dec+s,
                                                ra+s/4, dec-s/2, ra-s/4, dec-s/2, ra-s/4, dec+s, ra-s, dec+s]) END AS r
FROM (SELECT i, 176 + 8 * random() AS ra, 4 * (2 * random() - 1) AS dec, 0.05 + 0.5 * random() AS s
      FROM generate_series(1, 3000) i) t;

CREATE FUNCTION region_counts(OUT name text, OUT seq text, OUT idx text) RETURNS SETOF record
LANGUAGE plpgsql AS $$
DECLARE q text;
BEGIN
  FOR name, q IN VALUES
    ('overlap U',      'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM preg, shape WHERE r && u'),
    ('in U',           'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM preg, shape WHERE r <@ u'),
    ('U holds',        'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM preg, shape WHERE u <@ r'),
    ('has notch pt',   'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM preg WHERE r @> point(''ICRS'', 180, 1)'),
    ('overlap circle', 'SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM preg WHERE r && circle(''ICRS'', 180, 1, 0.4)')
  LOOP
    SET LOCAL enable_seqscan = on;
    SET LOCAL enable_indexscan = off;
    SET LOCAL enable_bitmapscan = off;
    EXECUTE q INTO seq;
    SET LOCAL enable_seqscan = off;
    SET LOCAL enable_indexscan = on;
    SET LOCAL enable_bitmapscan = on;
    EXECUTE q INTO idx;
    RETURN NEXT;
  END LOOP;
END $$;

CREATE INDEX preg_r ON preg USING gist (r);
ANALYZE preg;
SELECT name, seq = idx AS agree, split_part(seq, ':', 1)::int > 0 AS nonempty
FROM region_counts() ORDER BY name;
DROP INDEX preg_r;
CREATE INDEX preg_box ON preg USING gist (r skyregion_box_gist_ops);
SELECT name, seq = idx AS agree FROM region_counts() ORDER BY name;
DROP INDEX preg_box;
CREATE INDEX preg_box4 ON preg USING gist (r skyregion_box4_gist_ops);
SELECT name, seq = idx AS agree FROM region_counts() ORDER BY name;

DROP TABLE preg, polys, pcat, shape;
DROP FUNCTION region_counts(), poly_counts(float8[]), poly_plan_uses_index(text);
