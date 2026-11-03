-- Estimates of the number of rows in a region (0.41): read from the
-- statistics the planner uses, no row read; and materialised density maps
-- (0.42), exact per cell.  The catalogue is small enough
-- (20,000 rows) that ANALYZE reads all of it, so the histogram, and the
-- estimates, are the same at every run.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;

CREATE TABLE es_cat AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 20000.0)) AS dec
FROM generate_series(0, 19999) i;
CREATE INDEX es_cat_cell ON es_cat (skycell_ang2cell(ra, dec));
ANALYZE es_cat;

-- regions holding many rows: within a few percent of the exact count
SELECT name, round(est::numeric / exact, 2) AS est_over_exact
FROM (SELECT name, skycell_count_estimate('es_cat', r) AS est,
             (SELECT count(*) FROM es_cat WHERE skycell_in_region(point('ICRS', ra, dec), r, -1)) AS exact
      FROM (VALUES ('cone 20 deg', circle('ICRS', 120, 30, 20)),
                   ('cone 60 deg', circle('ICRS', 300, -40, 60)),
                   ('polygon', polygon('ICRS', 10, -10, 80, -10, 80, 40, 10, 40)),
                   ('box', box('ICRS', 200, 0, 60, 40)),
                   ('annulus', region('DIFFERENCE ICRS (CIRCLE 0 0 40 CIRCLE 0 0 20)')),
                   ('rest of the sky', region('NOT ICRS (CIRCLE 0 90 30)'))) v(name, r)) s
ORDER BY 1;

-- every cell of an order together holds the whole table
SELECT round(sum(skycell_count_estimate('es_cat', 0, p))) AS faces FROM generate_series(0, 11) p;
SELECT round(sum(skycell_count_estimate('es_cat', 2, p))) AS order2_cells FROM generate_series(0, 191) p;

-- a cell's estimate is the cells' of the next order summed
SELECT round(skycell_count_estimate('es_cat', 3, 100)::numeric, 6)
     = round((SELECT sum(skycell_count_estimate('es_cat', 4, 400 + c)) FROM generate_series(0, 3) c)::numeric, 6) AS cells_nest;

-- the cells a region meets, in cell order; the cells inside it lie inside
SELECT count(*) AS cells, count(*) FILTER (WHERE inside) AS inside, round(sum(estimate)) AS rows,
       bool_and(ipix >= 0 AND ipix < 12 * 4^4) AS valid,
       (array_agg(ipix ORDER BY ipix) = array_agg(ipix)) AS in_cell_order
FROM skycell_density_estimate('es_cat', circle('ICRS', 120, 30, 20), 4);
SELECT count(*) = 12 * 4 ^ 2 AS sky_has_every_cell, bool_and(inside) AS all_inside
FROM skycell_density_estimate('es_cat', circle('ICRS', 0, 0, 180), 2);
SELECT count(*) AS nothing FROM skycell_density_estimate('es_cat', circle('ICRS', 0, 0, -1), 3);

-- a partitioned table: the sum over its partitions
CREATE TABLE es_part (id int, ra float8, dec float8) PARTITION BY RANGE (dec);
CREATE TABLE es_part_s PARTITION OF es_part FOR VALUES FROM (-90) TO (0);
CREATE TABLE es_part_n PARTITION OF es_part FOR VALUES FROM (0) TO (90.0001);
INSERT INTO es_part SELECT * FROM es_cat;
CREATE INDEX ON es_part (skycell_ang2cell(ra, dec));
ANALYZE es_part;
SELECT round(skycell_count_estimate('es_part', circle('ICRS', 120, 0, 40))::numeric
             / (SELECT count(*) FROM es_cat WHERE skycell_in_cone(ra, dec, 120, 0, 40)), 2) AS part_over_exact,
       round(sum(skycell_count_estimate('es_part', 1, p))) = 20000 AS part_cells_sum
FROM generate_series(0, 47) p;

-- a stored cell column; a schema-qualified, quoted name; an empty table
CREATE SCHEMA "Es Schema";
CREATE TABLE "Es Schema"."Cat" AS SELECT id, ra, dec, skycell_ang2cell(ra, dec) AS cell FROM es_cat;
CREATE INDEX ON "Es Schema"."Cat" (cell);
ANALYZE "Es Schema"."Cat";
SELECT round(skycell_count_estimate('"Es Schema"."Cat"', circle('ICRS', 120, 30, 20))) =
       round(skycell_count_estimate('es_cat', circle('ICRS', 120, 30, 20))) AS stored_column_same;
CREATE TABLE es_empty (ra float8, dec float8);
CREATE INDEX ON es_empty (skycell_ang2cell(ra, dec));
ANALYZE es_empty;
SELECT skycell_count_estimate('es_empty', circle('ICRS', 0, 0, 10)) AS empty_region,
       skycell_count_estimate('es_empty', 3, 7) AS empty_cell;

-- refused: a table with no cell index, no such table, a bad order or cell
CREATE TABLE es_plain (ra float8, dec float8);
SELECT skycell_count_estimate('es_plain', circle('ICRS', 0, 0, 1));
SELECT skycell_count_estimate('es_missing', circle('ICRS', 0, 0, 1));
SELECT skycell_count_estimate('es_cat', 30, 0);
SELECT skycell_count_estimate('es_cat', 2, 192);
SELECT count(*) FROM skycell_density_estimate('es_cat', circle('ICRS', 0, 0, 1), 21);
SELECT count(*) FROM skycell_density_estimate('es_cat', circle('ICRS', 0, 0, 180), 8);

-- materialised density maps (0.42): rows per occupied cell, orders 0 to 5
SELECT skycell_density_view('es_cat', 'es_dens', 5) AS cells;
SELECT hpx_order, count(*) AS cells, sum(n) AS rows FROM es_dens GROUP BY 1 ORDER BY 1;
-- every cell's count is the exact count of its rows
SELECT count(*) AS cells_checked,
       count(*) FILTER (WHERE n <> (SELECT count(*) FROM es_cat
                                    WHERE skycell_ang2cell(ra, dec) >> (2 * (29 - d.hpx_order)) = d.ipix)) AS wrong
FROM (SELECT * FROM es_dens WHERE hpx_order IN (0, 3, 5) AND ipix % 7 = 0) d;
-- a view's cells through the index, as ADQL writes it; the same without the
-- rewrite (every cell of the order tested)
EXPLAIN (COSTS OFF)
SELECT ipix, n FROM es_dens WHERE hpx_order = 4 AND 1 = skycell_cell_overlaps(4, ipix, CIRCLE('ICRS', 120, 30, 10));
SELECT (SELECT array_agg(ipix ORDER BY ipix) FROM es_dens
        WHERE hpx_order = 4 AND 1 = skycell_cell_overlaps(4, ipix, CIRCLE('ICRS', 120, 30, 10)))
     = (SELECT array_agg(ipix ORDER BY ipix) FROM es_dens
        WHERE hpx_order = 4 AND skycell_cell_overlaps(4, ipix, CIRCLE('ICRS', 120, 30, 10)) + 0 = 1) AS indexed_same,
       (SELECT array_agg(ipix ORDER BY ipix) FROM es_dens
        WHERE hpx_order = 5 AND 1 = skycell_cell_overlaps(5, ipix, region('DIFFERENCE ICRS (CIRCLE 0 0 20 CIRCLE 0 0 10)')))
     = (SELECT array_agg(ipix ORDER BY ipix) FROM es_dens
        WHERE hpx_order = 5 AND skycell_cell_overlaps(5, ipix, region('DIFFERENCE ICRS (CIRCLE 0 0 20 CIRCLE 0 0 10)')) + 0 = 1) AS indexed_same_annulus;
-- the cells meeting a region hold all its rows
SELECT (SELECT sum(n) FROM es_dens WHERE hpx_order = 4 AND 1 = skycell_cell_overlaps(4, ipix, CIRCLE('ICRS', 120, 30, 10)))
       >= (SELECT count(*) FROM es_cat WHERE skycell_in_cone(ra, dec, 120, 30, 10)) AS cells_hold_the_cone;
-- cell 0 of order 3 is the southern tip of face 0, at (45, 0); face 10 is
-- the southern sky's
SELECT skycell_cell_overlaps(3, 0, CIRCLE('ICRS', 45, 0, 1)) AS at_the_tip,
       skycell_cell_overlaps(3, 640, CIRCLE('ICRS', 45, 0, 1)) AS far,
       skycell_cell_overlaps(0, 4, CIRCLE('ICRS', 45, 0, 1)) AS face;
-- rebuilt on demand, after rows are loaded
INSERT INTO es_cat SELECT 30000 + i, 120, 30 FROM generate_series(1, 100) i;
SELECT sum(n) AS before_refresh FROM es_dens WHERE hpx_order = 0;
REFRESH MATERIALIZED VIEW CONCURRENTLY es_dens;
SELECT sum(n) AS after_refresh FROM es_dens WHERE hpx_order = 0;
-- a partitioned table, a schema-qualified target and a window of orders
SELECT skycell_density_view('es_part', '"Es Schema"."Part Density"', 4, 2) AS cells;
SELECT hpx_order, sum(n) AS rows FROM "Es Schema"."Part Density" GROUP BY 1 ORDER BY 1;
-- refused
SELECT skycell_density_view('es_plain', 'es_plain_dens');
SELECT skycell_density_view('es_cat', 'es_bad', 21);
SELECT skycell_cell_overlaps(30, 0, CIRCLE('ICRS', 0, 0, 1));

DROP MATERIALIZED VIEW es_dens;
DROP SCHEMA "Es Schema" CASCADE;
DROP TABLE es_cat, es_part, es_empty, es_plain;
