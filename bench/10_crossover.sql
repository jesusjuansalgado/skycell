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
                                           seed float8 DEFAULT 0.19, reps int DEFAULT 3)
RETURNS void
LANGUAGE plpgsql AS $$
DECLARE cls text; ms text[]; i int; rep int; j json; p json; nn bigint; ctr record;
        acc jsonb := '{}'::jsonb; k text;
BEGIN
  DELETE FROM bench_cross b WHERE b.rows_m = bench_cross_run.rows_m;

  PERFORM setseed(seed);
  DROP TABLE IF EXISTS oc_centers;
  CREATE TEMP TABLE oc_centers AS
  SELECT row_number() OVER () AS qid, s_ra AS ra, s_dec AS dec
  FROM oc ORDER BY random() LIMIT nq;

  /*
   * Randomized and paired, like the cone benchmark: within a trial the two
   * translations answer the same query back to back in an order drawn for that
   * trial, and each is warmed on its own query first.  Measuring one method's
   * whole set and then the other's gives whichever went second a warmer cache
   * -- on this workload that was worth more than the difference between them,
   * and reversing the order reversed the verdict.
   */
  CREATE TEMP TABLE IF NOT EXISTS cross_raw (class text, method text, ms float8, n bigint)
    ON COMMIT DROP;
  DELETE FROM cross_raw;

  FOREACH cls IN ARRAY ARRAY['Q05', 'Q06', 'Q07', 'Q12'] LOOP
    FOR rep IN 1 .. reps LOOP
      FOR ctr IN SELECT * FROM oc_centers ORDER BY random() LOOP
        SELECT array_agg(m ORDER BY random()) INTO ms
        FROM unnest(ARRAY['pgsphere', 'skycell']) m;
        FOR i IN 1 .. 2 LOOP
          EXECUTE oc_sql(cls, ms[i], ctr.ra, ctr.dec) INTO nn;     -- warm this one
          EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) '
                  || oc_sql(cls, ms[i], ctr.ra, ctr.dec) INTO j;
          p := j -> 0 -> 'Plan';
          INSERT INTO cross_raw VALUES (cls, ms[i],
            (j -> 0 ->> 'Planning Time')::float8 + (j -> 0 ->> 'Execution Time')::float8,
            (p ->> 'Actual Rows')::float8::bigint);
          INSERT INTO bench_cross
          SELECT rows_m, cls, ms[i], (j -> 0 ->> 'Planning Time')::float8,
                 (j -> 0 ->> 'Execution Time')::float8,
                 (p ->> 'Shared Hit Blocks')::float8 + (p ->> 'Shared Read Blocks')::float8,
                 nn;
        END LOOP;
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
