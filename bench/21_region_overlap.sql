-- Region-region INTERSECTS: genuine geometric overlap between two stored
-- footprints, not point-in-region. skyregion has no GiST opclass, so unlike
-- the skyregion-column CONTAINS/INTERSECTS cross-match in 20_region_xmatch.sql
-- this has no indexed skycell form at all *unless* both sides are decomposed
-- into MOC-derived [lo,hi] cell-id ranges (skycell_region_moc_ranges) and
-- joined on PostgreSQL's own built-in int8range GiST opclass -- see the
-- worked example next to skycell_region_moc in skycell--0.9.sql and the
-- "region-region INTERSECTS via MOC ranges" test in ext/test/sql/skycell.sql.
-- This script measures that recipe against pgSphere, whose scircle/spoly *do*
-- carry a native && GiST strategy.
--
-- Depends on `fpr` from 20_region_xmatch.sql (2500 circle + 2500 polygon
-- footprints): run that script first.
\set ON_ERROR_STOP 1
\if :{?nprobe}
\else
  \set nprobe 200
\endif

-- Probes are nearby, overlapping variants of a sampled footprint (offset by
-- 0.3*r, scaled by 1.2x) rather than the exact same shape: this exercises
-- genuine cross-region overlap rather than every probe trivially matching
-- its own footprint row.
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

-- skycell: MOC-range side table + GiST index on the (larger) footprint side.
DROP TABLE IF EXISTS rox_fpr_moc;
CREATE TABLE rox_fpr_moc AS
SELECT f.fid, rng FROM fpr f, skycell_region_moc_ranges(f.s_region, 8);
CREATE INDEX rox_fpr_moc_gist ON rox_fpr_moc USING gist (rng);
ANALYZE rox_fpr_moc;
ANALYZE rox_probe;

-- pgSphere: native GiST on the typed circle/polygon columns (mirrors the
-- "mixed" scope in 20_region_xmatch.sql: two joins, one per column, summed).
CREATE INDEX IF NOT EXISTS rox_fpr_circ_gist ON fpr USING gist (reg_circ) WHERE NOT is_poly;
CREATE INDEX IF NOT EXISTS rox_fpr_poly_gist ON fpr USING gist (reg_poly) WHERE is_poly;
ANALYZE fpr;

CREATE TABLE IF NOT EXISTS bench_region_overlap (method text, pass int, n bigint, ms float8, plan text);
TRUNCATE bench_region_overlap;

CREATE OR REPLACE FUNCTION region_overlap_sql(method text) RETURNS text LANGUAGE sql AS $$
  SELECT CASE method
    WHEN 'skycell' THEN
      $q$SELECT count(*) FROM (
           SELECT DISTINCT p.pid, f.fid FROM rox_probe p
           JOIN LATERAL skycell_region_moc_ranges(p.s_region, 8) pm ON true
           JOIN rox_fpr_moc f ON f.rng && pm.rng
           JOIN fpr fr ON fr.fid = f.fid
           WHERE intersects(p.s_region, fr.s_region) = 1
         ) c$q$
    WHEN 'pgsphere' THEN
      $q$SELECT
           (SELECT count(*) FROM rox_probe p, fpr f WHERE NOT f.is_poly AND p.reg_circ && f.reg_circ)
         + (SELECT count(*) FROM rox_probe p, fpr f WHERE f.is_poly AND p.reg_circ && f.reg_poly)
         + (SELECT count(*) FROM rox_probe p, fpr f WHERE NOT f.is_poly AND p.reg_poly && f.reg_circ)
         + (SELECT count(*) FROM rox_probe p, fpr f WHERE f.is_poly AND p.reg_poly && f.reg_poly)$q$
    WHEN 'brute' THEN
      $q$SELECT count(*) FROM rox_probe p, fpr f WHERE intersects(p.s_region, f.s_region) = 1$q$
  END
$$;

CREATE OR REPLACE FUNCTION bench_region_overlap_run(method text, pass int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n bigint; t0 timestamptz; plan text := ''; l text; q text;
BEGIN
  q := region_overlap_sql(method);
  IF pass = 1 THEN
    FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  END IF;
  t0 := clock_timestamp();
  EXECUTE q INTO n;
  INSERT INTO bench_region_overlap (method, pass, n, ms, plan)
  VALUES (method, pass, n, extract(epoch FROM clock_timestamp() - t0) * 1000, plan);
END $$;

-- pgSphere's reg_poly column mixes each footprint's own polygon and NULLs for
-- circle rows, and vice versa for reg_circ -- symmetric to the probe side, so
-- the four-term sum above double-counts nothing (each pgSphere pair of typed
-- columns contributes exactly one non-null combination per probe/footprint
-- pair).  Verify all three methods agree, then compare timing over two passes:
SELECT bench_region_overlap_run('brute', 1);
SELECT bench_region_overlap_run('skycell', 1);
SELECT bench_region_overlap_run('pgsphere', 1);
SELECT (SELECT n FROM bench_region_overlap WHERE method = 'brute')    AS brute_n,
       (SELECT n FROM bench_region_overlap WHERE method = 'skycell')  AS skycell_n,
       (SELECT n FROM bench_region_overlap WHERE method = 'pgsphere') AS pgsphere_n;

TRUNCATE bench_region_overlap;
SELECT bench_region_overlap_run('skycell', 1);
SELECT bench_region_overlap_run('skycell', 2);
SELECT bench_region_overlap_run('pgsphere', 1);
SELECT bench_region_overlap_run('pgsphere', 2);

SELECT method, pass, n, round(ms::numeric, 2) AS ms FROM bench_region_overlap ORDER BY method, pass;
