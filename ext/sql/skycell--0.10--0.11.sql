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

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL: indexes region-region && and @>, and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @> reuses &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, and bench/24_region_contains_region.sql before relying on this.';
