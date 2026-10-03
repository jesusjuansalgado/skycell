\echo Use "ALTER EXTENSION skycell UPDATE TO '0.16'" to load this file. \quit

-- Restriction selectivity for the region-region operators: &&, @>, <@
-- between two skyregion values. Like skypos_region_sel/skycell_region_pos_sel
-- (0.14) before them, these were declared RESTRICT = areasel/contsel,
-- PostgreSQL's generic, radius-blind defaults -- which cost the query
-- region's own size out of the estimate entirely, the same bug on a
-- different set of operators. None of the three backing functions
-- (skycell_region_overlap/_covers/_covered_by) has a SUPPORT clause --
-- per PostgreSQL's own documented rule, one would be dead code for
-- selectivity anyway once a function backs an operator -- so the fix is
-- the same oprrest-shaped mechanism as before: a plain RESTRICT function,
-- reusing the same area(region)/4pi uniform-sky estimate.
--
-- && is symmetric (overlap doesn't care which side is "the query"), so
-- either constant operand gives a usable estimate. @>/<@ are not: the
-- ratio only answers the right question when the *container* side
-- (@>'s LEFTARG, <@'s RIGHTARG) is the constant one -- a constant on the
-- *contained* side asks a different question ("how many of the table's
-- regions contain this one") that this ratio does not answer, so that
-- case keeps the flat default rather than apply it backwards. See
-- ext/src/adql.c's region_area_sel() and its three callers.
--
-- This has no analogue yet to skycell_pos_region_sel's own later,
-- density-aware round (region_pos_density_sel(), which reads a real point
-- density map when one exists): there is no equivalent statistics source
-- here -- a histogram of the sizes and sky positions of the regions
-- actually stored in a column -- so this is deliberately only the
-- uniform-sky estimate, not a claim of parity with the point operators'
-- current state. See GIST_REGION_DESIGN.md for the follow-up that would
-- take.
CREATE FUNCTION skycell_region_overlap_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION skycell_region_covers_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;
CREATE FUNCTION skycell_region_covered_by_sel(internal, oid, internal, int4) RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STABLE STRICT;

ALTER OPERATOR && (skyregion, skyregion) SET (RESTRICT = skycell_region_overlap_sel);
ALTER OPERATOR @> (skyregion, skyregion) SET (RESTRICT = skycell_region_covers_sel);
ALTER OPERATOR <@ (skyregion, skyregion) SET (RESTRICT = skycell_region_covered_by_sel);
