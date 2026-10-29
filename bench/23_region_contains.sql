-- Point-in-region CONTAINS (skypos <@ skyregion / skyregion @> skypos), via
-- the skyregion GiST opclass's second strategy (ext/src/gist_region.c,
-- EXPERIMENTAL): a plain `CREATE INDEX ON fpr USING gist (s_region)` and
-- `p.pos <@ f.s_region`, no covering precomputed per row -- unlike
-- 20_region_xmatch.sql's point-in-footprint recipe, which needs the query
-- side to be the *large*, cell-indexed table (skycell's density-based
-- planner rewrite turns the join into B-tree ranges over that table's own
-- per-row cell id, found from its indexes -- it has nothing to do with an
-- index on the region column at all, and only applies in that one
-- direction). This script tests the *other* direction: many candidate
-- regions, a few query points, which the existing recipe cannot answer
-- efficiently at all (no per-region index existed for this before this
-- opclass) -- the honest baseline is a plain seq-scan-and-filter.
--
-- Depends on `fpr` from 20_region_xmatch.sql (2500 circle + 2500 polygon
-- footprints): run that script first.
--
-- Same probe construction as 21_region_overlap.sql/22_region_gist.sql (same
-- seed, same offset), so this is directly comparable to both; run alongside
-- them, not instead of.
--
-- Results at 200x5,000 and 500x50,000 (measured in a scratch database with a
-- freshly-created 0.10 extension -- see the gotcha below): ~110-160x faster
-- than the brute-force baseline at both scales, and within ~2x of pgSphere's
-- native point-in-shape containment at 5,000 rows, widening to ~5.5x at
-- 50,000 -- not because this opclass's pruning got worse (its own absolute
-- time barely moves between this and the && strategy at the same scale), but
-- because pgSphere's native CONTAINS is cheaper than its native && to begin
-- with (exact point-in-shape vs shape-shape overlap), a gap this opclass's
-- angle-and-radius sub-cap test doesn't shrink the same way for a
-- zero-radius query. See GIST_REGION_DESIGN.md's "Round four" for the full
-- numbers and that explanation in more detail.
--
-- GOTCHA: adding this strategy to an already-`CREATE EXTENSION`'d database
-- still sitting at skycell 0.10 from before this change needs a real
-- extension reinstall (DROP EXTENSION skycell CASCADE; CREATE EXTENSION
-- skycell; then rebuild whatever depended on skyregion/skypos-typed
-- columns) -- editing the 0.10 SQL files in place does not retroactively
-- reach a database that already ran them once. A fresh `CREATE EXTENSION`
-- picks up the new operator with no extra steps.
\set ON_ERROR_STOP 1
\if :{?nprobe}
\else
  \set nprobe 200
\endif

SELECT setseed(0.11);
DROP TABLE IF EXISTS rox_probe_pts;
CREATE TABLE rox_probe_pts AS
SELECT fid AS pid,
       point('ICRS', ra0 + 0.3*r, dec0) AS pos,
       spoint(radians(ra0 + 0.3*r), radians(dec0)) AS spos
FROM fpr
WHERE abs(dec0) < 88
ORDER BY random()
LIMIT :nprobe;
ANALYZE rox_probe_pts;

-- the opclass is DEFAULT for skyregion, so this and the pgSphere indexes
-- below are the whole setup: no side table, no per-query LATERAL, no manual
-- recheck.
CREATE INDEX IF NOT EXISTS fpr_region_gist ON fpr USING gist (s_region);
CREATE INDEX IF NOT EXISTS rox_fpr_circ_gist ON fpr USING gist (reg_circ) WHERE NOT is_poly;
CREATE INDEX IF NOT EXISTS rox_fpr_poly_gist ON fpr USING gist (reg_poly) WHERE is_poly;
ANALYZE fpr;

CREATE TABLE IF NOT EXISTS bench_region_contains (method text, pass int, n bigint, ms float8, plan text);
TRUNCATE bench_region_contains;

CREATE OR REPLACE FUNCTION region_contains_sql(method text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'gist' THEN
      $q$SELECT count(*) FROM rox_probe_pts p, fpr f WHERE p.pos <@ f.s_region$q$
    WHEN 'brute' THEN
      $q$SELECT count(*) FROM rox_probe_pts p, fpr f WHERE skycell_in_region(p.pos, f.s_region)$q$
    WHEN 'pgsphere' THEN
      $q$SELECT
           (SELECT count(*) FROM rox_probe_pts p, fpr f WHERE NOT f.is_poly AND p.spos <@ f.reg_circ)
         + (SELECT count(*) FROM rox_probe_pts p, fpr f WHERE f.is_poly AND p.spos <@ f.reg_poly)$q$
  END
$$;

CREATE OR REPLACE FUNCTION bench_region_contains_run(method text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; t0 timestamptz; plan text := ''; l text; q text;
BEGIN
  q := region_contains_sql(method);
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE q INTO n;
  INSERT INTO bench_region_contains (method, pass, n, ms, plan)
  VALUES (method, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;

\echo '=== consistency: brute force vs the GiST-indexed <@ vs pgSphere native ==='
SELECT bench_region_contains_run('brute', 1);
SELECT bench_region_contains_run('gist', 1);
SELECT bench_region_contains_run('pgsphere', 1);
SELECT (SELECT n FROM bench_region_contains WHERE method = 'brute')    AS brute_n,
       (SELECT n FROM bench_region_contains WHERE method = 'gist')    AS gist_n,
       (SELECT n FROM bench_region_contains WHERE method = 'pgsphere') AS pgsphere_n;

\echo '=== plan (should be an Index Scan using fpr_region_gist, Index Cond: region @> pos) ==='
SELECT plan FROM bench_region_contains WHERE method = 'gist' AND pass = 1;

TRUNCATE bench_region_contains;
SELECT bench_region_contains_run('brute', 1);
SELECT bench_region_contains_run('brute', 2);
SELECT bench_region_contains_run('gist', 1);
SELECT bench_region_contains_run('gist', 2);
SELECT bench_region_contains_run('pgsphere', 1);
SELECT bench_region_contains_run('pgsphere', 2);

SELECT method, pass, n, round(ms::numeric, 2) AS ms FROM bench_region_contains ORDER BY method, pass;
