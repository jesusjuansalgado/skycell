\echo Use "ALTER EXTENSION skycell UPDATE TO '0.22'" to load this file. \quit

-- Corrects a wrong crossover figure 0.21 shipped in both opclasses'
-- COMMENT ON OPERATOR CLASS text (and the comment blocks above their
-- CREATE OPERATOR CLASS statements in skycell--0.22.sql, which only a
-- fresh CREATE EXTENSION reads). No opclass, operator, or function
-- changes -- comment text only, so this is a plain UPDATE/COMMENT ON,
-- not a DROP/CREATE like 0.20--0.21.sql needed.
--
-- GIST_REGION_DESIGN.md's "Round fifty-three" (0.21's own round) first
-- measured the box-opclass-vs-multicap crossover at roughly 0.3-1
-- degree and promoted skyregion_box_gist_ops to DEFAULT on that basis.
-- "Round fifty-four" pinned the crossover down precisely, as asked
-- directly rather than left as a bracket, and found that first estimate
-- conflated two genuinely separate effects:
--
--   1. The real uniform-scale crossover (one index, one roughly
--      constant region size) sits at roughly 5-8 degrees, not 0.3-1 --
--      confirmed by isolating same-scale corpora at a dense sweep of
--      fixed radii (0.3 through 60 degrees) and finding skyregion_box_
--      gist_ops wins consistently through ~5 degrees, skyregion_gist_
--      ops from ~8 degrees on, with the two within measurement noise of
--      each other in between.
--   2. A column mixing very different region scales in the *same*
--      index shifts the balance toward skyregion_gist_ops even for a
--      small probe radius that wins for the box opclass in an isolated,
--      single-scale corpus -- confirmed directly: identical small-
--      radius probes against an isolated small-only corpus favoured the
--      box opclass ~1.6x; the same probes against a corpus mixing that
--      small population with a huge-radius one sharing one index
--      favoured skyregion_gist_ops ~1.5x instead. Round fifty-three's
--      own first estimate came from comparing a mixed-scale corpus at
--      one radius against a separate, roughly-uniform corpus at
--      another, smaller radius, and attributed the whole difference to
--      radius alone.
--
-- The decision to promote skyregion_box_gist_ops to DEFAULT stands --
-- if anything it is better supported now, since its real domain (one
-- uniform small-to-few-degree scale) is wider than first measured, not
-- narrower -- only the stated crossover number and its explanation were
-- wrong. See GIST_REGION_DESIGN.md's "Round fifty-four" for the full
-- numbers.

COMMENT ON OPERATOR CLASS skyregion_box_gist_ops USING gist IS
  'EXPERIMENTAL, DEFAULT as of 0.21: a plain axis-aligned 3D box key for skyregion (no spherical caps), for && and region-region/region-point containment. Wins on size and speed at realistic catalogue-footprint radii, for a column holding one roughly uniform scale of region, up to roughly 5-8 degrees (re-measured in GIST_REGION_DESIGN.md''s "Round fifty-four", correcting "Round fifty-three"''s own first, too-low ~0.3-1-degree estimate); skyregion_gist_ops wins back above that, and also for a column mixing very different region scales in one index even below it. Select skyregion_gist_ops explicitly for either case. See ext/src/gist_region_box.c and GIST_REGION_DESIGN.md''s "Round forty-two", "Round fifty-three", and "Round fifty-four".';

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL, non-default as of 0.21: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). Wins over skyregion_box_gist_ops (the new default) for a uniform-scale column above roughly 5-8 degrees radius, and for a column mixing very different region scales in one index even below that -- select it explicitly for either case. See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, bench/24_region_contains_region.sql, and GIST_REGION_DESIGN.md''s "Round fifty-four" (correcting "Round fifty-three"''s first, too-low crossover estimate) before relying on this.';
