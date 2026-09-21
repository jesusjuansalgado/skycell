
-- ------------------------------------------------------------------
-- Q3C-shaped spellings
-- ------------------------------------------------------------------
-- Same predicate as skycell_cone, without the explicit index expression:
-- the planner support function synthesises skycell_ang2cell(ra, dec) from the
-- first two arguments, so migrating from Q3C is a prefix replacement.
--   q3c_join(a.ra, a.dec, b.ra, b.dec, r)  ->  skycell_join(...)
--   q3c_radial_query(ra, dec, ra0, dec0, r) -> skycell_radial_query(...)
-- Both need an index on skycell_ang2cell(ra, dec) to be rewritten to ranges,
-- exactly as q3c needs one on q3c_ang2ipix(ra, dec); without it the plan is a
-- correct sequential scan.

CREATE FUNCTION skycell_join(ra1 float8, dec1 float8, ra2 float8, dec2 float8,
                             radius float8) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_cone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

CREATE FUNCTION skycell_radial_query(ra float8, "dec" float8, ra0 float8, dec0 float8,
                                     radius float8) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_cone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;
