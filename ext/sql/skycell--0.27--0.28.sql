\echo Use "ALTER EXTENSION skycell UPDATE TO '0.28'" to load this file. \quit

-- A distance comparison the planner has given a cone (0.27) is estimated
-- twice: once by the cone, with skycell's own estimate, and once more as an
-- inequality on an expression, which PostgreSQL guesses at 1/3. So once the
-- cone is the custom scan's, the comparison becomes
-- skycell_distance_below(d, r, strict) -- d < r, or d <= r -- the same test
-- with the float type's ordering, whose support function estimates it as
-- always true. Planner-internal: written by hand it would be estimated
-- as always true too.
CREATE FUNCTION skycell_distance_below_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_distance_below(d float8, r float8, strict bool) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_distance_below_support;
