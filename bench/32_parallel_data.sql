-- Parallel cross-matching (paper, Sect. "Parallel cross-matching"): the data.
-- 5M catalogue rows, 70% uniform and 30% in 200 Gaussian clusters of
-- 0.05-1 deg, stored once at random (cat_rand) and once in cell order
-- (cat_clus); 1M targets on catalogue sources, displaced by up to 0.3 arcsec,
-- and a 200k subset.
CREATE EXTENSION IF NOT EXISTS skycell;
SELECT setseed(0.42);
CREATE TABLE cl AS SELECT k, 360*random() AS ra, degrees(asin(2*random()-1)) AS dec, 0.05 + 0.95*random() AS s FROM generate_series(1,200) k;
CREATE TABLE cat_rand AS
SELECT row_number() OVER () AS id, ra, dec, skycell_ang2cell(ra, dec) AS cell, random() AS mag FROM (
  SELECT 360*random() AS ra, degrees(asin(2*random()-1)) AS dec FROM generate_series(1, 3500000)
  UNION ALL
  SELECT ((cl.ra + cl.s * sqrt(-2*ln(random())) * cos(2*pi()*random()) / greatest(cos(radians(cl.dec)), 0.05))::numeric % 360 + 360)::numeric % 360,
         greatest(-90, least(90, cl.dec + cl.s * sqrt(-2*ln(random())) * cos(2*pi()*random())))
  FROM cl, generate_series(1, 7500)) s
ORDER BY random();
CREATE TABLE cat_clus AS SELECT * FROM cat_rand ORDER BY cell;
CREATE INDEX ON cat_rand (cell);
CREATE INDEX ON cat_clus (cell);
ANALYZE cat_rand;
ANALYZE cat_clus;
CREATE TABLE probe_1m AS
SELECT row_number() OVER () AS pid,
       ra + (random()-0.5)*1.6e-4/greatest(cos(radians(dec)),0.01) AS ra,
       dec + (random()-0.5)*1.6e-4 AS dec
FROM (SELECT ra, dec FROM cat_rand WHERE id % 5 = 0) s;
CREATE TABLE probe_200k AS SELECT * FROM probe_1m WHERE pid % 5 = 0;
ANALYZE probe_1m;
ANALYZE probe_200k;
