-- The covering the cost model chose for part (b)'s cones under the current
-- (last) sample: est_fields x {0.0028, 0.05, 0.5} deg
SELECT f.field, r, c.chosen_order, c.nranges, round(c.exp_rows::numeric, 1) AS exp_rows,
       round(c.rho::numeric) AS rho
FROM est_fields f, unnest(ARRAY[0.0028, 0.05, 0.5]) r,
     LATERAL skycell_cover_info(f.ra, f.dec, r, (:'tbl' || '_cell')::regclass, 'skycell_ang2cell') c
ORDER BY array_position(ARRAY['omega_cen','47tuc','lmc','baade','ngp','gal_centre'], f.field), r;
