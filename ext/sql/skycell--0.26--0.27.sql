\echo Use "ALTER EXTENSION skycell UPDATE TO '0.27'" to load this file. \quit

-- distance(p1, p2) < r, ADQL's other spelling of a cone or a cross-match,
-- is planned as the cone it stands for (ext/src/adql.c,
-- skycell_index_distances): the planner adds `p1 <@ circle(p2, r')` next to
-- the comparison, which keeps deciding. That happens in a planner hook, so
-- the library has to be loaded before the query's tables are looked at,
-- and a comparison of a float8 calls nothing of skycell's while it is
-- planned. This support function does nothing but get called -- loading
-- the library -- when the planner simplifies the distance() call.
CREATE FUNCTION skycell_distance_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
ALTER FUNCTION distance(skypos, skypos) SUPPORT skycell_distance_support;
ALTER FUNCTION distance(float8, float8, float8, float8) SUPPORT skycell_distance_support;
ALTER FUNCTION skycell_dist(float8, float8, float8, float8) SUPPORT skycell_distance_support;
