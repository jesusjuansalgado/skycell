\echo Use "ALTER EXTENSION skycell UPDATE TO '0.26'" to load this file. \quit

-- Promotes skyregion_box4_gist_ops to DEFAULT FOR TYPE skyregion USING
-- gist, demoting skyregion_box_gist_ops (DEFAULT since 0.21). Rounds
-- fifty-eight through sixty built box4 (ext/src/gist_region_box4.c) as a
-- controlled clone of box8's own key, geometry, and consistent()/
-- picksplit() logic -- float4 in a fixed 24-byte type instead of float8
-- in a bytea, cloning pgSphere's own spherekey layout exactly (round
-- fifty-seven) -- and found it winning on every corpus tried: isolated
-- single-scale bands (fifty-eight), a mixed circle/polygon corpus
-- (fifty-nine), and all four strategies on the real 50,000-row fpr
-- corpus (sixty), against both box8 and pgSphere's own native GiST,
-- including the one strategy (@>(region,point)) where box8 itself loses
-- to pgSphere.
--
-- Promotion scrutiny ("Round sixty-one", matching the breadth that
-- earned box8 its own DEFAULT status across rounds forty-two/fifty-
-- three/fifty-four): pole proximity (clean, box4's advantage over box8
-- holds), large radii (no crossover to multicap found through 50
-- degrees in a carefully plan-verified sweep -- an open discrepancy
-- with round fifty-four's own 5-8 degree claim, flagged rather than
-- silently resolved, see the design doc), and a mixed-scale column
-- (small and huge regions sharing one index) -- the one regime that
-- does reproduce round fifty-four's finding: multicap still wins there,
-- for box4 same as box8. Net: box4 was never worse than box8 in any
-- regime tested, so this promotion carries no regression versus the
-- status quo, and is a strict improvement in the common case plus the
-- point-containment strategy box8 itself was weak on. See GIST_REGION_
-- DESIGN.md's "Round sixty-one" for the full numbers before relying on
-- the large-radius finding specifically.
--
-- Same DROP-and-recreate dance as 0.21's own box8 promotion (no ALTER
-- OPERATOR CLASS ... SET DEFAULT in PostgreSQL, and only one DEFAULT
-- opclass per type/access-method pair) -- operator and function lists
-- unchanged from 0.25, only which one carries DEFAULT and the two
-- COMMENT ON strings change. DROP OPERATOR FAMILY, not just DROP
-- OPERATOR CLASS, for the same reason 0.21 used it: both families hold
-- their real pg_amop/pg_amproc rows directly (neither opclass was
-- created with an explicit FAMILY clause), so dropping only the class
-- leaves the family and its rows behind, colliding with the re-create
-- below.
--
-- CASCADE: any index already built with a plain, opclass-unqualified
-- `CREATE INDEX ... USING gist (region)` is bound to whichever opclass
-- was DEFAULT when it was created, and gets dropped along with that
-- opclass's family. This extension's own install scripts never create
-- such an index themselves; a downstream user with one needs to
-- CREATE INDEX it again after this upgrade (unqualified, to pick up
-- box4, or naming skyregion_box_gist_ops explicitly to keep the old
-- behaviour) -- same caveat 0.21's own box8 promotion already carried.
DROP OPERATOR FAMILY skyregion_box_gist_ops USING gist CASCADE;
DROP OPERATOR FAMILY skyregion_box4_gist_ops USING gist CASCADE;

CREATE OPERATOR CLASS skyregion_box4_gist_ops
    DEFAULT FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    OPERATOR 2 @> (skyregion, skypos),
    OPERATOR 3 @> (skyregion, skyregion),
    OPERATOR 4 <@ (skyregion, skyregion),
    FUNCTION 1 skyregion_box4_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_box4_gist_union(internal, internal),
    FUNCTION 3 skyregion_box4_gist_compress(internal),
    FUNCTION 4 skyregion_box4_gist_decompress(internal),
    FUNCTION 5 skyregion_box4_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_box4_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_box4_gist_same(skyregion_box4, skyregion_box4, internal),
    STORAGE skyregion_box4;

COMMENT ON OPERATOR CLASS skyregion_box4_gist_ops USING gist IS
  'DEFAULT as of 0.26: an axis-aligned 3D box key for skyregion (no spherical caps) -- the same box8 geometry as skyregion_box_gist_ops, but keyed exactly like pgSphere''s own spherekey (six float4s, fixed 24-byte type, no varlena header, bounds rounded strictly outward on the float4 cast for soundness -- see ext/src/gist_region_box4.c). Beats skyregion_box_gist_ops and pgSphere''s native GiST on every corpus measured: isolated single-scale bands, a mixed circle/polygon corpus, and all four strategies (&&, @>(region,point), @>(region,region), <@(region,region)) on the real 50,000-row fpr corpus -- including @>(region,point), the one strategy box8 itself loses to pgSphere on. Same domain and caveats as box8 otherwise: skyregion_gist_ops (multicap) still wins for a column mixing very different region scales in one index (confirmed directly for box4, not just inherited from box8); the isolated-single-scale large-radius crossover box8 was known to have (round fifty-four, ~5-8 degrees) did not reproduce for either opclass in a fresh, plan-verified re-measurement through 50 degrees -- an open discrepancy with that round, not yet resolved, so treat any specific large-radius crossover claim for either opclass as unsettled rather than relying on round fifty-four''s number. See ext/src/gist_region_box4.c and GIST_REGION_DESIGN.md''s "Round fifty-seven" through "Round sixty-one".';

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
  'EXPERIMENTAL, non-default as of 0.26 (DEFAULT 0.21-0.25): the same axis-aligned 3D box key as skyregion_box4_gist_ops, but float8 in a bytea instead of float4 in a fixed-length type -- superseded by box4_gist_ops, which beat it on every corpus measured (rounds fifty-eight through sixty) with no regression found in any regime (round sixty-one''s promotion scrutiny). Kept available for comparison and as a fallback; select it explicitly: CREATE INDEX ... USING gist (col skyregion_box_gist_ops). See ext/src/gist_region_box.c and GIST_REGION_DESIGN.md''s "Round forty-two" through "Round sixty-one".';

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL, non-default as of 0.21: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). Wins over both box opclasses (skyregion_box_gist_ops and the new default skyregion_box4_gist_ops alike -- confirmed directly for box4, round sixty-one) for a column mixing very different region scales in one index -- select it explicitly for that case. Does NOT beat either box opclass on a uniform-scale column at any radius found so far: round fifty-four''s own claimed 5-8 degree crossover (where this opclass was said to win back on radius alone, not just scale-mixing) did not reproduce in round sixty-one''s fresh, plan-verified re-measurement through 50 degrees -- an open discrepancy, not resolved, so no specific uniform-scale crossover radius is currently a safe claim either way. Neither box opclass was known to beat pgSphere''s native GiST before round fifty-seven through sixty; box4_gist_ops now does, on every corpus tried, so this opclass is now behind on three fronts (both box opclasses and pgSphere) outside the one regime -- scale-mixing -- where it still clearly wins. See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, bench/24_region_contains_region.sql, and GIST_REGION_DESIGN.md''s "Round fifty-four", "Round fifty-five", "Round fifty-six", and "Round sixty-one" before relying on this.';
