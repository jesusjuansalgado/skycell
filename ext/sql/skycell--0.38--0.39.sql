\echo Use "ALTER EXTENSION skycell UPDATE TO '0.39'" to load this file. \quit

-- ------------------------------------------------------------------
-- 0.39: a constant composite region through the custom scan
--
-- A composite (UNION, INTERSECTION, DIFFERENCE, NOT) written as a top-level
-- term, CONTAINS(POINT, REGION('...')) = 1 or pos <@ region, is planned as
-- skycell_region_scan(cell, ra, dec, region): the custom scan reads its
-- covering's ranges in cell order, as for a cone, instead of an OR of range
-- conditions planned as a BitmapOr.  On its own the function is the exact
-- test.  Anywhere else (under an OR, in a target list) the composite keeps
-- the range rewrite.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_region_scan_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION skycell_region_scan(cell int8, ra float8, "dec" float8, r skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_scan_support;
COMMENT ON FUNCTION skycell_region_scan(int8, float8, float8, skyregion) IS
  'Planner-internal: a position (ra, dec, with its index cell) in a constant composite region, answered by the custom scan. Write CONTAINS(POINT(...), REGION(...)) = 1 or pos <@ region instead.';
