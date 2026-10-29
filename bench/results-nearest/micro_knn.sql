-- per centre, method and k: :n EXECUTEs (plan + run) timed together, a fresh
-- centre each time (1e-9 deg steps), so nothing is cached between them
CREATE TEMP TABLE IF NOT EXISTS mk (run text, k int, qid int, method text, us float8, n bigint);
CREATE OR REPLACE FUNCTION mk_sql(m text, tbl text, ra0 float8, de0 float8, k int) RETURNS text LANGUAGE sql AS $$
  SELECT CASE m
    WHEN 'pgsphere' THEN format('SELECT count(*) FROM (SELECT 1 FROM %s %s ORDER BY pos <-> spoint(radians(%s), radians(%s)) LIMIT %s) x', tbl, CASE WHEN k < 0 THEN format('WHERE pos <-> spoint(radians(%s), radians(%s)) > radians(2)', ra0, de0) ELSE '' END, ra0, de0, abs(k))
    ELSE format('SELECT count(*) FROM (SELECT 1 FROM %1$s %7$s ORDER BY skycell_dist(%2$s, %3$s, %4$s, %5$s) LIMIT %6$s) x', tbl,
                CASE WHEN tbl = 'oc' THEN 's_ra' ELSE 'ra' END, CASE WHEN tbl = 'oc' THEN 's_dec' ELSE 'dec' END, ra0, de0, abs(k),
                CASE WHEN k < 0 THEN format('WHERE skycell_dist(%s, %s, %s, %s) > 2', CASE WHEN tbl = 'oc' THEN 's_ra' ELSE 'ra' END,
                     CASE WHEN tbl = 'oc' THEN 's_dec' ELSE 'dec' END, ra0, de0) ELSE '' END) END $$;
CREATE OR REPLACE FUNCTION mk_run(tag text, tbl text, n int, reps int, ks int[]) RETURNS void LANGUAGE plpgsql AS $$
DECLARE c record; m text; k int; t0 timestamptz; x bigint;
BEGIN
  FOR rep IN 1 .. reps LOOP
   FOR c IN SELECT * FROM mk_centres ORDER BY random() LOOP
    FOREACH k IN ARRAY ks LOOP
     FOR m IN SELECT unnest(ARRAY['pgsphere', 'skycell']) ORDER BY random() LOOP
      EXECUTE mk_sql(m, tbl, c.ra, c.dec, k) INTO x;
      t0 := clock_timestamp();
      FOR i IN 1 .. n LOOP
        EXECUTE mk_sql(m, tbl, c.ra + 1e-9 * (i + n * rep), c.dec, k) INTO x;
      END LOOP;
      INSERT INTO mk VALUES (tag, k, c.qid, m, extract(epoch FROM clock_timestamp() - t0) * 1e6 / n, x);
     END LOOP;
    END LOOP;
   END LOOP;
  END LOOP;
END $$;
