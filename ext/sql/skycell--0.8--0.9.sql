
-- ------------------------------------------------------------------
-- Non-constant polygon cross-matches: the polygon analogue of
-- skycell_cone_bound / skycell_join / skycell_radial_query, so
-- CONTAINS(POINT, POLYGON)/INTERSECTS(POINT, POLYGON) over a per-row
-- polygon (not a compile-time constant) reaches an index the same way a
-- non-constant CIRCLE cross-match already does, instead of falling back to
-- a sequential scan.
-- ------------------------------------------------------------------

-- run-time range slot i (lo = even, hi = odd) for non-constant polygons
-- (joins): the polygon analogue of skycell_cone_bound, keyed on the array's
-- own bytes rather than three scalars.
CREATE FUNCTION skycell_poly_bound(poly float8[], i int, nslots int, ntotal float8,
                                   hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- The polygon analogue of skycell_join/skycell_radial_query, for a
-- cross-match whose polygon comes from another table's row (a q3c_poly_query
-- has no such counterpart -- Q3C has no region type -- so this has no
-- existing spelling to match; ra/dec/poly is skycell_poly's own order,
-- without the leading cell argument).
CREATE FUNCTION skycell_poly_join(ra float8, "dec" float8, poly float8[]) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_poly'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;
