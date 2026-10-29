-- Nearest neighbours: ORDER BY distance(...) [LIMIT k] from the cell index
-- (SkycellNearest).  Every answer must be the one PostgreSQL's own sort
-- gives: the same distances in the same order, and the same rows, except
-- among rows tied at the first or last distance of the window, whose order a
-- sort leaves open.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;
SET max_parallel_workers_per_gather = 0;

-- a catalogue spread over the sky (a golden-angle spiral), plus: duplicated
-- positions (ties), rows with a NULL coordinate, and a dense knot just past
-- RA 0 near the north pole.  nn_cat's heap is out of cell order (the bitmap
-- mode), nn_sorted's in it (the ordered mode).
CREATE TABLE nn_cat AS
SELECT i AS id,
       mod(i * 137.50776405003785, 360.0)::float8 AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 20000.0)) AS dec
FROM generate_series(0, 19999) i;
INSERT INTO nn_cat SELECT 20000 + 10 * id + k, ra, dec FROM nn_cat, generate_series(1, 3) k
  WHERE id % 500 = 1;
INSERT INTO nn_cat SELECT 30000 + i, NULL, 10 FROM generate_series(1, 5) i;
INSERT INTO nn_cat SELECT 30100 + i, 20, NULL FROM generate_series(1, 5) i;
INSERT INTO nn_cat SELECT 31000 + i, mod(360 + (i % 40 - 20) * 0.001, 360), 89.99 - (i / 40) * 0.0005
  FROM generate_series(0, 1999) i;
ALTER TABLE nn_cat ADD COLUMN pos skypos, ADD COLUMN mag float8;
UPDATE nn_cat SET pos = skycell_point(ra, dec) WHERE ra IS NOT NULL AND dec IS NOT NULL;
UPDATE nn_cat SET mag = mod(id * 7919, 1000) / 50.0;
CREATE TABLE nn_sorted AS SELECT * FROM nn_cat ORDER BY skycell_ang2cell(ra, dec);
CREATE INDEX nn_cat_cell ON nn_cat (skycell_ang2cell(ra, dec));
CREATE INDEX nn_cat_pcell ON nn_cat (skycell_cell(pos));
CREATE INDEX nn_sorted_cell ON nn_sorted (skycell_ang2cell(ra, dec));
ANALYZE nn_cat;
ANALYZE nn_sorted;

CREATE FUNCTION nn_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; p text := '';
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'SkycellNearest' THEN p := p || 'nearest '; END IF;
    IF l ~ 'Mode: ' THEN p := p || substring(l from 'Mode: ([a-z]+)') || ' '; END IF;
    IF l ~ '^(.*-> +| *)Sort *$' THEN p := p || 'sort '; END IF;
    IF l ~ '^(.*-> +| *)Incremental Sort *$' THEN p := p || 'incremental '; END IF;
  END LOOP;
  RETURN rtrim(p);
END $$;

-- q returns (id, d), in the query's order; compared with the sort's answer
CREATE FUNCTION nn_same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a float8[]; b float8[]; ia int[]; ib int[]; lo float8; hi float8; n int;
BEGIN
  PERFORM set_config('skycell.nearest_scan', 'on', true);
  EXECUTE format('SELECT array_agg(d ORDER BY o), array_agg(id ORDER BY o)
                  FROM (SELECT row_number() OVER () o, * FROM (%s) s) t', q) INTO a, ia;
  PERFORM set_config('skycell.nearest_scan', 'off', true);
  EXECUTE format('SELECT array_agg(d ORDER BY o), array_agg(id ORDER BY o)
                  FROM (SELECT row_number() OVER () o, * FROM (%s) s) t', q) INTO b, ib;
  PERFORM set_config('skycell.nearest_scan', 'on', true);
  IF a IS DISTINCT FROM b THEN
    RETURN false;
  END IF;
  n := coalesce(array_length(a, 1), 0);
  IF n = 0 THEN
    RETURN true;
  END IF;
  lo := a[1];
  hi := a[n];
  RETURN (SELECT array_agg(x ORDER BY x) FROM unnest(ia, a) u(x, d)
          WHERE d IS DISTINCT FROM lo AND d IS DISTINCT FROM hi) IS NOT DISTINCT FROM
         (SELECT array_agg(x ORDER BY x) FROM unnest(ib, b) u(x, d)
          WHERE d IS DISTINCT FROM lo AND d IS DISTINCT FROM hi);
END $$;

-- the spellings the scan answers, and the ones it leaves to a sort
SELECT nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5') AS dist,
       nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(10, 20, ra, dec) LIMIT 5') AS dist_swapped,
       nn_plan('SELECT id FROM nn_cat ORDER BY distance(ra, dec, 10, 20) LIMIT 5') AS distance4,
       nn_plan('SELECT id FROM nn_cat ORDER BY pos <-> skycell_point(10, 20) LIMIT 5') AS op,
       nn_plan('SELECT id FROM nn_cat ORDER BY distance(skycell_point(10, 20), pos) LIMIT 5') AS skypos,
       nn_plan($q$SELECT id FROM nn_cat ORDER BY distance(point('ICRS', ra, dec), point('ICRS', 10, 20)) LIMIT 5$q$) AS adql,
       nn_plan('SELECT id FROM nn_sorted ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5') AS sorted_heap;
SELECT nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20)') AS no_limit,
       nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) DESC LIMIT 5') AS descending,
       nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) NULLS FIRST LIMIT 5') AS nulls_first,
       nn_plan('SELECT ctid FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5') AS system_column,
       nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5 FOR UPDATE') AS locked,
       nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, ra, 20) LIMIT 5') AS centre_reads_row;
SELECT nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20), id LIMIT 5') AS second_key;
SET skycell.nearest_scan = off;
SELECT nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5') AS guc_off;
RESET skycell.nearest_scan;
SET skycell.custom_scan = off;
SELECT nn_plan('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5') AS custom_scan_off;
RESET skycell.custom_scan;

-- centres all over: the poles, either side of RA 0, the knot, and k from one
-- row to past the whole relation (the NULL distances last)
CREATE TABLE nn_centre (cid int, ra float8, dec float8);
INSERT INTO nn_centre VALUES (1, 10, 20), (2, 0, 89.99), (3, 359.9995, 89.9), (4, 0, -90),
  (5, 180, 0), (6, 359.99, -45), (7, 0.001, 0), (8, 250, 60);
SELECT t.tbl, count(*) AS queries, count(*) FILTER (WHERE nn_same(q)) AS same,
       count(*) FILTER (WHERE nn_plan(q) ~ 'nearest') AS nearest
FROM (SELECT tbl, format('SELECT id, skycell_dist(ra, dec, %s, %s) d FROM %s ORDER BY 2 LIMIT %s',
                         c.ra, c.dec, tbl, k) q
      FROM nn_centre c, unnest(ARRAY[1, 7, 50, 1000]) k, unnest(ARRAY['nn_cat', 'nn_sorted']) tbl) t
GROUP BY 1 ORDER BY 1;

-- the other spellings over the same centres
SELECT count(*) AS queries, count(*) FILTER (WHERE nn_same(q)) AS same,
       count(*) FILTER (WHERE nn_plan(q) ~ 'nearest') AS nearest
FROM (SELECT format('SELECT id, pos <-> skycell_point(%s, %s) d FROM nn_cat ORDER BY pos <-> skycell_point(%s, %s) LIMIT %s',
                    c.ra, c.dec, c.ra, c.dec, k) q FROM nn_centre c, unnest(ARRAY[3, 300]) k
      UNION ALL
      SELECT format('SELECT id, distance(point(''ICRS'', %s, %s), point(''ICRS'', ra, dec)) d FROM nn_cat ORDER BY 2 LIMIT %s',
                    c.ra, c.dec, k) FROM nn_centre c, unnest(ARRAY[3, 300]) k) t;

-- with conditions: one the planner estimates, and the nearest rows past a
-- distance, which takes several rings (and their interiors left out)
SELECT count(*) AS queries, count(*) FILTER (WHERE nn_same(q)) AS same,
       count(*) FILTER (WHERE nn_plan(q) ~ 'nearest') AS nearest
FROM (SELECT format('SELECT id, skycell_dist(ra, dec, %s, %s) d FROM nn_cat WHERE mag < 2 ORDER BY 2 LIMIT %s',
                    c.ra, c.dec, k) q FROM nn_centre c, unnest(ARRAY[5, 40]) k
      UNION ALL
      SELECT format('SELECT id, skycell_dist(ra, dec, %s, %s) d FROM nn_cat WHERE skycell_dist(ra, dec, %s, %s) > %s ORDER BY 2 LIMIT 20',
                    c.ra, c.dec, c.ra, c.dec, r) FROM nn_centre c, unnest(ARRAY[3, 40, 120]) r) t;
CREATE FUNCTION nn_rings(q text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l ~ 'Rings: ' THEN RETURN substring(l from 'Rings: ([0-9.]+)')::float8; END IF;
  END LOOP;
  RETURN NULL;
END $$;
SELECT nn_rings('SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 5') AS one_ring,
       nn_rings('SELECT id FROM nn_cat WHERE skycell_dist(ra, dec, 10, 20) > 40
                 ORDER BY skycell_dist(ra, dec, 10, 20) LIMIT 20') > 2 AS several_rings;

-- past the relation: every row, NULL distances last, through the final ring
-- (forced: a sort is cheaper); OFFSET into the tail; a tiny work_mem, so that
-- a ring outgrows memory and goes to a tuplesort
SET enable_sort = off;
SELECT nn_same('SELECT id, skycell_dist(ra, dec, 10, 20) d FROM nn_cat ORDER BY 2 LIMIT 30000') AS all_rows,
       nn_same('SELECT id, skycell_dist(ra, dec, 10, 20) d FROM nn_cat ORDER BY 2 OFFSET 21990 LIMIT 40') AS tail,
       nn_plan('SELECT id, skycell_dist(ra, dec, 10, 20) d FROM nn_cat ORDER BY 2 LIMIT 30000') AS plan;
SET work_mem = '64kB';
SELECT nn_same('SELECT id, skycell_dist(ra, dec, 10, 20) d FROM nn_cat ORDER BY 2 LIMIT 30000') AS spilled,
       nn_same('SELECT id, skycell_dist(ra, dec, 250, 60) d FROM nn_cat ORDER BY 2 LIMIT 3000') AS spilled_ring;
RESET work_mem;
-- no centre: a NULL one, or one no cone can be drawn round, leaves every
-- distance to the expression, in one ring that is the relation
SELECT nn_same('SELECT id, skycell_dist(ra, dec, NULL, 20) d FROM nn_cat ORDER BY 2 LIMIT 10') AS null_centre,
       nn_same('SELECT id, skycell_dist(ra, dec, 10, 95) d FROM nn_cat ORDER BY 2 LIMIT 10') AS invalid_centre,
       nn_same($q$SELECT id, skycell_dist(ra, dec, 'Infinity', 20) d FROM nn_cat ORDER BY 2 LIMIT 10$q$) AS infinite_centre;
RESET enable_sort;

-- a generic plan: the centre and the LIMIT are parameters
CREATE FUNCTION nn_param(r0 float8, d0 float8, k int) RETURNS TABLE (id int, d float8) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT c.id, skycell_dist(c.ra, c.dec, r0, d0) FROM nn_cat c
    ORDER BY skycell_dist(c.ra, c.dec, r0, d0) LIMIT k;
END $$;
SET plan_cache_mode = force_generic_plan;
PREPARE nn_p(float8, float8, int) AS SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, $1, $2) LIMIT $3;
SELECT nn_plan('EXECUTE nn_p(10, 20, 5)') AS generic;
DEALLOCATE nn_p;
SELECT count(*) AS queries,
       count(*) FILTER (WHERE nn_same(format('SELECT * FROM nn_param(%s, %s, %s)', c.ra, c.dec, k))) AS same
FROM nn_centre c, unnest(ARRAY[1, 25, 400]) k;
RESET plan_cache_mode;

-- per row of another relation: LATERAL, the scan rescanned with a new centre
SELECT nn_plan('SELECT c.cid, n.id FROM nn_centre c CROSS JOIN LATERAL
  (SELECT id FROM nn_cat ORDER BY skycell_dist(ra, dec, c.ra, c.dec) LIMIT 3) n') AS lateral;
SELECT nn_same('SELECT n.id, n.d FROM nn_centre c CROSS JOIN LATERAL
  (SELECT id, skycell_dist(ra, dec, c.ra, c.dec) d FROM nn_cat ORDER BY 2 LIMIT 3) n ORDER BY c.cid, n.d') AS lateral_same;

-- inside a parallel worker
SET max_parallel_workers_per_gather = 2;
SET debug_parallel_query = on;
SELECT nn_same('SELECT id, skycell_dist(ra, dec, 10, 20) d FROM nn_cat ORDER BY 2 LIMIT 50') AS in_worker;
RESET debug_parallel_query;
SET max_parallel_workers_per_gather = 0;

DROP FUNCTION nn_param(float8, float8, int);
DROP FUNCTION nn_rings(text);
DROP FUNCTION nn_same(text);
DROP FUNCTION nn_plan(text);
DROP TABLE nn_centre, nn_sorted, nn_cat;
