-- A MOC as a search region (0.44): read from MOC ASCII or JSON, a point in it
-- when its order-29 cell is; through the custom scan, the range rewrite and a
-- sequential scan; in composites; region relations; stored and indexed.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;

-- reading: MOC 2.0 ASCII, MOC 1.0 commas, the "s" prefix, JSON; normalised
SELECT skycell_moc_region('5/4965 4967 6/19771-19775 10/');
SELECT skycell_moc_region('1/0,1,2,3 2/40');
SELECT skycell_moc_region('s3/5 s4/24-27');
SELECT skycell_moc_region('{"5": [4965, 4967], "6": [19771, 19772], "10": []}');
SELECT skycell_moc_region('{}') AS empty_json, skycell_moc_region('') AS empty, skycell_moc_region('7/') AS empty_depth;
SELECT skycell_moc_region('3/5 4/20-23 5/80') AS overlapping_cells;
SELECT region('MOC 5/4965 4967') AS stcs, 'MOC 3/1-2'::skyregion AS cast;
SELECT region('UNION ICRS (MOC 3/5 CIRCLE 10 20 1)') AS in_union,
       region('DIFFERENCE ICRS (MOC 1/0-3 NOT (MOC 2/0))') AS nested;
-- what skycell_moc() writes reads back as the same MOC
SELECT skycell_moc(skycell_moc_region(skycell_moc(circle('ICRS', 10, 20, 2), 8)), 8)
       = skycell_moc(circle('ICRS', 10, 20, 2), 8) AS round_trip;
SELECT skycell_moc(skycell_moc_region('5/4965 4967 6/19771-19775'), 4) AS coarser;

-- AREA and CENTROID
SELECT round(area(skycell_moc_region('0/0-11'))::numeric, 6) AS full_sky,
       round(area(skycell_moc_region('2/7'))::numeric, 6) AS order2_cell,
       area(skycell_moc_region('3/')) AS empty;
SELECT round(coord1(centroid(skycell_moc_region('0/4')))::numeric, 9) AS ra,
       round(coord2(centroid(skycell_moc_region('0/4')))::numeric, 9) AS dec;
SELECT centroid(skycell_moc_region('3/'));
SELECT centroid(skycell_moc_region('0/0-11'));

-- errors
SELECT skycell_moc_region('5/99999');
SELECT skycell_moc_region('4965');
SELECT skycell_moc_region('30/1');
SELECT skycell_moc_region('5/1 x');
SELECT skycell_moc_region('5/7-3');
SELECT skycell_moc_region('5/1;2');
SELECT skycell_moc_region('{"5": [1, 2}');
SELECT skycell_moc_region('{5: [1]}');
SELECT region('MOC 5/1 POINT');

-- a catalogue: the rows of a MOC are those whose cell is in it, by any plan
CREATE TABLE mr_cat AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 50000.0)) AS dec
FROM generate_series(0, 49999) i;
CREATE INDEX mr_cat_cell ON mr_cat (skycell_ang2cell(ra, dec));
-- every row in the sample (300 x the target), so the estimates are the same every run
ALTER INDEX mr_cat_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE mr_cat;
CREATE TABLE mr_mocs (name text, m text);
INSERT INTO mr_mocs VALUES
  ('one cell', '4/1234'),
  ('mixed orders', '3/100-110 5/2000-2010 7/40000-40100'),
  ('cone at 8', skycell_moc(circle('ICRS', 120, 30, 5), 8)),
  ('polygon at 10', skycell_moc(polygon('ICRS', 200, -10, 215, -8, 208, 4), 10)),
  ('annulus at 7', skycell_moc(region('DIFFERENCE ICRS (CIRCLE 300 60 12 CIRCLE 300 60 6)'), 7)),
  ('pole at 6', skycell_moc(circle('ICRS', 0, 90, 8), 6)),
  ('scattered', '0/3 4/2000 6/40000 8/600000 10/9000000-9000100'),
  ('whole sky', '0/0-11'),
  ('empty', '5/');
-- by cell, independently of the region code
CREATE FUNCTION mr_by_cell(moc text) RETURNS bigint LANGUAGE sql AS $$
  WITH t AS (SELECT split_part(x, '/', 1) o, split_part(x, '/', 2) cells
             FROM regexp_split_to_table(trim(moc), ' (?=[0-9]+/)') x),
       r AS (SELECT o::int o, split_part(c, '-', 1)::bigint a,
                    coalesce(nullif(split_part(c, '-', 2), ''), split_part(c, '-', 1))::bigint b
             FROM t, regexp_split_to_table(t.cells, ' ') c WHERE c <> '')
  SELECT count(*) FROM mr_cat WHERE EXISTS (
    SELECT 1 FROM r WHERE (skycell_ang2cell(ra, dec) >> (2 * (29 - r.o))) BETWEEN r.a AND r.b) $$;
SELECT name, mr_by_cell(m) AS by_cell,
       (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region(m))) AS adql
FROM mr_mocs ORDER BY name;

CREATE FUNCTION mr_plans(moc text) RETURNS TABLE (scan bigint, rewrite bigint, seq bigint)
LANGUAGE plpgsql AS $$
BEGIN
  EXECUTE format('SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), skycell_moc_region(%L))', moc) INTO scan;
  SET LOCAL skycell.custom_scan = off;
  EXECUTE format('SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), skycell_moc_region(%L))', moc) INTO rewrite;
  SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
  EXECUTE format('SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT(''ICRS'', ra, dec), skycell_moc_region(%L))', moc) INTO seq;
  RESET skycell.custom_scan; RESET enable_indexscan; RESET enable_bitmapscan;
  RETURN NEXT;
END $$;
SELECT name, p.scan = mr_by_cell(m) AND p.rewrite = p.scan AND p.seq = p.scan AS same, p.scan
FROM mr_mocs, LATERAL mr_plans(m) p ORDER BY name;

-- a constant MOC takes the custom scan
EXPLAIN (COSTS OFF)
SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100-110 5/2000-2010'));

-- in composites: the boolean combination of the tests
SELECT name,
  (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec),
     region('DIFFERENCE ICRS (MOC ' || m || ' CIRCLE 120 30 3)'))) =
  (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region(m))
     AND 0 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 3))) AS difference,
  (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec),
     region('INTERSECTION ICRS (MOC ' || m || ' POLYGON 100 0 140 0 140 50 100 50)'))) =
  (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region(m))
     AND 1 = CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', 100, 0, 140, 0, 140, 50, 100, 50))) AS intersection,
  (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), region('NOT ICRS (MOC ' || m || ')'))) =
  50000 - (SELECT count(*) FROM mr_cat WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region(m))) AS negation
FROM mr_mocs ORDER BY name;

-- relations: MOC with MOC exactly; with a cone as the cone's own MOC says
SELECT CONTAINS(skycell_moc_region('4/400-401'), skycell_moc_region('3/100')) AS a_in_b,
       CONTAINS(skycell_moc_region('3/100'), skycell_moc_region('4/400-403')) AS same,
       CONTAINS(skycell_moc_region('3/100'), skycell_moc_region('4/400-402')) AS b_in_a,
       INTERSECTS(skycell_moc_region('4/403'), skycell_moc_region('3/100')) AS touching,
       INTERSECTS(skycell_moc_region('4/404'), skycell_moc_region('3/100')) AS apart,
       CONTAINS(skycell_moc_region('5/'), skycell_moc_region('3/100')) AS empty_in;
CREATE TABLE mr_fp AS
SELECT i AS id, skycell_moc_region(skycell_moc(circle('ICRS', mod(i * 37.0, 360), mod(i * 13.0, 160) - 80,
                                                      1 + mod(i, 5)), 6)) AS r
FROM generate_series(1, 200) i;
CREATE TEMP TABLE mr_cones AS
SELECT j, c, skycell_moc_region(skycell_moc(c, 12)) AS cm
FROM (SELECT j, circle('ICRS', mod(j * 71.3, 360), mod(j * 29.7, 170) - 85, 0.3 + mod(j, 7)) c
      FROM generate_series(1, 25) j) s;
SELECT count(*) AS pairs, sum(INTERSECTS(r, c)) AS meets,
       count(*) FILTER (WHERE INTERSECTS(r, c) <> INTERSECTS(r, cm)) AS differ_meets,
       count(*) FILTER (WHERE CONTAINS(r, c) <> CONTAINS(r, cm)) AS differ_moc_in_cone,
       count(*) FILTER (WHERE CONTAINS(c, r) <> CONTAINS(cm, r)) AS differ_cone_in_moc
FROM mr_fp, mr_cones;

-- stored MOCs under a GiST index: the sequential scan's answers
CREATE INDEX mr_fp_r ON mr_fp USING gist (r);
ANALYZE mr_fp;
CREATE TEMP TABLE mr_seq AS
SELECT j, (SELECT array_agg(id ORDER BY id) FROM mr_fp WHERE r && c) AS meets,
          (SELECT array_agg(id ORDER BY id) FROM mr_fp WHERE 1 = CONTAINS(centroid(c), r)) AS holds
FROM mr_cones;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM mr_fp WHERE r && CIRCLE('ICRS', 120, 10, 5);
SELECT count(*) AS cones, count(*) FILTER (WHERE s.meets IS NOT DISTINCT FROM
         (SELECT array_agg(id ORDER BY id) FROM mr_fp WHERE r && c)) AS same_meets,
       count(*) FILTER (WHERE s.holds IS NOT DISTINCT FROM
         (SELECT array_agg(id ORDER BY id) FROM mr_fp WHERE 1 = CONTAINS(centroid(c), r))) AS same_holds,
       sum(cardinality(s.meets)) AS meets
FROM mr_cones JOIN mr_seq s USING (j);
RESET enable_seqscan;

-- estimates and cell overlaps
SELECT name, round((skycell_count_estimate('mr_cat', skycell_moc_region(m)) /
                    nullif(mr_by_cell(m), 0))::numeric, 1) AS est_over_exact
FROM mr_mocs WHERE name IN ('cone at 8', 'annulus at 7', 'whole sky') ORDER BY name;
SELECT skycell_moc_agg(5, ipix) AS cells_overlapping
FROM generate_series(0::bigint, (12 * 4^5 - 1)::bigint) ipix
WHERE skycell_cell_overlaps(5, ipix, skycell_moc_region('3/100 4/2000 6/40000-40001')) = 1;

-- a catalogue range-partitioned by cell: a MOC prunes to the partitions it reaches
CREATE TABLE mr_part (id int, ra float8, dec float8) PARTITION BY RANGE (skycell_ang2cell(ra, dec));
CREATE TABLE mr_part_0 PARTITION OF mr_part FOR VALUES FROM (MINVALUE) TO (1152921504606846976);
CREATE TABLE mr_part_1 PARTITION OF mr_part FOR VALUES FROM (1152921504606846976) TO (2305843009213693952);
CREATE TABLE mr_part_2 PARTITION OF mr_part FOR VALUES FROM (2305843009213693952) TO (MAXVALUE);
INSERT INTO mr_part SELECT id, ra, dec FROM mr_cat;
CREATE INDEX ON mr_part (skycell_ang2cell(ra, dec));
ANALYZE mr_part;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM mr_part WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100-110'));
SELECT (SELECT count(*) FROM mr_part WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100-110 1/40')))
     = mr_by_cell('3/100-110 1/40') AS partitioned_same;

-- MOC set operations (0.46): exact, on the cells; the result's depth the finer
SELECT skycell_moc_union(skycell_moc_region('3/100-110 5/2000-2010'), skycell_moc_region('2/25-27 4/1700')) AS union_,
       skycell_moc_intersection(skycell_moc_region('3/100-110 5/2000-2010'), skycell_moc_region('2/25-27 4/1700')) AS intersection_;
SELECT skycell_moc_difference(skycell_moc_region('3/100-110 5/2000-2010'), skycell_moc_region('2/25-27 4/1700')) AS difference_,
       skycell_moc_difference(skycell_moc_region('1/1'), skycell_moc_region('1/5')) AS apart,
       skycell_moc_difference(skycell_moc_region('1/1'), skycell_moc_region('0/0')) AS all_gone;
SELECT skycell_moc_complement(skycell_moc_region('0/0-10')) AS one_left,
       skycell_moc_complement(skycell_moc_region('0/0-11')) AS none_left,
       skycell_moc_complement(skycell_moc_region('3/')) AS everything;
SELECT skycell_moc_degrade(skycell_moc_region('3/100-110 5/2000-2010'), 2) AS order2,
       skycell_moc_degrade(skycell_moc_region('3/100-110 5/2000-2010'), 9) AS finer_unchanged;
-- any region as a MOC: what skycell_moc writes, and a MOC degraded
SELECT skycell_moc_region(circle('ICRS', 10, 20, 2), 6)::text = 'MOC ' || skycell_moc(circle('ICRS', 10, 20, 2), 6) AS as_skycell_moc,
       skycell_moc_region(skycell_moc_region('3/100-110 5/2000-2010'), 3) AS degraded;
-- identities: (a less b) with (a and b) is a; a with its complement is the sky
WITH m(a, b) AS (VALUES (skycell_moc_region(circle('ICRS', 120, 30, 5), 8), skycell_moc_region(polygon('ICRS', 118, 25, 130, 28, 125, 36), 9)))
SELECT skycell_moc(skycell_moc_union(skycell_moc_difference(a, b), skycell_moc_intersection(a, b)), 29)
       = skycell_moc(a, 29) AS split_and_join,
       area(skycell_moc_union(a, skycell_moc_complement(a))) = area(skycell_moc_region('0/0-11')) AS whole_sky,
       area(skycell_moc_intersection(a, skycell_moc_complement(a))) = 0 AS nothing,
       round((area(skycell_moc_union(a, b)) + area(skycell_moc_intersection(a, b)) - area(a) - area(b))::numeric, 9) AS inclusion_exclusion
FROM m;
-- the union of stored footprints, MOCs or (with an order) any regions; in parallel too
CREATE TABLE mr_obs AS
SELECT i AS id, skycell_moc_region(circle('ICRS', mod(i * 37.0, 360), mod(i * 13.0, 160) - 80, 0.5 + mod(i, 3)), 7) AS fp,
       circle('ICRS', mod(i * 37.0, 360), mod(i * 13.0, 160) - 80, 0.5 + mod(i, 3)) AS c
FROM generate_series(1, 3000) i;
ANALYZE mr_obs;
CREATE TEMP TABLE mr_u AS SELECT skycell_moc_union_agg(fp) AS a, skycell_moc_union_agg(c, 7) AS b FROM mr_obs;
SELECT a::text = b::text AS regions_as_mocs, round(area(a)::numeric, 3) AS area FROM mr_u;
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 3; SET parallel_leader_participation = off;
EXPLAIN (COSTS OFF) SELECT skycell_moc_union_agg(fp) FROM mr_obs;
SELECT (SELECT skycell_moc_union_agg(fp) FROM mr_obs)::text = a::text AS parallel_same FROM mr_u;
RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather; RESET parallel_leader_participation;
SELECT skycell_moc_union_agg(fp) IS NULL AS no_rows FROM mr_obs WHERE id < 0;
-- errors
SELECT skycell_moc_union(skycell_moc_region('1/1'), circle('ICRS', 10, 20, 1));
SELECT skycell_moc_union_agg(c) FROM mr_obs;
SELECT skycell_moc_degrade(skycell_moc_region('1/1'), 30);

DROP TABLE mr_cat, mr_mocs, mr_fp, mr_part, mr_obs;
DROP FUNCTION mr_by_cell(text), mr_plans(text);
