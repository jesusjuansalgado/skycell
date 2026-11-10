-- Partitioned catalogues (0.44). A partition plans the custom scan as a
-- plain table does: a constant cone, polygon or composite, and a cross-match,
-- on every partition. On a catalogue partitioned by cell range, a cone
-- carries the interval of cells it can hold (skycell_cone_cell_lo/hi, SQL
-- 0.40), from which PostgreSQL prunes the partitions it cannot reach: at plan
-- time for a constant cone, at every rescan for a cross-match's. A scan that
-- runs clips its coverings to the keys its index holds, so a cone over another
-- partition's part of the sky reads none of its index. Every answer must be
-- the unpartitioned table's.
CREATE EXTENSION IF NOT EXISTS skycell;
SET extra_float_digits = 0;
SET max_parallel_workers_per_gather = 0;

-- a deterministic Fibonacci-sphere catalogue, 30,000 rows, plus 2,000 rows
-- clustered round (120, 30)
CREATE TABLE pt_flat AS
SELECT i AS id, mod(i * 137.50776405003785, 360.0) AS ra,
       degrees(asin(1 - 2 * (i + 0.5) / 30000.0)) AS dec
FROM generate_series(0, 29999) i
UNION ALL
SELECT 30000 + i, 120 + 0.5 * sin(i * 1.7), 30 + 0.5 * cos(i * 2.3) * sin(i * 0.7)
FROM generate_series(0, 1999) i;
CREATE INDEX pt_flat_cell ON pt_flat (skycell_ang2cell(ra, dec));
ANALYZE pt_flat;

-- by cell range, as an archive partitions by HEALPix index or by Gaia's
-- source_id: four quarters of the key space and an empty partition
CREATE TABLE pt_cell (id int, ra float8, dec float8) PARTITION BY RANGE (skycell_ang2cell(ra, dec));
CREATE TABLE pt_cell0 PARTITION OF pt_cell FOR VALUES FROM (MINVALUE) TO (864691128455135232);
CREATE TABLE pt_cell1 PARTITION OF pt_cell FOR VALUES FROM (864691128455135232) TO (1729382256910270464);
CREATE TABLE pt_cell2 PARTITION OF pt_cell FOR VALUES FROM (1729382256910270464) TO (2594073385365405696);
CREATE TABLE pt_cell3 PARTITION OF pt_cell FOR VALUES FROM (2594073385365405696) TO (3000000000000000000);
CREATE TABLE pt_cell4 PARTITION OF pt_cell FOR VALUES FROM (3000000000000000000) TO (MAXVALUE);
INSERT INTO pt_cell SELECT * FROM pt_flat;
CREATE INDEX ON pt_cell (skycell_ang2cell(ra, dec));
ANALYZE pt_cell;

-- by declination band, every band holding cells of the whole key space; a
-- sub-partitioned band, a default partition for the NULL declinations, and a
-- polar cap whose index is missing (its scan stays sequential, and correct)
CREATE TABLE pt_dec (id int, ra float8, dec float8) PARTITION BY RANGE (dec);
CREATE TABLE pt_dec_s PARTITION OF pt_dec FOR VALUES FROM (-90) TO (-30);
CREATE TABLE pt_dec_m PARTITION OF pt_dec FOR VALUES FROM (-30) TO (30) PARTITION BY RANGE (ra);
CREATE TABLE pt_dec_m1 PARTITION OF pt_dec_m FOR VALUES FROM (0) TO (180);
CREATE TABLE pt_dec_m2 PARTITION OF pt_dec_m FOR VALUES FROM (180) TO (360);
CREATE TABLE pt_dec_n PARTITION OF pt_dec FOR VALUES FROM (30) TO (70);
CREATE TABLE pt_dec_p PARTITION OF pt_dec FOR VALUES FROM (70) TO (90.0001);
CREATE TABLE pt_dec_d PARTITION OF pt_dec DEFAULT;
INSERT INTO pt_dec SELECT * FROM pt_flat;
INSERT INTO pt_dec VALUES (-1, 10, NULL);
INSERT INTO pt_flat VALUES (-1, 10, NULL);
CREATE INDEX ON pt_dec_s (skycell_ang2cell(ra, dec));
CREATE INDEX ON pt_dec_m (skycell_ang2cell(ra, dec));
CREATE INDEX ON pt_dec_n (skycell_ang2cell(ra, dec));
CREATE INDEX ON pt_dec_d (skycell_ang2cell(ra, dec));
ANALYZE pt_dec;
ANALYZE pt_flat;

-- a plain table holding one patch of sky: a cross-match's probes elsewhere
-- read nothing
CREATE TABLE pt_patch AS SELECT * FROM pt_flat WHERE ra BETWEEN 100 AND 140 AND dec BETWEEN 10 AND 50;
CREATE INDEX ON pt_patch (skycell_ang2cell(ra, dec));
ANALYZE pt_patch;

-- probes: catalogue rows nudged by 1", 300 over the sky and 200 in the cluster
CREATE TABLE pt_probe AS
SELECT row_number() OVER ()::int AS pid, ra + 1.0 / 3600 AS ra, dec, 0.0005 * (1 + id % 7) AS r
FROM pt_flat WHERE dec IS NOT NULL AND (id % 100 = 0 OR (id >= 31000 AND id % 5 = 0));
ANALYZE pt_probe;

-- the answer on a partitioned table against the unpartitioned one's (on the
-- patch, against its own with the custom scan off), and how many of the
-- relation's scans can be the custom scan (sequential scans off for that
-- count only: wide cones on these small tables are near a tie with them)
CREATE FUNCTION pt_cmp(q text, tbl text, OUT same bool, OUT custom int, OUT scans int)
LANGUAGE plpgsql AS $$
DECLARE a text; b text; l text;
BEGIN
  IF tbl = 'pt_patch' THEN
    PERFORM set_config('skycell.custom_scan', 'off', true);
    EXECUTE 'SELECT (' || replace(q, 'CAT', tbl) || ')::text' INTO a;
    PERFORM set_config('skycell.custom_scan', 'on', true);
  ELSE
    EXECUTE 'SELECT (' || replace(q, 'CAT', 'pt_flat') || ')::text' INTO a;
  END IF;
  EXECUTE 'SELECT (' || replace(q, 'CAT', tbl) || ')::text' INTO b;
  same := a IS NOT DISTINCT FROM b;
  custom := 0; scans := 0;
  PERFORM set_config('enable_seqscan', 'off', true);
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || replace(q, 'CAT', tbl) LOOP
    IF l ~ ('Custom Scan \(SkycellCone\) on ' || tbl) THEN custom := custom + 1; END IF;
    IF l ~ ('Scan.* on ' || tbl) THEN scans := scans + 1; END IF;
  END LOOP;
  PERFORM set_config('enable_seqscan', 'on', true);
END $$;

-- (on pt_dec a cross-match cannot be the inner side of a parameterized
-- Append, its unindexed partition having no such path, and PostgreSQL
-- versions pick different plans instead: its counts are left out)
SELECT name, tbl, same,
       CASE WHEN tbl = 'pt_dec' AND name LIKE 'xmatch%' THEN NULL ELSE custom END AS custom, scans
FROM (VALUES
  ('cone',        'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE 1 = contains(point(''ICRS'', ra, dec), circle(''ICRS'', 120, 30, 0.3))'),
  ('wide cone',   'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE point(''ICRS'', ra, dec) <@ circle(''ICRS'', 100, 20, 50)'),
  ('native cone', 'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 120, 30, 2)'),
  ('distance',    'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE distance(ra, dec, 300, -40) < 4'),
  ('polygon',     'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE 1 = contains(point(''ICRS'', ra, dec), polygon(''ICRS'', 110, 20, 130, 20, 125, 40, 115, 40))'),
  ('box',         'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE 1 = contains(point(''ICRS'', ra, dec), box(''ICRS'', 0, 0, 30, 20))'),
  ('composite',   'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE 1 = contains(point(''ICRS'', ra, dec), region(''DIFFERENCE ICRS (CIRCLE 120 30 3 CIRCLE 120 30 0.2)''))'),
  ('cone or',     'SELECT count(*) || '':'' || sum(id) FROM CAT WHERE 1 = contains(point(''ICRS'', ra, dec), circle(''ICRS'', 120, 30, 1)) OR id = 5'),
  ('xmatch',      'SELECT count(*) || '':'' || sum(c.id) FROM pt_probe p JOIN CAT c ON 1 = contains(point(''ICRS'', c.ra, c.dec), circle(''ICRS'', p.ra, p.dec, 0.001))'),
  ('xmatch r',    'SELECT count(*) || '':'' || sum(c.id) FROM pt_probe p JOIN CAT c ON 1 = contains(point(''ICRS'', c.ra, c.dec), circle(''ICRS'', p.ra, p.dec, p.r))'),
  ('xmatch left', 'SELECT count(*) || '':'' || count(c.id) FROM pt_probe p LEFT JOIN CAT c ON skycell_join(c.ra, c.dec, p.ra, p.dec, 0.001)'),
  ('xmatch exists', 'SELECT count(*) FROM pt_probe p WHERE EXISTS (SELECT 1 FROM CAT c WHERE distance(c.ra, c.dec, p.ra, p.dec) < 0.001)')
) v(name, q), unnest(ARRAY['pt_cell', 'pt_dec', 'pt_patch']) tbl,
LATERAL pt_cmp(q, tbl)
ORDER BY tbl, name;

-- each partition's custom scan in an EXPLAIN ANALYZE, with one of its figures
CREATE FUNCTION pt_scans(q text, field text) RETURNS TABLE (rel text, value text)
LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
  EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS ON, FORMAT JSON) ' || q INTO j;
  RETURN QUERY
    SELECT n->>'Relation Name', coalesce(n->>field, '0')
    FROM jsonb_path_query(j, 'strict $.**') n
    WHERE jsonb_typeof(n) = 'object' AND n->>'Custom Plan Provider' = 'SkycellCone'
    ORDER BY 1;
END $$;

-- a cross-match whose probes all lie in pt_cell1's part of the sky: the
-- other partitions are pruned at every rescan (no loop)
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT * FROM pt_scans($q$
SELECT count(*) FROM (SELECT * FROM pt_probe WHERE skycell_ang2cell(ra, dec) >= 864691128455135232
                        AND skycell_ang2cell(ra, dec) < 1729382256910270464 LIMIT 30) p
JOIN pt_cell c ON 1 = contains(point('ICRS', c.ra, c.dec), circle('ICRS', p.ra, p.dec, 0.001))
$q$, 'Actual Loops');
RESET enable_hashjoin;
RESET enable_mergejoin;
-- a constant cone within one partition (pt_cell4): the others are pruned;
-- polygons and composites by their bounding cap (0.45), the box above too
EXPLAIN (COSTS OFF)
SELECT count(*) FROM pt_cell WHERE 1 = contains(point('ICRS', ra, dec), circle('ICRS', 300, -40, 1));

-- partitioned by a stored cell column: the same pruning, and the bounds the
-- planner added are left out of the scan's filter
CREATE TABLE pt_col (id int, ra float8, dec float8, cell int8) PARTITION BY RANGE (cell);
CREATE TABLE pt_col0 PARTITION OF pt_col FOR VALUES FROM (MINVALUE) TO (1729382256910270464);
CREATE TABLE pt_col1 PARTITION OF pt_col FOR VALUES FROM (1729382256910270464) TO (MAXVALUE);
INSERT INTO pt_col SELECT id, ra, dec, skycell_ang2cell(ra, dec) FROM pt_flat WHERE dec IS NOT NULL;
CREATE INDEX ON pt_col (cell);
ANALYZE pt_col;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM pt_col WHERE skycell_cone(cell, ra, dec, 120, 30, 0.5);
SELECT (SELECT count(*) || ':' || sum(id) FROM pt_col WHERE skycell_cone(cell, ra, dec, 120, 30, 0.5))
     = (SELECT count(*) || ':' || sum(id) FROM pt_flat WHERE skycell_in_cone(ra, dec, 120, 30, 0.5)) AS col_cone,
       (SELECT count(*) || ':' || sum(c.id) FROM pt_probe p JOIN pt_col c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.001))
     = (SELECT count(*) || ':' || sum(c.id) FROM pt_probe p JOIN pt_flat c ON skycell_cone(skycell_ang2cell(c.ra, c.dec), c.ra, c.dec, p.ra, p.dec, 0.001)) AS col_xmatch;

-- the bounds: every cell of a cone lies between them, and a cone holding
-- nothing (a negative radius) gets an empty interval
SELECT count(*) FILTER (WHERE skycell_ang2cell(ra, dec) NOT BETWEEN skycell_cone_cell_lo(120, 30, 2)
                                                        AND skycell_cone_cell_hi(120, 30, 2)) AS outside,
       count(*) AS inside
FROM pt_flat WHERE skycell_in_cone(ra, dec, 120, 30, 2);
-- and so for cones of every size, 0.07 to 88 deg, the poles and RA 0 among
-- the centres (wide ones are bounded by a coarser seed, 0.45)
SELECT count(*) FILTER (WHERE skycell_ang2cell(f.ra, f.dec) NOT BETWEEN skycell_cone_cell_lo(c.ra0, c.dec0, c.r)
                                                            AND skycell_cone_cell_hi(c.ra0, c.dec0, c.r)) AS outside,
       count(*) > 20000 AS many_checked
FROM (SELECT i, mod(i * 97.3, 360.0) ra0, CASE WHEN i % 10 = 0 THEN 89.9 ELSE 80 * sin(i * 1.3) END dec0,
             power(10, -1.2 + 3.15 * ((i * 0.618034) - floor(i * 0.618034))) r
      FROM generate_series(1, 60) i) c
JOIN pt_flat f ON skycell_in_cone(f.ra, f.dec, c.ra0, c.dec0, c.r);
SELECT skycell_cone_cell_lo(10, 20, -1) > skycell_cone_cell_hi(10, 20, -1) AS empty,
       skycell_cone_cell_lo(10, 20, 90) = 0 AS sky_lo,
       skycell_cone_cell_hi(10, 95, 1) = 12 * 4::int8 ^ 29 - 1 AS invalid_is_sky;

-- a cone written as skycell_cone(), with another condition, on the catalogue
-- partitioned by cell: the bounds are added once (until 0.45 the cone took
-- them again each time PostgreSQL simplified the AND they came in, without
-- end, until the server ran out of memory)
SELECT count(*) = (SELECT count(*) FROM pt_flat WHERE skycell_in_cone(ra, dec, 120, 30, 2) AND id % 2 = 0)
         AS written_same
FROM pt_cell WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 120::float8, 30::float8, 2::float8)
                   AND id % 2 = 0;

-- Shard keys (0.45): a coarse cell kept true by a CHECK constraint, the column
-- a sharded database (Citus) distributes by.  A constant cone or region gets
-- the key's cells it meets, and the answer is unchanged.
CREATE TABLE pt_key (id int, ra float8, dec float8,
                     hpx5 bigint NOT NULL CHECK (hpx5 = skycell_ang2cell(ra, dec) >> 48));
INSERT INTO pt_key SELECT id, ra, dec, skycell_ang2cell(ra, dec) >> 48 FROM pt_flat
WHERE ra IS NOT NULL AND dec IS NOT NULL;
CREATE INDEX pt_key_cell ON pt_key (skycell_ang2cell(ra, dec));
ANALYZE pt_key;
CREATE FUNCTION pt_filter(q text, OUT scan text, OUT filter text) LANGUAGE plpgsql AS $$
DECLARE j json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  scan := j -> 0 -> 'Plan' -> 'Plans' -> 0 ->> 'Custom Plan Provider';
  filter := j -> 0 -> 'Plan' -> 'Plans' -> 0 ->> 'Filter';
END $$;
SELECT (pt_filter($q$SELECT count(*) FROM pt_key
  WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2))$q$)).*;
SELECT (pt_filter($q$SELECT count(*) FROM pt_key
  WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100 4/2000'))$q$)).*;
-- w on pt_key against flat (w, unless given) on pt_flat
CREATE FUNCTION pt_keyed(w text, flat text DEFAULT NULL, OUT same bool, OUT keyed bool)
LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; j json;
BEGIN
  EXECUTE format('SELECT count(*) FROM pt_key WHERE %s', w) INTO a;
  EXECUTE format('SELECT count(*) FROM pt_flat WHERE %s', coalesce(flat, w)) INTO b;
  EXECUTE format('EXPLAIN (FORMAT JSON) SELECT count(*) FROM pt_key WHERE %s', w) INTO j;
  same := a = b;
  keyed := j::text LIKE '%hpx5 = ANY%';
END $$;
SELECT t.name, (pt_keyed(t.w, t.flat)).* FROM (VALUES
  ('cone',         $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2))$q$, NULL),
  ('cone written', $q$skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 120::float8, 30::float8, 2::float8) AND id > 5$q$, NULL),
  ('as a worker gets it', $q$skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 120::float8, 30::float8, 2::float8) AND hpx5 = ANY ('{1553,1555,1556,1557,1558,1559,1561,1564,1565}'::bigint[])$q$,
   $q$skycell_in_cone(ra, dec, 120, 30, 2)$q$),
  ('pole',         $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 0, 90, 3))$q$, NULL),
  ('distance',     $q$distance(ra, dec, 200, -40) < 1.5$q$, NULL),
  ('distance of points', $q$distance(POINT('ICRS', ra, dec), POINT('ICRS', 200, -40)) < 1.5$q$, NULL),
  ('polygon',      $q$1 = CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', 200, -10, 215, -8, 208, 4))$q$, NULL),
  ('composite',    $q$1 = CONTAINS(POINT('ICRS', ra, dec), REGION('DIFFERENCE ICRS (CIRCLE 300 60 6 CIRCLE 300 60 3)'))$q$, NULL),
  ('moc',          $q$1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100 4/2000 6/40000'))$q$, NULL),
  ('too many cells', $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 60))$q$, NULL),
  ('not constant', $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, id % 3))$q$, NULL)
) t(name, w, flat);
-- the constraint either way round counts; one not validated does not
ALTER TABLE pt_key DROP CONSTRAINT pt_key_check;
ALTER TABLE pt_key ADD CONSTRAINT pt_key_rev CHECK ((skycell_ang2cell(ra, dec) >> 48) = hpx5);
SELECT (pt_keyed($q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2))$q$)).*;
ALTER TABLE pt_key DROP CONSTRAINT pt_key_rev;
ALTER TABLE pt_key ADD CONSTRAINT pt_key_nv CHECK (hpx5 = skycell_ang2cell(ra, dec) >> 48) NOT VALID;
SELECT (pt_keyed($q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 2))$q$)).*;

-- nearest neighbours: each partition's scan reads a few rows first, and
-- more only as the Merge Append takes them; the same answers as unpartitioned
SELECT k, bool_and(
         (SELECT array_agg(round(d::numeric, 9)) FROM (SELECT DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', c.ra, c.dec)) d
            FROM pt_flat ORDER BY 1 LIMIT k) s)
       = (SELECT array_agg(round(d::numeric, 9)) FROM (SELECT DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', c.ra, c.dec)) d
            FROM pt_cell ORDER BY 1 LIMIT k) s)
       AND
         (SELECT array_agg(round(d::numeric, 9)) FROM (SELECT DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', c.ra, c.dec)) d
            FROM pt_flat ORDER BY 1 LIMIT k) s)
       = (SELECT array_agg(round(d::numeric, 9)) FROM (SELECT DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', c.ra, c.dec)) d
            FROM pt_dec ORDER BY 1 LIMIT k) s)) AS same
FROM (VALUES (120.0, 30.0), (0, 90), (300, -45), (359.99, 0)) c(ra, dec), unnest(ARRAY[1, 32, 33, 500, 3000]) k
GROUP BY k ORDER BY k;

DROP FUNCTION pt_cmp(text, text), pt_scans(text, text), pt_keyed(text, text), pt_filter(text);
DROP TABLE pt_flat, pt_cell, pt_dec, pt_patch, pt_probe, pt_col, pt_key;

-- the helpers (SQL 0.47): a cell's range of order-29 ids, and a table cut
-- into the cells of an order in one call, with a default partition for rows
-- without a position (a NULL cell); searches pruned to the cells they reach
SELECT * FROM skycell_cell_range(0, 0);
SELECT * FROM skycell_cell_range(0, 11);
SELECT skycell_ang2cell(10, 10) BETWEEN (skycell_cell_range(5, skycell_ang2pix(5, 10, 10))).lo
                                    AND (skycell_cell_range(5, skycell_ang2pix(5, 10, 10))).hi AS inside_its_cell;
SELECT * FROM skycell_cell_range(0, 12);                     -- error: no such cell
SELECT * FROM skycell_cell_range(30, 0);                     -- error: order
CREATE TABLE hp_cat (id bigint, ra float8, dec float8) PARTITION BY RANGE (skycell_ang2cell(ra, dec));
SELECT skycell_create_partitions('hp_cat', 1) AS partitions;
SELECT skycell_create_partitions('hp_cat', 1);               -- error: has partitions
INSERT INTO hp_cat SELECT i, mod(i * 137.50776405003785, 360.0)::float8,
  degrees(asin(1 - 2 * (i + 0.5) / 20000.0)) FROM generate_series(0, 19999) i;
INSERT INTO hp_cat VALUES (-1, NULL, 10);
CREATE INDEX ON hp_cat (skycell_ang2cell(ra, dec));
ANALYZE hp_cat;
SELECT tableoid::regclass AS no_position_goes_to FROM hp_cat WHERE id = -1;
SELECT (SELECT count(*) FROM hp_cat WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 3)) = 1)
     = (SELECT count(*) FROM hp_cat WHERE skycell_in_cone(ra, dec, 10, 20, 3)) AS same_rows;
DO $$
DECLARE l text; n int := 0;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT count(*) FROM hp_cat
                    WHERE CONTAINS(POINT(''ICRS'', ra, dec), CIRCLE(''ICRS'', 10, 20, 1)) = 1' LOOP
    IF l ~ ' on hp_cat_' THEN n := n + 1; END IF;
  END LOOP;
  RAISE NOTICE 'a 1 deg cone reads % of the 49 partitions', CASE WHEN n BETWEEN 1 AND 3 THEN 'one to three' ELSE n::text END;
END $$;
CREATE TABLE hp_list (id int, ra float8, dec float8) PARTITION BY LIST (id);
SELECT skycell_create_partitions('hp_list', 0);              -- error: not by range
DROP TABLE hp_cat, hp_list;
