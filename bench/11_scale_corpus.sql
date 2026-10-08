-- A corpus resampled from the Gaia DR3 density map: -v n rows (default 5e7, the
-- scale run of tab:scale; 1e7 gives the paper's 10M "Gaia corpus"), each placed
-- uniformly at random inside an order-9 cell drawn in proportion to that
-- cell's DR3 source count.  Needs gaia_map (bench/19_gaia_map.sh, or the copy
-- in bench/data/gaia_map_hpx9.csv.gz).  Replaces src; an existing src is kept
-- as src_prev.
\set ON_ERROR_STOP 1
\timing on
\if :{?n}
\else
  \set n 5e7
\endif
DROP TABLE IF EXISTS oc, cat_q3c, cat_sphere, cat_cell, src_prev;
ALTER TABLE IF EXISTS src RENAME TO src_prev;

SELECT setseed(0.5);
CREATE TABLE src AS
WITH tot AS (SELECT sum(n)::float8 AS s FROM gaia_map)
SELECT row_number() OVER () AS id,
       coord1(p) AS ra, coord2(p) AS dec, 'gaia'::text AS kind,
       (12 + 9 * random())::real AS mag
FROM (SELECT ivo_healpix_center(29, g.hpx9 * 1099511627776::bigint
                                    + floor(random() * 1099511627776)::bigint) AS p
      FROM gaia_map g, tot,
           LATERAL generate_series(1, floor(:n * g.n / tot.s + random())::int) k) q;
SELECT count(*) AS src50_rows FROM src;
