-- Partitioned catalogues (0.44). A partition plans the custom scan as a
-- plain table does: a constant cone, polygon or composite, and a cross-match,
-- on every partition. Each scan clips its coverings to the keys its index
-- holds, so a cone over another partition's part of the sky reads none of its
-- index; a cross-match does not even cover such a cone. Every answer must be
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

SELECT name, tbl, (pt_cmp(q, tbl)).*
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
) v(name, q), unnest(ARRAY['pt_cell', 'pt_dec', 'pt_patch']) tbl
ORDER BY tbl, name;

-- a cross-match whose probes all lie in pt_cell1's part of the sky: the
-- other partitions' scans run once per probe but cover no cone (0 ranges)
SET enable_hashjoin = off;
SET enable_mergejoin = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT count(*) FROM (SELECT * FROM pt_probe WHERE skycell_ang2cell(ra, dec) >= 864691128455135232
                        AND skycell_ang2cell(ra, dec) < 1729382256910270464 LIMIT 30) p
JOIN pt_cell c ON 1 = contains(point('ICRS', c.ra, c.dec), circle('ICRS', p.ra, p.dec, 0.001));
RESET enable_hashjoin;
RESET enable_mergejoin;
-- a constant cone within one partition (pt_cell4): the others read no page
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS ON)
SELECT count(*) FROM pt_cell WHERE 1 = contains(point('ICRS', ra, dec), circle('ICRS', 300, -40, 1));

DROP FUNCTION pt_cmp(text, text);
DROP TABLE pt_flat, pt_cell, pt_dec, pt_patch, pt_probe;
