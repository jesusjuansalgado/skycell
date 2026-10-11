\echo Use "ALTER EXTENSION skycell UPDATE TO '0.29'" to load this file. \quit

-- ADQL's CONTAINS and INTERSECTS return an integer, so a query compares
-- them with 1: `contains(p, r) = 1`. Each runs exactly an operator's test,
-- so the planner now replaces that comparison with the operator itself
-- (ext/src/adql.c, adql_predicate_operator), which is indexed:
-- contains/intersects(skypos, skyregion) and contains(skyregion,
-- skyregion) become <@, intersects(skyregion, skyregion) becomes &&.
-- As for distance() in 0.27, the support function does nothing but load
-- the library while the call is planned, so the planner hook is in place
-- for a session's first query.
ALTER FUNCTION contains(skypos, skyregion) SUPPORT skycell_distance_support;
ALTER FUNCTION contains(skyregion, skyregion) SUPPORT skycell_distance_support;
ALTER FUNCTION intersects(skypos, skyregion) SUPPORT skycell_distance_support;
ALTER FUNCTION intersects(skyregion, skyregion) SUPPORT skycell_distance_support;
