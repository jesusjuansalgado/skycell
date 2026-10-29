-- Generates the stored ObsCore field and query centres of 10_crossover.sql:
--
--   oc_fields.csv.gz  78125 field centres, enough for the 10M-row relation at
--                     128 observations per field, drawn in proportion to the
--                     DR3 order-9 density map and stored in draw order, so any
--                     prefix (the first nrows/128 fields) is an all-sky sample
--   oc_queries.csv    40 query centres, each inside one of the first 3906
--                     fields (the 0.5M relation's), so every size answers the
--                     same queries and every query lands on observations
--
-- It was run once and its output committed; 10_crossover.sql only reads the
-- files.  Rerunning it reproduces them (the draw is seeded) on a database with
-- gaia_map loaded and pgSphere installed (ivo_healpix_center), from bench/:
--
--   psql -v ON_ERROR_STOP=1 -f data/make_oc_fields.sql
\set ON_ERROR_STOP 1
CREATE TEMP TABLE gm AS
SELECT hpx9, (sum(n) OVER (ORDER BY hpx9) - n)::float8 / t AS lo
FROM gaia_map, (SELECT sum(n)::float8 AS t FROM gaia_map) s;
CREATE INDEX ON gm (lo);
ANALYZE gm;

SELECT setseed(0.23);
CREATE TEMP TABLE draws AS
SELECT k AS field_id, random() AS u, random() AS v FROM generate_series(1, 78125) k;

-- a cell in proportion to its DR3 count, then a uniform order-29 cell inside it
CREATE TEMP TABLE fields AS
SELECT d.field_id, round(coord1(p)::numeric, 6) AS ra, round(coord2(p)::numeric, 6) AS dec
FROM draws d
CROSS JOIN LATERAL (SELECT hpx9 FROM gm WHERE lo <= d.u ORDER BY lo DESC LIMIT 1) g
CROSS JOIN LATERAL (SELECT ivo_healpix_center(29, g.hpx9 * 1099511627776::bigint
                                                  + floor(d.v * 1099511627776)::bigint) AS p) c;

-- query centres: 40 distinct fields of the smallest relation, each offset the
-- way an observation is (uniform in a 0.4 deg box about the field centre)
SELECT setseed(0.37);
CREATE TEMP TABLE queries AS
SELECT row_number() OVER () AS qid, field_id,
       round(((ra + (random() - 0.5) * 0.4 + 360)::numeric % 360), 6) AS ra,
       round(greatest(-89.9, least(89.9, dec + (random() - 0.5) * 0.4))::numeric, 6) AS dec
FROM (SELECT * FROM fields WHERE field_id <= 3906 ORDER BY random() LIMIT 40) f;

\copy (SELECT * FROM fields ORDER BY field_id) TO PROGRAM 'gzip -n9 > data/oc_fields.csv.gz' WITH CSV HEADER
\copy (SELECT * FROM queries ORDER BY qid) TO 'data/oc_queries.csv' WITH CSV HEADER
