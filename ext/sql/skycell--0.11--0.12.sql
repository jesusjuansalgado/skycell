\echo Use "ALTER EXTENSION skycell UPDATE TO '0.12'" to load this file. \quit

-- ------------------------------------------------------------------
-- A real index on skypos itself: an SP-GiST opclass over <@(skypos,
-- skyregion) -- "which of these catalogue points are inside this region",
-- cone search and cross-match against a large point catalogue, currently
-- answered by a planner rewrite into OR-ed B-tree ranges over a separately
-- stored cell id (skycell.c/adql.c's SupportRequestSimplify machinery), not
-- a real index descent -- exactly the direction paper/response-to-referee-2
-- .md and bench/tap-ab/RESULTS.md already named as future work ("an SP-GiST
-- opclass over the HEALPix cell hierarchy... present one index qual, refine
-- during the index descent").
--
-- This is the mirror image of skyregion_gist_ops above: that one indexes
-- the *region* column (many candidate regions, one point); this indexes the
-- *point* column (one query region, many candidate points) -- skycell's
-- actual headline workload.
--
-- The tree IS the HEALPix NESTED pixel hierarchy: the root splits into up to
-- 12 nodes (order 0's 12 base pixels), every level below into up to 4 (one
-- more order each), no prefix compression (see ext/src/spgist_region.c's
-- file header for why that's a deliberate, revisitable simplification, not
-- an oversight). Pruning reuses cover.c's sc_region_classify unchanged;
-- leaf-level acceptance reuses the exact sc_region_contains test
-- skycell_pos_in_region() already uses. Exact, not lossy: recheck is always
-- false, unlike skyregion_gist_ops's multi-cap key.
--
-- EXPERIMENTAL, like skyregion_gist_ops before it: see
-- ext/src/spgist_region.c and GIST_REGION_DESIGN.md for the numbers this
-- was benchmarked against (pgSphere's native GiST, Q3C, skycell's own
-- B-tree-rewrite path, and PostGIS geography's GiST/SP-GiST) before relying
-- on it for anything beyond experimentation.
-- ------------------------------------------------------------------

CREATE FUNCTION spg_healpix_config(internal, internal) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION spg_healpix_choose(internal, internal) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION spg_healpix_picksplit(internal, internal) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION spg_healpix_inner_consistent(internal, internal) RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION spg_healpix_leaf_consistent(internal, internal) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS skypos_spgist_ops
    DEFAULT FOR TYPE skypos USING spgist AS
    OPERATOR 1 <@ (skypos, skyregion),
    FUNCTION 1 spg_healpix_config(internal, internal),
    FUNCTION 2 spg_healpix_choose(internal, internal),
    FUNCTION 3 spg_healpix_picksplit(internal, internal),
    FUNCTION 4 spg_healpix_inner_consistent(internal, internal),
    FUNCTION 5 spg_healpix_leaf_consistent(internal, internal);

COMMENT ON OPERATOR CLASS skypos_spgist_ops USING spgist IS
  'EXPERIMENTAL: indexes skypos <@ skyregion (cone search / cross-match against a point catalogue) as a real SP-GiST descent of the HEALPix NESTED hierarchy, exact (recheck always false). See ext/src/spgist_region.c and GIST_REGION_DESIGN.md.';
