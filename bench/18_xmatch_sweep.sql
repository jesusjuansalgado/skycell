-- Cross-match across outer-table size, match radius and target distribution
-- (referee 2, major 7).
--
-- The paper reported one point: 200k probes at 1" and 10" against one catalogue.
-- The referee asks whether the ordering survives elsewhere, and separates the
-- indexing method from the query formulation.  Both are varied here:
--
--   formulation  q3c_join | skycell_cone join through the custom scan
--                (skycell_cs) or the range rewrite's fixed slots
--                (skycell_slots) | skycell_join, Q3C's spelling, through the
--                custom scan | pgSphere   (skycell_lateral, the ranges-then-
--                index form, remains available but is not run by default)
--   outer size   1k, 10k, 100k probes
--   radius       0.2", 1", 5", 30"
--   targets      drawn from the catalogue (clustered) or uniform on the sphere
--
-- The plan shape is recorded with every measurement: on a wide relation the
-- range join can be mis-planned into a sequential scan, and a timing without
-- the plan beside it is not interpretable.
\set ON_ERROR_STOP 1

CREATE TABLE IF NOT EXISTS xms (n int, radius_arcsec float8, targets text, method text,
  ms float8, rows_out bigint, plan_shape text, timed_out bool);

CREATE OR REPLACE PROCEDURE xms_probes(n int, kind text)
LANGUAGE plpgsql AS $$
BEGIN
  DROP TABLE IF EXISTS xp;
  IF kind = 'clustered' THEN
    EXECUTE format('CREATE TABLE xp AS SELECT ra, dec FROM gaia_realc ORDER BY random() LIMIT %s', n);
  ELSE
    EXECUTE format('CREATE TABLE xp AS SELECT 360*random() AS ra,
                    degrees(asin(2*random()-1)) AS dec FROM generate_series(1, %s)', n);
  END IF;
  ANALYZE xp;
END $$;

CREATE OR REPLACE FUNCTION xms_sql(method text, r_deg float8) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE method
    WHEN 'q3c_join' THEN format(
      'SELECT count(*) FROM xp p, gaia_realc o WHERE q3c_join(p.ra, p.dec, o.ra, o.dec, %s)', r_deg)
    WHEN 'skycell_lateral' THEN format(
      'SELECT count(*) FROM xp p CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, %s) r
       JOIN gaia_realc o ON skycell_ang2cell(o.ra, o.dec) BETWEEN r.lo AND r.hi
       WHERE skycell_in_cone(o.ra, o.dec, p.ra, p.dec, %s)', r_deg, r_deg)
    -- the Q3C-shaped spelling; with skycell.custom_scan on it is the custom
    -- scan's per-probe covering through gaia_realc's expression index
    WHEN 'skycell_join' THEN format(
      'SELECT count(*) FROM xp p, gaia_realc o WHERE skycell_join(o.ra, o.dec, p.ra, p.dec, %s)', r_deg)
    -- skycell_slots and skycell_cs are the same query, the join written with
    -- skycell_cone: the range rewrite's fixed slots (custom_scan off) and the
    -- custom scan's covering per probe (on); xms_run sets the GUC per method
    WHEN 'skycell_slots' THEN format(
      'SELECT count(*) FROM xp p, gaia_realc o
       WHERE skycell_cone(skycell_ang2cell(o.ra, o.dec), o.ra, o.dec, p.ra, p.dec, %s)', r_deg)
    WHEN 'skycell_cs' THEN format(
      'SELECT count(*) FROM xp p, gaia_realc o
       WHERE skycell_cone(skycell_ang2cell(o.ra, o.dec), o.ra, o.dec, p.ra, p.dec, %s)', r_deg)
    WHEN 'pgsphere' THEN format(
      'SELECT count(*) FROM xp p, gaia_realc o
       WHERE o.pos <@ scircle(spoint(radians(p.ra), radians(p.dec)), radians(%s))', r_deg)
  END $$;

DROP FUNCTION IF EXISTS xms_run(int[], float8[], text[], int, int);
CREATE OR REPLACE FUNCTION xms_run(sizes int[], radii float8[], kinds text[],
                                   timeout_ms int DEFAULT 120000, reps int DEFAULT 3,
                                   methods text[] DEFAULT
                                     ARRAY['q3c_join','skycell_cs','skycell_join','skycell_slots','pgsphere'])
RETURNS void LANGUAGE plpgsql AS $$
DECLARE k text; n int; ra float8; m text; q text; t0 timestamptz; nn bigint;
        shape text; plan text; ms float8; to_ bool; rep int;
BEGIN
  DELETE FROM xms;
  FOREACH k IN ARRAY kinds LOOP
   FOREACH n IN ARRAY sizes LOOP
    CALL xms_probes(n, k);
    FOR rep IN 1 .. reps LOOP
    FOREACH ra IN ARRAY radii LOOP
     -- randomized order, per cell: a fixed order lets whichever method runs
     -- last read the heap pages the earlier ones faulted in, and all four
     -- answer the same cones from the same pages.  A first version of this
     -- script ran them in a fixed order and made the method that happened to
     -- run third look 6-45x faster than it is.
     FOR m IN SELECT unnest FROM unnest(methods) ORDER BY random() LOOP
      q := xms_sql(m, ra / 3600.0);
      PERFORM set_config('skycell.custom_scan',
                         CASE WHEN m = 'skycell_slots' THEN 'off' ELSE 'on' END, true);
      -- record what the planner decided, before timing it
      shape := 'unknown';
      BEGIN
        EXECUTE 'EXPLAIN (COSTS OFF, FORMAT JSON) ' || q INTO plan;
        shape := CASE WHEN plan LIKE '%Seq Scan%gaia_realc%' OR plan LIKE '%"Relation Name": "gaia_realc"%Seq Scan%'
                      THEN 'seqscan' ELSE 'index' END;
        IF plan LIKE '%Seq Scan%' AND plan NOT LIKE '%Index Scan%' AND plan NOT LIKE '%Bitmap Index Scan%'
          THEN shape := 'seqscan'; ELSE shape := 'index'; END IF;
      EXCEPTION WHEN OTHERS THEN shape := 'explain_failed'; END;
      PERFORM set_config('statement_timeout', timeout_ms::text, true);
      to_ := false; ms := NULL; nn := NULL;
      BEGIN
        EXECUTE q INTO nn;                       -- warm this method on this cell
        t0 := clock_timestamp();
        EXECUTE q INTO nn;                       -- then time it
        ms := extract(epoch FROM clock_timestamp() - t0) * 1000;
      EXCEPTION WHEN query_canceled THEN to_ := true; ms := timeout_ms; END;
      PERFORM set_config('statement_timeout', '0', true);
      INSERT INTO xms VALUES (n, ra, k, m, ms, nn, shape, to_);
     END LOOP;
    END LOOP;
    END LOOP;
   END LOOP;
  END LOOP;
END $$;
