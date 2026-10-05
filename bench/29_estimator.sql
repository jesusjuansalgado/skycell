-- The density estimate against the truth, per field: the paper's
-- tab:estimator (Sect. "estimator") and the second referee response's table.
--
-- rho_est is what the cost model reads (skycell_cover_info().rho, from the
-- cell index's ANALYZE histogram); rho_true is counted from the same relation
-- inside the same cone.  Both in rows sr^-1.  The table reports, per field, the
-- median and range of rho_est / rho_true over radii 0.01-1 deg.
--
-- ANALYZE draws a random sample that cannot be seeded, so the estimate moves
-- between runs.  The measurement is repeated over :reps fresh samples and each
-- is reported.  The paper's tab:estimator is this script on gaia_realc with
-- -v reps=10 (2026-10-05, PG16, statistics target 1000):
--
--   field        median of medians   spread across samples   range
--   omega_cen    0.07                0.06-0.08               0.05-0.56
--   47tuc        0.03                0.01-0.08               0.00-0.94
--   lmc          0.69                0.42-0.85               0.22-1.16
--   baade        0.96                0.92-0.99               0.47-1.14
--   ngp          1.31                1.18-1.58               0.49-1.80
--   gal_centre   3.69                3.48-3.83               1.99-5.47
--
-- The north Galactic pole holds 0 rows inside 0.1 deg and 4/9/41 at
-- 0.2/0.5/1 deg in the 10M sample, so its ratio rests on three small counts.
--
-- Part (b), -v decision=1, gives the paper's "within 0.96-1.55 of the best
-- fixed order" (three samples, 9 timed repetitions each; cluster cores at
-- 0.05 deg are the worst case, 1.37-1.55, and 1.76 in a sample that put
-- 47 Tuc at 0.01), against 0.96-1.25 at Baade's Window and the LMC.

-- Baade's Window is at RA 270.904, Dec -30.035.  19_gaia_real.sh used to put
-- it at RA 18.17 (its RA in hours), a high-latitude cone.
--
-- Usage:  psql -v tbl=gaia_realc -v reps=3 -f bench/29_estimator.sql
-- Needs the table loaded by 19_gaia_load.sh, with its cell index named
-- <tbl>_cell on skycell_ang2cell(ra, dec).
\set ON_ERROR_STOP 1
\if :{?tbl}
\else
  \set tbl gaia_realc
\endif
\if :{?reps}
\else
  \set reps 3
\endif

CREATE TABLE IF NOT EXISTS est_fields (field text PRIMARY KEY, ra float8, dec float8);
TRUNCATE est_fields;
INSERT INTO est_fields VALUES
  ('omega_cen',  201.697,   -47.4795),
  ('47tuc',        6.0236,  -72.0814),
  ('lmc',         80.8942,  -69.7561),
  ('baade',      270.904,   -30.035),
  ('ngp',        192.85948,  27.12825),
  ('gal_centre', 266.4168,  -29.0078);

CREATE TABLE IF NOT EXISTS est_truth (tbl text, field text, r float8, n bigint, rho_true float8);
CREATE TABLE IF NOT EXISTS est_runs (tbl text, rep int, field text, r float8, rho_est float8);

-- truth: does not depend on the statistics, so counted once
CREATE OR REPLACE FUNCTION est_truth_run(tbl text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE f record; rad float8; cnt bigint;
BEGIN
  DELETE FROM est_truth t WHERE t.tbl = est_truth_run.tbl;
  FOR f IN SELECT * FROM est_fields LOOP
    FOREACH rad IN ARRAY ARRAY[0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0] LOOP
      EXECUTE format('SELECT count(*) FROM %I WHERE point(''ICRS'', ra, dec) <@ circle(''ICRS'', %s, %s, %s)',
                     tbl, f.ra, f.dec, rad) INTO cnt;
      INSERT INTO est_truth VALUES (tbl, f.field, rad, cnt, cnt / (2 * pi() * (1 - cos(radians(rad)))));
    END LOOP;
  END LOOP;
END $$;

-- one estimate per (field, radius) under whatever statistics are current
CREATE OR REPLACE FUNCTION est_run(tbl text, rep int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE t record; rho float8;
BEGIN
  FOR t IN SELECT e.field, e.r, f.ra, f.dec FROM est_truth e JOIN est_fields f USING (field)
           WHERE e.tbl = est_run.tbl LOOP
    EXECUTE format('SELECT rho FROM skycell_cover_info(%s, %s, %s, %L, %L)',
                   t.ra, t.dec, t.r, tbl || '_cell', 'skycell_ang2cell') INTO rho;
    INSERT INTO est_runs VALUES (tbl, rep, t.field, t.r, rho);
  END LOOP;
END $$;

SELECT format('ALTER INDEX %I ALTER COLUMN 1 SET STATISTICS 1000', :'tbl' || '_cell') \gexec
DELETE FROM est_runs WHERE tbl = :'tbl';
SELECT est_truth_run(:'tbl');

-- ANALYZE as a top-level statement, never inside a DO block or function: there
-- it was seen to ignore the statistics target (19_gaia_load.sh).
SELECT format('ANALYZE %I', :'tbl'), format('SELECT est_run(%L, %s)', :'tbl', i)
FROM generate_series(1, :reps) i \gexec

-- the histogram really has 1000 buckets
SELECT array_length(histogram_bounds::text::text[], 1) - 1 AS buckets
FROM pg_stats WHERE tablename = :'tbl' || '_cell' AND attname = 'skycell_ang2cell';

-- per sample: median and range over radii, cones with at least one row
SELECT r.field, r.rep,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY r.rho_est / t.rho_true))::numeric, 2) AS median,
       round(min(r.rho_est / t.rho_true)::numeric, 2) || '-' ||
       round(max(r.rho_est / t.rho_true)::numeric, 2) AS range,
       sum(t.n) AS rows_counted
FROM est_runs r JOIN est_truth t USING (tbl, field, r)
WHERE r.tbl = :'tbl' AND t.n > 0
GROUP BY r.field, r.rep
ORDER BY array_position(ARRAY['omega_cen','47tuc','lmc','baade','ngp','gal_centre'], r.field), r.rep;

-- ------------------------------------------------------------------
-- (b) does the error matter?  The cost-chosen covering against every fixed
-- order, at each field, under the statistics of the last sample above.  As
-- 17_ablation.sql's factor 2: one timed, warmed EXPLAIN per (order, query),
-- orders shuffled within each trial, plan + execution time.
-- Usage: add -v decision=1, and -v dreps=N for N timed repetitions (default 9;
-- about 2 minutes on gaia_realc).
-- ------------------------------------------------------------------
\if :{?decision}
\if :{?dreps}
\else
  \set dreps 9
\endif
CREATE TABLE IF NOT EXISTS est_decision (tbl text, field text, r float8, rep int,
  setting text, ms float8, n bigint);

CREATE OR REPLACE FUNCTION est_one(tbl text, field text, rep int, setting text,
                                   ra float8, dc float8, r float8)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text; j json; nn bigint;
BEGIN
  q := format('SELECT count(*) FROM %I WHERE skycell_cone(skycell_ang2cell(ra,dec), ra, dec, %s, %s, %s)',
              tbl, ra, dc, r);
  EXECUTE q INTO nn;                                   -- warm this order on this query
  EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, FORMAT JSON) ' || q INTO j;
  INSERT INTO est_decision VALUES (tbl, field, r, rep, setting,
    (j->0->>'Planning Time')::float8 + (j->0->>'Execution Time')::float8, nn);
END $$;

CREATE OR REPLACE FUNCTION est_decision_run(tbl text, reps int DEFAULT 3)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE f record; rep int; r float8; s text;
BEGIN
  DELETE FROM est_decision d WHERE d.tbl = est_decision_run.tbl;
  PERFORM setseed(0.29);
  FOR rep IN 1 .. reps LOOP
    FOR f IN SELECT * FROM est_fields ORDER BY random() LOOP
      FOREACH r IN ARRAY ARRAY[0.0028, 0.05, 0.5] LOOP
        FOR s IN SELECT x FROM unnest(ARRAY['chosen','4','5','6','7','8','9','10','11','12','13']) x
                 ORDER BY random() LOOP
          PERFORM set_config('skycell.force_order', CASE s WHEN 'chosen' THEN '-1' ELSE s END, true);
          PERFORM est_one(tbl, f.field, rep, s, f.ra, f.dec, r);
        END LOOP;
        PERFORM set_config('skycell.force_order', '-1', true);
      END LOOP;
    END LOOP;
  END LOOP;
END $$;

SELECT est_decision_run(:'tbl', :dreps);

-- the estimate the decision was made with (last sample), and the chosen
-- covering's median time over the best fixed order's
WITH med AS (
  SELECT field, r, setting, percentile_cont(0.5) WITHIN GROUP (ORDER BY ms) AS ms, max(n) AS n
  FROM est_decision WHERE tbl = :'tbl' GROUP BY 1, 2, 3),
best AS (
  SELECT DISTINCT ON (field, r) field, r, setting AS best_order, ms AS best_ms
  FROM med WHERE setting <> 'chosen' ORDER BY field, r, ms),
est AS (
  SELECT r.field, round((percentile_cont(0.5) WITHIN GROUP (ORDER BY r.rho_est / t.rho_true))::numeric, 2) AS est_ratio
  FROM est_runs r JOIN est_truth t USING (tbl, field, r)
  WHERE r.tbl = :'tbl' AND t.n > 0
    AND r.rep = (SELECT max(rep) FROM est_runs WHERE tbl = :'tbl')
  GROUP BY r.field)
SELECT m.field, e.est_ratio, m.r, m.n, b.best_order,
       round((m.ms / b.best_ms)::numeric, 2) AS chosen_over_best
FROM med m JOIN best b USING (field, r) JOIN est e USING (field)
WHERE m.setting = 'chosen'
ORDER BY array_position(ARRAY['omega_cen','47tuc','lmc','baade','ngp','gal_centre'], m.field), m.r;
\endif
