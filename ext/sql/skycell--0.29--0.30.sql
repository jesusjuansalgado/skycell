\echo Use "ALTER EXTENSION skycell UPDATE TO '0.30'" to load this file. \quit

-- Plain descriptions for the operator classes and the box key type: what
-- each indexes and when to choose it.
COMMENT ON OPERATOR CLASS skyregion_gist_ops USING gist IS
  'GiST operator class for skyregion keyed by a few spherical caps per entry, exact for a circle at any radius. Indexes &&, @> and <@ between regions, and region @> point. Not the default: choose it for a column mixing very different region scales (survey tiles alongside catalogue footprints, say), where the default box key is loose around large circles.';
COMMENT ON OPERATOR CLASS skypos_spgist_ops USING spgist IS
  'Experimental SP-GiST operator class for skypos over the HEALPix nested hierarchy; indexes skypos <@ skyregion. The default B-tree on the cell (skycell_ang2cell(ra, dec) or skycell_cell(pos)) is faster; kept for comparison.';
COMMENT ON OPERATOR CLASS skypos_cap_gist_ops USING gist IS
  'Experimental GiST operator class for skypos with one spherical-cap key per entry; indexes skypos <@ skyregion. The default B-tree on the cell is faster; kept for comparison.';
COMMENT ON OPERATOR CLASS skyregion_box_gist_ops USING gist IS
  'GiST operator class for skyregion keyed by an axis-aligned 3D box in float8. Not the default: skyregion_box4_gist_ops uses the same box in float4 and is smaller and faster. Kept for comparison; select it explicitly with CREATE INDEX ... USING gist (col skyregion_box_gist_ops).';
COMMENT ON TYPE skyregion_box4 IS
  'Internal key type of skyregion_box4_gist_ops: an axis-aligned 3D box of six float4s, rounded outward. Not meant to be used as a column type.';
COMMENT ON OPERATOR CLASS skyregion_box4_gist_ops USING gist IS
  'Default GiST operator class for skyregion: an axis-aligned 3D box per entry, six float4s rounded outward so that the key always contains the region. Indexes &&, @> and <@ between regions, and region @> point. For a column mixing very different region scales, skyregion_gist_ops can be tighter.';
COMMENT ON OPERATOR CLASS skycell_cell_ops USING btree IS
  'Optional B-tree operator class for the int8 cell. Its operators (#<, #<=, #=, #>=, #>) estimate a range condition from the cell histogram, so the planner can cost a hand-written range join against skycell_cone_ranges(); write the join with #>= and #<=.';
