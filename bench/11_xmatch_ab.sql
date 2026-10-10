-- Cross-matching, measured the way the cone searches are.
--
-- Cross-match is Q3C's own speciality -- q3c_join is what most PostgreSQL
-- archives use for it -- and it is the operation an archive runs when a user
-- uploads a target list.  The earlier measurement ran each method once, end to
-- end, one after another, which is too weak to carry a claim of that kind.
--
-- Here the probe table is split into blocks, and a block is the unit of
-- pairing: for each block all methods run back to back in an order drawn for
-- that block, so probe difficulty cancels between methods.
--
-- Cache residency has to be handled explicitly, and this is the part a single
-- end-to-end run gets wrong.  At 50M rows the three catalogues and their
-- indexes are ~14 GB against 2 GB of shared_buffers, so a block's first touch
-- is I/O bound: the same skycell block query takes 6.2 s cold and 0.32 s once
-- its pages are resident.  Whichever method runs first on a block therefore
-- pays for the others, and the effect is larger than the difference between
-- the methods -- in an earlier version of this file, running first cost
-- skycell a factor ten while leaving q3c and pgSphere, whose own cost is
-- several seconds, visibly unchanged.
--
-- So each method is warmed on its own query for its own block immediately
-- before being timed.  Every method is then measured in the same state --
-- steady state, its own working set resident -- and what is compared is the
-- work each method does rather than which of them was unlucky enough to go
-- first.  The cold regime is a different measurement and is reported as such.
\set ON_ERROR_STOP 1

ALTER TABLE probe ADD COLUMN IF NOT EXISTS blk int;
UPDATE probe SET blk = pid % 8 WHERE blk IS DISTINCT FROM pid % 8;
CREATE INDEX IF NOT EXISTS probe_blk_idx ON probe (blk);
ANALYZE probe;

-- skycell_join, the Q3C-shaped spelling, synthesises skycell_ang2cell(ra, dec)
-- from its first coordinate pair, so it needs an index on that expression;
-- cat_cell's own (02_build.sql) is on the stored cell column.  Built here
-- rather than there so that the build-cost tables do not count it; the same
-- statistics target as the column, set before (not with) the ANALYZE.
CREATE INDEX IF NOT EXISTS cat_cell_a2c ON cat_cell (skycell_ang2cell(ra, dec));
ALTER INDEX cat_cell_a2c ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE cat_cell;

CREATE TABLE IF NOT EXISTS bench_xm_ab (
  rows_m float8, radius_arcsec float8, rep int, blk int,
  method text, slot int, n bigint, ms float8);

CREATE OR REPLACE FUNCTION xm_block_sql(method text, r float8, blk int) RETURNS text
LANGUAGE sql IMMUTABLE AS $$
  SELECT CASE method
    WHEN 'q3c' THEN format(
      'SELECT count(*), sum(c.id) FROM probe p JOIN cat_q3c c '
      'ON q3c_join(p.ra, p.dec, c.ra, c.dec, %s) WHERE p.blk = %s', r, blk)
    WHEN 'pgsphere' THEN format(
      'SELECT count(*), sum(c.id) FROM probe p JOIN cat_sphere c '
      'ON c.pos <@ scircle(p.pos, radians(%s)) WHERE p.blk = %s', r, blk)
    -- skycell_slots and skycell_cs are the same query: the rewrite's fixed
    -- range slots (skycell.custom_scan off) and the custom scan's covering
    -- per probe (on, the default); bench_xm_ab_run sets the GUC per method
    WHEN 'skycell_slots' THEN format(
      'SELECT count(*), sum(c.id) FROM probe p JOIN cat_cell c '
      'ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, %s) WHERE p.blk = %s', r, blk)
    WHEN 'skycell_cs' THEN format(
      'SELECT count(*), sum(c.id) FROM probe p JOIN cat_cell c '
      'ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, %s) WHERE p.blk = %s', r, blk)
    -- the Q3C-shaped spelling, as a q3c_join user would write it: the custom
    -- scan through the expression index above
    WHEN 'skycell_join' THEN format(
      'SELECT count(*), sum(c.id) FROM probe p JOIN cat_cell c '
      'ON skycell_join(c.ra, c.dec, p.ra, p.dec, %s) WHERE p.blk = %s', r, blk)
    WHEN 'skycell_lateral' THEN format(
      'SELECT count(*), sum(c.id) FROM probe p '
      'CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, %s, ''cat_cell'') g '
      'JOIN cat_cell c ON c.cell BETWEEN g.lo AND g.hi '
      'AND skycell_in_cone(c.ra, c.dec, p.ra, p.dec, %s) WHERE p.blk = %s', r, r, blk)
  END
$$;

CREATE OR REPLACE FUNCTION bench_xm_ab_run(rows_m float8, radii float8[],
                                           reps int DEFAULT 2,
                                           methods text[] DEFAULT
                                             ARRAY['q3c', 'pgsphere', 'skycell_slots', 'skycell_cs', 'skycell_join'],
                                           seed float8 DEFAULT 0.53) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE r float8; b int; i int; rep int; ms text[]; n bigint; s numeric;
        t0 timestamptz; nblk int;
BEGIN
  DELETE FROM bench_xm_ab x WHERE x.rows_m = bench_xm_ab_run.rows_m;
  SELECT count(DISTINCT blk) INTO nblk FROM probe;
  PERFORM setseed(seed);

  FOREACH r IN ARRAY radii LOOP
    FOR rep IN 1 .. reps LOOP
      FOR b IN 0 .. nblk - 1 LOOP
        SELECT array_agg(m ORDER BY random()) INTO ms FROM unnest(methods) m;
        FOR i IN 1 .. array_length(ms, 1) LOOP
          -- memoize keyed on (lo, hi) never hits for distinct probes
          PERFORM set_config('enable_memoize', 'off', true);
          PERFORM set_config('skycell.custom_scan',
                             CASE WHEN ms[i] = 'skycell_slots' THEN 'off' ELSE 'on' END, true);
          -- warm this method on this block, then time the same query
          EXECUTE xm_block_sql(ms[i], r / 3600, b) INTO n, s;
          t0 := clock_timestamp();
          EXECUTE xm_block_sql(ms[i], r / 3600, b) INTO n, s;
          INSERT INTO bench_xm_ab VALUES (rows_m, r, rep, b, ms[i], i, n,
            extract(epoch FROM clock_timestamp() - t0) * 1000);
        END LOOP;
      END LOOP;
    END LOOP;
  END LOOP;
END $$;
