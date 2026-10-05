-- Fresh, never-before-queried probe circles for a genuine cold-cache
-- measurement of the region-region crossover (26_region_crossover.sql) --
-- a separate table from that script's own `rc_probe`, not a replacement
-- of it: `rc_probe`'s rows have already been queried repeatedly for the
-- warm-cache crossover measurements (rounds forty-seven through
-- forty-nine), so reusing them here would not be cold at all. Same
-- four-band structure and per-band count as `rc_probe`, reseeded.
--
-- Depends on `rc_corpus` already existing (26_region_crossover.sql) --
-- this does not touch it, only adds a fresh probe table alongside it.
\set ON_ERROR_STOP 1
SELECT setseed(0.83);

DROP TABLE IF EXISTS rc_probe_cold;
CREATE TABLE rc_probe_cold AS
WITH bands(band, rmin, rmax) AS (
  VALUES ('small', 1.0, 3.0), ('medium', 20.0, 40.0), ('large', 60.0, 80.0), ('huge', 85.0, 89.0)
)
SELECT row_number() OVER () AS id, band,
       circle('ICRS', 360*random(), degrees(asin(2*random()-1)), rmin + (rmax-rmin)*random()) AS region
FROM bands CROSS JOIN LATERAL generate_series(1,60) g;

SELECT band, count(*) FROM rc_probe_cold GROUP BY band ORDER BY band;
