-- Region-region full containment, both directions, via the skyregion GiST
-- opclass's third and fourth strategies (ext/src/gist_region.c,
-- EXPERIMENTAL): a plain `CREATE INDEX ON fpr USING gist (s_region)` serves
-- both `f.s_region @> p.s_region` ("which of my footprints contain this
-- probe", round six) and `f.s_region <@ p.s_region` ("which of my
-- footprints are contained within this probe", round seven) -- same index
-- as 22_region_gist.sql's && and 23_region_contains.sql's @> (point), no
-- separate index, no side table.
--
-- Direction matters here in a way it doesn't for && or the point strategy.
-- Round six found @>(skyregion,skyregion) had no registered COMMUTATOR, so
-- only "fpr.s_region @> probe.s_region" (indexed table on the left) could
-- use fpr's index; round seven adds <@ and backfills @>'s COMMUTATOR, so
-- both operators now resolve through the index regardless of which literal
-- spelling a query uses -- but the *predicate* still has two distinct
-- directions ("does fpr contain the probe" vs "is fpr contained by the
-- probe"), which is what the two scopes below measure separately, each
-- with probes sized so genuine matches are common in that direction:
-- 'contains' uses probes far smaller than fpr's own footprints (fpr's r
-- ranges over roughly 0.02-0.3 deg -- see 20_region_xmatch.sql); 'covered_by'
-- uses probes far larger.
--
-- Neither strategy gets its own tighter consistent() test: both reuse
-- OVERLAP's sub-cap-overlap pruning rather than a dedicated containment
-- test (see gist_region.c's "round six"/"round seven" for why a tighter
-- per-sub-cap test would risk a false negative), so both should be expected
-- to prune less sharply than && does for its own predicate.
--
-- Depends on `fpr` from 20_region_xmatch.sql (2500 circle + 2500 polygon
-- footprints): run that script first.
\set ON_ERROR_STOP 1
\if :{?nprobe}
\else
  \set nprobe 200
\endif

SELECT setseed(0.23);
DROP TABLE IF EXISTS rox_probe_tiny, rox_probe_big;
CREATE TABLE rox_probe_tiny AS
SELECT fid AS pid,
       circle('ICRS', ra0 + 0.3*r, dec0, 0.01 * r) AS s_region,
       scircle(spoint(radians(ra0 + 0.3*r), radians(dec0)), radians(0.01 * r)) AS reg_circ
FROM fpr
WHERE abs(dec0) < 88
ORDER BY random()
LIMIT :nprobe;
ANALYZE rox_probe_tiny;

CREATE TABLE rox_probe_big AS
SELECT fid AS pid,
       circle('ICRS', ra0, dec0, 2.0 + 3*random()) AS s_region,
       scircle(spoint(radians(ra0), radians(dec0)), radians(2.0 + 3*random())) AS reg_circ
FROM fpr
WHERE abs(dec0) < 85
ORDER BY random()
LIMIT :nprobe;
ANALYZE rox_probe_big;

-- the opclass is DEFAULT for skyregion, so this and the pgSphere indexes
-- below are the whole setup: no side table, no per-query LATERAL, no manual
-- recheck.
CREATE INDEX IF NOT EXISTS fpr_region_gist ON fpr USING gist (s_region);
CREATE INDEX IF NOT EXISTS rox_fpr_circ_gist ON fpr USING gist (reg_circ) WHERE NOT is_poly;
CREATE INDEX IF NOT EXISTS rox_fpr_poly_gist ON fpr USING gist (reg_poly) WHERE is_poly;
ANALYZE fpr;

CREATE TABLE IF NOT EXISTS bench_region_contains_region (scope text, method text, pass int, n bigint, ms float8, plan text);
TRUNCATE bench_region_contains_region;

CREATE OR REPLACE FUNCTION region_contains_region_sql(scope text, method text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE scope || ':' || method
    WHEN 'contains:gist' THEN
      $q$SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE f.s_region @> p.s_region$q$
    WHEN 'contains:brute' THEN
      $q$SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE skycell_region_covers(f.s_region, p.s_region)$q$
    WHEN 'contains:pgsphere' THEN
      $q$SELECT
           (SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE NOT f.is_poly AND f.reg_circ ~ p.reg_circ)
         + (SELECT count(*) FROM rox_probe_tiny p, fpr f WHERE f.is_poly AND f.reg_poly ~ p.reg_circ)$q$
    WHEN 'covered_by:gist' THEN
      $q$SELECT count(*) FROM rox_probe_big p, fpr f WHERE f.s_region <@ p.s_region$q$
    WHEN 'covered_by:brute' THEN
      $q$SELECT count(*) FROM rox_probe_big p, fpr f WHERE skycell_region_covered_by(f.s_region, p.s_region)$q$
    WHEN 'covered_by:pgsphere' THEN
      $q$SELECT
           (SELECT count(*) FROM rox_probe_big p, fpr f WHERE NOT f.is_poly AND f.reg_circ <@ p.reg_circ)
         + (SELECT count(*) FROM rox_probe_big p, fpr f WHERE f.is_poly AND f.reg_poly <@ p.reg_circ)$q$
  END
$$;

CREATE OR REPLACE FUNCTION bench_region_contains_region_run(scope text, method text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; t0 timestamptz; plan text := ''; l text; q text;
BEGIN
  q := region_contains_region_sql(scope, method);
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE q INTO n;
  INSERT INTO bench_region_contains_region (scope, method, pass, n, ms, plan)
  VALUES (scope, method, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;

\echo '=== consistency: brute force vs the GiST-indexed @>/<@ vs pgSphere native ~/<@ ==='
SELECT bench_region_contains_region_run('contains', 'brute', 1);
SELECT bench_region_contains_region_run('contains', 'gist', 1);
SELECT bench_region_contains_region_run('contains', 'pgsphere', 1);
SELECT bench_region_contains_region_run('covered_by', 'brute', 1);
SELECT bench_region_contains_region_run('covered_by', 'gist', 1);
SELECT bench_region_contains_region_run('covered_by', 'pgsphere', 1);
SELECT scope,
       (SELECT n FROM bench_region_contains_region WHERE scope = t.scope AND method = 'brute')    AS brute_n,
       (SELECT n FROM bench_region_contains_region WHERE scope = t.scope AND method = 'gist')    AS gist_n,
       (SELECT n FROM bench_region_contains_region WHERE scope = t.scope AND method = 'pgsphere') AS pgsphere_n
FROM (SELECT DISTINCT scope FROM bench_region_contains_region) t;

\echo '=== plans (should be Bitmap/Index Scans using fpr_region_gist) ==='
SELECT scope, plan FROM bench_region_contains_region WHERE method = 'gist' AND pass = 1;

TRUNCATE bench_region_contains_region;
SELECT bench_region_contains_region_run('contains', 'brute', 1);
SELECT bench_region_contains_region_run('contains', 'brute', 2);
SELECT bench_region_contains_region_run('contains', 'gist', 1);
SELECT bench_region_contains_region_run('contains', 'gist', 2);
SELECT bench_region_contains_region_run('contains', 'pgsphere', 1);
SELECT bench_region_contains_region_run('contains', 'pgsphere', 2);
SELECT bench_region_contains_region_run('covered_by', 'brute', 1);
SELECT bench_region_contains_region_run('covered_by', 'brute', 2);
SELECT bench_region_contains_region_run('covered_by', 'gist', 1);
SELECT bench_region_contains_region_run('covered_by', 'gist', 2);
SELECT bench_region_contains_region_run('covered_by', 'pgsphere', 1);
SELECT bench_region_contains_region_run('covered_by', 'pgsphere', 2);

SELECT scope, method, pass, n, round(ms::numeric, 2) AS ms FROM bench_region_contains_region ORDER BY scope, method, pass;
