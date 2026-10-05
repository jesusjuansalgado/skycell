\echo Use "ALTER EXTENSION skycell UPDATE TO '0.24'" to load this file. \quit

-- Resolves "Round fifty-five"'s open question (why "Round forty-two"'s
-- "box beats pgSphere" claim doesn't reproduce), chased down in "Round
-- fifty-six": that round's own disposable test database, splitcost_
-- test, still exists on this machine and still has its original data.
-- Running today's (unchanged since round forty-two) box opclass code
-- against that data reproduced the original claim exactly (box faster
-- by a similar margin) -- so the discrepancy was real, not a stale
-- claim. Comparing EXPLAIN plans for the identical query found pgSphere's
-- own GiST index there picking a plain Index Scan paying for many
-- redundant false-positive rechecks, instead of the Bitmap Heap Scan a
-- freshly built pgSphere index picks for the same data; a single
-- REINDEX on that same index, in that same database, with that round's
-- own original data, dropped its median time 17x and flipped the
-- comparison back to pgSphere winning -- matching every other
-- measurement this session has made. REINDEXing skycell's own box
-- index in the same database changed nothing, confirming the
-- degradation was specific to that one pgSphere index instance, not a
-- property of either opclass's code.
--
-- Comment text only, both opclasses' COMMENT ON OPERATOR CLASS and the
-- preceding block above skyregion_box_gist_ops's CREATE OPERATOR CLASS
-- in skycell--0.24.sql -- stating the resolved explanation instead of
-- leaving "Round fifty-five"'s retraction as an open mystery. No
-- opclass, operator, or function change; the DEFAULT decision (0.21)
-- and the retraction itself (0.23) both stand unchanged. See GIST_
-- REGION_DESIGN.md's "Round fifty-six" for the full investigation.

COMMENT ON OPERATOR CLASS skyregion_box_gist_ops USING gist IS
  'EXPERIMENTAL, DEFAULT as of 0.21: a plain axis-aligned 3D box key for skyregion (no spherical caps), for && and region-region/region-point containment. Wins on size and speed at realistic catalogue-footprint radii, for a column holding one roughly uniform scale of region, up to roughly 5-8 degrees (re-measured in GIST_REGION_DESIGN.md''s "Round fifty-four", correcting "Round fifty-three"''s own first, too-low ~0.3-1-degree estimate); skyregion_gist_ops wins back above that, and also for a column mixing very different region scales in one index even below it. Select skyregion_gist_ops explicitly for either case. Does NOT beat pgSphere''s native GiST -- an earlier claim here to the contrary ("Round forty-two") turned out to have been measured against a degraded pgSphere index in a disposable test database that was never REINDEXed; "Round fifty-six" found and fixed that same index, which then beat this opclass by the same margin seen everywhere else. This opclass is only known to compete with skyregion_gist_ops, not with pgSphere. See ext/src/gist_region_box.c and GIST_REGION_DESIGN.md''s "Round forty-two", "Round fifty-three", "Round fifty-four", "Round fifty-five", and "Round fifty-six".';

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL, non-default as of 0.21: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). Wins over skyregion_box_gist_ops (the new default) for a uniform-scale column above roughly 5-8 degrees radius, and for a column mixing very different region scales in one index even below that -- select it explicitly for either case; neither opclass is known to beat pgSphere''s native GiST on this corpus. See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, bench/24_region_contains_region.sql, and GIST_REGION_DESIGN.md''s "Round fifty-four" (correcting "Round fifty-three"''s first, too-low crossover estimate), "Round fifty-five", and "Round fifty-six" before relying on this.';
