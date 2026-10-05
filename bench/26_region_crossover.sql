-- A radius-stratified skyregion corpus and probe set -- small (1-3 deg),
-- medium (20-40), large (60-80), huge (85-89 deg radius circles), 3,000
-- rows/band, uniform-on-sphere centres -- for measuring the crossover
-- between skyregion_gist_ops (the default multi-cap opclass) and
-- skyregion_box_gist_ops (the experimental box opclass) when both are
-- registered on the same column at once.
--
-- Mirrors the band structure of GIST_REGION_DESIGN.md's "Round forty-two"
-- own large-radius addendum (which found the crossover: box wins small
-- footprints, the multi-cap key wins back from roughly 20-40 degrees on)
-- deliberately smaller than that round's own corpus (3,000 rows/band here
-- against its 50,000-row fpr) to keep this script fast to (re)run; the
-- `rc_corpus_area` expression index below is what "Round forty-eight"/
-- "Round forty-nine" added on top of that round's own setup, to let the
-- region-region &&/@>/<@ operators' selectivity actually reflect this
-- corpus's own mixed scales instead of assuming every stored row is
-- point-like.
--
-- Self-contained: does not depend on `src` or any other script in this
-- directory, only on the skycell extension itself.
\set ON_ERROR_STOP 1
SELECT setseed(0.61);

DROP TABLE IF EXISTS rc_corpus, rc_probe CASCADE;

CREATE TABLE rc_corpus AS
WITH bands(band, rmin, rmax) AS (
  VALUES ('small', 1.0, 3.0), ('medium', 20.0, 40.0), ('large', 60.0, 80.0), ('huge', 85.0, 89.0)
)
SELECT row_number() OVER () AS id, band,
       circle('ICRS', 360*random(), degrees(asin(2*random()-1)), rmin + (rmax-rmin)*random()) AS region
FROM bands CROSS JOIN LATERAL generate_series(1,3000) g;

-- 60 fresh, independently-sampled circles per band, same distribution as
-- that band's own corpus rows (not derived from them), so matches aren't
-- vacuously empty or artificially tied to specific corpus rows.
CREATE TABLE rc_probe AS
WITH bands(band, rmin, rmax) AS (
  VALUES ('small', 1.0, 3.0), ('medium', 20.0, 40.0), ('large', 60.0, 80.0), ('huge', 85.0, 89.0)
)
SELECT row_number() OVER () AS id, band,
       circle('ICRS', 360*random(), degrees(asin(2*random()-1)), rmin + (rmax-rmin)*random()) AS region
FROM bands CROSS JOIN LATERAL generate_series(1,60) g;

CREATE INDEX rc_corpus_multicap ON rc_corpus USING gist (region);
CREATE INDEX rc_corpus_box ON rc_corpus USING gist (region skyregion_box_gist_ops);
CREATE INDEX rc_corpus_area ON rc_corpus (area(region));
ANALYZE rc_corpus;
ANALYZE rc_probe;

-- Correctness, not just plan shape, before trusting any buffer count:
-- brute-force intersects() against the indexed && over the full cross
-- product (every probe against every corpus row, the exact shape the
-- selectivity formulas in ext/src/adql.c were measured against).
SELECT p.band,
       count(*) FILTER (WHERE intersects(p.region, c.region) = 1) AS brute,
       count(*) FILTER (WHERE p.region && c.region) AS indexed
FROM rc_probe p CROSS JOIN rc_corpus c
GROUP BY p.band ORDER BY p.band;
