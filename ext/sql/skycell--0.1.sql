\echo Use "CREATE EXTENSION skycell" to load this file. \quit

-- ------------------------------------------------------------------
-- keys and distances
-- ------------------------------------------------------------------

-- order-29 HEALPix NESTED cell of a position (the index key)
CREATE FUNCTION skycell_ang2cell(ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_ang2pix("order" int, ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- angular distance in degrees
CREATE FUNCTION skycell_dist(ra1 float8, dec1 float8, ra2 float8, dec2 float8) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- exact predicates (the "refine" step); last arg is a selectivity hint
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_exact_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION skycell_in_cone(ra float8, "dec" float8, ra0 float8, dec0 float8,
                                radius float8, sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_exact_support;

CREATE FUNCTION skycell_in_poly(ra float8, "dec" float8, poly float8[],
                                sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_exact_support;

-- ------------------------------------------------------------------
-- indexable predicates: rewritten by the planner support function into
-- B-tree range conditions on "cell" + the exact test
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- cell must be skycell_ang2cell(ra, dec)
CREATE FUNCTION skycell_cone(cell int8, ra float8, "dec" float8,
                             ra0 float8, dec0 float8, radius float8) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- poly = ARRAY[ra1, dec1, ra2, dec2, ...] (convex, degrees)
CREATE FUNCTION skycell_poly(cell int8, ra float8, "dec" float8, poly float8[]) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- run-time range slot i (lo = even, hi = odd) for non-constant cones (joins)
CREATE FUNCTION skycell_cone_bound(ra0 float8, dec0 float8, radius float8, i int,
                                   nslots int, ntotal float8, hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- explicit coverings (LATERAL joins, inspection)
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_cone_ranges(ra0 float8, dec0 float8, radius float8,
                                    tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                    OUT lo int8, OUT hi int8) RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE ROWS 3;

CREATE FUNCTION skycell_poly_ranges(poly float8[], tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                    OUT lo int8, OUT hi int8) RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE ROWS 8;

CREATE FUNCTION skycell_cover_info(ra0 float8, dec0 float8, radius float8,
                                   tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                   OUT nranges int, OUT steps int, OUT deepest int,
                                   OUT exp_rows float8, OUT area_ratio float8) RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- regions as multi-order coverages (IVOA MOC NUNIQ), for indexing stored
-- footprints in a B-tree: a point's candidate regions are those with a
-- covering cell equal to one of the point's 30 ancestors.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_ancestors(cell int8, min_order int DEFAULT 0, max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_order(nuniq int8) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_lo(nuniq int8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_hi(nuniq int8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_cone_moc(ra0 float8, dec0 float8, radius float8,
                                 max_cells int DEFAULT 8, max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_poly_moc(poly float8[], max_cells int DEFAULT 8,
                                 max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
