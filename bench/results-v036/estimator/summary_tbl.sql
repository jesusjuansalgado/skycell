-- summary.txt: per field, the median over the ANALYZE samples of the
-- per-sample median rho-hat/rho (radii 0.01-1 deg, cones with rows), the
-- spread of that median across samples, and the range over all radii and samples
WITH per AS (
  SELECT r.field, r.rep,
         percentile_cont(0.5) WITHIN GROUP (ORDER BY r.rho_est / t.rho_true) AS med,
         min(r.rho_est / t.rho_true) AS lo, max(r.rho_est / t.rho_true) AS hi
  FROM est_runs r JOIN est_truth t USING (tbl, field, r)
  WHERE r.tbl = :'tbl' AND t.n > 0 GROUP BY 1, 2)
SELECT field,
       round((percentile_cont(0.5) WITHIN GROUP (ORDER BY med))::numeric, 2) AS median,
       round(min(med)::numeric, 2) || '-' || round(max(med)::numeric, 2) AS across_samples,
       round(min(lo)::numeric, 2) || '-' || round(max(hi)::numeric, 2) AS range
FROM per GROUP BY field
ORDER BY array_position(ARRAY['omega_cen','47tuc','lmc','baade','ngp','gal_centre'], field);
