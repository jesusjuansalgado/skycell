\echo Use "ALTER EXTENSION skycell UPDATE TO '0.35'" to load this file. \quit

-- Nothing in the catalog changes. 0.35 is the library's: polygons may be
-- non-convex (any simple polygon whose vertices lie within 90 degrees of
-- their mean), and CENTROID is a polygon's centre of mass, no longer the
-- mean of its vertices. The version moves so that a database can tell,
-- from pg_extension.extversion, which of these it has.
