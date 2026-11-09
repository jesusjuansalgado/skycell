-- skycell 0.46 -> 0.47: PostgreSQL's point as a sky position

-- With skycell in public, ADQL's bare POINT(ra, dec) is pg_catalog's
-- point(float8, float8), and CONTAINS(POINT(ra, dec), CIRCLE(...)) found no
-- function.  An implicit cast reads that point as (ra, dec) in degrees; its
-- support function plans the cast of point(ra, dec) as skycell's own
-- point(ra, dec), so the search stays indexed.
CREATE FUNCTION skycell_pos_from_point_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_pos_from_point(p point) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_pos_from_point_support;
COMMENT ON FUNCTION skycell_pos_from_point(point) IS
  'PostgreSQL''s point as a sky position: x the right ascension, y the declination, degrees.';
CREATE CAST (point AS skypos) WITH FUNCTION skycell_pos_from_point(point) AS IMPLICIT;
