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
-- is reported; the paper's figures are one such sample.  Measured on
-- 2026-10-05 (PG16, statistics target 1000, gaia_realc), three samples:
--
--   field        median per sample    range           paper
--   omega_cen    0.08 0.09 0.06       0.05-0.55       0.08  (0.05-0.51)
--   47tuc        0.02 0.03 0.11       0.01-0.94       0.12  (0.06-0.93)
--   lmc          0.80 0.57 0.51       0.22-1.09       0.76  (0.25-0.93)
--   baade        0.96 0.92 0.92       0.45-1.01       0.80  (0.68-1.01)
--   ngp          1.29 1.30 1.35       0.52-1.54       0.90  (0.53-1.28)
--   gal_centre   3.63 3.75 3.70       2.41-4.85       3.74  (2.53-3.95)
--
-- The sample-to-sample spread is large in compact fields (47 Tuc 0.02-0.11,
-- the LMC 0.51-0.80).  The paper's omega Cen, LMC and Galactic-centre values
-- fall inside it and its 47 Tuc value just outside.  Two rows do not match:
-- the north Galactic pole, where the 10M sample holds 0 rows inside 0.1 deg
-- and 4/9/41 at 0.2/0.5/1 deg, so its ratio rests on three small counts; and
-- Baade, above the paper's 0.80 (see GIST_REGION_DESIGN.md round sixty-six,
-- addendum).

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
