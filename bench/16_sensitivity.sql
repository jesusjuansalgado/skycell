-- Sensitivity of the cost model and the density estimator (referee 2, major 2 and 4).
--
-- Major 2 asks whether a method whose cost model reads the planner's own cost
-- parameters still behaves when those parameters change.  Major 4 asks how far
-- the density estimate can be trusted: it is an equi-depth histogram over a
-- space-filling key, so a compact structure on the sky need not be a compact
-- interval in the key, and a structure smaller than the histogram's resolution
-- cannot be resolved at all.
--
-- Both are answered against the table named in :tbl / :idx / :col, so the same
-- script runs on the synthetic corpus and on the real Gaia positions.
\set ON_ERROR_STOP 1

CREATE TABLE IF NOT EXISTS sens_cost (corpus text, rpc float8, spc float8,
  c_range float8, radius float8, nranges int, chosen_order int, area_ratio float8);
CREATE TABLE IF NOT EXISTS sens_dens (corpus text, stat_target int, radius float8,
  rho_est float8, rho_true float8, ratio float8, nranges int, area_ratio float8, buckets int);

-- ------------------------------------------------------------------
-- (a) does the covering follow the planner's cost parameters?
-- ------------------------------------------------------------------
-- c_range is derived from relpages/reltuples and the cost GUCs, so changing
-- random_page_cost should move it, and the covering should get coarser as a
-- range gets dearer.  If it does not move, the "derived" claim is empty.
CREATE OR REPLACE FUNCTION sens_cost_run(corpus text, tbl regclass, col name)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE rpc float8; spc float8; r float8; info record; cr float8;
BEGIN
  DELETE FROM sens_cost s WHERE s.corpus = sens_cost_run.corpus;
  FOREACH rpc IN ARRAY ARRAY[1.1, 2.0, 4.0, 10.0] LOOP
    FOREACH spc IN ARRAY ARRAY[1.0, 0.5] LOOP
      PERFORM set_config('random_page_cost', rpc::text, true);
      PERFORM set_config('seq_page_cost', spc::text, true);
      PERFORM set_config('skycell.range_cost', '-1', true);   -- derived, not fixed
      EXECUTE format('SELECT skycell_range_cost(%L, %L)', tbl, col) INTO cr;
      FOREACH r IN ARRAY ARRAY[0.00028, 0.0028, 0.1, 1.0, 3.0] LOOP
        EXECUTE format('SELECT * FROM skycell_cover_info(266.4, -29.0, %s, %L, %L)', r, tbl, col)
          INTO info;
        INSERT INTO sens_cost VALUES (corpus, rpc, spc, cr, r,
                                      info.nranges, info.chosen_order, info.area_ratio);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;

-- ------------------------------------------------------------------
-- (b) how good is the density estimate, and does it matter?
-- ------------------------------------------------------------------
-- Estimated rho against rho counted from the table itself, at several statistics
-- targets.  The quantity that matters operationally is not the estimate but the
-- covering it produces, so the number of ranges and the area ratio are recorded
-- with it.
-- A procedure, not a function, so that it can COMMIT between setting the
-- statistics target and running ANALYZE: inside one transaction ANALYZE kept
-- the target the index had when the transaction began, so every "target"
-- measured the same histogram.  The bucket count each pass actually got is
-- recorded, so a result shows whether the target took effect.  rho_true is in
-- rows per steradian, the unit skycell_cover_info().rho uses (it was per square
-- degree, which put every ratio 3,283 times too high).
DROP FUNCTION IF EXISTS sens_dens_run(text, regclass, regclass, name, name);
ALTER TABLE sens_dens ADD COLUMN IF NOT EXISTS buckets int;
CREATE OR REPLACE PROCEDURE sens_dens_run(corpus text, tbl regclass, idx regclass,
                                          col name, statcol name)
LANGUAGE plpgsql AS $$
DECLARE st int; r float8; info record; true_rho float8; n bigint; nb int;
BEGIN
  DELETE FROM sens_dens s WHERE s.corpus = sens_dens_run.corpus;
  COMMIT;
  FOREACH st IN ARRAY ARRAY[10, 100, 1000] LOOP
    EXECUTE format('ALTER INDEX %s ALTER COLUMN 1 SET STATISTICS %s', idx, st);
    COMMIT;
    EXECUTE format('ANALYZE %s', tbl);
    SELECT array_length(histogram_bounds::text::text[], 1) - 1 INTO nb
      FROM pg_stats WHERE (schemaname || '.' || tablename)::regclass = idx;
    FOREACH r IN ARRAY ARRAY[0.0028, 0.1, 1.0] LOOP
      -- truth: count what is really inside the cone, per steradian
      EXECUTE format('SELECT count(*) FROM %s WHERE skycell_in_cone(ra, dec, 266.4, -29.0, %s)', tbl, r)
        INTO n;
      true_rho := n / (2*pi()*(1 - cos(radians(r))));
      EXECUTE format('SELECT * FROM skycell_cover_info(266.4, -29.0, %s, %L, %L)', r, idx, statcol)
        INTO info;
      INSERT INTO sens_dens VALUES (corpus, st, r, info.rho, true_rho,
        CASE WHEN true_rho > 0 THEN info.rho / true_rho END, info.nranges, info.area_ratio, nb);
    END LOOP;
    COMMIT;
  END LOOP;
END $$;
