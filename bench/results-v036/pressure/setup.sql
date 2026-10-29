-- The memory-pressure experiment's relation, scaled to 10M rows.
--
-- The original (results-pressure/iso_results.txt) was a 20.5M-row, 23-column,
-- 22 GB ObsCore-shaped relation `oc20`, in a 3 GiB container with
-- shared_buffers = 768MB: pgSphere's GiST (1162 MB) was 1.5 times the buffer
-- pool, skycell's B-tree (439 MB) 0.57 times. The script that built it was
-- never committed. This file builds the same kind of relation at 10M rows,
-- with rows of the same width (about 1.17 kB), so that run.sh can scale the
-- buffer pool and the server's memory by 10/20.5 and keep both ratios.
--
-- - Rows: observations in groups of 128 sharing a field centre, from the stored
--   DR3-weighted field centres (data/oc_fields.csv.gz, the 78,125 that
--   10_crossover.sql's build_obscore uses at 10M), ordered by collection and
--   field as an observation table is, not by position.
-- - Columns: build_obscore's seven, plus 16 text columns standing for
--   ObsCore's identifiers, URLs and descriptions, filled deterministically to
--   58 bytes each, so that a row is about 1.17 kB, seven to a page, as in the
--   original (22 GB for 20,485,632 rows).
-- - The objects keep the original's names (oc20, oc20_gist, oc20_cell, c20c,
--   iso_sql, cmp_iso) so that 13_pressure.sql runs unchanged.
-- - Rows are written in that order straight from the ordered field list (the
--   128 copies of each field come from a set-returning function in its select
--   list), so no sort of the 11 GB relation is needed.
-- - Queries: 3,498 2-degree cones drawn uniformly on the sphere (covering it
--   about once), as the original's c20c; 1,166 are measured, every third.
--
-- Run from bench/: psql -f results-v036/pressure/setup.sql
\set ON_ERROR_STOP 1
CREATE EXTENSION IF NOT EXISTS skycell;
CREATE EXTENSION IF NOT EXISTS pg_sphere;

CREATE TABLE IF NOT EXISTS oc_fields (field_id int PRIMARY KEY, ra float8, "dec" float8);
SELECT (SELECT count(*) FROM oc_fields) = 0 AS oc_load_fields \gset
\if :oc_load_fields
  \copy oc_fields FROM PROGRAM 'gzip -dc data/oc_fields.csv.gz' WITH CSV HEADER
\endif

DROP TABLE IF EXISTS oc20;
SELECT setseed(0.29);
CREATE TABLE oc20 AS
SELECT s.*, spoint(radians(s.s_ra), radians(s.s_dec)) AS pos FROM (
SELECT (f.pos - 1) * 128 + f.k AS obs_id,
       f.ra + (random() - 0.5) * 0.4 AS s_ra,
       greatest(-89.9, least(89.9, f.dec + (random() - 0.5) * 0.4)) AS s_dec,
       55000 + (f.rn % 3000) + random() AS t_min,
       (f.rn % 3)::int AS calib_level,
       'C' || (f.rn % 8)::text AS obs_collection,
       left(repeat(md5(f.rn::text || ':' || f.k), 2), 58) AS obs_publisher_did,
       left(repeat(md5(f.k || ':' || f.rn), 2), 58) AS obs_id_text,
       left(repeat(md5(f.rn::text || 'a' || f.k), 2), 58) AS access_url,
       left(repeat(md5(f.rn::text || 'b' || f.k), 2), 58) AS access_format,
       left(repeat(md5(f.rn::text || 'c' || f.k), 2), 58) AS target_name,
       left(repeat(md5(f.rn::text || 'd' || f.k), 2), 58) AS facility_name,
       left(repeat(md5(f.rn::text || 'e' || f.k), 2), 58) AS instrument_name,
       left(repeat(md5(f.rn::text || 'f' || f.k), 2), 58) AS obs_title,
       left(repeat(md5(f.rn::text || 'g' || f.k), 2), 58) AS obs_creator_name,
       left(repeat(md5(f.rn::text || 'h' || f.k), 2), 58) AS dataproduct_type,
       left(repeat(md5(f.rn::text || 'i' || f.k), 2), 58) AS pol_states,
       left(repeat(md5(f.rn::text || 'j' || f.k), 2), 58) AS o_ucd,
       left(repeat(md5(f.rn::text || 'k' || f.k), 2), 58) AS em_ucd,
       left(repeat(md5(f.rn::text || 'l' || f.k), 2), 58) AS proposal_id,
       left(repeat(md5(f.rn::text || 'm' || f.k), 2), 58) AS preview_url,
       left(repeat(md5(f.rn::text || 'n' || f.k), 2), 58) AS obs_release_note
FROM (SELECT o.*, generate_series(1, 128) AS k
      FROM (SELECT field_id AS rn, ra, dec,
                   row_number() OVER (ORDER BY field_id % 8, field_id) AS pos
            FROM oc_fields ORDER BY field_id % 8, field_id) o) f) s;
VACUUM (FREEZE, ANALYZE) oc20;
CREATE INDEX oc20_gist ON oc20 USING gist (pos);
CREATE INDEX oc20_cell ON oc20 (skycell_ang2cell(s_ra, s_dec));
ALTER INDEX oc20_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE oc20;

-- the query stream: 3,498 2-degree cones drawn uniformly on the sphere
DROP TABLE IF EXISTS c20c;
SELECT setseed(0.31);
CREATE TABLE c20c AS
SELECT i AS qid, 360 * random() AS ra, degrees(asin(2 * random() - 1)) AS "dec"
FROM generate_series(1, 3498) i;

-- the two translations of a 2-degree cone; sum(t_min) makes both read the heap
CREATE OR REPLACE FUNCTION iso_sql(method text, ra float8, "dec" float8) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE method
    WHEN 'pgsphere' THEN format('SELECT count(*), sum(t_min) FROM oc20 WHERE pos <@ scircle(spoint(radians(%s), radians(%s)), radians(2))', ra, "dec")
    ELSE format('SELECT count(*), sum(t_min) FROM oc20 WHERE skycell_cone(skycell_ang2cell(s_ra, s_dec), s_ra, s_dec, %s, %s, 2)', ra, "dec")
  END
$$;

CREATE TABLE IF NOT EXISTS cmp_iso (phase text, method text, qid bigint, ms float8, hit float8, read float8);

SELECT pg_size_pretty(pg_relation_size('oc20')) AS heap,
       pg_relation_size('oc20') / (SELECT count(*) FROM oc20) AS bytes_per_row,
       pg_size_pretty(pg_relation_size('oc20_gist')) AS gist,
       pg_size_pretty(pg_relation_size('oc20_cell')) AS cell,
       pg_relation_size('oc20_gist') / 1048576 AS gist_mb,
       pg_relation_size('oc20_cell') / 1048576 AS cell_mb;
