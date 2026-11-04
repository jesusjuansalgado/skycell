-- skycell 0.43 -> 0.44: a MOC as a search region

CREATE FUNCTION skycell_moc_region(moc text) RETURNS skyregion
AS 'MODULE_PATHNAME', 'skycell_moc_region' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
COMMENT ON FUNCTION skycell_moc_region(text) IS
  'An IVOA MOC, in its ASCII ("5/4965 4967 6/19771-19775") or JSON form, as a region: a point is in it when its order-29 HEALPix cell is. REGION(''MOC 5/4965 ...'') reads the ASCII form too.';
