\echo Use "ALTER EXTENSION skycell UPDATE TO '0.32'" to load this file. \quit

-- The epoch propagation functions take NULL arguments as the IVOA catalogue
-- of ADQL user-defined functions has them (ivo_epoch_prop_pos): a missing
-- parallax, proper motion or radial velocity enters as 0, a missing position
-- or epoch is an error. STRICT answered NULL for all of these instead. The
-- library does the work; the functions only stop being STRICT.
ALTER FUNCTION ivo_epoch_prop(float8, float8, float8, float8, float8, float8, float8, float8)
    CALLED ON NULL INPUT;
ALTER FUNCTION ivo_epoch_prop_pos(float8, float8, float8, float8, float8, float8, float8, float8)
    CALLED ON NULL INPUT;
ALTER FUNCTION ivo_epoch_prop_pos(float8, float8, float8, float8, float8, float8)
    CALLED ON NULL INPUT;
