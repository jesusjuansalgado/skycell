-- 10M ObsCore relation: pgSphere as planned (index scan), pgSphere forced to a
-- bitmap heap scan on the same GiST, and skycell; Q05/Q06/Q07/Q12 radii and
-- cuts as 10_crossover.sql, 30 stored centres, warmed then EXPLAINed, methods
-- in random order per centre, 3 repetitions
CREATE TEMP TABLE bm (cls text, m text, ms float8, plan float8, buf int, rd int, n bigint, node text);
CREATE OR REPLACE FUNCTION bm_run(reps int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record; cls text; m text; rep int; q text; j json; p json;
BEGIN
  PERFORM setseed(0.08);
  FOREACH cls IN ARRAY ARRAY['Q05','Q06','Q07','Q12'] LOOP
    FOR rep IN 1 .. reps LOOP
      FOR c IN SELECT * FROM oc_queries ORDER BY qid LIMIT 30 LOOP
        FOR m IN SELECT x FROM unnest(ARRAY['pgsphere','pgsphere-bitmap','skycell']) x ORDER BY random() LOOP
          PERFORM set_config('enable_indexscan', CASE m WHEN 'pgsphere-bitmap' THEN 'off' ELSE 'on' END, true);
          q := oc_sql(cls, CASE m WHEN 'skycell' THEN 'skycell' ELSE 'pgsphere' END, c.ra, c."dec");
          EXECUTE q;
          EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) ' || q INTO j;
          p := j -> 0 -> 'Plan';
          INSERT INTO bm VALUES (cls, m, (j->0->>'Planning Time')::float8 + (j->0->>'Execution Time')::float8,
            (j->0->>'Planning Time')::float8, (p->>'Shared Hit Blocks')::int + (p->>'Shared Read Blocks')::int, (p->>'Shared Read Blocks')::int,
            (p->'Plans'->0->>'Actual Rows')::float8::bigint, p->'Plans'->0->>'Node Type');
        END LOOP;
      END LOOP;
    END LOOP;
  END LOOP;
  PERFORM set_config('enable_indexscan', 'on', true);
END $$;
SELECT bm_run(1);   -- warm-up pass, discarded
TRUNCATE bm;
SELECT bm_run(3);
SELECT cls, m, string_agg(DISTINCT node, ',') AS node,
       round((sum(ms) / sum(sum(ms)) FILTER (WHERE m = 'pgsphere') OVER (PARTITION BY cls))::numeric, 2) AS vs_pgsphere_as_planned,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY ms)::numeric, 3) AS median_ms,
       round(avg(buf)) AS buffers, round(avg(rd), 1) AS reads, round(avg(n)) AS rows
FROM bm GROUP BY cls, m ORDER BY cls, m;
