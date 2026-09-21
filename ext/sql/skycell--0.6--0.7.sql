
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
