-- Where does the advantage actually come from? (referee 2, question 1)
--
-- The comparison with pgSphere confounds three things at once: an adaptive
-- covering resolution, a compact B-tree over an integer key instead of a GiST
-- over bounding boxes, and a heap laid out in the same space-filling order as
-- the index.  This script separates them.
--
--   factor 1  heap clustering      gaia_realu (random sky order) vs gaia_realc
--   factor 2  adaptive covering    cost-chosen order vs skycell.force_order
--   factor 3  index representation skycell vs q3c (both int8 B-trees) vs gist
--
-- Factor 3 is the one the paper already reports; comparing against q3c rather
-- than pgSphere holds the index structure fixed, so the difference is the
-- covering strategy alone.
\set ON_ERROR_STOP 1

CREATE TABLE IF NOT EXISTS abl (factor text, corpus text, setting text,
  radius float8, method text, ms float8, buffers float8, n bigint);

-- one timed, warmed, EXPLAIN-measured query
CREATE OR REPLACE FUNCTION abl_one(factor text, corpus text, setting text,
                                   tbl text, method text, ra float8, dc float8, r float8)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text; j json; p json; nn bigint;
BEGIN
  q := CASE method
    WHEN 'skycell'  THEN format('SELECT count(*) FROM %s WHERE skycell_cone(skycell_ang2cell(ra,dec), ra, dec, %s, %s, %s)', tbl, ra, dc, r)
    WHEN 'q3c'      THEN format('SELECT count(*) FROM %s WHERE q3c_radial_query(ra, dec, %s, %s, %s)', tbl, ra, dc, r)
    WHEN 'pgsphere' THEN format('SELECT count(*) FROM %s WHERE pos <@ scircle(spoint(radians(%s), radians(%s)), radians(%s))', tbl, ra, dc, r)
  END;
  EXECUTE q INTO nn;                                   -- warm this method on this query
  EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON, FORMAT JSON) ' || q INTO j;
  p := j -> 0 -> 'Plan';
  INSERT INTO abl VALUES (factor, corpus, setting, r, method,
    (j->0->>'Planning Time')::float8 + (j->0->>'Execution Time')::float8,
    (p->>'Shared Hit Blocks')::float8 + (p->>'Shared Read Blocks')::float8, nn);
END $$;

-- factors 1 and 3 together: the same queries on the clustered and unclustered
-- heap, all three methods, paired inside a trial and in randomized order.
CREATE OR REPLACE FUNCTION abl_layout(nq int DEFAULT 25, reps int DEFAULT 3)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record; ms text[]; i int; rep int; r float8; t text;
BEGIN
  DELETE FROM abl WHERE factor IN ('layout');
  PERFORM setseed(0.23);
  DROP TABLE IF EXISTS abl_centres;
  CREATE TEMP TABLE abl_centres AS SELECT ra, dec FROM gaia_realc ORDER BY random() LIMIT nq;
  FOR rep IN 1 .. reps LOOP
    FOR c IN SELECT * FROM abl_centres ORDER BY random() LOOP
      FOREACH r IN ARRAY ARRAY[0.0028, 0.1, 1.0] LOOP
        FOREACH t IN ARRAY ARRAY['gaia_realu', 'gaia_realc'] LOOP
          SELECT array_agg(m ORDER BY random()) INTO ms
          FROM unnest(ARRAY['skycell','q3c','pgsphere']) m;
          FOR i IN 1 .. 3 LOOP
            PERFORM abl_one('layout', t,
              CASE t WHEN 'gaia_realu' THEN 'unclustered' ELSE 'clustered' END,
              t, ms[i], c.ra, c.dec, r);
          END LOOP;
        END LOOP;
      END LOOP;
    END LOOP;
  END LOOP;
END $$;

-- factor 2: the cost model's chosen order against every fixed order.  If the
-- model is worth having, the chosen order must be at or near the best fixed one
-- across radii -- and no single fixed order should win everywhere.
CREATE OR REPLACE FUNCTION abl_order(nq int DEFAULT 15, reps int DEFAULT 3)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record; rep int; r float8; o int;
BEGIN
  DELETE FROM abl WHERE factor = 'order';
  PERFORM setseed(0.31);
  DROP TABLE IF EXISTS abl_centres2;
  CREATE TEMP TABLE abl_centres2 AS SELECT ra, dec FROM gaia_realc ORDER BY random() LIMIT nq;
  FOR rep IN 1 .. reps LOOP
    FOR c IN SELECT * FROM abl_centres2 ORDER BY random() LOOP
      FOREACH r IN ARRAY ARRAY[0.0028, 0.1, 1.0] LOOP
        FOR o IN 3 .. 13 LOOP
          PERFORM set_config('skycell.force_order', o::text, true);
          PERFORM abl_one('order', 'gaia_realc', 'forced_' || o, 'gaia_realc', 'skycell', c.ra, c.dec, r);
        END LOOP;
        PERFORM set_config('skycell.force_order', '-1', true);   -- cost-chosen
        PERFORM abl_one('order', 'gaia_realc', 'chosen', 'gaia_realc', 'skycell', c.ra, c.dec, r);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
