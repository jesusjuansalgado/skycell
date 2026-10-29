-- Synthetic Gaia-like catalogue with strong density contrast.
--   35% uniform sky
--   45% Galactic disk   (l uniform, |b| exponential, scale 3 deg)
--   12% bulge           (l, b Gaussian, sigma 7 deg around the Galactic centre)
--    8% 200 clusters    (Gaussian, sigma 0.02 .. 0.5 deg: up to ~10^4 x the mean density)
-- usage: psql -v n=10000000 -f 01_data.sql
\set ON_ERROR_STOP 1
\if :{?n}
\else
  \set n 10000000
\endif

SELECT setseed(0.2026);

-- Galactic (l, b) radians -> ICRS (ra, dec) degrees
CREATE OR REPLACE FUNCTION gal2eq(l float8, b float8, OUT ra float8, OUT "dec" float8)
LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$
  SELECT (degrees(atan2(y, x)) + 360)::numeric % 360, degrees(asin(greatest(-1, least(1, z))))
  FROM (SELECT gx, gy, gz FROM (SELECT cos(b) * cos(l) AS gx, cos(b) * sin(l) AS gy, sin(b) AS gz) g) g,
  LATERAL (SELECT -0.0548755604162154 * gx + 0.4941094278755837 * gy - 0.8676661490190047 * gz AS x,
                  -0.8734370902348850 * gx - 0.4448296299600112 * gy - 0.1980763734312015 * gz AS y,
                  -0.4838350155487132 * gx + 0.7469822444972189 * gy + 0.4559837761750669 * gz AS z) e
$$;

DROP TABLE IF EXISTS src, clusters;
CREATE TABLE clusters AS
SELECT k, 360 * random() AS ra, degrees(asin(1.9 * random() - 0.95)) AS dec,
       power(10, -1.7 + 1.4 * random()) AS sig
FROM generate_series(0, 199) k;

-- Logged on purpose: an unlogged table is truncated by crash recovery, and a
-- benchmark that silently measures an empty catalogue is worse than a slow load.
CREATE TABLE src (id bigint, ra float8, dec float8, kind text,
                          mag float4 DEFAULT (12 + 9 * random())::float4);

INSERT INTO src (id, ra, dec, kind)
SELECT i, 360 * random(), degrees(asin(2 * random() - 1)), 'uniform'
FROM generate_series(1, (:n * 0.35)::bigint) i;

INSERT INTO src (id, ra, dec, kind)
SELECT i, e.ra, e.dec, 'disk'
FROM (SELECT i, radians(360 * random()) AS l,
             radians(least(89.0, -3 * ln(1 - random())) * (CASE WHEN random() < 0.5 THEN -1 ELSE 1 END)) AS b
      FROM generate_series((:n * 0.35)::bigint + 1, (:n * 0.80)::bigint) i) s,
     LATERAL gal2eq(s.l, s.b) e;

INSERT INTO src (id, ra, dec, kind)
SELECT i, e.ra, e.dec, 'bulge'
FROM (SELECT i, radians(7 * sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random())) AS l,
             radians(greatest(-89.0, least(89.0, 7 * sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random())))) AS b
      FROM generate_series((:n * 0.80)::bigint + 1, (:n * 0.92)::bigint) i) s,
     LATERAL gal2eq(s.l, s.b) e;

INSERT INTO src (id, ra, dec, kind)
SELECT i, ((c.ra + c.sig * g1 / cos(radians(c.dec))) + 360)::numeric % 360,
       greatest(-90, least(90, c.dec + c.sig * g2)), 'cluster'
FROM (SELECT i, (random() * 200)::int % 200 AS k,
             sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random()) AS g1,
             sqrt(-2 * ln(1 - random())) * cos(2 * pi() * random()) AS g2
      FROM generate_series((:n * 0.92)::bigint + 1, :n) i) s
JOIN clusters c USING (k);

ANALYZE src;
SELECT kind, count(*) FROM src GROUP BY kind ORDER BY 1;
