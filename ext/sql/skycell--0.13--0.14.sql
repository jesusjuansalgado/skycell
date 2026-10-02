\echo Use "ALTER EXTENSION skycell UPDATE TO '0.14'" to load this file. \quit

-- Restriction selectivity for the raw, un-rewritten <@(skypos,skyregion)
-- and its commutator @>(skyregion,skypos) -- the ones a GiST or SP-GiST
-- index on skypos indexes directly (skypos_spgist_ops, the experimental
-- skypos_cap_gist_ops below), with no B-tree rewrite in the picture to
-- hand off to skycell_in_region/skycell_region_sel_support. A function's
-- own SUPPORT clause cannot fix this selectivity for an operator-backed
-- function (PostgreSQL's own documented rule: the support function is
-- never consulted for selectivity when that function backs an operator),
-- so this is the classic oprrest-shaped mechanism instead, replacing
-- contsel's flat, radius-blind default with the same area(region)/4pi
-- "uniform sky" estimate skycell_region_sel_support already uses for the
-- post-rewrite exact test. See GIST_REGION_DESIGN.md's "Round forty-three"
-- for the measured effect (a real, structurally-explained bug: both this
-- operator and skycell's own SP-GiST opclass were costed from a flat
-- ~0.1%-of-table estimate regardless of actual query radius, which skews
-- the planner away from a plain Index Scan toward an unneeded Bitmap Scan
-- at small radii).
CREATE FUNCTION skycell_pos_region_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION skycell_region_pos_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;

ALTER OPERATOR <@ (skypos, skyregion) SET (RESTRICT = skycell_pos_region_sel);
ALTER OPERATOR @> (skyregion, skypos) SET (RESTRICT = skycell_region_pos_sel);


-- ------------------------------------------------------------------
-- A non-default GiST opclass for skypos: a single spherical-cap key per
-- entry, competing directly with pgSphere's native spoint GiST (a
-- float-precision axis-aligned 3D box, confirmed via pageinspect) rather
-- than with skypos_spgist_ops or the B-tree rewrite path.
--
-- Leaves store the exact point as a zero-radius cap -- no precision lost,
-- unlike pgSphere's own leaf key, which is a lossy box even for a single
-- point -- so leaf-level consistent() is exact (recheck always false).
-- Internal nodes store a bounding cap, pruned by a trig-free overlap test
-- against the query region's own bounding cap; picksplit minimises cap
-- overlap via the same R*-tree sweep skyregion_gist_ops's own picksplit
-- uses. Structural motivation: for an isotropic point cluster (a globular
-- cluster, a density peak, a HEALPix-sorted run of nearby catalogue
-- sources -- the common case in a real catalogue), a bounding cap is
-- tighter than pgSphere's bounding box, the mirror image of why a box
-- beats skyregion_gist_ops's multi-cap key for *regions*.
--
-- Not DEFAULT: measured against pgSphere's native GiST, this opclass
-- loses on both buffers and wall-clock below roughly 30 arcminutes (its
-- smaller, lossy-anyway box key wins on fanout when few candidates matter
-- regardless of pruning precision) and wins on buffers from there on
-- (2.2x fewer at 1 degree), though pgSphere still wins the median
-- wall-clock comparison at every radius tested. It is, however, a strict
-- improvement over skypos_spgist_ops at every radius measured. Select it
-- explicitly: `CREATE INDEX ON t USING gist (pos skypos_cap_gist_ops)`.
--
-- EXPERIMENTAL: correctness-verified (0 mismatches, 210 brute-force
-- probes, 1 arcsec to 3 degrees, 10M rows) but with far less scrutiny
-- than skyregion_gist_ops's own forty-plus rounds of hardening. See
-- ext/src/gist_point_cap.c and GIST_REGION_DESIGN.md's "Round forty-three"
-- for the full numbers, including two tried-and-reverted variants (a
-- float-precision key that traded the exact leaf test for recheck=true
-- and lost 3-5x in wall-clock; a density-aware selectivity refinement
-- that regressed on density-clustered queries) before relying on this
-- for anything beyond experimentation.
-- ------------------------------------------------------------------

CREATE FUNCTION skypos_cap_gist_compress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skypos_cap_gist_decompress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skypos_cap_gist_union(internal, internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skypos_cap_gist_penalty(internal, internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skypos_cap_gist_picksplit(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skypos_cap_gist_same(bytea, bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skypos_cap_gist_consistent(internal, internal, int4, oid, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS skypos_cap_gist_ops
    FOR TYPE skypos USING gist AS
    OPERATOR 1 <@ (skypos, skyregion),
    FUNCTION 1 skypos_cap_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skypos_cap_gist_union(internal, internal),
    FUNCTION 3 skypos_cap_gist_compress(internal),
    FUNCTION 4 skypos_cap_gist_decompress(internal),
    FUNCTION 5 skypos_cap_gist_penalty(internal, internal, internal),
    FUNCTION 6 skypos_cap_gist_picksplit(internal, internal),
    FUNCTION 7 skypos_cap_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skypos_cap_gist_ops USING gist IS
  'EXPERIMENTAL, non-default: a single spherical-cap key per entry for skypos, competing with pgSphere''s native spoint GiST. Beats skypos_spgist_ops at every radius tested; beats pgSphere on buffers from ~30 arcmin on, loses below that and on median wall-clock throughout. See ext/src/gist_point_cap.c and GIST_REGION_DESIGN.md''s "Round forty-three".';
