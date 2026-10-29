-- Footprints of a single size: skycell's default region GiST (the float4 box
-- key) against pgSphere's native GiST on scircle, for overlap (&&).
--
-- The laptop measurement this repeats (GIST_REGION_DESIGN.md, "Round
-- fifty-eight") was run from scratch tables that were never committed; this
-- script rebuilds it from that round's description:
-- - five bands, each a separate pair of tables (one per index), of 25,000
--   circles of one radius (0.05, 0.1, 0.3, 0.5 and 1 deg) with centres uniform
--   on the sphere;
-- - 20 fresh probe circles per band, of the band's radius, drawn
--   independently of the stored rows;
-- - each probe a literal query, `count(*)` of the stored circles it overlaps;
-- - buffers from EXPLAIN (ANALYZE, BUFFERS), summed over every plan node.
-- Unlike that round, time is measured as paired trials: after an untimed
-- pass, 5 repetitions, the two methods in an order drawn per probe and
-- repetition, as 07_ab.sql does.
--
--   psql -v seed=0.58 -f bands.sql     # run once per seed
\set ON_ERROR_STOP 1
CREATE EXTENSION IF NOT EXISTS skycell;
CREATE EXTENSION IF NOT EXISTS pg_sphere;
SELECT setseed(:seed), set_config('bs.seed', :'seed', false);

DROP TABLE IF EXISTS bs_probe, bs_rows CASCADE;
CREATE TABLE bs_probe AS
SELECT b.band, b.r, g AS pid, 360 * random() AS ra, degrees(asin(2 * random() - 1)) AS "dec"
FROM (VALUES (1, 0.05), (2, 0.1), (3, 0.3), (4, 0.5), (5, 1.0)) b(band, r),
     generate_series(1, 20) g;
CREATE TABLE bs_rows AS
SELECT b.band, b.r, g AS id, 360 * random() AS ra, degrees(asin(2 * random() - 1)) AS "dec"
FROM (VALUES (1, 0.05), (2, 0.1), (3, 0.3), (4, 0.5), (5, 1.0)) b(band, r),
     generate_series(1, 25000) g;

DO $$
DECLARE b int;
BEGIN
  FOR b IN 1..5 LOOP
    EXECUTE format('DROP TABLE IF EXISTS bs_sky%s, bs_sph%s', b, b);
    EXECUTE format('CREATE TABLE bs_sky%s AS SELECT id, circle(''ICRS'', ra, "dec", r) AS reg FROM bs_rows WHERE band = %s ORDER BY id', b, b);
    EXECUTE format('CREATE TABLE bs_sph%s AS SELECT id, scircle(spoint(radians(ra), radians("dec")), radians(r)) AS reg FROM bs_rows WHERE band = %s ORDER BY id', b, b);
    EXECUTE format('CREATE INDEX ON bs_sky%s USING gist (reg)', b);
    EXECUTE format('CREATE INDEX ON bs_sph%s USING gist (reg)', b);
  END LOOP;
END $$;
DROP TABLE bs_rows;
SELECT format('VACUUM ANALYZE bs_%s%s', m, b) FROM generate_series(1, 5) b, unnest(ARRAY['sky', 'sph']) m \gexec

CREATE OR REPLACE FUNCTION bs_sql(method text, p bs_probe) RETURNS text LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE method
    WHEN 'skycell' THEN format('SELECT count(*) FROM bs_sky%s WHERE reg && circle(''ICRS'', %s, %s, %s)', p.band, p.ra, p."dec", p.r)
    ELSE format('SELECT count(*) FROM bs_sph%s WHERE reg && scircle(spoint(radians(%s), radians(%s)), radians(%s))', p.band, p.ra, p."dec", p.r)
  END $$;

-- buffers summed over every plan node
CREATE OR REPLACE FUNCTION bs_bufs(j jsonb) RETURNS float8 LANGUAGE sql IMMUTABLE AS $$
  SELECT coalesce((j->>'Shared Hit Blocks')::float8, 0) + coalesce((j->>'Shared Read Blocks')::float8, 0)
       + coalesce((SELECT sum(bs_bufs(c)) FROM jsonb_array_elements(j->'Plans') c), 0) $$;

CREATE TABLE IF NOT EXISTS bs_time (seed float8, method text, band int, pid int, rep int, pos int, n bigint, ms float8);
CREATE TABLE IF NOT EXISTS bs_explain (seed float8, method text, band int, pid int, n bigint, bufs float8, node text);
DELETE FROM bs_time WHERE seed = :seed;
DELETE FROM bs_explain WHERE seed = :seed;

DO $$
DECLARE p bs_probe; m text; ms text[]; rep int; k int; n bigint; t0 timestamptz; j jsonb;
        sd float8 := current_setting('bs.seed')::float8;
BEGIN
  FOR p IN SELECT * FROM bs_probe ORDER BY band, pid LOOP      -- untimed pass
    FOREACH m IN ARRAY ARRAY['skycell', 'pgsphere'] LOOP EXECUTE bs_sql(m, p) INTO n; END LOOP;
  END LOOP;
  FOR rep IN 1..5 LOOP
    FOR p IN SELECT * FROM bs_probe ORDER BY band, pid LOOP
      ms := CASE WHEN random() < 0.5 THEN ARRAY['skycell', 'pgsphere'] ELSE ARRAY['pgsphere', 'skycell'] END;
      FOR k IN 1..2 LOOP
        t0 := clock_timestamp();
        EXECUTE bs_sql(ms[k], p) INTO n;
        INSERT INTO bs_time VALUES (sd, ms[k], p.band, p.pid, rep, k, n,
                                    extract(epoch FROM clock_timestamp() - t0) * 1000);
      END LOOP;
    END LOOP;
  END LOOP;
  FOR p IN SELECT * FROM bs_probe ORDER BY band, pid LOOP
    FOREACH m IN ARRAY ARRAY['skycell', 'pgsphere'] LOOP
      EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || bs_sql(m, p) INTO j;
      INSERT INTO bs_explain VALUES (sd, m, p.band, p.pid,
        (SELECT round((c->>'Actual Rows')::numeric)::bigint FROM jsonb_array_elements(j->0->'Plan'->'Plans') c LIMIT 1),
        bs_bufs(j->0->'Plan'), j->0->'Plan'->'Plans'->0->>'Node Type');
    END LOOP;
  END LOOP;
END $$;

-- the two methods agree on every probe
SELECT count(*) AS probes_disagreeing FROM (
  SELECT band, pid FROM bs_time WHERE seed = :seed GROUP BY band, pid HAVING count(DISTINCT n) > 1) x;

-- per band: median buffers, median time, and the paired median ratio
WITH t AS (
  SELECT band, pid, rep, max(ms) FILTER (WHERE method = 'skycell') AS s, max(ms) FILTER (WHERE method = 'pgsphere') AS g
  FROM bs_time WHERE seed = :seed GROUP BY band, pid, rep)
SELECT b.band, r AS radius_deg,
       (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY bufs) FROM bs_explain e WHERE e.seed = :seed AND e.band = b.band AND method = 'skycell') AS sky_buf,
       (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY bufs) FROM bs_explain e WHERE e.seed = :seed AND e.band = b.band AND method = 'pgsphere') AS sph_buf,
       round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY s) FROM t WHERE t.band = b.band)::numeric, 4) AS sky_ms,
       round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY g) FROM t WHERE t.band = b.band)::numeric, 4) AS sph_ms,
       round((SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY s / g) FROM t WHERE t.band = b.band)::numeric, 3) AS ratio
FROM (SELECT DISTINCT band, r FROM bs_probe) b ORDER BY b.band;
