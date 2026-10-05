\echo Use "ALTER EXTENSION skycell UPDATE TO '0.13'" to load this file. \quit

-- ------------------------------------------------------------------
-- A second, non-default GiST opclass for skyregion: a plain axis-aligned
-- 3D box key (six doubles, no spherical caps), modelled directly on
-- pgSphere's own Box3D rather than skyregion_gist_ops's multi-cap key.
--
-- WHY A SECOND OPCLASS RATHER THAN CHANGING THE FIRST: benchmarking found a
-- real, radius-dependent tradeoff, not a strict improvement. Against
-- skyregion_gist_ops on the same corpus, this opclass wins && and both
-- region-region/region-point containment strategies decisively at
-- realistic catalogue-footprint radii (arcseconds to a few degrees) --
-- smaller index, fewer buffers, faster wall-clock, and it closes the gap
-- to pgSphere's native GiST that skyregion_gist_ops's own containment
-- strategies don't close. But the advantage reverses from roughly 20
-- degrees on: a circle's multi-cap key is the circle itself, exact at any
-- radius, while this key's circle-in-a-square slack grows and stays
-- costly through the medium-to-large radius range. Neither opclass
-- dominates the other, so both ship: pick the box opclass explicitly for
-- a column you know holds small, catalogue-scale footprints; leave the
-- default (skyregion_gist_ops) for anything wide-area or mixed-scale.
--
-- Not marked DEFAULT: skyregion_gist_ops already holds that for (skyregion,
-- gist). Select this one explicitly:
--   CREATE INDEX ON t USING gist (region skyregion_box_gist_ops);
--
-- EXPERIMENTAL, like skyregion_gist_ops and skypos_spgist_ops before it --
-- correctness-verified (exact brute-force match at every radius and
-- declination band tested, including near-pole and near-hemisphere caps)
-- but with far less scrutiny than skyregion_gist_ops's own forty-plus
-- rounds of hardening. See ext/src/gist_region_box.c and
-- GIST_REGION_DESIGN.md's "Round forty-two" (and its two addenda, on pole
-- proximity and on large radii) for the full numbers before relying on
-- this for anything beyond experimentation.
-- ------------------------------------------------------------------

CREATE FUNCTION skyregion_box_gist_consistent(internal, internal, int4, oid, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box_gist_union(internal, internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box_gist_compress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box_gist_decompress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box_gist_penalty(internal, internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box_gist_picksplit(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_box_gist_same(bytea, bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS skyregion_box_gist_ops
    FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    OPERATOR 2 @> (skyregion, skypos),
    OPERATOR 3 @> (skyregion, skyregion),
    OPERATOR 4 <@ (skyregion, skyregion),
    FUNCTION 1 skyregion_box_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_box_gist_union(internal, internal),
    FUNCTION 3 skyregion_box_gist_compress(internal),
    FUNCTION 4 skyregion_box_gist_decompress(internal),
    FUNCTION 5 skyregion_box_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_box_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_box_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skyregion_box_gist_ops USING gist IS
  'EXPERIMENTAL, non-default: a plain axis-aligned 3D box key for skyregion (no spherical caps), for && and region-region/region-point containment. Opt into this explicitly for small, catalogue-scale footprints (arcsec-few degrees), where it beats skyregion_gist_ops on size and speed; skyregion_gist_ops wins back from roughly 20 degrees radius on. See ext/src/gist_region_box.c and GIST_REGION_DESIGN.md''s "Round forty-two".';
