-- Shard keys (0.45): the catalogue distributed by its order-5 cell, kept true
-- by a CHECK constraint, against the same catalogue distributed by id.  How
-- many shards each query goes to, and whether the rows are the single node's.
\pset pager off
DROP TABLE IF EXISTS cat_key;
CREATE TABLE cat_key (id int, ra float8, dec float8, mag real,
                      hpx5 bigint NOT NULL CHECK (hpx5 = skycell_ang2cell(ra, dec) >> 48));
SELECT create_distributed_table('cat_key', 'hpx5', shard_count := 32);
INSERT INTO cat_key SELECT id, ra, dec, mag, skycell_ang2cell(ra, dec) >> 48 FROM cat_local;
CREATE INDEX ON cat_key (skycell_ang2cell(ra, dec));
ANALYZE cat_key;

-- the shards a query is sent to, from Citus's own plan
CREATE OR REPLACE FUNCTION shard_tasks(q text) RETURNS int LANGUAGE plpgsql AS $f$
DECLARE j json; n int;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  SELECT (regexp_match(j::text, '"Task Count": ([0-9]+)'))[1]::int INTO n;
  RETURN n;
END $f$;

CREATE TEMP TABLE kq (name text, w text);
INSERT INTO kq VALUES
 ('cone 0.1 deg',  $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 0.1))$q$),
 ('cone 1 deg',    $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 1))$q$),
 ('cone 5 deg',    $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 5))$q$),
 ('cone 20 deg',   $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 20))$q$),
 ('pole 3 deg',    $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 0, 90, 3))$q$),
 ('polygon',       $q$1 = CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', 200, -10, 215, -8, 208, 4))$q$),
 ('composite',     $q$1 = CONTAINS(POINT('ICRS', ra, dec), REGION('DIFFERENCE ICRS (CIRCLE 300 60 6 CIRCLE 300 60 3)'))$q$),
 ('moc',           $q$1 = CONTAINS(POINT('ICRS', ra, dec), skycell_moc_region('3/100-110 5/2000-2010'))$q$),
 ('distance',      $q$distance(ra, dec, 200, -40) < 1.5$q$),
 ('cone and mag',  $q$1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 120, 30, 5)) AND mag < 5$q$);

-- shards: of 8 for cat_dist (by id), of 32 for cat_key (by cell)
DO $d$
DECLARE r record; a text; b text; c text;
BEGIN
  CREATE TEMP TABLE kres (name text, shards_by_id int, shards_by_cell int, rows text, same bool);
  FOR r IN SELECT * FROM kq LOOP
    EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM cat_local WHERE %s', r.w) INTO a;
    EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM cat_dist WHERE %s', r.w) INTO b;
    EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) FROM cat_key WHERE %s', r.w) INTO c;
    INSERT INTO kres VALUES (r.name, shard_tasks(format('SELECT id FROM cat_dist WHERE %s', r.w)),
                             shard_tasks(format('SELECT id FROM cat_key WHERE %s', r.w)), a, a = b AND a = c);
  END LOOP;
END $d$;
SELECT * FROM kres;
