\echo Use "ALTER EXTENSION skycell UPDATE TO '0.11'" to load this file. \quit

-- ------------------------------------------------------------------
-- A third GiST strategy for skyregion_gist_ops: @>(skyregion,skyregion),
-- "does this row's region wholly contain that region" -- the case
-- README.md and the paper both used to describe as "not yet indexed,
-- evaluated by sequential scan". skycell_region_covers (the exact test
-- backing @>) and the operator itself already existed; only the index
-- strategy is new here, added to the family skyregion_gist_ops already
-- created for (region, region) below.
--
-- No new support functions: this reuses skyregion_gist_consistent, whose
-- shared library already knows strategy 3 (see ext/src/gist_region.c's
-- "round six"). It prunes on the same test OVERLAP uses -- a row can only
-- be pruned when its cover provably doesn't even touch the query region --
-- which is sound (containment implies overlap) though not as tight as a
-- dedicated containment test would be; recheck (skycell_region_covers)
-- decides the real predicate. See bench/24_region_contains_region.sql.
-- ------------------------------------------------------------------

ALTER OPERATOR FAMILY skyregion_gist_ops USING gist
    ADD OPERATOR 3 @> (skyregion, skyregion);

-- ------------------------------------------------------------------
-- A fourth strategy: <@(skyregion,skyregion), CONTAINS_REGION's mirror --
-- "is this row's region wholly contained *within* that region" -- the
-- direction the third strategy's own comment flagged as unindexable, since
-- @>(skyregion,skyregion) had no COMMUTATOR and no <@ counterpart existed
-- at all. Needs no new pruning logic (see gist_region.c's "round seven"):
-- the same overlap-of-covers necessary condition CONTAINS_REGION already
-- established holds regardless of which side is "the container", so this
-- reuses CONTAINS_REGION's whole consistent() case; recheck falls back to
-- the new exact function, skycell_region_covered_by, for whichever queries
-- actually use <@.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_region_covered_by(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- @>(skyregion,skyregion) has existed, uncommutated, since skycell 0.1; this
-- CREATE OPERATOR's own COMMUTATOR clause backfills @>'s missing commutator
-- link automatically (PostgreSQL's documented forward-reference fixup for a
-- not-yet-linked operator), so both directions resolve without a separate
-- ALTER OPERATOR statement.
CREATE OPERATOR <@ (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_covered_by, COMMUTATOR = @>,
    RESTRICT = contsel, JOIN = contjoinsel
);

ALTER OPERATOR FAMILY skyregion_gist_ops USING gist
    ADD OPERATOR 4 <@ (skyregion, skyregion);

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, and bench/24_region_contains_region.sql before relying on this.';
