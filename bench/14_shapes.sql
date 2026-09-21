-- How tight is the covering, and for which shapes?
--
-- Two questions the cone benchmark does not answer:
--   1. how much sky does a covering actually scan, as a function of radius?
--   2. which region shapes does a space-filling curve handle badly?
--
-- (1) turns out to be non-monotonic, with a peak around 0.25-1 arcsec -- the
-- cross-match radius -- which is why skycell only reaches parity with q3c_join
-- there rather than beating it.  (2) turns out the opposite way round from the
-- textbook expectation: elongated regions are skycell's best case, because
-- pgSphere's bounding structure for a long strip is looser than the covering.
--
-- Prerequisites: an ObsCore-shaped relation `oc20` with both indexes, as built
-- for bench/13_pressure.sql.  Raw numbers reported in the README.
\set ON_ERROR_STOP 1

CREATE OR REPLACE FUNCTION boxarr(ra0 float8, dec0 float8, w float8, h float8) RETURNS float8[]
LANGUAGE sql IMMUTABLE AS $$
  SELECT ARRAY[ra0-w/2, dec0-h/2, ra0+w/2, dec0-h/2,
               ra0+w/2, dec0+h/2, ra0-w/2, dec0+h/2] $$;

-- area the covering scans, from the ranges themselves
CREATE OR REPLACE FUNCTION cov_area(p float8[]) RETURNS float8 LANGUAGE sql AS $$
  SELECT coalesce(sum(hi-lo+1),0)::float8 / (12.0*pow(4,29)) * 41252.96
  FROM skycell_poly_ranges(p) $$;

\echo '=== (1) covering tightness vs radius (galactic centre, real density) ==='
SELECT (r*3600)::numeric(10,2) AS radius_arcsec, i.nranges, i.deepest,
       round(i.area_ratio::numeric,1) AS area_scanned_over_area_asked
FROM unnest(ARRAY[0.0000278,0.000278,0.00139,0.00278,0.0139,0.0278,0.1,1.0,3.0,10.0,30.0,90.0]) r,
     LATERAL skycell_cover_info(266.4,-29.0,r,'oc20_cell','skycell_ang2cell') i;

\echo '=== (1b) cones at the pole do not fragment ==='
SELECT d AS dec, i.nranges, round(i.area_ratio::numeric,2) AS area_ratio
FROM unnest(ARRAY[0,30,45,60,75,85,89,89.9]) d,
     LATERAL skycell_cover_info(0, d, 0.5) i;

\echo '=== (2) constant-area boxes, increasing aspect ratio: covering waste ==='
SELECT w::text||' x '||h::text AS shape, round((w/h)::numeric,0) AS aspect,
       (SELECT count(*) FROM skycell_poly_ranges(boxarr(180,0,w,h))) AS nranges,
       round((cov_area(boxarr(180,0,w,h))/(w*h))::numeric,1) AS waste_factor
FROM (VALUES (1.0,1.0),(2.0,0.5),(4.0,0.25),(10.0,0.1),(30.0,0.0333),
             (60.0,0.0167),(120.0,0.00833)) v(w,h);

\echo '=== (2b) and what that costs against pgSphere, paired and randomized ==='
CREATE OR REPLACE FUNCTION spolyarr(p float8[]) RETURNS spoly LANGUAGE sql IMMUTABLE AS $$
  SELECT spoly(array_agg(spoint(radians(p[2*i-1]), radians(p[2*i])) ORDER BY i))
  FROM generate_series(1, array_length(p,1)/2) i $$;

CREATE TABLE IF NOT EXISTS thin (aspect float8, method text, ms float8, buffers float8, n bigint);
DELETE FROM thin;
DO $$
DECLARE v record; ms text[]; i int; rep int; j json; p json; nn bigint; arr float8[];
BEGIN
  PERFORM setseed(0.5);
  FOR rep IN 1..3 LOOP
  FOR v IN SELECT * FROM (VALUES (1.0,1.0),(4.0,0.25),(10.0,0.1),
                                 (30.0,0.0333),(60.0,0.0167)) t(w,h) LOOP
    arr := boxarr(266, -29, v.w, v.h);
    SELECT array_agg(m ORDER BY random()) INTO ms FROM unnest(ARRAY['pgsphere','skycell']) m;
    FOR i IN 1..2 LOOP
      IF ms[i]='pgsphere' THEN
        EXECUTE 'SELECT count(*) FROM oc20 WHERE pos <@ $1' INTO nn USING spolyarr(arr);
        EXECUTE 'EXPLAIN (ANALYZE,BUFFERS,TIMING OFF,SUMMARY ON,FORMAT JSON) SELECT count(*) FROM oc20 WHERE pos <@ '
                || quote_literal(spolyarr(arr)::text) || '::spoly' INTO j;
      ELSE
        EXECUTE 'SELECT count(*) FROM oc20 WHERE skycell_poly(skycell_ang2cell(s_ra,s_dec), s_ra, s_dec, $1)'
                INTO nn USING arr;
        EXECUTE 'EXPLAIN (ANALYZE,BUFFERS,TIMING OFF,SUMMARY ON,FORMAT JSON) SELECT count(*) FROM oc20 WHERE skycell_poly(skycell_ang2cell(s_ra,s_dec), s_ra, s_dec, '
                || quote_literal(arr::text) || '::float8[])' INTO j;
      END IF;
      p := j->0->'Plan';
      INSERT INTO thin VALUES (v.w/v.h, ms[i],
        (j->0->>'Planning Time')::float8 + (j->0->>'Execution Time')::float8,
        (p->>'Shared Hit Blocks')::float8 + (p->>'Shared Read Blocks')::float8, nn);
    END LOOP;
  END LOOP; END LOOP;
END $$;

SELECT round(aspect::numeric,0) AS aspect, max(n) AS rows_returned,
  round(avg(ms) FILTER (WHERE method='pgsphere')::numeric,2) AS pgs_ms,
  round(avg(ms) FILTER (WHERE method='skycell')::numeric,2)  AS sky_ms,
  round((avg(ms) FILTER (WHERE method='skycell')
         / avg(ms) FILTER (WHERE method='pgsphere'))::numeric,2) AS ratio,
  round(avg(buffers) FILTER (WHERE method='pgsphere')::numeric,0) AS pgs_buf,
  round(avg(buffers) FILTER (WHERE method='skycell')::numeric,0)  AS sky_buf
FROM thin GROUP BY 1 ORDER BY 1;
