"""Translate ADQL geometry into PostgreSQL, for skycell or for pgSphere+Q3C.

This is not a full ADQL parser and does not try to be: it tokenises the query,
finds the geometry constructs, rewrites them, and passes everything else
through untouched.  That is enough to serve a TAP layer whose SQL is otherwise
already PostgreSQL, and it keeps the part that matters -- what the geometry
becomes -- small enough to read.

    >>> translate("SELECT * FROM t WHERE "
    ...           "CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 0.5)) = 1")
    "SELECT * FROM t WHERE (point('ICRS', ra, dec) <@ circle('ICRS', 10, 20, 0.5))"

Versions: ADQL 2.0 requires a coordinate-system argument on POINT, CIRCLE, BOX
and POLYGON; 2.1 deprecates it and allows it to be omitted.  Both are accepted;
`version` only controls whether a missing coordinate system is an error.

Dialects: 'skycell' emits ADQL's own spellings, which the extension provides.
'pgsphere' emits pgSphere types, using Q3C for the cone and cross-match forms
where it is faster -- the translation an archive runs today, and the reason the
comparison in the paper is against a pair of extensions rather than one.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

__all__ = ["translate", "TranslationError", "Dialect"]

GEOMETRY = {"POINT", "CIRCLE", "BOX", "POLYGON", "REGION", "CENTROID"}
PREDICATE = {"CONTAINS", "INTERSECTS"}
SCALAR = {"DISTANCE", "AREA", "COORD1", "COORD2"}
FRAMES = {"ICRS", "FK5", "FK4", "GALACTIC", "ECLIPTIC", "UNKNOWN", ""}


class TranslationError(ValueError):
    """The query uses ADQL geometry this translator cannot render."""


@dataclass
class Dialect:
    name: str
    #: how the position column is reached: a stored type, or scalar ra/dec
    positions_are_typed: bool


SKYCELL = Dialect("skycell", positions_are_typed=False)
PGSPHERE = Dialect("pgsphere", positions_are_typed=True)


# --------------------------------------------------------------------------
# tokenizer: enough to find function calls and balanced argument lists
# --------------------------------------------------------------------------

_TOKEN = re.compile(
    r"""
    (?P<ws>\s+)
  | (?P<str>'(?:[^']|'')*')
  | (?P<num>\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+(?:[eE][-+]?\d+)?)
  | (?P<name>[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*)
  | (?P<op><=|>=|<>|!=|\|\||[-+*/%<>=(),.])
  | (?P<other>.)
    """,
    re.VERBOSE,
)


@dataclass
class Tok:
    kind: str
    text: str
    pos: int


def tokenize(sql: str) -> list[Tok]:
    out, i = [], 0
    while i < len(sql):
        m = _TOKEN.match(sql, i)
        if m is None:                      # pragma: no cover - regex matches any char
            raise TranslationError(f"cannot tokenise at offset {i}")
        kind = m.lastgroup
        out.append(Tok(kind, m.group(), i))
        i = m.end()
    return out


def _find_call(toks: list[Tok], i: int) -> tuple[int, list[str]] | None:
    """If toks[i] starts NAME '(' ... ')', return (index after ')', arg texts)."""
    if toks[i].kind != "name":
        return None
    j = i + 1
    while j < len(toks) and toks[j].kind == "ws":
        j += 1
    if j >= len(toks) or toks[j].text != "(":
        return None
    depth, k, args, start = 0, j, [], j + 1
    while k < len(toks):
        t = toks[k].text
        if t == "(":
            depth += 1
        elif t == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(x.text for x in toks[start:k]).strip())
                return k + 1, [a for a in args if a != ""]
        elif t == "," and depth == 1:
            args.append("".join(x.text for x in toks[start:k]).strip())
            start = k + 1
        k += 1
    raise TranslationError("unbalanced parentheses in a geometry call")


def _frame(args: list[str], version: str, fn: str) -> list[str]:
    """Strip and validate a leading coordinate-system literal."""
    if args and args[0].startswith("'"):
        frame = args[0][1:-1].replace("''", "'").upper()
        if frame not in FRAMES:
            raise TranslationError(f"{fn}: coordinate system {frame!r} is not supported")
        if frame not in ("ICRS", "UNKNOWN", ""):
            raise TranslationError(
                f"{fn}: coordinate system {frame!r}; convert with gal2icrs()/ecl2icrs() "
                "before the query, both dialects work in ICRS"
            )
        return args[1:]
    if version == "2.0":
        raise TranslationError(f"{fn}: ADQL 2.0 requires a coordinate system argument")
    return args


# --------------------------------------------------------------------------
# rendering
# --------------------------------------------------------------------------

def _point(a: list[str], d: Dialect) -> str:
    if len(a) != 2:
        raise TranslationError("POINT takes a coordinate system and two coordinates")
    if d is SKYCELL:
        return f"point('ICRS', {a[0]}, {a[1]})"
    return f"spoint(radians({a[0]}), radians({a[1]}))"


def _circle(a: list[str], d: Dialect) -> str:
    if len(a) != 3:
        raise TranslationError("CIRCLE takes a coordinate system, a centre and a radius")
    if d is SKYCELL:
        return f"circle('ICRS', {a[0]}, {a[1]}, {a[2]})"
    return f"scircle(spoint(radians({a[0]}), radians({a[1]})), radians({a[2]}))"


def _box(a: list[str], d: Dialect) -> str:
    if len(a) != 4:
        raise TranslationError("BOX takes a coordinate system, a centre, a width and a height")
    if d is SKYCELL:
        return f"box('ICRS', {a[0]}, {a[1]}, {a[2]}, {a[3]})"
    # pgSphere's sbox is two corners, not a centre and an extent.  The conversion
    # below is what a translator has to do, and it is wrong near the poles, where
    # a box of fixed width in right ascension is not a rectangle.
    ra, dec, w, h = a
    return (f"sbox(spoint(radians(({ra}) - ({w})/2.0), radians(({dec}) - ({h})/2.0)), "
            f"spoint(radians(({ra}) + ({w})/2.0), radians(({dec}) + ({h})/2.0)))")


def _polygon(a: list[str], d: Dialect) -> str:
    if len(a) < 6 or len(a) % 2:
        raise TranslationError("POLYGON takes a coordinate system and at least three vertices")
    if d is SKYCELL:
        return "polygon('ICRS', " + ", ".join(a) + ")"
    pts = ", ".join(f"({a[i]}d,{a[i + 1]}d)" for i in range(0, len(a), 2))
    return f"spoly('{{{pts}}}')"


RENDER = {"POINT": _point, "CIRCLE": _circle, "BOX": _box, "POLYGON": _polygon}


def _geometry(fn: str, args: list[str], d: Dialect, version: str) -> str:
    args = _frame(args, version, fn)
    if fn not in RENDER:
        raise TranslationError(f"{fn} is not supported")
    return RENDER[fn](args, d)


def _is_point_of_columns(inner: str) -> tuple[str, str] | None:
    """POINT('ICRS', ra, dec) over plain columns -> (ra, dec), else None."""
    m = re.fullmatch(r"\s*POINT\s*\((.*)\)\s*", inner, re.I | re.S)
    if not m:
        return None
    parts = [p.strip() for p in _split_args(m.group(1))]
    if parts and parts[0].startswith("'"):
        parts = parts[1:]
    return (parts[0], parts[1]) if len(parts) == 2 else None


_CONST_NUM = re.compile(
    r"[-+]?(?:\d+\.\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|\d+(?:[eE][-+]?\d+)?)"
)


def _all_const(a: list[str]) -> bool:
    """True if every argument (a CIRCLE's centre/radius, a POLYGON's vertices)
    is a literal number, not a column or expression."""
    return all(_CONST_NUM.fullmatch(x.strip()) for x in a)


def _split_args(s: str) -> list[str]:
    out, depth, start = [], 0, 0
    for i, ch in enumerate(s):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(s[start:i])
            start = i + 1
    out.append(s[start:])
    return out


def _crossmatch_radial(cols: tuple[str, str] | None, region: str, version: str) -> str | None:
    """skycell_radial_query(...) for a cross-match shape: `cols` a point over a
    pair of plain columns and `region` a CIRCLE whose centre or radius is not a
    literal constant -- it comes from another table's row. `<@`'s rewrite
    (skycell_region_support) gives up outright when the region isn't a
    compile-time constant, so this would otherwise reach no index at all.
    skycell_radial_query (Q3C's own argument order) reaches simplify_cone's
    non-constant branch instead, which still covers with an index, if capped
    to skycell.join_slots ranges. None if the shape doesn't match -- a literal
    circle is left alone, since `<@`/`intersects` already reach the same,
    uncapped covering for that case.
    """
    if not cols:
        return None
    m = re.fullmatch(r"\s*CIRCLE\s*\((.*)\)\s*", region, re.I | re.S)
    if not m:
        return None
    a = _frame([p.strip() for p in _split_args(m.group(1))], version, "CIRCLE")
    if len(a) == 3 and not _all_const(a):
        return f"skycell_radial_query({cols[0]}, {cols[1]}, {a[0]}, {a[1]}, {a[2]})"
    return None


def _crossmatch_poly(cols: tuple[str, str] | None, region: str, version: str) -> str | None:
    """skycell_poly_join(...) for a cross-match shape: `cols` a point over a
    pair of plain columns and `region` a POLYGON whose vertices are not all
    literal constants -- they come from another table's row. The polygon
    analogue of _crossmatch_radial: `<@`'s rewrite gives up outright on a
    non-constant region regardless of shape, and skycell_poly_join reaches
    simplify_poly's non-constant branch instead, capped to skycell.join_slots
    ranges. None if the shape doesn't match -- a literal polygon is left
    alone, since `<@`/`intersects` already reach the same, uncapped covering.
    """
    if not cols:
        return None
    m = re.fullmatch(r"\s*POLYGON\s*\((.*)\)\s*", region, re.I | re.S)
    if not m:
        return None
    a = _frame([p.strip() for p in _split_args(m.group(1))], version, "POLYGON")
    if len(a) >= 6 and len(a) % 2 == 0 and not _all_const(a):
        return f"skycell_poly_join({cols[0]}, {cols[1]}, ARRAY[{', '.join(a)}]::float8[])"
    return None


def _crossmatch_box(cols: tuple[str, str] | None, region: str, version: str) -> str | None:
    """point('ICRS', ra, dec) <@ box('ICRS', ...) for a cross-match shape:
    `cols` a point over a pair of plain columns and `region` a BOX whose
    centre or extent is not all literal constants. Unlike CIRCLE/POLYGON,
    this doesn't need a dedicated join function -- box(...) returns a plain
    skyregion (it's a four-corner polygon under the hood, see box_region() in
    adql.c), and skycell_region_support's non-constant branch already covers
    any skyregion value, however it was built, via skycell_region_bound. So
    the redirect target is exactly the `<@` form CONTAINS already falls back
    to; this only matters for INTERSECTS, whose own function has no support
    function at all. None if the shape doesn't match, or the box is a
    literal -- the same fallback already covers that case.
    """
    if not cols:
        return None
    m = re.fullmatch(r"\s*BOX\s*\((.*)\)\s*", region, re.I | re.S)
    if not m:
        return None
    a = _frame([p.strip() for p in _split_args(m.group(1))], version, "BOX")
    if len(a) == 4 and not _all_const(a):
        return f"(point('ICRS', {cols[0]}, {cols[1]}) <@ box('ICRS', {a[0]}, {a[1]}, {a[2]}, {a[3]}))"
    return None


def _crossmatch(cols: tuple[str, str] | None, region: str, version: str) -> str | None:
    """skycell_radial_query/skycell_poly_join/<@ for whichever cross-match
    shape `region` is, else None."""
    return (_crossmatch_radial(cols, region, version)
            or _crossmatch_poly(cols, region, version)
            or _crossmatch_box(cols, region, version))


def _contains(args: list[str], d: Dialect, version: str) -> str:
    if len(args) != 2:
        raise TranslationError("CONTAINS takes two geometries")
    inner, outer = args
    cols = _is_point_of_columns(inner)
    if d is SKYCELL:
        xm = _crossmatch(cols, outer, version)
        if xm is not None:
            return xm
        return f"({translate_expr(inner, d, version)} <@ {translate_expr(outer, d, version)})"
    # pgSphere+Q3C: use Q3C when the position is a pair of columns and the region
    # is a circle or polygon, because that is the fast path; otherwise pgSphere.
    m = re.fullmatch(r"\s*(CIRCLE|POLYGON)\s*\((.*)\)\s*", outer, re.I | re.S)
    if cols and m:
        kind = m.group(1).upper()
        a = [p.strip() for p in _split_args(m.group(2))]
        if a and a[0].startswith("'"):
            a = a[1:]
        if kind == "CIRCLE":
            return f"q3c_radial_query({cols[0]}, {cols[1]}, {a[0]}, {a[1]}, {a[2]})"
        return (f"q3c_poly_query({cols[0]}, {cols[1]}, "
                f"ARRAY[{', '.join(a)}]::float8[])")
    return f"({translate_expr(inner, d, version)} <@ {translate_expr(outer, d, version)})"


def _intersects(args: list[str], d: Dialect, version: str) -> str:
    if len(args) != 2:
        raise TranslationError("INTERSECTS takes two geometries")
    if d is SKYCELL:
        # A point has no area: intersecting a region is the same as being
        # contained in it (see skycell_intersects_pos's own comment). A
        # cross-match written as INTERSECTS(POINT, CIRCLE-or-POLYGON) either
        # way round hits the same unindexed-non-constant-region gap as
        # CONTAINS; reuse its detection so it gets the same redirect.
        for pos, region in (args, (args[1], args[0])):
            xm = _crossmatch(_is_point_of_columns(pos), region, version)
            if xm is not None:
                return xm
        a, b = (translate_expr(x, d, version) for x in args)
        return f"intersects({a}, {b})"
    a, b = (translate_expr(x, d, version) for x in args)
    return f"({a} && {b})"          # pgSphere only; Q3C has no region type


def _distance(args: list[str], d: Dialect, version: str) -> str:
    if len(args) == 4:              # ADQL 2.1 scalar form
        if d is SKYCELL:
            return f"skycell_dist({', '.join(args)})"
        return f"q3c_dist({', '.join(args)})"
    if len(args) != 2:
        raise TranslationError("DISTANCE takes two points or four coordinates")
    if d is SKYCELL:
        return f"distance({translate_expr(args[0], d, version)}, " \
               f"{translate_expr(args[1], d, version)})"
    cols = [_is_point_of_columns(x) for x in args]
    if all(cols):
        return f"q3c_dist({cols[0][0]}, {cols[0][1]}, {cols[1][0]}, {cols[1][1]})"
    return (f"degrees({translate_expr(args[0], d, version)} <-> "
            f"{translate_expr(args[1], d, version)})")


def _area(args: list[str], d: Dialect, version: str) -> str:
    if len(args) != 1:
        raise TranslationError("AREA takes one region")
    inner = translate_expr(args[0], d, version)
    if d is SKYCELL:
        return f"area({inner})"
    return f"(area({inner}) * (180.0/pi())^2)"      # pgSphere returns steradians


def _coord(n: int, args: list[str], d: Dialect, version: str) -> str:
    if len(args) != 1:
        raise TranslationError(f"COORD{n} takes one point")
    inner = translate_expr(args[0], d, version)
    if d is SKYCELL:
        return f"coord{n}({inner})"
    return f"degrees({'long' if n == 1 else 'lat'}({inner}))"


def _centroid(args: list[str], d: Dialect, version: str) -> str:
    if len(args) != 1:
        raise TranslationError("CENTROID takes one region")
    if d is SKYCELL:
        return f"centroid({translate_expr(args[0], d, version)})"
    raise TranslationError(
        "CENTROID has no direct pgSphere equivalent for a polygon; compute it "
        "in the client or use skycell"
    )


def translate_expr(sql: str, d: Dialect = SKYCELL, version: str = "2.1") -> str:
    """Rewrite every ADQL geometry construct in `sql`; pass the rest through."""
    toks = tokenize(sql)
    out, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t.kind == "name":
            fn = t.text.upper()
            call = _find_call(toks, i)
            if call and (fn in GEOMETRY or fn in PREDICATE or fn in SCALAR):
                end, args = call
                if fn in RENDER:
                    out.append(_geometry(fn, args, d, version))
                elif fn == "CONTAINS":
                    out.append(_contains(args, d, version))
                elif fn == "INTERSECTS":
                    out.append(_intersects(args, d, version))
                elif fn == "DISTANCE":
                    out.append(_distance(args, d, version))
                elif fn == "AREA":
                    out.append(_area(args, d, version))
                elif fn in ("COORD1", "COORD2"):
                    out.append(_coord(int(fn[-1]), args, d, version))
                elif fn == "CENTROID":
                    out.append(_centroid(args, d, version))
                else:
                    raise TranslationError(f"{fn} is not supported")
                i = end
                continue
        out.append(t.text)
        i += 1
    return "".join(out)


#: `CONTAINS(...) = 1`, `1 = CONTAINS(...)`, and the `= 0` negations
_EQ1 = re.compile(r"(\((?:[^()]|\([^()]*\))*\))\s*=\s*([01])\b")
_EQ2 = re.compile(r"\b([01])\s*=\s*(\((?:[^()]|\([^()]*\))*\))")


def _fold_predicates(sql: str) -> str:
    """CONTAINS(...) = 1 is a boolean in PostgreSQL; drop the comparison."""
    def one(m):
        expr, val = m.group(1), m.group(2)
        return expr if val == "1" else f"NOT {expr}"

    def two(m):
        val, expr = m.group(1), m.group(2)
        return expr if val == "1" else f"NOT {expr}"

    prev = None
    while prev != sql:
        prev = sql
        sql = _EQ1.sub(one, sql)
        sql = _EQ2.sub(two, sql)
    return sql


def translate(adql: str, dialect: str = "skycell", version: str = "2.1") -> str:
    """Translate an ADQL query's geometry to PostgreSQL.

    dialect: 'skycell' or 'pgsphere' (pgSphere with Q3C for cones and joins).
    version: '2.0' (coordinate system required) or '2.1' (optional).
    """
    d = {"skycell": SKYCELL, "pgsphere": PGSPHERE}.get(dialect.lower())
    if d is None:
        raise TranslationError(f"unknown dialect {dialect!r}")
    if version not in ("2.0", "2.1"):
        raise TranslationError(f"unknown ADQL version {version!r}")
    return _fold_predicates(translate_expr(adql, d, version))


if __name__ == "__main__":                 # pragma: no cover
    import sys
    text = sys.stdin.read() if len(sys.argv) < 2 else sys.argv[1]
    dia = sys.argv[2] if len(sys.argv) > 2 else "skycell"
    print(translate(text, dia))
