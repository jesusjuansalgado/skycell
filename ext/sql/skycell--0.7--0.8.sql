
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
