-- skycell 0.3 -> 0.4
--
-- skycell.range_cost now defaults to -1, meaning: derive the price of an index
-- range from the relation's own statistics (rows per page) and the planner's
-- cost parameters, instead of a fixed constant.  skycell_range_cost() reports
-- what that derivation gives for a relation.
\echo Use "ALTER EXTENSION skycell UPDATE TO '0.4'" to load this file. \quit

-- the price of one index range, in rows, that the cost model derives for a
-- relation (skycell.range_cost = -1, the default); see auto_range_cost()
CREATE FUNCTION skycell_range_cost(tbl regclass DEFAULT NULL, col name DEFAULT 'cell')
RETURNS float8
AS 'MODULE_PATHNAME', 'skycell_range_cost_for' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;
