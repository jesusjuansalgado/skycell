-- Region-region full containment (skyregion @> skyregion), via the skyregion
-- GiST opclass's third strategy (ext/src/gist_region.c, EXPERIMENTAL): a
-- plain `CREATE INDEX ON fpr USING gist (s_region)` and `f.s_region @>
-- p.s_region`, same index as 22_region_gist.sql's && and 23_region_contains
-- .sql's @> (point) -- no separate index, no side table.
--
-- Direction matters here in a way it doesn't for && or the point strategy:
-- @>(skyregion,skyregion) has no registered COMMUTATOR (there is no
-- <@(skyregion,skyregion) operator at all), so the indexed table's region
-- must be written as the operator's LEFT argument for the planner to use
-- its index -- `fpr.s_region @> probe.s_region` can use fpr's index,
-- `probe.s_region @> fpr.s_region` cannot, even though it is a different
-- (and differently selective) predicate, not just a spelling of the same
-- one. This script uses the indexable direction: "which of my (indexed)
-- footprints wholly contain this probe", so probes are small regions that
-- plausibly nest inside a fpr footprint (fpr's own r ranges over roughly
-- 0.02-0.3 deg -- see 20_region_xmatch.sql), not the other way around.
--
-- Unlike && and the point strategy, this one does not get its own tighter
-- consistent() test: it reuses OVERLAP's own sub-cap-overlap pruning rather
-- than a dedicated containment test (see gist_region.c's "round six" for why
-- a tighter per-sub-cap test would risk a false negative), so it should be
-- expected to prune less sharply than && does for its own predicate.
--
-- Depends on `fpr` from 20_region_xmatch.sql (2500 circle + 2500 polygon
-- footprints): run that script first.
\set ON_ERROR_STOP 1
\if :{?nprobe}
\else
  \set nprobe 200
\endif

SELECT setseed(0.23);
DROP TABLE IF EXISTS rox_probe_tiny;
CREATE TABLE rox_probe_tiny AS
SELECT fid AS pid,
       circle('ICRS', ra0 + 0.3*r, dec0, 0.01 * r) AS s_region,
       scircle(spoint(radians(ra0 + 0.3*r), radians(dec0)), radians(0.01 * r)) AS reg_circ
FROM fpr
WHERE abs(dec0) < 88
ORDER BY random()
LIMIT :nprobe;
ANALYZE rox_probe_tiny;

-- the opclass is DEFAULT for skyregion, so this and the pgSphere indexes
-- below are the whole setup: no side table, no per-query LATERAL, no manual
-- recheck.
CREATE INDEX IF NOT EXISTS fpr_region_gist ON fpr USING gist (s_region);
CREATE INDEX IF NOT EXISTS rox_fpr_circ_gist ON fpr USING gist (reg_circ) WHERE NOT is_poly;
CREATE INDEX IF NOT EXISTS rox_fpr_poly_gist ON fpr USING gist (reg_poly) WHERE is_poly;
ANALYZE fpr;

CREATE TABLE IF NOT EXISTS bench_region_contains_region (method text, pass int, n bigint, ms float8, plan text);
TRUNCATE bench_region_contains_region;

CREATE OR REPLACE FUNCTION region_contains_region_sql(method text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'gist' THEN
      $q$SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE f.s_region @> p.s_region$q$
    WHEN 'brute' THEN
      $q$SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE skycell_region_covers(f.s_region, p.s_region)$q$
    WHEN 'pgsphere' THEN
      $q$SELECT
           (SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE NOT f.is_poly AND f.reg_circ ~ p.reg_circ)
         + (SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE f.is_poly AND f.reg_poly ~ p.reg_circ)$q$
  END
$$;

CREATE OR REPLACE FUNCTION bench_region_contains_region_run(method text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; t0 timestamptz; plan text := ''; l text; q text;
BEGIN
  q := region_contains_region_sql(method);
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE q INTO n;
  INSERT INTO bench_region_contains_region (method, pass, n, ms, plan)
  VALUES (method, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;

\echo '=== consistency: brute force vs the GiST-indexed @> vs pgSphere native ~ ==='
SELECT bench_region_contains_region_run('brute', 1);
SELECT bench_region_contains_region_run('gist', 1);
SELECT bench_region_contains_region_run('pgsphere', 1);
SELECT (SELECT n FROM bench_region_contains_region WHERE method = 'brute')    AS brute_n,
       (SELECT n FROM bench_region_contains_region WHERE method = 'gist')    AS gist_n,
       (SELECT n FROM bench_region_contains_region WHERE method = 'pgsphere') AS pgsphere_n;

\echo '=== plan (should be a Bitmap/Index Scan using fpr_region_gist, Index Cond: s_region @> ...) ==='
SELECT plan FROM bench_region_contains_region WHERE method = 'gist' AND pass = 1;

TRUNCATE bench_region_contains_region;
SELECT bench_region_contains_region_run('brute', 1);
SELECT bench_region_contains_region_run('brute', 2);
SELECT bench_region_contains_region_run('gist', 1);
SELECT bench_region_contains_region_run('gist', 2);
SELECT bench_region_contains_region_run('pgsphere', 1);
SELECT bench_region_contains_region_run('pgsphere', 2);

SELECT method, pass, n, round(ms::numeric, 2) AS ms FROM bench_region_contains_region ORDER BY method, pass;
