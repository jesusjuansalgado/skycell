-- skycell 0.49 -> 0.50: the region tests are LEAKPROOF

-- A security-barrier view (or a view's security_barrier option) keeps a
-- condition out of the view unless every function in it that reads the
-- view's columns is LEAKPROOF: it says nothing of its arguments but its
-- result, raising no error that depends on them.  These do not: a position,
-- a region and the coordinates are tested as they are, and a stored region
-- that no longer decodes (only a circle of NaN radius stored before 0.56 can
-- be one) is empty rather than an error.  So a search through such a view
-- reaches the index of the table under it.
--
-- Not LEAKPROOF, by design: skycell_ang2cell() and ADQL's POINT(), which
-- reject impossible coordinates (an error that names the value), and the
-- functions taking a polygon as an array, which reject malformed arrays.
-- POINT() on a security-barrier view's columns is read as
-- skycell_point_or_null() (below).
ALTER FUNCTION coord1(skypos) LEAKPROOF;
ALTER FUNCTION coord2(skypos) LEAKPROOF;
ALTER FUNCTION distance(skypos, skypos) LEAKPROOF;
ALTER FUNCTION distance(float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_dist(float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_cell(skypos) LEAKPROOF;
ALTER FUNCTION skycell_in_cone(float8, float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_cone(int8, float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_join(float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_radial_query(float8, float8, float8, float8, float8) LEAKPROOF;
ALTER FUNCTION skycell_distance_below(float8, float8, bool) LEAKPROOF;
ALTER FUNCTION contains(skypos, skyregion) LEAKPROOF;
ALTER FUNCTION contains(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION intersects(skypos, skyregion) LEAKPROOF;
ALTER FUNCTION intersects(skyregion, skypos) LEAKPROOF;
ALTER FUNCTION intersects(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_pos_in_region(skypos, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_has_pos(skyregion, skypos) LEAKPROOF;
ALTER FUNCTION skycell_in_region(skypos, skyregion, float8) LEAKPROOF;
ALTER FUNCTION skycell_region_overlap(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_covers(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_covered_by(skyregion, skyregion) LEAKPROOF;
ALTER FUNCTION skycell_region_scan(int8, float8, float8, skyregion) LEAKPROOF;

-- ADQL's POINT(ra, dec) is not LEAKPROOF: it rejects impossible coordinates
-- with an error naming them.  On a security-barrier view's columns the
-- planner reads it as skycell_point_or_null(ra, dec), which returns NULL for
-- them instead -- a position no region holds, so the row is not returned, as
-- it is not when the index answers -- and so a search written in ADQL
-- reaches the index under the view.
CREATE FUNCTION skycell_point_or_null(ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
COMMENT ON FUNCTION skycell_point_or_null(float8, float8) IS
  'The position (ra, dec) in degrees, or NULL for coordinates no position has (a non-finite right ascension, a declination outside [-90, 90]).';

-- ADQL's CIRCLE() centred on a security-barrier view's columns (the reversed
-- cone, a point in a circle round each row) is read as this, likewise.
CREATE FUNCTION skycell_circle_or_null(ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE LEAKPROOF;
COMMENT ON FUNCTION skycell_circle_or_null(float8, float8, float8) IS
  'The circle of centre (ra, dec) and radius radius in degrees, or NULL where circle() would reject them.';
