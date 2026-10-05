\echo Use "ALTER EXTENSION skycell UPDATE TO '0.23'" to load this file. \quit

-- Retracts a claim both opclasses' COMMENT ON OPERATOR CLASS text (and
-- skyregion_box_gist_ops's preceding comment block in skycell--0.23.sql,
-- which only a fresh CREATE EXTENSION reads) carried since "Round forty-
-- two" and repeated through 0.21/0.22: that skyregion_box_gist_ops
-- "closes the gap to pgSphere's native GiST." Comment text only -- no
-- opclass, operator, or function changes, same as 0.21--0.22.sql.
--
-- Asked directly whether that claim still holds, and it does not.
-- GIST_REGION_DESIGN.md's "Round fifty-five" re-measured box against
-- pgSphere directly: several isolated single-scale corpora (0.05
-- through 1 degree) and the exact query shape "Round forty-two" itself
-- used (a batched nested-loop join, re-run against the same fpr corpus
-- that round measured). pgSphere won every comparison tried, by 1.3-7x
-- depending on scale -- the opposite of what was shipped.
--
-- Could not forensically explain the discrepancy: "Round forty-two"'s
-- own benchmark was explicitly ad hoc, registered against a disposable
-- table and never committed to this repository's own bench/ scripts,
-- so its exact query is not recoverable to re-run verbatim. Checked and
-- ruled out the explanations available: the batched-nested-loop
-- execution-shape artifact "Round fifty-four" found for the box-vs-
-- multicap comparison does not apply here (reproduced with that exact
-- shape and pgSphere still won); JIT is off in this environment and
-- irrelevant at this query cost regardless; pgSphere's own scircle
-- GiST opclass indexes cross-type operators (scircle && spoly and
-- back) natively, so the mixed circle/polygon corpus is not a query-
-- expressivity limitation either. Whatever the original number
-- reflected, it does not describe current, repeatable behaviour.
--
-- Does not reopen the DEFAULT decision (0.21): that was based on the
-- box-vs-multicap comparison, independently re-verified multiple times
-- this session with methodology artifacts controlled for, not on the
-- pgSphere comparison, which was always a secondary claim. See GIST_
-- REGION_DESIGN.md's "Round fifty-five" for the full numbers.

COMMENT ON OPERATOR CLASS skyregion_box_gist_ops USING gist IS
  'EXPERIMENTAL, DEFAULT as of 0.21: a plain axis-aligned 3D box key for skyregion (no spherical caps), for && and region-region/region-point containment. Wins on size and speed at realistic catalogue-footprint radii, for a column holding one roughly uniform scale of region, up to roughly 5-8 degrees (re-measured in GIST_REGION_DESIGN.md''s "Round fifty-four", correcting "Round fifty-three"''s own first, too-low ~0.3-1-degree estimate); skyregion_gist_ops wins back above that, and also for a column mixing very different region scales in one index even below it. Select skyregion_gist_ops explicitly for either case. Does NOT beat pgSphere''s native GiST (an earlier claim here to the contrary did not reproduce; see "Round fifty-five") -- this opclass is only known to compete with skyregion_gist_ops, not with pgSphere. See ext/src/gist_region_box.c and GIST_REGION_DESIGN.md''s "Round forty-two", "Round fifty-three", "Round fifty-four", and "Round fifty-five".';

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL, non-default as of 0.21: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). Wins over skyregion_box_gist_ops (the new default) for a uniform-scale column above roughly 5-8 degrees radius, and for a column mixing very different region scales in one index even below that -- select it explicitly for either case; neither opclass is known to beat pgSphere''s native GiST on this corpus (see "Round fifty-five"). See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, bench/24_region_contains_region.sql, and GIST_REGION_DESIGN.md''s "Round fifty-four" (correcting "Round fifty-three"''s first, too-low crossover estimate) and "Round fifty-five" before relying on this.';
