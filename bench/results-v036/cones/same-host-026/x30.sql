CREATE OR REPLACE FUNCTION x30(lab text) RETURNS TABLE(n int, plan_ms float8, exec_ms float8, buffers float8, ranges text, mode text, removed float8)
LANGUAGE plpgsql AS $$
DECLARE c bench_centers; j json; p json; k int := 0; q text;
BEGIN
  plan_ms := 0; exec_ms := 0; buffers := 0; removed := 0; ranges := ''; mode := '';
  FOR c IN SELECT * FROM bench_centers WHERE label = lab ORDER BY qid LOOP
    q := cone_sql('skycell', c);
    EXECUTE q;
    EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) ' || q INTO j;
    p := j->0->'Plan'->'Plans'->0;
    plan_ms := plan_ms + (j->0->>'Planning Time')::float8; exec_ms := exec_ms + (j->0->>'Execution Time')::float8;
    buffers := buffers + (j->0->'Plan'->>'Shared Hit Blocks')::float8;
    removed := removed + coalesce((p->>'Rows Removed by Filter')::float8, 0);
    IF k < 3 THEN ranges := ranges || coalesce(p->>'Ranges', '?') || ' '; mode := mode || coalesce(p->>'Mode', p->>'Node Type') || ' '; END IF;
    k := k + 1;
  END LOOP;
  n := k; RETURN NEXT;
END $$;
