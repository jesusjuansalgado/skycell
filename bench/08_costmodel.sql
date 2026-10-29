-- Calibration and sensitivity of the cost model (referee experiment 3).
--
--   cm_sweep   -- query time and covering shape against skycell.range_cost,
--                 over four decades instead of the factor 10 in 03_cone.sql
--   cm_hist    -- the same against the resolution of the density histogram
--   cm_rho     -- the histogram's density estimate against the true count in
--                 the same footprint: how wrong the estimator is, and whether
--                 being wrong changes the order it picks
--   cm_curve   -- query time at every order around the model's choice
--                 (skycell.force_order), so the choice can be compared with
--                 the empirical optimum rather than assumed right
\set ON_ERROR_STOP 1

CREATE TABLE IF NOT EXISTS cm_sweep (corpus text, label text, range_cost float8,
  ms float8, nranges float8, area_ratio float8, deepest float8, buffers float8);
CREATE TABLE IF NOT EXISTS cm_hist (corpus text, label text, nbuckets int,
  ms float8, nranges float8, area_ratio float8, analyze_s float8);
CREATE TABLE IF NOT EXISTS cm_rho (corpus text, label text, qid bigint,
  rho_hist float8, rho_true float8, order_hist int, order_true int,
  footprint_order int, footprint_rows bigint);
CREATE TABLE IF NOT EXISTS cm_curve (corpus text, label text, qid bigint,
  ord int, chosen int, ms float8, n bigint);

-- a subset of the centres: enough for a median, few enough for 4 decades x 7 radii
CREATE OR REPLACE FUNCTION cm_centers(lab text, lim int) RETURNS SETOF bench_centers
LANGUAGE sql STABLE AS $$
  SELECT * FROM bench_centers WHERE label = lab ORDER BY qid LIMIT lim
$$;

-- ---------------------------------------------------------------- range_cost
CREATE OR REPLACE FUNCTION cm_run_sweep(corpus text, costs float8[],
                                        labels text[], lim int DEFAULT 40,
                                        do_warm bool DEFAULT true,
                                        keep bool DEFAULT false)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE c bench_centers; lab text; rc float8; n bigint; s float8; t0 timestamptz;
        tms float8[]; inf record; nr float8[]; ar float8[]; dp float8[];
BEGIN
  IF NOT keep THEN
    DELETE FROM cm_sweep w WHERE w.corpus = cm_run_sweep.corpus;
  END IF;
  FOREACH lab IN ARRAY labels LOOP
    FOREACH rc IN ARRAY costs LOOP
      PERFORM set_config('skycell.range_cost', rc::text, true);
      tms := '{}'; nr := '{}'; ar := '{}'; dp := '{}';
      IF do_warm THEN
        FOR c IN SELECT * FROM cm_centers(lab, lim) LOOP
          EXECUTE cone_sql('skycell', c) INTO n, s;
        END LOOP;
      END IF;
      FOR c IN SELECT * FROM cm_centers(lab, lim) LOOP
        t0 := clock_timestamp();
        EXECUTE cone_sql('skycell', c) INTO n, s;
        tms := tms || (extract(epoch FROM clock_timestamp() - t0) * 1000);
        SELECT * INTO inf FROM skycell_cover_info(c.ra0, c.dec0, c.r, 'cat_cell');
        nr := nr || inf.nranges::float8;
        ar := ar || inf.area_ratio;
        dp := dp || inf.deepest::float8;
      END LOOP;
      INSERT INTO cm_sweep
      SELECT corpus, lab, rc,
             (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY x) FROM unnest(tms) x),
             (SELECT avg(x) FROM unnest(nr) x), (SELECT avg(x) FROM unnest(ar) x),
             (SELECT avg(x) FROM unnest(dp) x), NULL;
    END LOOP;
  END LOOP;
  PERFORM set_config('skycell.range_cost', '30', true);
END $$;

-- ------------------------------------------------------- estimator vs. truth
/*
 * choose_order() reads the density from the histogram in the order-m cell that
 * contains the cone, with m the order whose cells are about four times the
 * cone's radius.  Recompute that cell here, ask the histogram (via
 * skycell_cover_info) and count the rows actually in it.
 */
CREATE OR REPLACE FUNCTION cm_run_rho(corpus text, labels text[], lim int DEFAULT 60)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE c bench_centers; lab text; inf record; m int; pix bigint;
        lo bigint; hi bigint; cnt bigint; area float8; rho_t float8; o_true int;
BEGIN
  DELETE FROM cm_rho w WHERE w.corpus = cm_run_rho.corpus;
  FOREACH lab IN ARRAY labels LOOP
    FOR c IN SELECT * FROM cm_centers(lab, lim) LOOP
      SELECT * INTO inf FROM skycell_cover_info(c.ra0, c.dec0, c.r, 'cat_cell');
      m := greatest(0, least(29, floor(ln(1.0233267079464885
             / greatest(4 * radians(c.r), 1e-6)) / ln(2))::int));
      pix := skycell_ang2pix(m, c.ra0, c.dec0);
      lo := pix << (2 * (29 - m));
      hi := lo + (1::bigint << (2 * (29 - m))) - 1;
      SELECT count(*) INTO cnt FROM cat_cell WHERE cell BETWEEN lo AND hi;
      area := 4 * pi() / (12 * (1::bigint << (2 * m))::float8);
      rho_t := cnt / area;
      -- the order the model would have chosen from the true density: same
      -- closed form, s* = sqrt(alpha * range_cost / rho)
      o_true := greatest(0, least(29, round(ln(1.0233267079464885
                  / sqrt(2.0 * 30.0 / (2 * pi() * 0.7 * greatest(rho_t, 1e-9)))) / ln(2))::int));
      INSERT INTO cm_rho VALUES (corpus, lab, c.qid, inf.rho, rho_t,
                                 inf.chosen_order, o_true, m, cnt);
    END LOOP;
  END LOOP;
END $$;

-- ------------------------------------------------------------- cost curve
CREATE OR REPLACE FUNCTION cm_run_curve(corpus text, labels text[],
                                        lim int DEFAULT 20, span int DEFAULT 4)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE c bench_centers; lab text; inf record; k int; n bigint; s float8;
        t0 timestamptz; best float8;
BEGIN
  DELETE FROM cm_curve w WHERE w.corpus = cm_run_curve.corpus;
  FOREACH lab IN ARRAY labels LOOP
    FOR c IN SELECT * FROM cm_centers(lab, lim) LOOP
      SELECT * INTO inf FROM skycell_cover_info(c.ra0, c.dec0, c.r, 'cat_cell');
      FOR k IN greatest(0, inf.chosen_order - span) .. least(29, inf.chosen_order + span) LOOP
        PERFORM set_config('skycell.force_order', k::text, true);
        EXECUTE cone_sql('skycell', c) INTO n, s;          -- warm this order
        t0 := clock_timestamp();
        EXECUTE cone_sql('skycell', c) INTO n, s;
        INSERT INTO cm_curve VALUES (corpus, lab, c.qid, k, inf.chosen_order,
          extract(epoch FROM clock_timestamp() - t0) * 1000, n);
      END LOOP;
      PERFORM set_config('skycell.force_order', '-1', true);
    END LOOP;
  END LOOP;
END $$;
