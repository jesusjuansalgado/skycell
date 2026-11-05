-- MOC export (0.43): IVOA MOC 2.0 ASCII and JSON, normalised (each cell at
-- the coarsest order it can be written at, orders ascending, the MOC's
-- depth declared by an empty max order written last).
CREATE EXTENSION IF NOT EXISTS skycell;

-- a region's MOC
SELECT skycell_moc(circle('ICRS', 10, 20, 2), 6);
SELECT skycell_moc(circle('ICRS', 10, 20, 2), 6, 'json');
SELECT skycell_moc(circle('ICRS', 10, 20, 2), 6, 'JSON') = skycell_moc(circle('ICRS', 10, 20, 2), 6, 'json') AS fmt_case_insensitive;
SELECT skycell_moc(circle('ICRS', 0, 0, 180), 10) AS full_sky;
SELECT skycell_moc(circle('ICRS', 0, 0, 180), 3, 'json') AS full_sky_json;
SELECT skycell_moc(polygon('ICRS', 10, 10, 30, 12, 25, 30), 4);
SELECT skycell_moc(region('DIFFERENCE ICRS (CIRCLE 100 0 12 CIRCLE 100 0 6)'), 3);
SELECT length(skycell_moc(circle('ICRS', 10, 20, 2))) AS default_order_10;

-- a small cone meets the one cell holding it, not the neighbours whose
-- bounding caps reach it
SELECT skycell_moc(circle('ICRS', 265.95665416560644, 78.0202866317311, 0.01), 3) AS small_cone;
SELECT skycell_moc(circle('ICRS', 168.54353574393403, -79.35732915328946, 0.01), 7) AS small_cone;

-- the cells of max order a MOC holds are those skycell_cell_overlaps() counts
SELECT skycell_moc_agg(6, ipix) = skycell_moc(circle('ICRS', 200, -40, 7), 6) AS moc_is_cell_overlaps
FROM generate_series(0::bigint, (12 * 4^6 - 1)::bigint) ipix
WHERE skycell_cell_overlaps(6, ipix, circle('ICRS', 200, -40, 7)) = 1;
SELECT skycell_moc_agg(5, ipix) = skycell_moc(region('UNION ICRS (CIRCLE 30 60 5 POLYGON 40 50 60 52 50 70)'), 5) AS moc_is_cell_overlaps
FROM generate_series(0::bigint, (12 * 4^5 - 1)::bigint) ipix
WHERE skycell_cell_overlaps(5, ipix, region('UNION ICRS (CIRCLE 30 60 5 POLYGON 40 50 60 52 50 70)')) = 1;

-- aggregating cells: duplicates are one cell, four siblings their parent
SELECT skycell_moc_agg(2, ipix) FROM (VALUES (8), (9), (10), (11), (9), (40), (191)) v(ipix);
SELECT skycell_moc_agg(2, ipix, 'json') FROM (VALUES (8), (9), (10), (11), (9), (40), (191)) v(ipix);
SELECT skycell_moc_agg(0, ipix) FROM generate_series(0, 11) ipix;
SELECT skycell_moc_agg(3, ipix) FROM (VALUES (NULL::bigint), (5)) v(ipix);
SELECT skycell_moc_agg(3, ipix) IS NULL AS no_rows FROM generate_series(1, 0) ipix;
SELECT skycell_moc_agg(3, NULL::bigint) IS NULL AS no_cells FROM generate_series(1, 3);
SELECT skycell_moc_agg(3, CASE WHEN i = 3 THEN 7 END) AS last_row_only FROM generate_series(1, 3) i;

-- a table's coverage, by expression index or stored cell column
CREATE TABLE moc_cat AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 2000.0)) * 0.1 + 30 AS dec
FROM generate_series(0, 1999) i;
CREATE INDEX moc_cat_cell ON moc_cat (skycell_ang2cell(ra, dec));
SELECT skycell_coverage_moc('moc_cat', 1);
SELECT skycell_coverage_moc('moc_cat', 2, 'json');
SELECT skycell_coverage_moc('moc_cat') =
       (SELECT skycell_moc_agg(10, skycell_ang2cell(ra, dec) >> 38) FROM moc_cat) AS coverage_is_agg;
CREATE TABLE moc_cells AS SELECT id, skycell_ang2cell(ra, dec) AS cell FROM moc_cat;
CREATE INDEX ON moc_cells (cell);
SELECT skycell_coverage_moc('moc_cells', 7) = skycell_coverage_moc('moc_cat', 7) AS cell_column;
DELETE FROM moc_cells;
SELECT skycell_coverage_moc('moc_cells', 7) AS empty_table;
SELECT skycell_coverage_moc('moc_cells', 7, 'json') AS empty_table;

-- errors
SELECT skycell_moc(circle('ICRS', 10, 20, 2), 30);
SELECT skycell_moc(circle('ICRS', 10, 20, 2), -1);
SELECT skycell_moc(circle('ICRS', 10, 20, 2), 6, 'fits');
SELECT skycell_moc_agg(13, ipix) FROM generate_series(0, 3) ipix;
SELECT skycell_moc_agg(2, ipix) FROM (VALUES (192)) v(ipix);
SELECT skycell_coverage_moc('moc_cat', 13);

-- in parallel (0.45): workers aggregate their own rows, the leader combines
-- them; the same MOC as one process makes
CREATE TABLE moc_par AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 100000.0)) * 0.3 AS dec
FROM generate_series(0, 99999) i;
CREATE INDEX moc_par_cell ON moc_par (skycell_ang2cell(ra, dec));
ANALYZE moc_par;
CREATE TEMP TABLE moc_serial AS
SELECT o, skycell_moc_agg(o, skycell_ang2cell(ra, dec) >> (2 * (29 - o))) AS a,
          skycell_moc_agg(o, skycell_ang2cell(ra, dec) >> (2 * (29 - o)), 'json') AS j
FROM moc_par, (VALUES (0), (3), (8), (12)) v(o) GROUP BY o;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 3;
SET parallel_leader_participation = off;
EXPLAIN (COSTS OFF) SELECT skycell_moc_agg(8, skycell_ang2cell(ra, dec) >> 42) FROM moc_par;
SELECT o, skycell_moc_agg(o, skycell_ang2cell(ra, dec) >> (2 * (29 - o))) = a AS ascii_same
FROM moc_par, moc_serial WHERE o = 0 GROUP BY o, a
UNION ALL SELECT 3, (SELECT skycell_moc_agg(3, skycell_ang2cell(ra, dec) >> 52) FROM moc_par) = a FROM moc_serial WHERE o = 3
UNION ALL SELECT 8, (SELECT skycell_moc_agg(8, skycell_ang2cell(ra, dec) >> 42) FROM moc_par) = a FROM moc_serial WHERE o = 8
UNION ALL SELECT 12, (SELECT skycell_moc_agg(12, skycell_ang2cell(ra, dec) >> 34) FROM moc_par) = a FROM moc_serial WHERE o = 12
ORDER BY 1;
SELECT (SELECT skycell_moc_agg(12, skycell_ang2cell(ra, dec) >> 34, 'json') FROM moc_par) = j AS json_same
FROM moc_serial WHERE o = 12;
-- a worker whose rows hold no cell, and no rows at all
SELECT skycell_moc_agg(5, CASE WHEN id = 77777 THEN skycell_ang2cell(ra, dec) >> 48 END) =
       (SELECT skycell_moc_agg(5, skycell_ang2cell(ra, dec) >> 48) FROM moc_par WHERE id = 77777) AS one_cell
FROM moc_par;
SELECT skycell_moc_agg(5, skycell_ang2cell(ra, dec) >> 48) IS NULL AS none FROM moc_par WHERE id < 0;
SELECT skycell_coverage_moc('moc_par', 8) = (SELECT a FROM moc_serial WHERE o = 8) AS coverage_same;
RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather; RESET parallel_leader_participation;
DROP TABLE moc_par;

DROP TABLE moc_cat, moc_cells;
