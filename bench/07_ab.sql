-- Controlled A/B/C cone benchmark.
--
-- The phase-ordered grid in 03_cone.sql measures one method at a time, which
-- lets a method measured later profit from a warmer cache.  This file removes
-- that: every query is a *trial* in which all three methods run back to back in
-- a random order, and the analysis is paired (same query, same repetition, so
-- the difficulty of the query cancels).  Repetitions give a distribution to put
-- an interval on instead of a single median.
--
--   bench_ab   -- one row per (corpus, cache, rep, query, method)
--   bench_ab_x -- planning/execution split and buffers, also randomized
--
-- `cache` is 'warm' (a warming pass first, several repetitions) or 'cold' (the
-- server restarted and the VM page cache dropped by run.sh, one repetition:
-- rep 2 would no longer be cold).
\set ON_ERROR_STOP 1

CREATE TABLE IF NOT EXISTS bench_ab (
  corpus text, cache text, rep int, qid bigint, label text,
  method text, slot int, n bigint, ms float8);
CREATE TABLE IF NOT EXISTS bench_ab_x (
  corpus text, qid bigint, label text, method text,
  est float8, act float8, buffers bigint, plan_ms float8, exec_ms float8);

/*
 * The settings a method name stands for, applied before its query.
 *
 * skycell@<x> is skycell with skycell.range_cost set to <x> for this query, so
 * that two settings can be compared inside a trial rather than across runs.
 * Comparing them across runs measures machine drift: the cost curve is flat to
 * ~10% over a factor 30 in this parameter, which is the same size as the drift.
 * skycell#<n> sets skycell.probe_orders.  skycell is skycell's defaults, which
 * answer a constant cone with its custom scan (skycell.custom_scan); skycell-rw
 * is the range rewrite instead, and skycell+cs names the custom scan
 * explicitly.  The suffixes combine (skycell-rw@30).  Every skycell method sets
 * custom_scan and range_cost explicitly, since set_config(..., true) lasts for
 * the rest of the transaction and a run is one transaction.
 */
CREATE OR REPLACE FUNCTION ab_method_gucs(method text) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  IF method LIKE 'skycell%' THEN
    PERFORM set_config('skycell.custom_scan',
                       CASE WHEN method LIKE 'skycell-rw%' THEN 'off' ELSE 'on' END, true);
    PERFORM set_config('skycell.range_cost',
                       CASE WHEN method LIKE '%@%' THEN split_part(method, '@', 2) ELSE '-1' END, true);
    IF method LIKE '%#%' THEN
      PERFORM set_config('skycell.probe_orders', split_part(method, '#', 2), true);
    END IF;
  END IF;
END $$;

-- one trial: the methods on the same query, in the given order
CREATE OR REPLACE FUNCTION ab_trial(corpus text, cache text, rep int, c bench_centers,
                                    methods text[]) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE i int; n bigint; s float8; t0 timestamptz;
BEGIN
  FOR i IN 1 .. array_length(methods, 1) LOOP
    PERFORM ab_method_gucs(methods[i]);
    t0 := clock_timestamp();
    EXECUTE cone_sql(methods[i], c) INTO n, s;
    INSERT INTO bench_ab VALUES (corpus, cache, rep, c.qid, c.label, methods[i], i, n,
                                 extract(epoch FROM clock_timestamp() - t0) * 1000);
  END LOOP;
END $$;

/*
 * reps repetitions over every query.  The method order is drawn per trial, so
 * no method is systematically first; `slot` records where each one landed so
 * that the order effect can be measured rather than assumed away.
 */
DROP FUNCTION IF EXISTS bench_ab_run(text, text, int, float8);
CREATE OR REPLACE FUNCTION bench_ab_run(corpus text, cache text, reps int,
                                        seed float8 DEFAULT 0.31,
                                        methods text[] DEFAULT ARRAY['q3c', 'pgsphere', 'skycell'])
RETURNS void
LANGUAGE plpgsql AS $$
DECLARE c bench_centers; r int; ms text[];
BEGIN
  DELETE FROM bench_ab b WHERE b.corpus = bench_ab_run.corpus AND b.cache = bench_ab_run.cache;
  PERFORM setseed(seed);

  -- warm cache: one untimed pass so every method starts from the same state
  IF cache = 'warm' THEN
    FOR c IN SELECT * FROM bench_centers ORDER BY qid LOOP
      PERFORM ab_trial(corpus, 'discard', 0, c, methods);
    END LOOP;
    DELETE FROM bench_ab b WHERE b.cache = 'discard';
  END IF;

  FOR r IN 1 .. reps LOOP
    FOR c IN SELECT * FROM bench_centers ORDER BY random() LOOP
      SELECT array_agg(m ORDER BY random()) INTO ms FROM unnest(methods) m;
      PERFORM ab_trial(corpus, cache, r, c, ms);
    END LOOP;
  END LOOP;
END $$;

-- planning/execution split, in randomized trial order as well
DROP FUNCTION IF EXISTS bench_ab_explain(text, float8);
CREATE OR REPLACE FUNCTION bench_ab_explain(corpus text, seed float8 DEFAULT 0.41,
                                            methods text[] DEFAULT ARRAY['q3c', 'pgsphere', 'skycell'])
RETURNS void
LANGUAGE plpgsql AS $$
DECLARE c bench_centers; ms text[]; i int; j json; p json; scan json;
BEGIN
  DELETE FROM bench_ab_x b WHERE b.corpus = bench_ab_explain.corpus;
  PERFORM setseed(seed);
  FOR c IN SELECT * FROM bench_centers ORDER BY random() LOOP
    SELECT array_agg(m ORDER BY random()) INTO ms FROM unnest(methods) m;
    FOR i IN 1 .. array_length(ms, 1) LOOP
      PERFORM ab_method_gucs(ms[i]);
      EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) '
              || cone_sql(ms[i], c) INTO j;
      p := j -> 0 -> 'Plan';
      scan := coalesce(p -> 'Plans' -> 0, p);
      INSERT INTO bench_ab_x VALUES (corpus, c.qid, c.label, ms[i],
        (scan ->> 'Plan Rows')::float8, (scan ->> 'Actual Rows')::float8,
        (p ->> 'Shared Hit Blocks')::bigint + (p ->> 'Shared Read Blocks')::bigint,
        (j -> 0 ->> 'Planning Time')::float8, (j -> 0 ->> 'Execution Time')::float8);
    END LOOP;
  END LOOP;
END $$;
