-- Stored composite footprints: an ObsCore-like table whose s_region holds each
-- observation's footprint as it is -- a union of detectors, a field less its
-- coronagraph mask -- searched with a plain polygon, and asked which
-- observations contain a position.  skycell stores one skyregion per
-- observation under its default GiST index.  pgSphere has no composite type,
-- so it stores one spoly per detector and one scircle per mask, each under its
-- GiST index, and a query returns the distinct observations, less those whose
-- mask holds the whole field (or the position).  For a convex field and a mask
-- strictly inside its square, that is exactly the overlap with the square less
-- the mask, so both answer the same rows.
--
--   nircam    two modules of 2 x 2 detectors, 64" each, 5" gaps, 44" apart
--   uvis      two 162" x 81" CCDs either side of a 1.2" gap
--   parallel  a 3.4' x 3.6' field and a 2.2' field, 7' apart
--   corona    a 24" square less a 1" mask at its centre
--
-- Observations: :nobs, half over the whole sky, half in twenty deep fields of
-- 0.5 deg, each kind in both; position angles random.  Search fields: convex quadrilaterals of
-- 1'-20', centred near an observation.  Positions: a third on a detector, a
-- third in a gap or beside a footprint, a third in a mask.  Per query, both
-- methods in random order, :n executions each shifted 1e-9 deg.
\set ON_ERROR_STOP 1

SELECT setseed(0.35);

-- a rectangle of half-sizes (w, h) deg centred at (x, y) deg in the tangent
-- plane at (ra0, dec0), the frame turned by pa: its corners as ra, dec pairs
CREATE OR REPLACE FUNCTION fc_rect(ra0 float8, de0 float8, pa float8, x float8, y float8,
                                   w float8, h float8) RETURNS float8[]
LANGUAGE sql IMMUTABLE AS $$
  SELECT array_agg(v ORDER BY k, j) FROM (
    SELECT k, j, CASE j WHEN 0 THEN ra0 + (cos(radians(pa)) * (x + sx * w) - sin(radians(pa)) * (y + sy * h)) / cos(radians(de0))
                        ELSE de0 + sin(radians(pa)) * (x + sx * w) + cos(radians(pa)) * (y + sy * h) END AS v
    FROM (VALUES (1, -1, -1), (2, 1, -1), (3, 1, 1), (4, -1, 1)) c(k, sx, sy), generate_series(0, 1) j) t $$;

CREATE OR REPLACE FUNCTION fc_stcs_poly(v float8[]) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT 'POLYGON ' || array_to_string(v, ' ') $$;

-- the observations
DROP TABLE IF EXISTS fc_obs, fc_det, fc_mask, fc_fp, fc_field, fc_pos;
CREATE TABLE fc_obs AS
SELECT i AS obs_id, (ARRAY['nircam', 'uvis', 'parallel', 'corona'])[1 + (i / 2) % 4] AS kind,
       ra, greatest(-80, least(80, de)) AS de, 360 * random() AS pa
FROM (SELECT i,
             CASE WHEN i % 2 = 0 THEN 360 * random() ELSE (i % 20) * 18 + 0.5 * random() END AS ra,
             CASE WHEN i % 2 = 0 THEN degrees(asin(2 * random() - 1)) ELSE ((i % 20) - 10) * 7 + 0.5 * random() END AS de
      FROM generate_series(1, :nobs) i) s;

-- the detectors (pgSphere's rows, and the pieces of skycell's unions)
CREATE TABLE fc_det AS
SELECT obs_id, fc_rect(ra, de, pa, x, y, w, h) AS v
FROM fc_obs o,
     LATERAL (SELECT * FROM (VALUES
       -- NIRCam short wavelength: modules A and B, 2 x 2 detectors each
       ('nircam', -0.0343, -0.0096, 0.0089, 0.0089), ('nircam', -0.0343, 0.0096, 0.0089, 0.0089),
       ('nircam', -0.0151, -0.0096, 0.0089, 0.0089), ('nircam', -0.0151, 0.0096, 0.0089, 0.0089),
       ('nircam', 0.0151, -0.0096, 0.0089, 0.0089), ('nircam', 0.0151, 0.0096, 0.0089, 0.0089),
       ('nircam', 0.0343, -0.0096, 0.0089, 0.0089), ('nircam', 0.0343, 0.0096, 0.0089, 0.0089),
       -- WFC3/UVIS: two CCDs, 1.2" apart
       ('uvis', 0, -0.01142, 0.0225, 0.01125), ('uvis', 0, 0.01142, 0.0225, 0.01125),
       -- a coordinated parallel: two fields 7' apart
       ('parallel', -0.0583, 0, 0.0283, 0.0300), ('parallel', 0.0583, 0, 0.0183, 0.0183),
       -- a coronagraph's field (its mask below)
       ('corona', 0, 0, 0.00333, 0.00333)) d(kind, x, y, w, h)
       WHERE d.kind = o.kind) d;
CREATE TABLE fc_mask AS
SELECT obs_id, ra, de, 1.0 / 3600 AS r FROM fc_obs WHERE kind = 'corona';

-- skycell: one footprint per observation
CREATE TABLE fc_fp AS
SELECT o.obs_id, o.kind,
       (CASE WHEN o.kind = 'corona'
             THEN 'DIFFERENCE ICRS (' || (SELECT fc_stcs_poly(v) FROM fc_det d WHERE d.obs_id = o.obs_id)
                  || format(' CIRCLE %s %s %s)', o.ra, o.de, 1.0 / 3600)
             ELSE 'UNION ICRS (' || (SELECT string_agg(fc_stcs_poly(v), ' ') FROM fc_det d WHERE d.obs_id = o.obs_id) || ')'
        END)::skyregion AS s_region
FROM fc_obs o;
CREATE INDEX fc_fp_r ON fc_fp USING gist (s_region);

-- pgSphere: a row per detector, a row per mask
ALTER TABLE fc_det ADD COLUMN poly spoly;
UPDATE fc_det SET poly = spoly_deg(v);
CREATE INDEX fc_det_poly ON fc_det USING gist (poly);
ALTER TABLE fc_mask ADD COLUMN circ scircle;
UPDATE fc_mask SET circ = scircle(spoint(radians(ra), radians(de)), radians(r));
CREATE INDEX fc_mask_circ ON fc_mask USING gist (circ);
ANALYZE fc_obs; ANALYZE fc_det; ANALYZE fc_mask; ANALYZE fc_fp;

-- search fields: quadrilaterals 1'-20' across round an observation, turned
CREATE TABLE fc_field AS
SELECT q, o.kind, s, fc_rect(o.ra + (random() - 0.5) * 0.05 / cos(radians(o.de)), o.de + (random() - 0.5) * 0.05,
                             360 * random(), 0, 0, s / 2, s / 2 * (0.5 + random())) AS v
FROM (SELECT q, power(10, log(1.0 / 60) + random() * log(20.0)) AS s,
             1 + (random() * (:nobs - 1))::int AS obs_id
      FROM generate_series(1, :nfields) q) f JOIN fc_obs o USING (obs_id);

-- positions: on a detector, in a gap or beside, in a mask
CREATE TABLE fc_pos AS
SELECT p, kind, CASE WHEN p % 3 = 0 THEN (SELECT v[1] + (v[3] - v[1]) * 0.3 FROM fc_det d WHERE d.obs_id = o.obs_id LIMIT 1)
                     WHEN p % 3 = 1 THEN ra + 0.0002 / cos(radians(de))
                     ELSE ra + 0.0001 / cos(radians(de)) END AS ra,
       CASE WHEN p % 3 = 0 THEN (SELECT v[2] + (v[6] - v[2]) * 0.3 FROM fc_det d WHERE d.obs_id = o.obs_id LIMIT 1)
            ELSE de END AS de
FROM (SELECT p, 1 + (random() * (:nobs - 1))::int AS obs_id FROM generate_series(1, :npos) p) x
JOIN fc_obs o USING (obs_id);

CREATE OR REPLACE FUNCTION fc_sql(m text, w text, v float8[], ra float8, de float8) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE
    WHEN w = 'overlap' AND m = 'skycell' THEN format(
      'SELECT count(*), coalesce(sum(obs_id), 0) FROM fc_fp WHERE INTERSECTS(s_region, POLYGON(''ICRS'', VARIADIC %L::float8[])) = 1', v)
    WHEN w = 'overlap' THEN format(
      'SELECT count(*), coalesce(sum(obs_id), 0) FROM (SELECT DISTINCT d.obs_id FROM fc_det d WHERE d.poly && spoly_deg(%L::float8[])
         AND NOT EXISTS (SELECT 1 FROM fc_mask m WHERE m.obs_id = d.obs_id AND m.circ @> spoly_deg(%L::float8[]))) x', v, v)
    WHEN m = 'skycell' THEN format(
      'SELECT count(*), coalesce(sum(obs_id), 0) FROM fc_fp WHERE CONTAINS(POINT(''ICRS'', %s, %s), s_region) = 1', ra, de)
    ELSE format(
      'SELECT count(*), coalesce(sum(obs_id), 0) FROM (SELECT DISTINCT d.obs_id FROM fc_det d WHERE d.poly @> spoint(radians(%s), radians(%s))
         AND NOT EXISTS (SELECT 1 FROM fc_mask m WHERE m.obs_id = d.obs_id AND m.circ @> spoint(radians(%s), radians(%s)))) x', ra, de, ra, de)
  END $$;

CREATE TEMP TABLE fc_time (workload text, q int, kind text, method text, us float8, n bigint, s bigint);
CREATE OR REPLACE FUNCTION fc_run(n int, reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r record; m text; t0 timestamptz; x bigint; y bigint;
BEGIN
  FOR rep IN 1 .. reps LOOP
    FOR r IN SELECT * FROM (SELECT 'overlap' AS w, q, kind, v, NULL::float8 AS ra, NULL::float8 AS de FROM fc_field
                            UNION ALL SELECT 'position', p, kind, NULL, ra, de FROM fc_pos) a ORDER BY random() LOOP
      FOR m IN SELECT unnest(ARRAY['skycell', 'pgsphere']) ORDER BY random() LOOP
        EXECUTE fc_sql(m, r.w, r.v, r.ra, r.de) INTO x, y;          -- warm its pages
        t0 := clock_timestamp();
        FOR i IN 1 .. n LOOP
          EXECUTE fc_sql(m, r.w,
                         (SELECT array_agg(CASE WHEN k % 2 = 1 THEN r.v[k] + 1e-9 * i ELSE r.v[k] END ORDER BY k)
                          FROM generate_subscripts(r.v, 1) k),
                         r.ra + 1e-9 * i, r.de) INTO x, y;
        END LOOP;
        INSERT INTO fc_time VALUES (r.w, r.q, r.kind, m,
          extract(epoch FROM clock_timestamp() - t0) * 1e6 / n, x, y);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
SELECT setseed(0.5);
SELECT fc_run(:n, :reps);
\copy fc_time TO 'footprint.csv' WITH CSV HEADER

-- the two methods must return the same observations for every query
SELECT workload, count(DISTINCT q) AS queries,
       count(*) FILTER (WHERE n_sky <> n_pgs OR s_sky <> s_pgs) AS methods_disagree,
       sum(n_sky) AS observations_found
FROM (SELECT workload, q, max(n) FILTER (WHERE method = 'skycell') n_sky, max(n) FILTER (WHERE method = 'pgsphere') n_pgs,
             max(s) FILTER (WHERE method = 'skycell') s_sky, max(s) FILTER (WHERE method = 'pgsphere') s_pgs
      FROM fc_time WHERE us IS NOT NULL GROUP BY 1, 2) a GROUP BY 1 ORDER BY 1;

-- index sizes
SELECT 'skycell s_region GiST' AS index, pg_size_pretty(pg_relation_size('fc_fp_r')) AS size
UNION ALL SELECT 'pgSphere detectors GiST', pg_size_pretty(pg_relation_size('fc_det_poly'))
UNION ALL SELECT 'pgSphere masks GiST', pg_size_pretty(pg_relation_size('fc_mask_circ'));
