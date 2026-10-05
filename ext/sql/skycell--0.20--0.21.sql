\echo Use "ALTER EXTENSION skycell UPDATE TO '0.21'" to load this file. \quit

-- Promotes skyregion_box_gist_ops to DEFAULT FOR TYPE skyregion USING gist,
-- demoting skyregion_gist_ops (the multi-cap key, default since it was
-- introduced). GIST_REGION_DESIGN.md's "Round fifty-three" has the full
-- story: re-measured directly rather than trusting the ~20-degree
-- crossover 0.9-0.18 era benchmarks found, because 0.19/0.20's per-row
-- exact-test fix changed the balance between the two keys' index-level
-- candidate pruning and their recheck cost. Found the crossover has
-- moved down to roughly 0.3-1 degree -- below the realistic catalogue-
-- footprint range the box opclass was already winning at, and well
-- below the ~20-degree figure the old (pre-0.21) comments on both
-- opclasses quoted. The box opclass also wins decisively on fpr's own
-- mixed circle/polygon corpus (re-confirmed fresh under current code,
-- ~3.4x on buffers and wall-clock, both the circle-only and polygon-only
-- subsets), and has full feature parity with the multi-cap key (all four
-- strategies, && / @>(region,point) / @>(region,region) / <@(region,
-- region)) -- there is no capability lost by swapping which one is
-- unqualified-default. skyregion_gist_ops remains available, explicitly,
-- for wide-area or very large regions, where it still wins.
--
-- PostgreSQL has no ALTER OPERATOR CLASS ... SET DEFAULT, and only one
-- default operator class is allowed per (type, access method) pair, so
-- this has to DROP both opclasses and recreate them with the DEFAULT
-- keyword moved -- the operator and function lists are unchanged from
-- 0.9 (skyregion_gist_ops) and 0.17 (skyregion_box_gist_ops) respectively,
-- only which one carries DEFAULT and the two COMMENT ON strings change.
--
-- CASCADE on both DROPs: any index this database already built with a
-- plain, opclass-unqualified `CREATE INDEX ... USING gist (region)` is
-- bound to skyregion_gist_ops by name in the catalog, and any index that
-- named skyregion_box_gist_ops explicitly is bound to that -- either way
-- it gets dropped along with its opclass. There is no way to re-point an
-- existing GiST index at a different opclass in place; whoever has one
-- needs to CREATE INDEX it again after this upgrade (unqualified, to
-- pick up the new default, or naming an opclass explicitly to keep a
-- specific behaviour). This extension's own install scripts never
-- create such an index themselves, so a fresh CREATE EXTENSION / ALTER
-- EXTENSION UPDATE has nothing of its own to rebuild -- only a
-- downstream user's own indexes are affected.

-- DROP OPERATOR FAMILY, not just DROP OPERATOR CLASS: both opclasses were
-- originally created without an explicit FAMILY clause, which implicitly
-- creates a same-named family holding the real OPERATOR/FUNCTION catalog
-- rows (pg_amop/pg_amproc key on the family, not the class). Dropping only
-- the class leaves that family and its rows behind, so the CREATE below
-- would hit a duplicate-key conflict re-registering the same strategies.
-- Dropping the family cascades to the class (and, with CASCADE, to any
-- dependent index) in one step.
DROP OPERATOR FAMILY skyregion_gist_ops USING gist CASCADE;
DROP OPERATOR FAMILY skyregion_box_gist_ops USING gist CASCADE;

CREATE OPERATOR CLASS skyregion_box_gist_ops
    DEFAULT FOR TYPE skyregion USING gist AS
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
  'EXPERIMENTAL, DEFAULT as of 0.21: a plain axis-aligned 3D box key for skyregion (no spherical caps), for && and region-region/region-point containment. Wins on size and speed at realistic catalogue-footprint radii (arcsec to roughly a degree, circle or polygon); skyregion_gist_ops wins back for sufficiently large regions -- the crossover was re-measured at roughly 0.3-1 degree (down from an earlier, pre-0.19 figure of ~20 degrees), see GIST_REGION_DESIGN.md''s "Round fifty-three". Select skyregion_gist_ops explicitly for a column you know holds wide-area or very large regions. See ext/src/gist_region_box.c and GIST_REGION_DESIGN.md''s "Round forty-two" and "Round fifty-three".';

CREATE OPERATOR CLASS skyregion_gist_ops
    FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    OPERATOR 2 @> (skyregion, skypos),
    OPERATOR 3 @> (skyregion, skyregion),
    OPERATOR 4 <@ (skyregion, skyregion),
    FUNCTION 1 skyregion_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_gist_union(internal, internal),
    FUNCTION 3 skyregion_gist_compress(internal),
    FUNCTION 4 skyregion_gist_decompress(internal),
    FUNCTION 5 skyregion_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL, non-default as of 0.21: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). Wins over skyregion_box_gist_ops (the new default) for wide-area/very-large regions; select it explicitly for those. See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, bench/24_region_contains_region.sql, and GIST_REGION_DESIGN.md''s "Round fifty-three" before relying on this.';
