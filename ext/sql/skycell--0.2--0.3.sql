-- skycell 0.2 -> 0.3
--
-- skycell_cover_info() also reports the density the cost model used and the
-- order it asked for, so that the density estimator and the order choice can
-- be studied from SQL (see bench/ and the paper's cost-model section).
\echo Use "ALTER EXTENSION skycell UPDATE TO '0.3'" to load this file. \quit

DROP FUNCTION IF EXISTS skycell_cover_info(float8, float8, float8, regclass, name);

CREATE FUNCTION skycell_cover_info(ra0 float8, dec0 float8, radius float8,
                                   tbl regclass DEFAULT NULL, col name DEFAULT 'cell',
                                   OUT nranges int, OUT steps int, OUT deepest int,
                                   OUT exp_rows float8, OUT area_ratio float8,
                                   OUT rho float8, OUT chosen_order int) RETURNS record
AS 'MODULE_PATHNAME' LANGUAGE C STABLE CALLED ON NULL INPUT PARALLEL SAFE;
