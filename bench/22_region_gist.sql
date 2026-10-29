-- Region-region INTERSECTS via the new skyregion GiST opclass (ext/src/
-- gist_region.c, EXPERIMENTAL): a plain `CREATE INDEX ON fpr USING gist
-- (s_region)` and `a.s_region && b.s_region`, no side table, no hand-written
-- join -- unlike 21_region_overlap.sql's MOC-ranges recipe, which needs
-- both. Same probe construction as that script (same seed, same offset/
-- scale), so the two are directly comparable; run alongside it, not instead
-- of it.
--
-- Depends on `fpr` from 20_region_xmatch.sql (2500 circle + 2500 polygon
-- footprints): run that script first.
--
-- As of the multi-cap key redesign (GIST_REGION_DESIGN.md's "Round three"),
-- this opclass beats pgSphere's native && at fpr's default 5000 rows and
-- stays within ~1.2x of it at 50,000 (`20_region_xmatch.sql -v nfp=50000`,
-- this script with `-v nprobe=500`), and beats the MOC-ranges recipe at both
-- scales. Earlier single-cap versions of this opclass were competitive only
-- at the smaller scale -- see "Picksplit, round two" for that history if
-- comparing against an older commit.
\set ON_ERROR_STOP 1
\if :{?nprobe}
\else
  \set nprobe 200
\endif

SELECT setseed(0.11);
DROP TABLE IF EXISTS rox_probe;
CREATE TABLE rox_probe AS
SELECT fid AS pid,
       CASE WHEN is_poly THEN polygon('ICRS', ra0+0.3*r-1.2*r, dec0-1.2*r, ra0+0.3*r+1.2*r, dec0-1.2*r,
                                              ra0+0.3*r+1.2*r, dec0+1.2*r, ra0+0.3*r-1.2*r, dec0+1.2*r)
            ELSE circle('ICRS', ra0 + 0.3*r, dec0, r * 1.2)
       END AS s_region,
       CASE WHEN is_poly THEN NULL ELSE scircle(spoint(radians(ra0+0.3*r), radians(dec0)), radians(r*1.2)) END AS reg_circ,
       CASE WHEN is_poly THEN spoly(format('{(%1$sd,%2$sd),(%3$sd,%2$sd),(%3$sd,%4$sd),(%1$sd,%4$sd)}',
                                           ra0+0.3*r-1.2*r, dec0-1.2*r, ra0+0.3*r+1.2*r, dec0+1.2*r)) ELSE NULL END AS reg_poly,
       is_poly
FROM fpr
WHERE abs(dec0) < 88
ORDER BY random()
LIMIT :nprobe;
ANALYZE rox_probe;

-- the opclass is DEFAULT for skyregion, so this is the whole setup: no side
-- table, no per-query LATERAL, no manual recheck (GiST's own recheck
-- mechanism runs intersects() automatically wherever the cap test alone
-- isn't conclusive).
CREATE INDEX IF NOT EXISTS fpr_region_gist ON fpr USING gist (s_region);
ANALYZE fpr;

CREATE TABLE IF NOT EXISTS bench_region_gist (method text, pass int, n bigint, ms float8, plan text);
TRUNCATE bench_region_gist;

CREATE OR REPLACE FUNCTION region_gist_sql(method text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'gist' THEN
      $q$SELECT count(*) FROM rox_probe p, fpr f WHERE p.s_region && f.s_region$q$
    WHEN 'brute' THEN
      $q$SELECT count(*) FROM rox_probe p, fpr f WHERE intersects(p.s_region, f.s_region) = 1$q$
  END
$$;

CREATE OR REPLACE FUNCTION bench_region_gist_run(method text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; t0 timestamptz; plan text := ''; l text; q text;
BEGIN
  q := region_gist_sql(method);
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE q INTO n;
  INSERT INTO bench_region_gist (method, pass, n, ms, plan)
  VALUES (method, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;

\echo '=== consistency: brute force vs the GiST-indexed && ==='
SELECT bench_region_gist_run('brute', 1);
SELECT bench_region_gist_run('gist', 1);
SELECT (SELECT n FROM bench_region_gist WHERE method = 'brute') AS brute_n,
       (SELECT n FROM bench_region_gist WHERE method = 'gist')  AS gist_n;

\echo '=== plan (should be an Index Scan using fpr_region_gist) ==='
SELECT plan FROM bench_region_gist WHERE method = 'gist' AND pass = 1;

TRUNCATE bench_region_gist;
SELECT bench_region_gist_run('gist', 1);
SELECT bench_region_gist_run('gist', 2);

SELECT method, pass, n, round(ms::numeric, 2) AS ms FROM bench_region_gist ORDER BY method, pass;
