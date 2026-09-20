\set ON_ERROR_STOP 1
\timing on
DROP TABLE IF EXISTS oc, cat_q3c, cat_sphere, cat_cell;
ALTER TABLE src RENAME TO src_g10;

SELECT setseed(0.5);
CREATE TABLE src AS
WITH tot AS (SELECT sum(n)::float8 AS s FROM gaia_map)
SELECT row_number() OVER () AS id,
       coord1(p) AS ra, coord2(p) AS dec, 'gaia'::text AS kind,
       (12 + 9 * random())::real AS mag
FROM (SELECT ivo_healpix_center(29, g.hpx9 * 1099511627776::bigint
                                    + floor(random() * 1099511627776)::bigint) AS p
      FROM gaia_map g, tot,
           LATERAL generate_series(1, floor(5e7 * g.n / tot.s + random())::int) k) q;
SELECT count(*) AS src50_rows FROM src;
