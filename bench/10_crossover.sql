-- Where does the crossover actually sit? (referee experiment 6)
--
-- The TAP case study found skycell slower than pgSphere on a 500k-row ObsCore
-- table and faster on a 10M-row catalogue, and explained the difference by the
-- size of the relation.  That explanation is testable: build the *same*
-- ObsCore-shaped relation, with the same query classes, at several sizes, and
-- measure where the two cross.
--
-- The rows are observations, not sources: products come in groups that share a
-- field centre, and the table is clustered by collection and field, which is
-- what makes a GiST bitmap scan read so few pages in the TAP corpus.  That
-- structure is reproduced here; field centres are drawn from the current `src`
-- corpus, so the sky distribution matches the catalogue runs.
\set ON_ERROR_STOP 1

CREATE TABLE IF NOT EXISTS bench_cross (rows_m float8, class text, method text,
  plan_ms float8, exec_ms float8, buffers float8, n bigint);

CREATE OR REPLACE PROCEDURE build_obscore(nrows bigint, per_field int DEFAULT 128)
LANGUAGE plpgsql AS $$
DECLARE nfields bigint := greatest(1, nrows / per_field);
BEGIN
  DROP TABLE IF EXISTS oc;
  EXECUTE format(
    'CREATE TABLE oc AS
     SELECT row_number() OVER () AS obs_id,
            f.ra + (random() - 0.5) * 0.4 AS s_ra,
            greatest(-89.9, least(89.9, f.dec + (random() - 0.5) * 0.4)) AS s_dec,
            55000 + (f.rn %% 3000) + random() AS t_min,
            (f.rn %% 3)::int AS calib_level,
            ''C'' || (f.rn %% 8)::text AS obs_collection
     FROM (SELECT row_number() OVER () AS rn, ra, dec FROM src ORDER BY id LIMIT %s) f,
          LATERAL generate_series(1, %s) k
     ORDER BY f.rn %% 8, f.rn', nfields, per_field);

  ALTER TABLE oc ADD COLUMN pos spoint;
  UPDATE oc SET pos = spoint(radians(s_ra), radians(s_dec));
  CREATE INDEX oc_gist ON oc USING gist (pos);
  CREATE INDEX oc_cell ON oc (skycell_ang2cell(s_ra, s_dec));
  ALTER INDEX oc_cell ALTER COLUMN 1 SET STATISTICS 1000;
  ANALYZE oc;
END $$;

/*
 * The four cone classes of the TAP corpus, in both translations.  Q07 adds the
 * non-spatial cuts the corpus applies with the cone; Q12 is a cone so small it
 * returns nothing, which is what a client does when it probes a position.
 */
CREATE OR REPLACE FUNCTION oc_sql(class text, method text, ra float8, "dec" float8) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE class
    WHEN 'Q05' THEN format('SELECT count(*) FROM oc WHERE %s', pred)
    WHEN 'Q06' THEN format('SELECT count(*) FROM oc WHERE %s', pred)
    WHEN 'Q07' THEN format('SELECT count(*) FROM oc WHERE %s AND calib_level >= 1 AND t_min > 55000', pred)
    WHEN 'Q12' THEN format('SELECT count(*) FROM oc WHERE %s', pred)
  END
  FROM (SELECT CASE method
      WHEN 'pgsphere' THEN format('pos <@ scircle(spoint(radians(%s), radians(%s)), radians(%s))', ra, "dec", r)
      ELSE format('skycell_cone(skycell_ang2cell(s_ra, s_dec), s_ra, s_dec, %s, %s, %s)', ra, "dec", r)
    END AS pred
    FROM (SELECT CASE class WHEN 'Q05' THEN 0.15 WHEN 'Q06' THEN 2.0
                            WHEN 'Q07' THEN 0.5 ELSE 0.0002 END AS r) rr) p
$$;

CREATE OR REPLACE FUNCTION bench_cross_run(rows_m float8, nq int DEFAULT 40,
                                           seed float8 DEFAULT 0.19) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE cls text; m text; i int; j json; p json; nn bigint;
        ctr record; pl float8[]; ex float8[]; bu float8[]; last_n bigint;
BEGIN
  DELETE FROM bench_cross b WHERE b.rows_m = bench_cross_run.rows_m;

  -- one set of centres, drawn from the data, used by both translations
  PERFORM setseed(seed);
  DROP TABLE IF EXISTS oc_centers;
  CREATE TEMP TABLE oc_centers AS
  SELECT row_number() OVER () AS qid, s_ra AS ra, s_dec AS dec
  FROM oc ORDER BY random() LIMIT nq;

  FOREACH cls IN ARRAY ARRAY['Q05', 'Q06', 'Q07', 'Q12'] LOOP
    FOR i IN 1 .. 2 LOOP
      m := (ARRAY['pgsphere', 'skycell'])[i];
      pl := '{}'; ex := '{}'; bu := '{}'; last_n := NULL;
      FOR ctr IN SELECT * FROM oc_centers ORDER BY qid LOOP
        EXECUTE oc_sql(cls, m, ctr.ra, ctr.dec) INTO nn;          -- warm
        EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) '
                || oc_sql(cls, m, ctr.ra, ctr.dec) INTO j;
        p := j -> 0 -> 'Plan';
        pl := pl || (j -> 0 ->> 'Planning Time')::float8;
        ex := ex || (j -> 0 ->> 'Execution Time')::float8;
        bu := bu || ((p ->> 'Shared Hit Blocks')::float8 + (p ->> 'Shared Read Blocks')::float8);
        last_n := nn;
      END LOOP;
      INSERT INTO bench_cross
      SELECT rows_m, cls, m,
             (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM unnest(pl) x),
             (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM unnest(ex) x),
             (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM unnest(bu) x),
             last_n;
    END LOOP;
  END LOOP;
END $$;
