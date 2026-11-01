\echo Use "ALTER EXTENSION skycell UPDATE TO '0.38'" to load this file. \quit

-- 0.38 adds composite regions: a skyregion may be the union, intersection or
-- difference of others, or the sky less one, with REGION() reading their STC-S.
-- Stored regions keep their format; the new kinds only add to it.

-- ------------------------------------------------------------------
-- composite regions (0.38): STC-S Union, Intersection, Difference, Not
--
-- A skyregion may combine others, nested: "UNION ICRS (CIRCLE 10 20 1
-- POLYGON ...)", "INTERSECTION ICRS (...)", "DIFFERENCE ICRS (a b)" (a less
-- b) and "NOT ICRS (a)" (the sky less a).  A position in one is answered
-- exactly, and through the index: the planner covers the composite as it
-- covers a polygon.  Containment and overlap between regions, AREA (but for
-- a NOT of a cone or polygon) and CENTROID are refused for a composite.
-- ------------------------------------------------------------------

-- ADQL REGION: a region from its STC-S
CREATE FUNCTION region(stcs text) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_region_stcs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION region(text) IS
  'ADQL REGION: a region from its STC-S, CIRCLE, POLYGON, UNION, INTERSECTION, DIFFERENCE or NOT';

CREATE FUNCTION skycell_union(VARIADIC regions skyregion[]) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_union(skyregion[]) IS 'the union of two regions or more (STC-S Union)';

CREATE FUNCTION skycell_intersection(VARIADIC regions skyregion[]) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_intersection(skyregion[]) IS 'the intersection of two regions or more (STC-S Intersection)';

CREATE FUNCTION skycell_difference(a skyregion, b skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_difference(skyregion, skyregion) IS 'a less b (STC-S Difference)';

CREATE FUNCTION skycell_not(r skyregion) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_not(skyregion) IS 'the sky less r (STC-S Not)';
