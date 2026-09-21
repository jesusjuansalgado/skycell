"""What the translator must produce, for both dialects and both ADQL versions."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "python"))
from skycell_adql import TranslationError, translate  # noqa: E402

CONE = "SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 0.5)) = 1"


def test_cone_skycell():
    assert translate(CONE) == (
        "SELECT * FROM t WHERE (point('ICRS', ra, dec) <@ circle('ICRS', 10, 20, 0.5))")


def test_cone_uses_q3c_for_scalar_columns():
    assert translate(CONE, "pgsphere") == (
        "SELECT * FROM t WHERE q3c_radial_query(ra, dec, 10, 20, 0.5)")


def test_cone_falls_back_to_pgsphere_for_a_typed_column():
    q = "SELECT * FROM t WHERE CONTAINS(pos, CIRCLE('ICRS', 10, 20, 0.5)) = 1"
    out = translate(q, "pgsphere")
    assert "scircle(spoint(radians(10), radians(20)), radians(0.5))" in out
    assert "q3c_" not in out


def test_negation():
    q = "SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 1, 2, 3)) = 0"
    assert translate(q).startswith("SELECT * FROM t WHERE NOT (")


def test_reversed_comparison():
    q = "SELECT * FROM t WHERE 1 = CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 1, 2, 3))"
    assert "NOT" not in translate(q)
    assert "<@" in translate(q)


def test_polygon_and_box():
    q = "SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), POLYGON('ICRS', 1,2, 3,4, 5,6)) = 1"
    assert "polygon('ICRS', 1, 2, 3, 4, 5, 6)" in translate(q)
    assert "q3c_poly_query(ra, dec, ARRAY[1, 2, 3, 4, 5, 6]::float8[])" in translate(q, "pgsphere")
    b = "SELECT BOX('ICRS', 10, 20, 1, 2) FROM t"
    assert "box('ICRS', 10, 20, 1, 2)" in translate(b)
    assert "sbox(" in translate(b, "pgsphere")          # centre+extent -> two corners


def test_distance_both_forms():
    two = "SELECT DISTANCE(POINT('ICRS', ra, dec), POINT('ICRS', 1, 2)) FROM t"
    assert translate(two).startswith("SELECT distance(point(")
    assert "q3c_dist(ra, dec, 1, 2)" in translate(two, "pgsphere")
    four = "SELECT DISTANCE(ra, dec, 1, 2) FROM t"          # ADQL 2.1 scalar form
    assert "skycell_dist(ra, dec, 1, 2)" in translate(four)


def test_area_units_differ():
    q = "SELECT AREA(CIRCLE('ICRS', 1, 2, 3)) FROM t"
    assert translate(q).startswith("SELECT area(circle(")
    assert "(180.0/pi())^2" in translate(q, "pgsphere")     # pgSphere returns steradians


def test_coord_accessors():
    q = "SELECT COORD1(POINT('ICRS', ra, dec)), COORD2(POINT('ICRS', ra, dec)) FROM t"
    assert "coord1(point('ICRS', ra, dec))" in translate(q)
    assert "degrees(long(spoint(" in translate(q, "pgsphere")


def test_centroid_has_no_pgsphere_form():
    q = "SELECT CENTROID(CIRCLE('ICRS', 1, 2, 3)) FROM t"
    assert "centroid(" in translate(q)
    with pytest.raises(TranslationError, match="no direct pgSphere equivalent"):
        translate(q, "pgsphere")


def test_version_2_0_requires_a_frame():
    q = "SELECT * FROM t WHERE CONTAINS(POINT(ra, dec), CIRCLE(1, 2, 3)) = 1"
    assert "point('ICRS', ra, dec)" in translate(q, version="2.1")
    with pytest.raises(TranslationError, match="requires a coordinate system"):
        translate(q, version="2.0")


def test_other_frames_are_refused_not_silently_wrong():
    q = "SELECT * FROM t WHERE CONTAINS(POINT('GALACTIC', l, b), CIRCLE('GALACTIC', 1, 2, 3)) = 1"
    with pytest.raises(TranslationError, match="gal2icrs"):
        translate(q)


def test_non_geometry_sql_passes_through_unchanged():
    q = "SELECT a, b FROM t WHERE x > 1 AND name LIKE 'a%' ORDER BY a"
    assert translate(q) == q


def test_nested_and_case_insensitive():
    q = "select * from t where contains(point('icrs',ra,dec), circle('icrs',1,2,3))=1 and mag<20"
    out = translate(q)
    assert "<@" in out and "mag<20" in out


def test_unbalanced_parens_is_an_error():
    with pytest.raises(TranslationError):
        translate("SELECT * FROM t WHERE CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS',1,2,3)")
