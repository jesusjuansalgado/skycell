\echo Use "ALTER EXTENSION skycell UPDATE TO '0.41'" to load this file. \quit

-- ------------------------------------------------------------------
-- 0.41: estimates of the number of sources in a region
--
-- The rows a table's statistics expect in a region or a HEALPix cell, read
-- from the density model the planner uses (the ANALYZE histogram of the
-- table's cell index), without reading a row: for a zoomed-out sky view,
-- the count of sources in it and a coarse density map, in microseconds.  As
-- current as the last ANALYZE.  Scalar, so that ADQL can call them, on a
-- TAP service that declares them; the table is named by a string.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_count_estimate(tbl text, r skyregion) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_count_estimate(text, skyregion) IS
  'The rows the statistics of table tbl (its skycell cell index; a partitioned table''s partitions) expect in region r, as the planner estimates them; no row is read. ADQL: skycell_count_estimate(''schema.table'', CIRCLE(''ICRS'', ra, dec, radius)).';

CREATE FUNCTION skycell_count_estimate(tbl text, hpx_order integer, ipix bigint) RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_count_estimate_cell' LANGUAGE C STABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_count_estimate(text, integer, bigint) IS
  'The rows the statistics of table tbl expect in the NESTED HEALPix cell ipix of order hpx_order (a HiPS tile); no row is read.';

CREATE FUNCTION skycell_density_estimate(tbl text, r skyregion, hpx_order integer,
                                         OUT ipix bigint, OUT estimate float8, OUT inside boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 100;
COMMENT ON FUNCTION skycell_density_estimate(text, skyregion, integer) IS
  'The HEALPix cells of order hpx_order (0-20) that region r meets, with the rows the statistics of table tbl expect in each and whether it lies wholly inside r; at most 200,000 cells. For SQL; ADQL calls skycell_count_estimate per cell.';
