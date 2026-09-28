-- skycell regression tests: every indexed query must return exactly what the
-- exact predicate returns on a sequential scan (no false negatives, no false
-- positives), on clustered data including poles and RA wrap-around.
CREATE EXTENSION skycell;
SET extra_float_digits = 0;
SELECT setseed(0.42);

-- HEALPix basics -------------------------------------------------------
SELECT skycell_ang2pix(0, 45, 41.8103148958) AS p0,
       skycell_ang2pix(0, 0, 0) AS p4,
       skycell_ang2pix(0, 315, -41.8103148958) AS p11,
       skycell_ang2pix(12, 10, 20) = skycell_ang2cell(10, 20) >> 34 AS nested;
SELECT array_length(skycell_ancestors(skycell_ang2cell(10, 20)), 1) AS n_ancestors,
       (skycell_ancestors(0))[1] AS nuniq_order0,
       skycell_nuniq_lo(4) AS lo, skycell_nuniq_hi(4) = (1::int8 << 58) - 1 AS hi_ok;
SELECT round(skycell_dist(0, 0, 90, 0)::numeric, 9) AS d90,
       round(skycell_dist(10, 89.9999, 190, 89.9999)::numeric * 3600, 4) AS d_pole_arcsec;
SELECT skycell_ang2cell(10, 91);   -- error

-- catalogue: 40% uniform, 60% in clusters (incl. both poles and RA=0) ----
CREATE TABLE cat AS
WITH c(k, cra, cdec, sig) AS (
  VALUES (0, 0.0, 89.95, 0.3), (1, 180.0, -89.98, 0.05), (2, 0.001, 10.0, 0.2),
         (3, 45.0, 41.8103, 0.1), (4, 120.0, -30.0, 1.5), (5, 250.0, 60.0, 0.02))
SELECT i AS id, ra, dec FROM (
  SELECT i,
         CASE WHEN u < 0.4 THEN 360 * random()
              ELSE cra + sig * g1 / greatest(cos(radians(cdec)), 0.01) END AS ra0,
         CASE WHEN u < 0.4 THEN degrees(asin(2 * random() - 1))
              ELSE cdec + sig * g2 END AS dec0
  FROM (SELECT i, random() AS u, (random() * 6)::int % 6 AS k,
               sqrt(-2 * ln(random())) * cos(2 * pi() * random()) AS g1,
               sqrt(-2 * ln(random())) * cos(2 * pi() * random()) AS g2
        FROM generate_series(1, 100000) i) s
  JOIN c USING (k)) s2,
LATERAL (SELECT CASE WHEN dec0 > 90 THEN 180 - dec0 WHEN dec0 < -90 THEN -180 - dec0 ELSE dec0 END AS dec,
                (CASE WHEN abs(dec0) > 90 THEN ra0 + 180 ELSE ra0 END + 720)::numeric % 360 AS ran) f,
LATERAL (SELECT ran::float8 AS ra) f2;
ALTER TABLE cat ADD COLUMN cell int8;
UPDATE cat SET cell = skycell_ang2cell(ra, dec);
CREATE INDEX cat_cell ON cat (cell);
ALTER TABLE cat ALTER COLUMN cell SET STATISTICS 1000;
ANALYZE cat;

-- the rewrite produces an index plan
CREATE FUNCTION plan_uses_index(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; found bool := false;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ 'Index (Only )?Scan|Bitmap Index Scan' THEN found := true; END IF;
  END LOOP;
  RETURN found;
END $$;
SELECT plan_uses_index('SELECT * FROM cat WHERE skycell_cone(cell, ra, dec, 10, 20, 0.1)') AS cone_indexed,
       plan_uses_index('SELECT * FROM cat WHERE skycell_poly(cell, ra, dec, ARRAY[10,10,11,10,11,11,10,11]::float8[])') AS poly_indexed;

-- Q3C-shaped spellings: no cell argument, the support function synthesises
-- skycell_ang2cell(ra, dec).  They must plan through the index and agree with
-- both the six-argument form and the unindexed exact test.
CREATE INDEX cat_a2c ON cat (skycell_ang2cell(ra, dec));
ANALYZE cat;
CREATE TABLE probe AS SELECT ra, dec FROM cat ORDER BY id LIMIT 20;
ANALYZE probe;
SELECT plan_uses_index('SELECT * FROM cat WHERE skycell_radial_query(ra, dec, 10, 20, 0.1)') AS radial_indexed,
       plan_uses_index('SELECT count(*) FROM probe p, cat c WHERE skycell_join(c.ra, c.dec, p.ra, p.dec, 0.05)') AS join_indexed;
SELECT (SELECT count(*) FROM cat WHERE skycell_radial_query(ra, dec, 10, 20, 0.5))
         = (SELECT count(*) FROM cat WHERE skycell_cone(cell, ra, dec, 10, 20, 0.5)) AS radial_matches_cone,
       (SELECT count(*) FROM cat WHERE skycell_radial_query(ra, dec, 10, 20, 0.5))
         = (SELECT count(*) FROM cat WHERE skycell_in_cone(ra, dec, 10, 20, 0.5)) AS radial_matches_exact,
       (SELECT count(*) FROM probe p, cat c WHERE skycell_join(c.ra, c.dec, p.ra, p.dec, 0.05))
         = (SELECT count(*) FROM probe p, cat c WHERE skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 0.05)) AS join_matches_exact;

-- skycell_poly_join: the polygon analogue of skycell_join/skycell_radial_query,
-- for a cross-match whose polygon comes from another table's row rather than a
-- compile-time constant. Dec is clamped away from the poles: a tiny quad
-- straddling one is a separate (already-tested) edge case, not what this
-- exercises.
CREATE TABLE tile AS SELECT ra, least(greatest(dec, -85), 85) AS dec FROM probe;
SELECT plan_uses_index(
  'SELECT count(*) FROM tile t, cat c WHERE skycell_poly_join(c.ra, c.dec, ' ||
  'ARRAY[t.ra-0.05,t.dec-0.05, t.ra+0.05,t.dec-0.05, t.ra+0.05,t.dec+0.05, t.ra-0.05,t.dec+0.05]::float8[])'
) AS poly_join_indexed;
SELECT (SELECT count(*) FROM tile t, cat c
          WHERE skycell_poly_join(c.ra, c.dec,
            ARRAY[t.ra-0.05,t.dec-0.05, t.ra+0.05,t.dec-0.05, t.ra+0.05,t.dec+0.05, t.ra-0.05,t.dec+0.05]::float8[]))
      = (SELECT count(*) FROM tile t, cat c
          WHERE skycell_in_poly(c.ra, c.dec,
            ARRAY[t.ra-0.05,t.dec-0.05, t.ra+0.05,t.dec-0.05, t.ra+0.05,t.dec+0.05, t.ra-0.05,t.dec+0.05]::float8[]))
      AS poly_join_matches_exact;
DROP TABLE tile;

-- skycell_region_bound / <@ against a non-constant skyregion column: the
-- generic-region analogue of skycell_poly_join, dispatching on each row's own
-- kind tag the same way skycell_region_from_datum already does for a
-- constant region. Covers a cross-match against another table's per-row
-- footprint column (e.g. an archive's s_region), whichever kind it holds.
CREATE TABLE footprint AS
  SELECT ra, least(greatest(dec, -85), 85) AS dec,
         CASE WHEN row_number() OVER () % 2 = 0
              THEN circle('ICRS', ra, least(greatest(dec, -85), 85), 0.05)
              ELSE polygon('ICRS', ra-0.05, least(greatest(dec, -85), 85)-0.05,
                                   ra+0.05, least(greatest(dec, -85), 85)-0.05,
                                   ra+0.05, least(greatest(dec, -85), 85)+0.05,
                                   ra-0.05, least(greatest(dec, -85), 85)+0.05)
         END AS s_region
  FROM probe;
SELECT plan_uses_index(
  $q$SELECT count(*) FROM footprint f, cat c
     WHERE point('ICRS', c.ra, c.dec) <@ f.s_region$q$
) AS region_join_indexed;
SELECT (SELECT count(*) FROM footprint f, cat c
          WHERE point('ICRS', c.ra, c.dec) <@ f.s_region)
      = (SELECT count(*) FROM footprint f, cat c
          WHERE skycell_in_region(point('ICRS', c.ra, c.dec), f.s_region))
      AS region_join_matches_exact;
DROP TABLE footprint;

-- BOX cross-match: box(...) is a plain skyregion (a four-corner polygon
-- under the hood, built by box_region()'s cos(dec) corner compression), so
-- no dedicated join function is needed -- <@'s non-constant branch already
-- covers it, whatever kind of region it happens to be built from.
SELECT plan_uses_index(
  $q$SELECT count(*) FROM probe p, cat c
     WHERE point('ICRS', c.ra, c.dec) <@ box('ICRS', p.ra, least(greatest(p.dec, -85), 85), 0.1, 0.1)$q$
) AS box_join_indexed;
SELECT (SELECT count(*) FROM probe p, cat c
          WHERE point('ICRS', c.ra, c.dec) <@ box('ICRS', p.ra, least(greatest(p.dec, -85), 85), 0.1, 0.1))
      = (SELECT count(*) FROM probe p, cat c
          WHERE skycell_in_region(point('ICRS', c.ra, c.dec), box('ICRS', p.ra, least(greatest(p.dec, -85), 85), 0.1, 0.1)))
      AS box_join_matches_exact;
DROP TABLE probe;
DROP INDEX cat_a2c;

-- The cell-range operator class: same answers as the stock operators, and the
-- estimator must not collapse to DEFAULT_INEQ_SEL when the bounds are not
-- constant (which is what makes a cross-match plan a sequential scan).
CREATE INDEX cat_sel ON cat (skycell_ang2cell(ra, dec) skycell_cell_ops);
ANALYZE cat;
CREATE TABLE pr2 AS SELECT ra, dec FROM cat ORDER BY id LIMIT 20;
ANALYZE pr2;
SELECT (SELECT count(*) FROM pr2 p
        CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, 0.05) g
        JOIN cat c ON skycell_ang2cell(c.ra, c.dec) #>= g.lo
                  AND skycell_ang2cell(c.ra, c.dec) #<= g.hi
        WHERE skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 0.05))
     = (SELECT count(*) FROM pr2 p, cat c
        WHERE skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 0.05)) AS opclass_matches_brute_force,
       plan_uses_index('SELECT count(*) FROM pr2 p
        CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, 0.05) g
        JOIN cat c ON skycell_ang2cell(c.ra, c.dec) #>= g.lo
                  AND skycell_ang2cell(c.ra, c.dec) #<= g.hi') AS opclass_indexed;
DROP TABLE pr2;
DROP INDEX cat_sel;

-- cones: indexed vs sequential exact --------------------------------------
CREATE TABLE cones AS
SELECT q, CASE WHEN q % 3 = 0 THEN (SELECT ra FROM cat WHERE id = 1 + (q * 7919) % 100000)
               WHEN q % 3 = 1 THEN 360 * random()
               ELSE (q % 8) * 45.0 END AS ra0,
          CASE WHEN q % 3 = 0 THEN (SELECT dec FROM cat WHERE id = 1 + (q * 7919) % 100000)
               WHEN q % 3 = 1 THEN degrees(asin(2 * random() - 1))
               ELSE (ARRAY[89.9999, -89.99, 41.8103148958, 0, -41.8103148958])[1 + q % 5] END AS dec0,
          (ARRAY[1/3600.0, 10/3600.0, 1/60.0, 0.1, 0.5, 2, 10, 45])[1 + q % 8] AS r
FROM generate_series(0, 239) q;

CREATE FUNCTION cone_mismatches() RETURNS TABLE(n_cones int, n_rows bigint, mismatches int)
LANGUAGE plpgsql AS $$
DECLARE c record; a bigint; b bigint;
BEGIN
  n_cones := 0; n_rows := 0; mismatches := 0;
  FOR c IN SELECT * FROM cones ORDER BY q LOOP
    EXECUTE format('SELECT count(*) FROM cat WHERE skycell_cone(cell, ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r) INTO a;
    EXECUTE format('SELECT count(*) FROM cat WHERE skycell_in_cone(ra, dec, %s, %s, %s)', c.ra0, c.dec0, c.r) INTO b;
    n_cones := n_cones + 1; n_rows := n_rows + b;
    IF a <> b THEN mismatches := mismatches + 1; RAISE NOTICE 'cone % (%,%,%): % vs %', c.q, c.ra0, c.dec0, c.r, a, b; END IF;
  END LOOP;
  RETURN NEXT;
END $$;
SELECT n_cones, n_rows > 100000 AS many_rows, mismatches FROM cone_mismatches();

-- same, with the density model switched off and a tiny range budget
SET skycell.use_stats = off;
SET skycell.max_ranges = 2;
SELECT n_cones, mismatches FROM cone_mismatches();
RESET skycell.use_stats;
RESET skycell.max_ranges;

-- polygons ---------------------------------------------------------------
CREATE FUNCTION poly_mismatches() RETURNS int LANGUAGE plpgsql AS $$
DECLARE i int; poly float8[]; a bigint; b bigint; bad int := 0; cra float8; cdec float8; s float8;
BEGIN
  FOR i IN 0..59 LOOP
    cra := CASE WHEN i % 4 = 0 THEN 0.0 ELSE 360 * random() END;
    cdec := CASE WHEN i % 5 = 0 THEN 89.9 WHEN i % 5 = 1 THEN 41.81 ELSE degrees(asin(2 * random() - 1)) END;
    s := power(10, -2 + 3 * random());
    poly := ARRAY[cra - s, cdec - s/2, cra + s, cdec - s/2, cra + s/2, least(cdec + s, 90), cra - s/2, least(cdec + s, 90)];
    BEGIN
      EXECUTE format('SELECT count(*) FROM cat WHERE skycell_poly(cell, ra, dec, %L::float8[])', poly) INTO a;
    EXCEPTION WHEN invalid_parameter_value THEN CONTINUE;   -- degenerate/non-convex near the pole
    END;
    EXECUTE format('SELECT count(*) FROM cat WHERE skycell_in_poly(ra, dec, %L::float8[])', poly) INTO b;
    IF a <> b THEN bad := bad + 1; RAISE NOTICE 'poly %: % vs %', poly, a, b; END IF;
  END LOOP;
  RETURN bad;
END $$;
SELECT poly_mismatches() AS poly_mismatches;
SELECT skycell_poly(1, 0, 0, ARRAY[0,0, 10,0, 1,1, 0,10]::float8[]);  -- non-convex: error

-- sc_region_contains's polygon branch used a strict "< 0" test on a vertex's
-- dot product against each edge's plane, so a vertex sitting exactly ON that
-- plane (up to independent floating-point rounding, not identically 0) could
-- read as outside -- every vertex of a polygon is exactly on its own two
-- adjacent edges, so this made a polygon fail to contain, or overlap, an
-- identical or boundary-touching copy of itself.  A -1e-12 tolerance (already
-- used for the same kind of dot product in poly_setup's convexity check)
-- fixes it; the circle branch was already boundary-inclusive (<=).
SELECT skycell_region_covers(p, p) AS self_covers,
       skycell_region_overlap(p, p) AS self_overlaps,
       intersects(p, p) = 1 AS self_intersects,
       (p && p) AS self_op
FROM (SELECT polygon('ICRS', 229.54244884307008-0.05, -48.37916509457751-0.05,
                              229.54244884307008+0.05, -48.37916509457751-0.05,
                              229.54244884307008+0.05, -48.37916509457751+0.05,
                              229.54244884307008-0.05, -48.37916509457751+0.05) AS p) s;
-- two neighbouring polygons sharing an edge (no gap, no overlap in area) must
-- still be reported as touching/overlapping (a shared boundary is not empty).
SELECT skycell_region_overlap(polygon('ICRS', 0,0, 1,0, 1,1, 0,1),
                               polygon('ICRS', 1,0, 2,0, 2,1, 1,1)) AS shared_edge_ok;

-- skycell_region_covers(a, b), backing @>, had its argument order backwards
-- since the extension's first commit: it called the same underlying
-- primitive CONTAINS(a, b) uses ("is every point of a also in b"), so
-- `a @> b` computed "a is inside b" instead of "a contains b". Undetected
-- because the only prior test here, self_covers above, compares a region
-- to itself, where direction cannot matter. These use a genuinely
-- asymmetric pair, so a reversed argument order fails them.
SELECT skycell_region_covers(circle('ICRS', 10, 20, 5.0), circle('ICRS', 10, 20, 1.0)) AS big_contains_small,
       skycell_region_covers(circle('ICRS', 10, 20, 1.0), circle('ICRS', 10, 20, 5.0)) AS small_does_not_contain_big,
       (circle('ICRS', 10, 20, 5.0) @> circle('ICRS', 10, 20, 1.0)) AS op_form_agrees,
       contains(circle('ICRS', 10, 20, 1.0), circle('ICRS', 10, 20, 5.0)) = 1 AS
         adql_contains_still_right_way_round;

-- joins: run-time slots and LATERAL vs brute force -------------------------
CREATE TABLE probe AS
SELECT id, ra + 0.0003 * (random() - 0.5) AS ra, greatest(-90, least(90, dec + 0.0003 * (random() - 0.5))) AS dec
FROM cat WHERE id % 97 = 0
UNION ALL SELECT -q, ra0, dec0 FROM cones;
ANALYZE probe;
WITH brute AS (SELECT count(*) AS n FROM probe p JOIN cat c ON skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 3/3600.0)),
     slots AS (SELECT count(*) AS n FROM probe p JOIN cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 3/3600.0)),
     lat AS (SELECT count(*) AS n FROM probe p CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, 3/3600.0, 'cat') r
             JOIN cat c ON c.cell BETWEEN r.lo AND r.hi AND skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 3/3600.0))
SELECT brute.n > 1000 AS many, brute.n = slots.n AS slots_ok, brute.n = lat.n AS lateral_ok FROM brute, slots, lat;
SET skycell.join_slots = 1;
SELECT (SELECT count(*) FROM probe p JOIN cat c ON skycell_cone(c.cell, c.ra, c.dec, p.ra, p.dec, 0.05))
     = (SELECT count(*) FROM probe p JOIN cat c ON skycell_in_cone(c.ra, c.dec, p.ra, p.dec, 0.05)) AS one_slot_ok;
RESET skycell.join_slots;

-- stored regions as MOCs in a B-tree: point-in-footprint -------------------
CREATE TABLE fp AS SELECT q AS fid, ra0, dec0, r FROM cones WHERE r <= 2;
CREATE TABLE fp_cells AS SELECT fid, unnest(skycell_cone_moc(ra0, dec0, r, 12)) AS nuniq FROM fp;
CREATE INDEX ON fp_cells (nuniq);
ANALYZE fp_cells;
WITH pts AS (SELECT * FROM cat WHERE id % 13 = 0),
moc AS (SELECT count(*) AS n FROM pts
        JOIN LATERAL (SELECT DISTINCT fid FROM fp_cells WHERE nuniq = ANY (skycell_ancestors(pts.cell))) m ON true
        JOIN fp USING (fid) WHERE skycell_in_cone(pts.ra, pts.dec, fp.ra0, fp.dec0, fp.r)),
brute AS (SELECT count(*) AS n FROM pts JOIN fp ON skycell_in_cone(pts.ra, pts.dec, fp.ra0, fp.dec0, fp.r))
SELECT brute.n > 1000 AS many, moc.n = brute.n AS moc_ok FROM moc, brute;
SELECT max(cardinality(skycell_cone_moc(ra0, dec0, r, 12))) <= 12 AS moc_size_ok FROM fp;

-- skycell_region_moc_ranges: the region-region recipe's own convenience
-- wrapper (skycell_region_moc() unnested and reduced to nuniq_lo/hi ranges
-- in one call) must return exactly as many rows as skycell_region_moc()
-- itself, capped the same way.
SELECT count(*) <= 8 AS ranges_capped,
       count(*) = cardinality(skycell_region_moc(circle('ICRS', 10, 20, 1), 8)) AS ranges_match_moc
FROM skycell_region_moc_ranges(circle('ICRS', 10, 20, 1), 8);

-- region-region INTERSECTS via MOC ranges: neither side of the join is a
-- point, so none of the per-row coverings above apply and skyregion has no
-- GiST opclass of its own -- but the same MOC decomposition, applied to
-- *both* sides and reduced to skycell_nuniq_lo/hi's native [lo,hi] cell-id
-- ranges, turns "do these two regions overlap" into a plain interval overlap
-- test that PostgreSQL's built-in int8range GiST opclass already indexes.
CREATE TABLE rr_a AS
  SELECT fid AS aid, CASE WHEN fid % 2 = 0 THEN circle('ICRS', ra0, dec0, r)
                          ELSE polygon('ICRS', ra0-r, dec0-r, ra0+r, dec0-r, ra0+r, dec0+r, ra0-r, dec0+r)
                     END AS region
  FROM fp WHERE abs(dec0) < 85;
CREATE TABLE rr_b AS
  SELECT fid AS bid, CASE WHEN fid % 2 = 1 THEN circle('ICRS', ra0 + r, dec0, r * 1.5)
                          ELSE polygon('ICRS', ra0-r*1.5, dec0-r, ra0+r*1.5, dec0-r, ra0+r*1.5, dec0+r, ra0-r*1.5, dec0+r)
                     END AS region
  FROM fp WHERE abs(dec0) < 85;
CREATE TABLE rr_b_moc AS
  SELECT bid, rng FROM rr_b, skycell_region_moc_ranges(region, 8);
CREATE INDEX ON rr_b_moc USING gist (rng);
ANALYZE rr_b_moc;
SELECT plan_uses_index(
  $q$SELECT DISTINCT a.aid FROM rr_a a
     JOIN LATERAL skycell_region_moc_ranges(a.region, 8) am ON true
     JOIN rr_b_moc b ON b.rng && am.rng$q$
) AS region_overlap_indexed;
WITH cand AS (
  SELECT DISTINCT a.aid, b.bid FROM rr_a a
  JOIN LATERAL skycell_region_moc_ranges(a.region, 8) am ON true
  JOIN rr_b_moc b ON b.rng && am.rng
),
moc AS (SELECT count(*) AS n FROM cand c
        JOIN rr_a a ON a.aid = c.aid JOIN rr_b b ON b.bid = c.bid
        WHERE intersects(a.region, b.region) = 1),
brute AS (SELECT count(*) AS n FROM rr_a a, rr_b b WHERE intersects(a.region, b.region) = 1)
SELECT brute.n > 50 AS many, moc.n = brute.n AS region_overlap_matches_exact FROM moc, brute;
DROP TABLE rr_a, rr_b, rr_b_moc;

-- skyregion GiST opclass (EXPERIMENTAL, ext/src/gist_region.c): the same
-- region-region INTERSECTS as above, but indexed directly (bounding-cap key)
-- with a plain CREATE INDEX ... USING gist (region) and no side table at
-- all. Both self-join (many-to-many) and literal-vs-column forms must plan
-- through the index and agree with the unindexed exact predicate.
CREATE TABLE rg_gist AS
  SELECT fid, ra0, dec0, r,
         CASE WHEN fid % 2 = 0 THEN circle('ICRS', ra0, dec0, r)
              ELSE polygon('ICRS', ra0-r, dec0-r, ra0+r, dec0-r, ra0+r, dec0+r, ra0-r, dec0+r)
         END AS region
  FROM fp WHERE abs(dec0) < 85;
CREATE INDEX ON rg_gist USING gist (region);
ANALYZE rg_gist;
SELECT plan_uses_index(
  $q$SELECT a.fid FROM rg_gist a, rg_gist b WHERE a.region && b.region AND a.fid < b.fid$q$
) AS gist_region_join_indexed;
SELECT (SELECT count(*) FROM rg_gist a, rg_gist b WHERE a.region && b.region AND a.fid < b.fid)
     = (SELECT count(*) FROM rg_gist a, rg_gist b WHERE intersects(a.region, b.region) = 1 AND a.fid < b.fid)
     AS gist_region_join_matches_exact;
-- (not asserting plan_uses_index for this single-table filter form: on a
-- fixture this small, a plain Seq Scan legitimately costs less than the
-- index regardless of operator, the same as any other tiny-table case in
-- this file -- the self-join above, where an index pays off even at this
-- size, is the one that must plan through it.)
SELECT (SELECT count(*) FROM rg_gist WHERE region && circle('ICRS', 0, 0, 20))
     = (SELECT count(*) FROM rg_gist WHERE intersects(region, circle('ICRS', 0, 0, 20)) = 1)
     AS gist_region_literal_matches_exact;

-- second strategy on the same opclass: @>(skyregion,skypos) / its commutator
-- <@(skypos,skyregion) ("which regions contain this point") -- the direction
-- the point-in-footprint recipe elsewhere in this file cannot serve at all
-- (it needs the point side to be the large, cell-indexed table). Probe
-- points are offset from each row's own centre, so most land inside their
-- source region (and sometimes a neighbour's, from overlap) rather than
-- every probe trivially matching only its own row.
CREATE TABLE rg_gist_pts AS
  SELECT fid AS pid, point('ICRS', ra0 + 0.3*r, dec0) AS pos FROM rg_gist;
SELECT plan_uses_index(
  $q$SELECT p.pid FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region$q$
) AS gist_region_contains_indexed;
SELECT (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region)
     = (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE skycell_in_region(p.pos, f.region))
     AS gist_region_contains_matches_exact;

-- an automatic alternative to the GiST strategy just above: a planner
-- support function (region_support_simplify, ext/src/adql.c) recognises a
-- GIN index on skycell_region_moc(region) and rewrites region_col @> pos /
-- pos <@ region_col into the array-overlap test the manual MOC-in-a-B-tree
-- recipe (skycell--0.11.sql's own comment block) computes by hand with a
-- side table -- same predicate, no side table, no hand-written join. The
-- rewrite replaces the @>/<@ clause outright whenever this index exists,
-- regardless of whether the planner then goes on to actually use that
-- index or a plain sequential scan of the rewritten form -- on a fixture
-- this small (a few hundred rows), GIN's own per-probe overhead legitimately
-- loses to a sequential scan, the same "not asserting plan_uses_index for
-- this small a table" caveat as the literal-vs-column && test above; the
-- rewrite firing at all, and computing the right answer, is what these
-- checks confirm. See GIST_REGION_DESIGN.md's "Round eight" for the numbers
-- (measured on a much larger fixture) showing this rewrite beats the GiST
-- strategy once a table is actually large enough for either index to pay
-- for itself.
CREATE FUNCTION plan_mentions(q text, needle text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; found bool := false;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l ~ needle THEN found := true; END IF;
  END LOOP;
  RETURN found;
END $$;
CREATE INDEX rg_gist_moc_gin ON rg_gist USING gin (skycell_region_moc(region));
ANALYZE rg_gist;
SELECT plan_mentions(
  $q$SELECT p.pid FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region$q$, 'skycell_region_moc'
) AS gin_region_contains_rewrite_fires;
SELECT (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region)
     = (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE skycell_in_region(p.pos, f.region))
     AS gin_region_contains_matches_exact;
-- the commutator spelling must rewrite and match identically
SELECT plan_mentions(
  $q$SELECT p.pid FROM rg_gist_pts p, rg_gist f WHERE f.region @> p.pos$q$, 'skycell_region_moc'
) AS gin_region_contains_commutator_rewrite_fires;
SELECT (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE f.region @> p.pos)
     = (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region)
     AS gin_region_contains_commutator_agrees;
-- a smaller, user-chosen max_order at index-creation time is read back and
-- used as the ancestor lookup's own upper bound (an exact, safe narrowing,
-- not a guess -- see gin_moc_index_for_region()'s own comment in
-- skycell.c); still must match exactly, not just rewrite.
DROP INDEX rg_gist_moc_gin;
CREATE INDEX rg_gist_moc_gin ON rg_gist USING gin (skycell_region_moc(region, 8, 16));
ANALYZE rg_gist;
SELECT plan_mentions(
  $q$SELECT p.pid FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region$q$, 'skycell_ancestors.*0, 16'
) AS gin_region_contains_narrowed_order;
SELECT (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE p.pos <@ f.region)
     = (SELECT count(*) FROM rg_gist_pts p, rg_gist f WHERE skycell_in_region(p.pos, f.region))
     AS gin_region_contains_narrowed_matches_exact;
DROP TABLE rg_gist_pts;

-- third strategy: @>(skyregion,skyregion) ("which rows wholly contain that
-- other region") -- reuses OVERLAP's own pruning test rather than a tighter
-- one (see ext/src/gist_region.c's "round six" for why a tighter test would
-- risk a false negative), so recheck (skycell_region_covers) does the real
-- work; this checks the index-accelerated answer still matches it exactly.
-- rg_gist's own row sizes span three orders of magnitude (fid <= 2 down to
-- sub-arcsecond, Table cones), so the self-join finds genuine containment,
-- not just the trivial a=b case skycell_region_covers(p, p) above covers.
SELECT plan_uses_index(
  $q$SELECT a.fid FROM rg_gist a, rg_gist b WHERE a.region @> b.region AND a.fid <> b.fid$q$
) AS gist_region_covers_join_indexed;
SELECT (SELECT count(*) FROM rg_gist a, rg_gist b WHERE a.region @> b.region AND a.fid <> b.fid)
     = (SELECT count(*) FROM rg_gist a, rg_gist b WHERE skycell_region_covers(a.region, b.region) AND a.fid <> b.fid)
     AS gist_region_covers_join_matches_exact;
SELECT (SELECT count(*) FROM rg_gist a, rg_gist b WHERE a.region @> b.region AND a.fid <> b.fid) > 0
     AS gist_region_covers_join_has_positives;

-- fourth strategy: <@(skyregion,skyregion), the third strategy's mirror --
-- "which rows are wholly contained within that other region". Round six's
-- @> had no COMMUTATOR, so only "indexed_col @> probe" could use the index;
-- round seven adds <@ (and backfills @>'s COMMUTATOR), so "indexed_col <@
-- probe" and the commutator spelling "probe @> indexed_col" both should.
SELECT plan_uses_index(
  $q$SELECT a.fid FROM rg_gist a, rg_gist b WHERE a.region <@ b.region AND a.fid <> b.fid$q$
) AS gist_region_covered_by_join_indexed;
SELECT (SELECT count(*) FROM rg_gist a, rg_gist b WHERE a.region <@ b.region AND a.fid <> b.fid)
     = (SELECT count(*) FROM rg_gist a, rg_gist b WHERE skycell_region_covered_by(a.region, b.region) AND a.fid <> b.fid)
     AS gist_region_covered_by_join_matches_exact;
SELECT (SELECT count(*) FROM rg_gist a, rg_gist b WHERE a.region <@ b.region AND a.fid <> b.fid) > 0
     AS gist_region_covered_by_join_has_positives;
-- the commutator spelling of the same predicate must agree exactly (both
-- plan through a's index, per the direction note above)
SELECT (SELECT count(*) FROM rg_gist a, rg_gist b WHERE a.region <@ b.region AND a.fid <> b.fid)
     = (SELECT count(*) FROM rg_gist a, rg_gist b WHERE b.region @> a.region AND a.fid <> b.fid)
     AS gist_region_covered_by_commutator_agrees;
DROP TABLE rg_gist;

-- expression index instead of a cell column (how egernia's ivoa.obscore is
-- indexed): the rewrite must use it and read its statistics
CREATE TABLE cat_expr AS SELECT id, ra, dec FROM cat;
CREATE INDEX cat_expr_cell ON cat_expr (skycell_ang2cell(ra, dec));
ALTER INDEX cat_expr_cell ALTER COLUMN 1 SET STATISTICS 1000;
ANALYZE cat_expr;
SELECT plan_uses_index('SELECT * FROM cat_expr WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, 10, 20, 0.1)') AS expr_indexed;
SELECT (SELECT count(*) FROM cat_expr WHERE skycell_cone(skycell_ang2cell(ra, dec), ra, dec, c.ra0, c.dec0, c.r))
     = (SELECT count(*) FROM cat_expr WHERE skycell_in_cone(ra, dec, c.ra0, c.dec0, c.r)) AS ok, count(*)
FROM cones c WHERE c.r <= 2 GROUP BY 1;

-- NULLs and edge cases -----------------------------------------------------
SELECT count(*) FROM cat WHERE skycell_cone(cell, ra, dec, NULL, 0, 1);
SELECT count(*) FROM cat WHERE skycell_cone(cell, ra, dec, 0, 0, -1);
SELECT count(*) = (SELECT count(*) FROM cat) AS all_sky FROM cat WHERE skycell_cone(cell, ra, dec, 0, 0, 180);
SELECT count(*) FROM skycell_cone_ranges(NULL, 0, 1);
