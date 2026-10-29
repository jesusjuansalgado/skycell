-- A 10M-row ObsCore-shaped relation whose cones return exactly what they
-- return at 0.5M: 10_crossover.sql's build_obscore(500000) rows, the first
-- 3,906 fields, plus filler fields placed where no stored query cone can
-- reach (centre at least 2.6 deg from every oc_queries centre: Q06's 2 deg
-- radius plus a field's 0.28 deg maximum offset, with margin).  Of the 78,125
-- stored fields, 65,585 qualify; at 128 rows each they hold 8.4M, so the
-- filler fields hold 145 each, cut to exactly 10M rows in all.
--
-- Built as build_obscore() builds: the same column formulas, the heap in
-- (collection, field) order, pos added by ALTER + UPDATE, the same indexes and
-- statistics target.  The 0.5M rows come from build_obscore's own query, seed
-- 0.29, with the field id carried along so that the merge can order by it.
-- Run from bench/, after 10_crossover.sql.
CREATE OR REPLACE PROCEDURE build_obscore_sparse(nrows bigint DEFAULT 10000000,
                                                 base_rows bigint DEFAULT 500000,
                                                 per_field int DEFAULT 128,
                                                 filler_per_field int DEFAULT 145,
                                                 margin_deg float8 DEFAULT 2.6)
LANGUAGE plpgsql AS $$
DECLARE nbase bigint := greatest(1, base_rows / per_field);
BEGIN
  DROP TABLE IF EXISTS oc, oc_sparse_base, oc_sparse_fill;
  PERFORM setseed(0.29);
  EXECUTE format(
    'CREATE TEMP TABLE oc_sparse_base AS
     SELECT row_number() OVER () AS obs_id, f.rn AS field,
            f.ra + (random() - 0.5) * 0.4 AS s_ra,
            greatest(-89.9, least(89.9, f.dec + (random() - 0.5) * 0.4)) AS s_dec,
            55000 + (f.rn %% 3000) + random() AS t_min,
            (f.rn %% 3)::int AS calib_level,
            ''C'' || (f.rn %% 8)::text AS obs_collection
     FROM (SELECT field_id AS rn, ra, dec FROM oc_fields ORDER BY field_id LIMIT %s) f,
          LATERAL generate_series(1, %s) k
     ORDER BY f.rn %% 8, f.rn', nbase, per_field);
  PERFORM setseed(0.71);
  EXECUTE format(
    'CREATE TEMP TABLE oc_sparse_fill AS
     SELECT f.rn AS field,
            f.ra + (random() - 0.5) * 0.4 AS s_ra,
            greatest(-89.9, least(89.9, f.dec + (random() - 0.5) * 0.4)) AS s_dec,
            55000 + (f.rn %% 3000) + random() AS t_min,
            (f.rn %% 3)::int AS calib_level,
            ''C'' || (f.rn %% 8)::text AS obs_collection
     FROM (SELECT field_id AS rn, ra, dec FROM oc_fields o
           WHERE field_id > %s
             AND NOT EXISTS (SELECT 1 FROM oc_queries q
                             WHERE degrees(spoint(radians(q.ra), radians(q."dec")) <-> spoint(radians(o.ra), radians(o."dec"))) < %s)
           ORDER BY field_id) f,
          LATERAL generate_series(1, %s) k
     ORDER BY f.rn, k
     LIMIT %s', nbase, margin_deg, filler_per_field, nrows - nbase * per_field);
  CREATE TABLE oc AS
  SELECT row_number() OVER (ORDER BY field % 8, field, obs_id) AS obs_id,
         s_ra, s_dec, t_min, calib_level, obs_collection
  FROM (SELECT field, obs_id, s_ra, s_dec, t_min, calib_level, obs_collection FROM oc_sparse_base
        UNION ALL
        SELECT field, base_rows + row_number() OVER (ORDER BY field), s_ra, s_dec, t_min, calib_level, obs_collection
        FROM oc_sparse_fill) u
  ORDER BY field % 8, field, obs_id;

  ALTER TABLE oc ADD COLUMN pos spoint;
  UPDATE oc SET pos = spoint(radians(s_ra), radians(s_dec));
  CREATE INDEX oc_gist ON oc USING gist (pos);
  CREATE INDEX oc_cell ON oc (skycell_ang2cell(s_ra, s_dec));
  ALTER INDEX oc_cell ALTER COLUMN 1 SET STATISTICS 1000;
  COMMIT;
  ANALYZE oc;
END $$;
