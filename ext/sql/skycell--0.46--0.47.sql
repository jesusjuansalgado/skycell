-- skycell 0.46 -> 0.47: PostgreSQL's point as a sky position

-- With skycell in public, ADQL's bare POINT(ra, dec) is pg_catalog's
-- point(float8, float8), and CONTAINS(POINT(ra, dec), CIRCLE(...)) found no
-- function.  An implicit cast reads that point as (ra, dec) in degrees; its
-- support function plans the cast of point(ra, dec) as skycell's own
-- point(ra, dec), so the search stays indexed.
CREATE FUNCTION skycell_pos_from_point_support(internal) RETURNS internal
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION skycell_pos_from_point(p point) RETURNS skypos
AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
SUPPORT skycell_pos_from_point_support;
COMMENT ON FUNCTION skycell_pos_from_point(point) IS
  'PostgreSQL''s point as a sky position: x the right ascension, y the declination, degrees.';
CREATE CAST (point AS skypos) WITH FUNCTION skycell_pos_from_point(point) AS IMPLICIT;

-- Partitioning by sky region: a HEALPix cell of order k is one range of the
-- order-29 ids the index holds, so a table partitioned by RANGE on the cell
-- can be cut into cells.  The id range of one, and every partition of an
-- order made in one call.
CREATE FUNCTION skycell_cell_range(hpx_order integer, ipix bigint, OUT lo bigint, OUT hi bigint)
LANGUAGE plpgsql IMMUTABLE STRICT PARALLEL SAFE AS $$
BEGIN
  IF hpx_order < 0 OR hpx_order > 29 THEN
    RAISE EXCEPTION 'HEALPix order % is out of range (0-29)', hpx_order;
  END IF;
  IF ipix < 0 OR ipix >= 12 * (1::bigint << (2 * hpx_order)) THEN
    RAISE EXCEPTION 'cell % does not exist at order % (0 to %)', ipix, hpx_order,
      12 * (1::bigint << (2 * hpx_order)) - 1;
  END IF;
  lo := ipix << (2 * (29 - hpx_order));
  hi := ((ipix + 1) << (2 * (29 - hpx_order))) - 1;
END $$;
COMMENT ON FUNCTION skycell_cell_range(integer, bigint) IS
  'The first and last order-29 cell ids inside a NESTED HEALPix cell: a partition holding that cell is FOR VALUES FROM (lo) TO (hi + 1).';

CREATE FUNCTION skycell_create_partitions(parent regclass, hpx_order integer,
                                          with_default boolean DEFAULT true)
RETURNS integer LANGUAGE plpgsql AS $$
DECLARE
  strat "char";
  nkeys smallint;
  nsp name;
  base name;
  n integer := 0;
  shift integer;
BEGIN
  SELECT partstrat, partnatts INTO strat, nkeys FROM pg_partitioned_table WHERE partrelid = parent;
  IF NOT FOUND OR strat <> 'r' OR nkeys <> 1 THEN
    RAISE EXCEPTION '% is not partitioned by RANGE on one key', parent
      USING HINT = 'CREATE TABLE ... PARTITION BY RANGE (skycell_ang2cell(ra, dec)), or on a stored cell column.';
  END IF;
  IF hpx_order IS NULL OR hpx_order < 0 OR hpx_order > 4 THEN
    RAISE EXCEPTION 'partitions of HEALPix order % (0-4: 12 to 3072 partitions)', hpx_order;
  END IF;
  IF EXISTS (SELECT FROM pg_inherits WHERE inhparent = parent) THEN
    RAISE EXCEPTION '% already has partitions', parent;
  END IF;
  SELECT n2.nspname, c.relname INTO nsp, base
  FROM pg_class c JOIN pg_namespace n2 ON n2.oid = c.relnamespace WHERE c.oid = parent;
  shift := 2 * (29 - hpx_order);
  FOR p IN 0 .. 12 * (1 << (2 * hpx_order)) - 1 LOOP
    EXECUTE format('CREATE TABLE %I.%I PARTITION OF %s FOR VALUES FROM (%s) TO (%s)',
                   nsp, left(base, 50) || '_' || hpx_order || '_' || p, parent,
                   p::bigint << shift, (p::bigint + 1) << shift);
    n := n + 1;
  END LOOP;
  IF with_default THEN
    EXECUTE format('CREATE TABLE %I.%I PARTITION OF %s DEFAULT', nsp, left(base, 50) || '_default', parent);
    n := n + 1;
  END IF;
  RETURN n;
END $$;
COMMENT ON FUNCTION skycell_create_partitions(regclass, integer, boolean) IS
  'Cut a table partitioned by RANGE on the cell into the 12 * 4^order HEALPix cells of an order (0-4), one partition each, named <table>_<order>_<cell>, and a default partition for rows without a position (their cell is NULL). Returns the partitions made.';
