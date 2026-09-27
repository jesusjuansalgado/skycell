\echo Use "CREATE EXTENSION skycell" to load this file. \quit

-- ------------------------------------------------------------------
-- keys and distances
-- ------------------------------------------------------------------

-- order-29 HEALPix NESTED cell of a position (the index key)
CREATE FUNCTION skycell_ang2cell(ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_ang2pix("order" int, ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- angular distance in degrees
CREATE FUNCTION skycell_dist(ra1 float8, dec1 float8, ra2 float8, dec2 float8) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- exact predicates (the "refine" step); last arg is a selectivity hint
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_exact_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION skycell_in_cone(ra float8, "dec" float8, ra0 float8, dec0 float8,
                                radius float8, sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_exact_support;

CREATE FUNCTION skycell_in_poly(ra float8, "dec" float8, poly float8[],
                                sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_exact_support;

-- ------------------------------------------------------------------
-- indexable predicates: rewritten by the planner support function into
-- B-tree range conditions on "cell" + the exact test
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- cell must be skycell_ang2cell(ra, dec)
CREATE FUNCTION skycell_cone(cell int8, ra float8, "dec" float8,
                             ra0 float8, dec0 float8, radius float8) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- poly = ARRAY[ra1, dec1, ra2, dec2, ...] (convex, degrees)
CREATE FUNCTION skycell_poly(cell int8, ra float8, "dec" float8, poly float8[]) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- run-time range slot i (lo = even, hi = odd) for non-constant cones (joins)
CREATE FUNCTION skycell_cone_bound(ra0 float8, dec0 float8, radius float8, i int,
                                   nslots int, ntotal float8, hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- run-time range slot i (lo = even, hi = odd) for non-constant polygons
-- (joins): the polygon analogue of skycell_cone_bound, keyed on the array's
-- own bytes rather than three scalars.
CREATE FUNCTION skycell_poly_bound(poly float8[], i int, nslots int, ntotal float8,
                                   hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- explicit coverings (LATERAL joins, inspection)
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_cone_ranges(ra0 float8, dec0 float8, radius float8,
                                    tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                    OUT lo int8, OUT hi int8) RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE ROWS 3;

CREATE FUNCTION skycell_poly_ranges(poly float8[], tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                    OUT lo int8, OUT hi int8) RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE ROWS 8;

CREATE FUNCTION skycell_cover_info(ra0 float8, dec0 float8, radius float8,
                                   tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                   OUT nranges int, OUT steps int, OUT deepest int,
                                   OUT exp_rows float8, OUT area_ratio float8,
                                   OUT rho float8, OUT chosen_order int) RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- regions as multi-order coverages (IVOA MOC NUNIQ), for indexing stored
-- footprints in a B-tree: a point's candidate regions are those with a
-- covering cell equal to one of the point's 30 ancestors.
--
-- This is a different problem from the cross-match join above: here it's the
-- *stored regions* being searched (which footprint contains this point?),
-- not a per-row region being matched against an indexed point column, so
-- there's no single outer row to compute one covering from -- the region
-- column itself needs the index.
--
--   CREATE TABLE fp_cells AS
--     SELECT fid, unnest(skycell_region_moc(s_region, 12)) AS nuniq FROM footprints;
--   CREATE INDEX ON fp_cells (nuniq);
--
--   SELECT DISTINCT f.* FROM points p
--   JOIN LATERAL (SELECT DISTINCT fid FROM fp_cells
--                 WHERE nuniq = ANY (skycell_ancestors(p.cell))) m ON true
--   JOIN footprints f ON f.fid = m.fid
--   WHERE skycell_pos_in_region(point(p.ra, p.dec), f.s_region);  -- refine: the MOC overshoots
--
-- Not something a query rewrite can do -- unlike every other recipe in this
-- file, it needs a side table and a join the original query doesn't have, so
-- it has no automatic form (planner support function or ADQL translator
-- alike): build fp_cells once, keep it in sync with footprints, and write the
-- join above by hand. See ext/test/sql/skycell.sql's "stored regions as MOCs
-- in a B-tree" for a complete, tested example.
-- ------------------------------------------------------------------

-- ------------------------------------------------------------------
-- region-region INTERSECTS (INTERSECTS(region, region), skyregion's own &&):
-- neither side is a point, so there is no per-row covering to compute and no
-- point-in-footprint MOC join above to reuse as-is -- skyregion has no GiST
-- opclass, so unlike pgSphere's scircle/spoly this is genuinely unindexed on
-- its own.
--
-- The same MOC decomposition still helps, applied to *both* sides instead of
-- one: skycell_region_moc_ranges() (below, next to skycell_region_moc's own
-- definition) breaks a region into a handful of NUNIQ cells and turns each
-- back into the native [lo,hi] cell-id range the cross-match coverings above
-- already use, as a ready int8range. Because a MOC is a strict hierarchical
-- partition, two regions can only truly overlap if one pair of their cell
-- ranges overlaps as plain integer intervals -- so explode each side into
-- (row_id, int8range), index one side with PostgreSQL's own built-in GiST
-- support for int8range (no custom opclass needed), and join on `&&`:
--
--   CREATE TABLE b_moc AS SELECT bid, rng FROM b, skycell_region_moc_ranges(b.s_region, 8);
--   CREATE INDEX ON b_moc USING gist (rng);
--
--   SELECT DISTINCT a.aid, b.bid FROM a
--   JOIN LATERAL skycell_region_moc_ranges(a.s_region, 8) am ON true
--   JOIN b_moc bm ON bm.rng && am.rng
--   JOIN b ON b.bid = bm.bid  -- refine: the MOC covering overshoots, same as the point case
--   WHERE intersects(a.s_region, b.s_region) = 1;
--
-- Like the point-in-footprint recipe above, this needs a side table and a
-- join no query rewrite can introduce, so it has no automatic form either --
-- build b_moc once, keep it in sync with b, and write the join by hand. It is
-- also an approximation twice over (the MOC covering, then the interval
-- overlap test), so the exact intersects() recheck at the end is required,
-- not optional, exactly as for point-in-footprint. See ext/test/sql/
-- skycell.sql's "region-region INTERSECTS via MOC ranges" for a complete,
-- tested example, and bench/21_region_overlap.sql for how this compares to
-- pgSphere's natively GiST-indexed && on scircle/spoly.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_ancestors(cell int8, min_order int DEFAULT 0, max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_order(nuniq int8) RETURNS int
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_lo(nuniq int8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_nuniq_hi(nuniq int8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_cone_moc(ra0 float8, dec0 float8, radius float8,
                                 max_cells int DEFAULT 8, max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_poly_moc(poly float8[], max_cells int DEFAULT 8,
                                 max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;


-- ------------------------------------------------------------------
-- The ADQL surface: two types and functions named as the standard names
-- them, so a TAP translator emits the standard's own spelling.
--
--   1 = CONTAINS(POINT('ICRS', s_ra, s_dec), CIRCLE('ICRS', 10, 20, 0.1))
--     -> 1 = contains(point('ICRS', s_ra, s_dec), circle('ICRS', 10, 20, 0.1))
--
-- The forms carrying ADQL's coordinate-system argument never collide with
-- PostgreSQL's built-in point/circle/box/polygon; the two-argument forms do,
-- so put this extension's schema before pg_catalog to use them unqualified:
--   SET search_path = skycell, public, pg_catalog;
-- ------------------------------------------------------------------

CREATE TYPE skypos;

CREATE FUNCTION skypos_in(cstring) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skypos_out(skypos) RETURNS cstring
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE skypos (
    INPUT = skypos_in, OUTPUT = skypos_out,
    INTERNALLENGTH = 16, ALIGNMENT = double, STORAGE = plain
);
COMMENT ON TYPE skypos IS 'a position on the sky: (ra, dec) in degrees, ICRS';

CREATE TYPE skyregion;

CREATE FUNCTION skyregion_in(cstring) RETURNS skyregion
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skyregion_out(skyregion) RETURNS cstring
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE skyregion (
    INPUT = skyregion_in, OUTPUT = skyregion_out,
    INTERNALLENGTH = VARIABLE, ALIGNMENT = double, STORAGE = extended
);
COMMENT ON TYPE skyregion IS 'a region on the sky: CIRCLE(ra, dec, radius) or POLYGON(ra1, dec1, ...), degrees, ICRS';

-- constructors ------------------------------------------------------

CREATE FUNCTION point(ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_point' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION point(coordsys text, ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_point_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION circle(ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_circle' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION circle(coordsys text, ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_circle_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION box(ra float8, "dec" float8, width float8, height float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_box' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION box(coordsys text, ra float8, "dec" float8, width float8, height float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_box_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION polygon(VARIADIC coords float8[]) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_polygon' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION polygon(coordsys text, VARIADIC coords float8[]) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_polygon_cs' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- PostgreSQL has its own point(float8, float8); the ADQL form with the
-- coordinate system never collides, and these aliases let a deployment that
-- cannot reorder search_path write the two-argument ones unambiguously.
CREATE FUNCTION skycell_point(ra float8, "dec" float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_point' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_circle(ra float8, "dec" float8, radius float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_circle' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_box(ra float8, "dec" float8, width float8, height float8) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_box' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_polygon(VARIADIC coords float8[]) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_polygon' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- predicates and measures -------------------------------------------

CREATE FUNCTION contains(p skypos, r skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_contains' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION contains(a skyregion, b skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_contains_region' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION intersects(p skypos, r skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_intersects_pos' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION intersects(a skyregion, b skyregion) RETURNS int
AS 'MODULE_PATHNAME', 'skycell_intersects' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION distance(a skypos, b skypos) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_distance' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION distance(ra1 float8, dec1 float8, ra2 float8, dec2 float8) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_dist' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION area(r skyregion) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_area' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION coord1(p skypos) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_coord1' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION coord2(p skypos) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_coord2' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION centroid(r skyregion) RETURNS skypos
AS 'MODULE_PATHNAME', 'skycell_centroid' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- the index key of a position, for CREATE INDEX ... (skycell_cell(pos))
CREATE FUNCTION skycell_cell(p skypos) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- operators: the indexable spelling ---------------------------------

CREATE FUNCTION skycell_region_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_region_has_pos_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_region_sel_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- the exact test the rewrite appends (last argument is a selectivity hint)
CREATE FUNCTION skycell_in_region(p skypos, r skyregion, sel float8 DEFAULT -1) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_pos_in_region_sel' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_sel_support;

CREATE FUNCTION skycell_pos_in_region(p skypos, r skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_support;

-- run-time range slot i (lo = even, hi = odd) for a non-constant skyregion
-- column (joins): the generic-region analogue of skycell_cone_bound/
-- skycell_poly_bound, for <@ against a per-row region of either kind (a
-- cross-match whose CIRCLE-or-POLYGON comes from another table's row, e.g.
-- an archive's per-row s_region footprint).
CREATE FUNCTION skycell_region_bound(region skyregion, i int, nslots int, ntotal float8,
                                     hist int8[]) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;

CREATE FUNCTION skycell_region_has_pos(r skyregion, p skypos) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_region_has_pos_support;
CREATE FUNCTION skycell_region_overlap(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_region_covers(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION skycell_region_covered_by(a skyregion, b skyregion) RETURNS bool
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR <@ (
    LEFTARG = skypos, RIGHTARG = skyregion,
    FUNCTION = skycell_pos_in_region, COMMUTATOR = @>,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR @> (
    LEFTARG = skyregion, RIGHTARG = skypos,
    FUNCTION = skycell_region_has_pos, COMMUTATOR = <@,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR && (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_overlap, COMMUTATOR = &&,
    RESTRICT = areasel, JOIN = areajoinsel
);
CREATE OPERATOR @> (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_covers, COMMUTATOR = <@,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR <@ (
    LEFTARG = skyregion, RIGHTARG = skyregion,
    FUNCTION = skycell_region_covered_by, COMMUTATOR = @>,
    RESTRICT = contsel, JOIN = contjoinsel
);
CREATE OPERATOR <-> (
    LEFTARG = skypos, RIGHTARG = skypos,
    FUNCTION = distance, COMMUTATOR = <->
);

-- ------------------------------------------------------------------
-- Astronomy: the IVOA UDF registry's names, so ADQL written for other
-- services runs here unchanged.
-- ------------------------------------------------------------------

CREATE FUNCTION ivo_epoch_prop(ra float8, "dec" float8, parallax float8,
                               pmra float8, pmdec float8, radial_velocity float8,
                               ref_epoch float8, out_epoch float8) RETURNS float8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION ivo_epoch_prop(float8, float8, float8, float8, float8, float8, float8, float8)
IS 'six-parameter astrometric solution propagated to out_epoch: {ra, dec, parallax, pmra, pmdec, radial_velocity}';

CREATE FUNCTION ivo_epoch_prop_pos(ra float8, "dec" float8, parallax float8,
                                   pmra float8, pmdec float8, radial_velocity float8,
                                   ref_epoch float8, out_epoch float8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_epoch_prop_pos(ra float8, "dec" float8, pmra float8, pmdec float8,
                                   ref_epoch float8, out_epoch float8) RETURNS skypos
AS 'MODULE_PATHNAME', 'ivo_epoch_prop_pos_pm' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_apply_pm(ra float8, "dec" float8, pmra float8, pmdec float8,
                             epdiff float8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- how far a proper motion can carry a source in dt years (degrees)
CREATE FUNCTION skycell_pm_margin(pmra float8, pmdec float8, dt float8) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION icrs2gal(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION gal2icrs(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION icrs2ecl(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ecl2icrs(p skypos) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION ivo_healpix_index("order" int, ra float8, "dec" float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_healpix_index("order" int, p skypos) RETURNS int8
AS 'MODULE_PATHNAME', 'ivo_healpix_index_pos' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION ivo_healpix_center("order" int, hpxindex int8) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- the four corners of a cell: ra, dec, ra, dec, ... (for plotting a covering)
CREATE FUNCTION skycell_cell_corners("order" int, pix int8) RETURNS float8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- the price of one index range, in rows, that the cost model derives for a
-- relation (skycell.range_cost = -1, the default); see auto_range_cost()
CREATE FUNCTION skycell_range_cost(tbl regclass DEFAULT NULL, col name DEFAULT 'cell')
RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_range_cost_for' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;

-- ------------------------------------------------------------------
-- multi-order count map
--
-- ANALYZE's histogram cannot see a cluster smaller than one of its buckets,
-- and because it is equi-depth, raising the statistics target does not help:
-- a denser region gets narrower buckets, so interpolating inside one is scale
-- invariant.  This map counts the rows in each of a set of disjoint cells,
-- split until a cell holds few enough rows for interpolation inside it to be
-- harmless.  The covering reads it in preference to the histogram.
-- ------------------------------------------------------------------

CREATE TABLE skycell_density_map (
  statrel  oid    NOT NULL,
  attnum   int2   NOT NULL,
  nuniq    int8   NOT NULL,
  n        int8   NOT NULL,
  PRIMARY KEY (statrel, attnum, nuniq)
);
SELECT pg_catalog.pg_extension_config_dump('skycell_density_map', '');

CREATE FUNCTION skycell_density_build(tbl regclass, col name DEFAULT 'cell',
                                      rows_per_cell int DEFAULT 100,
                                      max_order int DEFAULT 13)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
  srel oid; att int2; k int; nleaf bigint; expr text; idx oid;
BEGIN
  IF rows_per_cell < 1 OR max_order < 1 OR max_order > 20 THEN
    RAISE EXCEPTION 'skycell: rows_per_cell must be >= 1 and max_order within [1, 20]';
  END IF;

  -- statistics live on the table for a plain column, on the index for an
  -- expression index: match what the planner support function looks up.
  SELECT a.attnum, tbl INTO att, srel
  FROM pg_attribute a WHERE a.attrelid = tbl AND a.attname = col AND NOT a.attisdropped;
  IF att IS NULL THEN
    SELECT i.indexrelid INTO idx FROM pg_index i
    WHERE i.indrelid = tbl AND pg_get_indexdef(i.indexrelid) LIKE '%skycell_ang2cell%'
    ORDER BY i.indexrelid LIMIT 1;
    IF idx IS NULL THEN
      RAISE EXCEPTION 'skycell: no column % and no skycell_ang2cell index on %', col, tbl;
    END IF;
    srel := idx; att := 1;
    expr := (SELECT pg_get_expr(i.indexprs, i.indrelid) FROM pg_index i WHERE i.indexrelid = idx);
  ELSE
    expr := quote_ident(col);
  END IF;

  DELETE FROM skycell_density_map m WHERE m.statrel = srel AND m.attnum = att;

  /*
   * Count at the finest order allowed, then roll up one order at a time: a
   * cell is a leaf when it holds at most rows_per_cell and its parent does
   * not, plus the cells at the finest order that are still too full to split
   * further.  A recursive CTE cannot do this (no aggregates in the recursive
   * term), so it is a loop over the orders.
   */
  CREATE TEMP TABLE sc_lev (ord int, pix int8, n int8) ON COMMIT DROP;
  EXECUTE format(
    'INSERT INTO sc_lev SELECT %s, (%s) >> (2 * (29 - %s)), count(*) FROM %s '
    'WHERE (%s) IS NOT NULL GROUP BY 2',
    max_order, expr, max_order, tbl::text, expr);

  FOR k IN REVERSE max_order .. 1 LOOP
    INSERT INTO sc_lev SELECT k - 1, pix >> 2, sum(n) FROM sc_lev WHERE ord = k GROUP BY 2;
    EXIT WHEN NOT FOUND;
  END LOOP;
  CREATE INDEX ON sc_lev (ord, pix);

  INSERT INTO skycell_density_map (statrel, attnum, nuniq, n)
  SELECT srel, att, (4::int8 << (2 * u.ord)) + u.pix, u.n
  FROM sc_lev u
  LEFT JOIN sc_lev p ON p.ord = u.ord - 1 AND p.pix = u.pix >> 2
  WHERE (u.n <= rows_per_cell AND (p.n IS NULL OR p.n > rows_per_cell))
     OR (u.ord = max_order AND u.n > rows_per_cell);

  GET DIAGNOSTICS nleaf = ROW_COUNT;
  DROP TABLE sc_lev;
  RETURN nleaf;
END $$;

CREATE FUNCTION skycell_density_drop(tbl regclass, col name DEFAULT 'cell') RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE n bigint; idx oid;
BEGIN
  DELETE FROM skycell_density_map m
  WHERE m.statrel = tbl OR m.statrel IN (SELECT indexrelid FROM pg_index WHERE indrelid = tbl);
  GET DIAGNOSTICS n = ROW_COUNT;
  RETURN n;
END $$;

-- ------------------------------------------------------------------
-- Q3C-shaped spellings
-- ------------------------------------------------------------------
-- Same predicate as skycell_cone, without the explicit index expression:
-- the planner support function synthesises skycell_ang2cell(ra, dec) from the
-- first two arguments, so migrating from Q3C is a prefix replacement.
--   q3c_join(a.ra, a.dec, b.ra, b.dec, r)  ->  skycell_join(...)
--   q3c_radial_query(ra, dec, ra0, dec0, r) -> skycell_radial_query(...)
-- Both need an index on skycell_ang2cell(ra, dec) to be rewritten to ranges,
-- exactly as q3c needs one on q3c_ang2ipix(ra, dec); without it the plan is a
-- correct sequential scan.

CREATE FUNCTION skycell_join(ra1 float8, dec1 float8, ra2 float8, dec2 float8,
                             radius float8) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_cone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

CREATE FUNCTION skycell_radial_query(ra float8, "dec" float8, ra0 float8, dec0 float8,
                                     radius float8) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_cone'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- The polygon analogue of skycell_join/skycell_radial_query, for a
-- cross-match whose polygon comes from another table's row (a q3c_poly_query
-- has no such counterpart -- Q3C has no region type -- so this has no
-- existing spelling to match; ra/dec/poly is skycell_poly's own order,
-- without the leading cell argument).
CREATE FUNCTION skycell_poly_join(ra float8, "dec" float8, poly float8[]) RETURNS bool
AS 'MODULE_PATHNAME', 'skycell_in_poly'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_support;

-- ------------------------------------------------------------------
-- Cell-range selectivity: a b-tree operator class for wide relations
-- ------------------------------------------------------------------
-- A covering becomes `cell >= lo AND cell <= hi`.  With lo and hi constant the
-- stock estimator reads the histogram and does well.  In a cross-match they are
-- not constant -- they come from the probe row -- and PostgreSQL has no
-- estimator for that, so each side gets DEFAULT_INEQ_SEL and the pair estimates
-- a ninth of the relation per probe.  On a 20-million-row ObsCore relation that
-- is 2.3 million rows where the truth is about one; the parameterised index
-- path is costed out of existence and a sequential scan is chosen instead.
-- Measured: >120 s for the plan the planner picks, 50 ms for the one it rejects.
--
-- Selectivity for an operator comes from pg_operator.oprrest, and int8's cannot
-- be changed, so the fix is our own operators carrying our own estimator, in
-- our own b-tree operator class.  This is opt-in: build the index with
-- skycell_cell_ops and write the range join with #>= and #<=.  Indexes built
-- the ordinary way keep working exactly as before.
--
--   CREATE INDEX t_cell ON t (skycell_ang2cell(ra, dec) skycell_cell_ops);
--
--   SELECT ... FROM probes p
--   CROSS JOIN LATERAL skycell_cone_ranges(p.ra, p.dec, r) g
--   JOIN t ON skycell_ang2cell(t.ra, t.dec) #>= g.lo
--         AND skycell_ang2cell(t.ra, t.dec) #<= g.hi
--   WHERE skycell_in_cone(t.ra, t.dec, p.ra, p.dec, r);

CREATE FUNCTION skycell_cellsel(internal, oid, internal, integer) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE OPERATOR #< (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8lt,
  RESTRICT = skycell_cellsel, JOIN = scalarltjoinsel, COMMUTATOR = #>);
CREATE OPERATOR #> (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8gt,
  RESTRICT = skycell_cellsel, JOIN = scalargtjoinsel, COMMUTATOR = #<);
CREATE OPERATOR #<= (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8le,
  RESTRICT = skycell_cellsel, JOIN = scalarlejoinsel, COMMUTATOR = #>=);
CREATE OPERATOR #>= (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8ge,
  RESTRICT = skycell_cellsel, JOIN = scalargejoinsel, COMMUTATOR = #<=);
CREATE OPERATOR #= (LEFTARG = int8, RIGHTARG = int8, PROCEDURE = int8eq,
  RESTRICT = eqsel, JOIN = eqjoinsel, COMMUTATOR = #=, HASHES, MERGES);

CREATE OPERATOR CLASS skycell_cell_ops FOR TYPE int8 USING btree AS
  OPERATOR 1 #<, OPERATOR 2 #<=, OPERATOR 3 #=, OPERATOR 4 #>=, OPERATOR 5 #>,
  FUNCTION 1 btint8cmp(int8, int8);

-- ------------------------------------------------------------------
-- A single covering function for a skyregion value, whichever kind it
-- holds -- skyregion is one type for both circles and polygons (its own
-- "kind" tag), so one column can mix them; skycell_cone_moc/
-- skycell_poly_moc take raw arguments and require the caller to already
-- know which one a given row is, this dispatches on the stored value
-- itself the same way contains()/intersects()/area() already do.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_region_moc(region skyregion, max_cells int DEFAULT 8,
                                   max_order int DEFAULT 29) RETURNS int8[]
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Convenience wrapper for the region-region INTERSECTS recipe above (see the
-- worked example next to skycell_region_moc's own comment block, near the top
-- of this file): turns a region straight into its MOC cells' native [lo,hi]
-- cell-id ranges as int8range values, ready for PostgreSQL's own built-in
-- GiST opclass -- skips the unnest(skycell_region_moc(...)) +
-- skycell_nuniq_lo/skycell_nuniq_hi + int8range(...) boilerplate every use of
-- that recipe would otherwise repeat, on both the side-table build and the
-- per-query side.
CREATE FUNCTION skycell_region_moc_ranges(region skyregion, max_cells int DEFAULT 8,
                                          max_order int DEFAULT 29,
                                          OUT nuniq int8, OUT rng int8range) RETURNS SETOF record
AS $$ SELECT n, int8range(skycell_nuniq_lo(n), skycell_nuniq_hi(n), '[]')
      FROM unnest(skycell_region_moc(region, max_cells, max_order)) AS n $$
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE ROWS 8;

-- ------------------------------------------------------------------
-- A real index on skyregion itself: a GiST opclass over && (region-region
-- INTERSECTS) and @> (region contains point), so `a.region && b.region` and
-- `f.region @> point(...)` / `point(...) <@ f.region` all work off an
-- ordinary `CREATE INDEX ON b USING gist (region)` with no side table and no
-- hand-written join -- unlike the MOC-ranges recipe above, which needs both,
-- or the point-in-footprint covering recipe, which needs the covering
-- precomputed per row.
--
-- Tuned, not just a correctness spike: a multi-cap key (up to 4 bounding
-- caps per region, see ext/src/gist_region.c's own file header) and an
-- R*-tree-style picksplit. Benchmarked against the MOC-ranges recipe and
-- pgSphere's native && in bench/22_region_gist.sql, and against the
-- point-in-footprint covering recipe in bench/23_region_contains.sql.
-- ------------------------------------------------------------------

CREATE FUNCTION skyregion_gist_consistent(internal, internal, int4, oid, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_union(internal, internal) RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_compress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_decompress(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_penalty(internal, internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_picksplit(internal, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION skyregion_gist_same(bytea, bytea, internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR CLASS skyregion_gist_ops
    DEFAULT FOR TYPE skyregion USING gist AS
    OPERATOR 1 && (skyregion, skyregion),
    OPERATOR 2 @> (skyregion, skypos),
    OPERATOR 3 @> (skyregion, skyregion),
    OPERATOR 4 <@ (skyregion, skyregion),
    FUNCTION 1 skyregion_gist_consistent(internal, internal, int4, oid, internal),
    FUNCTION 2 skyregion_gist_union(internal, internal),
    FUNCTION 3 skyregion_gist_compress(internal),
    FUNCTION 4 skyregion_gist_decompress(internal),
    FUNCTION 5 skyregion_gist_penalty(internal, internal, internal),
    FUNCTION 6 skyregion_gist_picksplit(internal, internal),
    FUNCTION 7 skyregion_gist_same(bytea, bytea, internal),
    STORAGE bytea;

COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'EXPERIMENTAL: indexes region-region && and both directions of full containment (@>, <@), and region @> point, directly (multi-cap bounding key, R*-tree-style picksplit). Region-region @>/<@ reuse &&''s own pruning test (sound but not tight -- see ext/src/gist_region.c). See ext/src/gist_region.c, bench/22_region_gist.sql, bench/23_region_contains.sql, and bench/24_region_contains_region.sql before relying on this.';
