\set ON_ERROR_STOP 1
SELECT set_config('iso.method', :'method', false);
SELECT set_config('iso.phase',  :'phase',  false);

DO $$
DECLARE c record; n bigint; j json; p json; k int := 0;
        m text := current_setting('iso.method');
        ph text := current_setting('iso.phase');
BEGIN
  DELETE FROM cmp_iso WHERE method = m AND phase = ph;

  -- untimed warm sweep, this method only
  FOR c IN SELECT * FROM c20c ORDER BY qid LOOP
    EXECUTE iso_sql(m, c.ra, c.dec) INTO n;
  END LOOP;

  PERFORM pg_stat_reset();            -- statio counts the measured sweep only
  PERFORM setseed(0.77);
  FOR c IN SELECT * FROM c20c ORDER BY random() LOOP
    k := k + 1;
    IF k % 3 = 0 THEN
      EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) '
              || iso_sql(m, c.ra, c.dec) INTO j;
      p := j->0->'Plan';
      INSERT INTO cmp_iso VALUES (ph, m, c.qid,
        (j->0->>'Planning Time')::float8 + (j->0->>'Execution Time')::float8,
        (p->>'Shared Hit Blocks')::float8, (p->>'Shared Read Blocks')::float8);
    ELSE
      EXECUTE iso_sql(m, c.ra, c.dec) INTO n;
    END IF;
  END LOOP;
END $$;

\echo '--- timing / buffers (measured sweep) ---'
SELECT phase, method, count(*) AS measured,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY ms))::numeric,1) AS median_ms,
       round(avg(hit)::numeric,0)  AS pages_cached,
       round(avg(read)::numeric,0) AS pages_from_disk,
       round((100.0*sum(read)/nullif(sum(hit)+sum(read),0))::numeric,1) AS pct_from_disk
FROM cmp_iso WHERE phase = current_setting('iso.phase') AND method = current_setting('iso.method')
GROUP BY phase, method;

\echo '--- per-index I/O (measured sweep only) ---'
SELECT indexrelname, idx_blks_hit, idx_blks_read,
       round(100.0*idx_blks_read/nullif(idx_blks_hit+idx_blks_read,0),1) AS pct_from_disk
FROM pg_statio_user_indexes WHERE indexrelname IN ('oc20_gist','oc20_cell') ORDER BY 1;

\echo '--- shared_buffers residency at end of phase ---'
SELECT c.relname, pg_size_pretty(pg_relation_size(c.oid)) AS size,
       pg_size_pretty(count(b.bufferid)*8192::bigint) AS resident,
       round(100.0*count(b.bufferid)*8192/pg_relation_size(c.oid),1) AS pct_resident
FROM pg_class c LEFT JOIN pg_buffercache b ON b.relfilenode = pg_relation_filenode(c.oid)
WHERE c.relname IN ('oc20','oc20_gist','oc20_cell')
GROUP BY c.oid, c.relname ORDER BY 1;

-- Driver: bench/13_pressure_run.sh -- four phases (pgsphere, skycell, skycell,
-- pgsphere), each preceded by a container restart so shared_buffers starts empty
-- and the index not under test occupies none of it.  Raw output of the run
-- reported in the paper is in bench/results-pressure/iso_results.txt.
--
-- Prerequisites, built once in a container limited to 3 GiB with
-- shared_buffers=768MB:
--   oc20   -- 20,485,632-row ObsCore-shaped relation (23 columns, 22 GB heap)
--   oc20_gist ON oc20 USING gist (pos)                    -- 1162 MB, 59.5 B/row
--   oc20_cell ON oc20 (skycell_ang2cell(s_ra, s_dec))     --  439 MB, 22.5 B/row
--   c20c   -- 3500 uniformly drawn centres (covering the sphere about once)
