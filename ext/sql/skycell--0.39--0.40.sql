\echo Use "ALTER EXTENSION skycell UPDATE TO '0.40'" to load this file. \quit

-- ------------------------------------------------------------------
-- 0.40: partition pruning by a cone's cells
--
-- skycell_cone_cell_lo/hi(ra0, dec0, radius): the lowest and highest
-- order-29 cell a cone can hold.  Next to a cone on a catalogue partitioned
-- by cell range (by skycell_ang2cell(ra, dec), a stored cell column, or a
-- source_id that encodes the cell), the planner adds cell >= lo and
-- cell <= hi, from which PostgreSQL prunes the partitions the cone cannot
-- reach: at plan time for a constant cone, and at every rescan for a
-- cross-match's.  The custom scan leaves the two comparisons out, as the
-- cone implies them.
-- ------------------------------------------------------------------

CREATE FUNCTION skycell_cone_cell_lo(ra0 float8, dec0 float8, radius float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_cone_cell_lo(float8, float8, float8) IS
  'The lowest order-29 cell the cone (ra0, dec0, radius in degrees) can hold; with skycell_cone_cell_hi, an interval every cell of the cone lies in. The planner adds these next to a cone on a catalogue partitioned by cell range, for partition pruning.';

CREATE FUNCTION skycell_cone_cell_hi(ra0 float8, dec0 float8, radius float8) RETURNS int8
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_cone_cell_hi(float8, float8, float8) IS
  'The highest order-29 cell the cone (ra0, dec0, radius in degrees) can hold; see skycell_cone_cell_lo.';
