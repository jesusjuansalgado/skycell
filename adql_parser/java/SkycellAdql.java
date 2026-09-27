package skycell.adql;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Translate ADQL geometry into PostgreSQL, for skycell or for pgSphere+Q3C.
 *
 * <p>Not a full ADQL parser and not trying to be: it finds the geometry
 * constructs, rewrites them, and passes the rest of the statement through. That
 * is enough for a TAP layer whose SQL is otherwise already PostgreSQL, and it
 * keeps the part that matters small enough to read. A service that already
 * parses ADQL properly (for instance with the CDS ADQL library) should call
 * {@link #translateExpression} on the geometry nodes instead of the whole
 * statement.
 *
 * <p>ADQL 2.0 requires a coordinate-system argument on POINT, CIRCLE, BOX and
 * POLYGON; 2.1 makes it optional. Both are accepted; the version only decides
 * whether a missing coordinate system is an error. Only ICRS is translated:
 * anything else is refused rather than silently mis-translated.
 *
 * <pre>
 *   SkycellAdql t = new SkycellAdql(Dialect.SKYCELL, "2.1");
 *   t.translate("SELECT * FROM t WHERE "
 *             + "CONTAINS(POINT('ICRS', ra, dec), CIRCLE('ICRS', 10, 20, 0.5)) = 1");
 *   // SELECT * FROM t WHERE (point('ICRS', ra, dec) &lt;@ circle('ICRS', 10, 20, 0.5))
 * </pre>
 */
public final class SkycellAdql {

    /** Target SQL. PGSPHERE also emits Q3C where Q3C is the faster path. */
    public enum Dialect { SKYCELL, PGSPHERE }

    /** The query uses ADQL geometry this translator cannot render. */
    public static class TranslationException extends RuntimeException {
        public TranslationException(String message) { super(message); }
    }

    private static final List<String> FRAMES =
            Arrays.asList("ICRS", "UNKNOWN", "");
    private static final List<String> GEOMETRY =
            Arrays.asList("POINT", "CIRCLE", "BOX", "POLYGON", "CENTROID");
    private static final List<String> OTHER =
            Arrays.asList("CONTAINS", "INTERSECTS", "DISTANCE", "AREA", "COORD1", "COORD2");

    private final Dialect dialect;
    private final String version;

    public SkycellAdql(Dialect dialect, String version) {
        if (!"2.0".equals(version) && !"2.1".equals(version)) {
            throw new TranslationException("unknown ADQL version " + version);
        }
        this.dialect = dialect;
        this.version = version;
    }

    public String translate(String adql) {
        return foldPredicates(translateExpression(adql));
    }

    // ---------------------------------------------------------------- scanning

    /** Rewrite every ADQL geometry construct; pass the rest through. */
    public String translateExpression(String sql) {
        StringBuilder out = new StringBuilder();
        int i = 0;
        while (i < sql.length()) {
            char c = sql.charAt(i);
            if (c == '\'') {                        // string literal: copy verbatim
                int j = i + 1;
                while (j < sql.length()) {
                    if (sql.charAt(j) == '\'') {
                        if (j + 1 < sql.length() && sql.charAt(j + 1) == '\'') j++;
                        else break;
                    }
                    j++;
                }
                out.append(sql, i, Math.min(j + 1, sql.length()));
                i = j + 1;
                continue;
            }
            if (Character.isLetter(c) || c == '_') {
                int j = i;
                while (j < sql.length()
                        && (Character.isLetterOrDigit(sql.charAt(j)) || sql.charAt(j) == '_')) j++;
                String name = sql.substring(i, j).toUpperCase(Locale.ROOT);
                int k = j;
                while (k < sql.length() && Character.isWhitespace(sql.charAt(k))) k++;
                if (k < sql.length() && sql.charAt(k) == '('
                        && (GEOMETRY.contains(name) || OTHER.contains(name))) {
                    int close = matchParen(sql, k);
                    List<String> args = splitArgs(sql.substring(k + 1, close));
                    out.append(render(name, args));
                    i = close + 1;
                    continue;
                }
                out.append(sql, i, j);
                i = j;
                continue;
            }
            out.append(c);
            i++;
        }
        return out.toString();
    }

    private static int matchParen(String s, int open) {
        int depth = 0;
        for (int i = open; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\'') {                        // skip literals
                i++;
                while (i < s.length() && s.charAt(i) != '\'') i++;
            } else if (c == '(') depth++;
            else if (c == ')' && --depth == 0) return i;
        }
        throw new TranslationException("unbalanced parentheses in a geometry call");
    }

    private static List<String> splitArgs(String s) {
        List<String> out = new ArrayList<>();
        int depth = 0, start = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\'') { i++; while (i < s.length() && s.charAt(i) != '\'') i++; }
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (c == ',' && depth == 0) { out.add(s.substring(start, i).trim()); start = i + 1; }
        }
        String last = s.substring(start).trim();
        if (!last.isEmpty() || !out.isEmpty()) out.add(last);
        out.removeIf(String::isEmpty);
        return out;
    }

    // --------------------------------------------------------------- rendering

    private List<String> stripFrame(List<String> args, String fn) {
        if (!args.isEmpty() && args.get(0).startsWith("'")) {
            String f = args.get(0);
            f = f.substring(1, f.length() - 1).replace("''", "'").toUpperCase(Locale.ROOT);
            if (!FRAMES.contains(f)) {
                throw new TranslationException(fn + ": coordinate system '" + f
                        + "'; convert with gal2icrs()/ecl2icrs() before the query, "
                        + "both dialects work in ICRS");
            }
            return args.subList(1, args.size());
        }
        if ("2.0".equals(version)) {
            throw new TranslationException(fn + ": ADQL 2.0 requires a coordinate system argument");
        }
        return args;
    }

    private String render(String fn, List<String> raw) {
        switch (fn) {
            case "POINT": {
                List<String> a = stripFrame(raw, fn);
                need(a, 2, "POINT takes a coordinate system and two coordinates");
                return dialect == Dialect.SKYCELL
                        ? "point('ICRS', " + a.get(0) + ", " + a.get(1) + ")"
                        : "spoint(radians(" + a.get(0) + "), radians(" + a.get(1) + "))";
            }
            case "CIRCLE": {
                List<String> a = stripFrame(raw, fn);
                need(a, 3, "CIRCLE takes a coordinate system, a centre and a radius");
                return dialect == Dialect.SKYCELL
                        ? "circle('ICRS', " + a.get(0) + ", " + a.get(1) + ", " + a.get(2) + ")"
                        : "scircle(spoint(radians(" + a.get(0) + "), radians(" + a.get(1)
                          + ")), radians(" + a.get(2) + "))";
            }
            case "BOX": {
                List<String> a = stripFrame(raw, fn);
                need(a, 4, "BOX takes a coordinate system, a centre, a width and a height");
                if (dialect == Dialect.SKYCELL) {
                    return "box('ICRS', " + String.join(", ", a) + ")";
                }
                // sbox is two corners, not a centre and an extent; this conversion is
                // what a translator must do and it is wrong near the poles.
                String ra = a.get(0), dec = a.get(1), w = a.get(2), h = a.get(3);
                return "sbox(spoint(radians((" + ra + ") - (" + w + ")/2.0), radians((" + dec
                        + ") - (" + h + ")/2.0)), spoint(radians((" + ra + ") + (" + w
                        + ")/2.0), radians((" + dec + ") + (" + h + ")/2.0)))";
            }
            case "POLYGON": {
                List<String> a = stripFrame(raw, fn);
                if (a.size() < 6 || a.size() % 2 != 0) {
                    throw new TranslationException(
                            "POLYGON takes a coordinate system and at least three vertices");
                }
                if (dialect == Dialect.SKYCELL) {
                    return "polygon('ICRS', " + String.join(", ", a) + ")";
                }
                StringBuilder pts = new StringBuilder();
                for (int i = 0; i < a.size(); i += 2) {
                    if (pts.length() > 0) pts.append(", ");
                    pts.append('(').append(a.get(i)).append("d,").append(a.get(i + 1)).append("d)");
                }
                return "spoly('{" + pts + "}')";
            }
            case "CONTAINS": {
                need(raw, 2, "CONTAINS takes two geometries");
                String inner = raw.get(0), outer = raw.get(1);
                if (dialect == Dialect.SKYCELL) {
                    return containsSkycell(inner, outer);
                }
                String[] cols = pointOfColumns(inner);
                Matcher m = Pattern.compile("\\s*(CIRCLE|POLYGON)\\s*\\((.*)\\)\\s*",
                        Pattern.CASE_INSENSITIVE | Pattern.DOTALL).matcher(outer);
                if (cols != null && m.matches()) {
                    List<String> a = splitArgs(m.group(2));
                    if (!a.isEmpty() && a.get(0).startsWith("'")) a = a.subList(1, a.size());
                    if (m.group(1).equalsIgnoreCase("CIRCLE")) {
                        return "q3c_radial_query(" + cols[0] + ", " + cols[1] + ", "
                                + a.get(0) + ", " + a.get(1) + ", " + a.get(2) + ")";
                    }
                    return "q3c_poly_query(" + cols[0] + ", " + cols[1] + ", ARRAY["
                            + String.join(", ", a) + "]::float8[])";
                }
                return "(" + translateExpression(inner) + " <@ " + translateExpression(outer) + ")";
            }
            case "INTERSECTS": {
                need(raw, 2, "INTERSECTS takes two geometries");
                if (dialect == Dialect.SKYCELL) {
                    // A point has no area: intersecting a region is exactly containment
                    // (see skycell_intersects_pos's own comment) -- for a literal region
                    // just as much as a per-row one. So this delegates entirely to
                    // containsSkycell, not just its cross-match detection, whenever
                    // either argument is a point over plain columns: that picks up
                    // <@'s already-indexed constant-region path for free, alongside the
                    // cross-match redirect for a non-constant one. "Two regions, no
                    // point at all" used to fall through to the generic intersects()
                    // function -- unindexable at the time this branch was written, and
                    // actually a latent bug besides: intersects() returns int (ADQL
                    // compatibility), so the fold-predicates pass that strips this
                    // case's own "= 1" wrapper left a bare int4-valued function call as
                    // the whole WHERE condition, a type error PostgreSQL rejects
                    // outright ("argument of WHERE must be type boolean, not type
                    // integer"). skyregion's GiST opclass now gives && a real index
                    // (CREATE INDEX ... USING gist (region)), the same operator
                    // pgSphere already used below, so emit that directly instead: it is
                    // boolean already, so the wrapper this translator strips is the
                    // right one to strip, and no database-side rewrite could fix this
                    // from inside intersects() itself -- its own support hook only
                    // ever sees the int4-typed call, never the "= 1" around it.
                    for (int i = 0; i < 2; i++) {
                        String pos = raw.get(i), region = raw.get(1 - i);
                        if (pointOfColumns(pos) != null) return containsSkycell(pos, region);
                    }
                    return "(" + translateExpression(raw.get(0)) + " && "
                            + translateExpression(raw.get(1)) + ")";
                }
                return "(" + translateExpression(raw.get(0)) + " && "
                        + translateExpression(raw.get(1)) + ")";
            }
            case "DISTANCE": {
                if (raw.size() == 4) {              // ADQL 2.1 scalar form
                    return (dialect == Dialect.SKYCELL ? "skycell_dist(" : "q3c_dist(")
                            + String.join(", ", raw) + ")";
                }
                need(raw, 2, "DISTANCE takes two points or four coordinates");
                if (dialect == Dialect.SKYCELL) {
                    return "distance(" + translateExpression(raw.get(0)) + ", "
                            + translateExpression(raw.get(1)) + ")";
                }
                String[] p = pointOfColumns(raw.get(0)), q = pointOfColumns(raw.get(1));
                if (p != null && q != null) {
                    return "q3c_dist(" + p[0] + ", " + p[1] + ", " + q[0] + ", " + q[1] + ")";
                }
                return "degrees(" + translateExpression(raw.get(0)) + " <-> "
                        + translateExpression(raw.get(1)) + ")";
            }
            case "AREA": {
                need(raw, 1, "AREA takes one region");
                String inner = translateExpression(raw.get(0));
                return dialect == Dialect.SKYCELL
                        ? "area(" + inner + ")"
                        : "(area(" + inner + ") * (180.0/pi())^2)";   // pgSphere: steradians
            }
            case "COORD1":
            case "COORD2": {
                need(raw, 1, fn + " takes one point");
                String inner = translateExpression(raw.get(0));
                if (dialect == Dialect.SKYCELL) return fn.toLowerCase(Locale.ROOT) + "(" + inner + ")";
                return "degrees(" + (fn.equals("COORD1") ? "long(" : "lat(") + inner + "))";
            }
            case "CENTROID": {
                need(raw, 1, "CENTROID takes one region");
                if (dialect == Dialect.SKYCELL) {
                    return "centroid(" + translateExpression(raw.get(0)) + ")";
                }
                throw new TranslationException("CENTROID has no direct pgSphere equivalent "
                        + "for a polygon; compute it in the client or use skycell");
            }
            default:
                throw new TranslationException(fn + " is not supported");
        }
    }

    private static void need(List<String> a, int n, String message) {
        if (a.size() != n) throw new TranslationException(message);
    }

    /**
     * skycell_radial_query(...) for a cross-match shape: {@code cols} a point
     * over a pair of plain columns and {@code region} a CIRCLE whose centre or
     * radius is not a literal constant -- it comes from another table's row.
     * {@code <@}'s rewrite (skycell_region_support) gives up outright when the
     * region isn't a compile-time constant, so this would otherwise reach no
     * index at all. skycell_radial_query (Q3C's own argument order) reaches
     * simplify_cone's non-constant branch instead, which still covers with an
     * index, if capped to skycell.join_slots ranges. Null if the shape doesn't
     * match -- a literal circle is left alone, since {@code <@}/{@code
     * intersects} already reach the same, uncapped covering for that case.
     */
    private String crossmatchRadial(String[] cols, String region) {
        if (cols == null) return null;
        Matcher cm = Pattern.compile("\\s*CIRCLE\\s*\\((.*)\\)\\s*",
                Pattern.CASE_INSENSITIVE | Pattern.DOTALL).matcher(region);
        if (!cm.matches()) return null;
        List<String> a = stripFrame(splitArgs(cm.group(1)), "CIRCLE");
        if (a.size() == 3 && !allConst(a)) {
            return "skycell_radial_query(" + cols[0] + ", " + cols[1] + ", "
                    + a.get(0) + ", " + a.get(1) + ", " + a.get(2) + ")";
        }
        return null;
    }

    /**
     * skycell_poly_join(...) for a cross-match shape: {@code cols} a point
     * over a pair of plain columns and {@code region} a POLYGON whose
     * vertices are not all literal constants -- they come from another
     * table's row. The polygon analogue of crossmatchRadial: {@code <@}'s
     * rewrite gives up outright on a non-constant region regardless of
     * shape, and skycell_poly_join reaches simplify_poly's non-constant
     * branch instead, capped to skycell.join_slots ranges. Null if the shape
     * doesn't match -- a literal polygon is left alone, since {@code <@}/
     * {@code intersects} already reach the same, uncapped covering.
     */
    private String crossmatchPoly(String[] cols, String region) {
        if (cols == null) return null;
        Matcher pm = Pattern.compile("\\s*POLYGON\\s*\\((.*)\\)\\s*",
                Pattern.CASE_INSENSITIVE | Pattern.DOTALL).matcher(region);
        if (!pm.matches()) return null;
        List<String> a = stripFrame(splitArgs(pm.group(1)), "POLYGON");
        if (a.size() >= 6 && a.size() % 2 == 0 && !allConst(a)) {
            return "skycell_poly_join(" + cols[0] + ", " + cols[1] + ", ARRAY["
                    + String.join(", ", a) + "]::float8[])";
        }
        return null;
    }

    /**
     * point('ICRS', ra, dec) &lt;@ box('ICRS', ...) for a cross-match shape:
     * {@code cols} a point over a pair of plain columns and {@code region} a
     * BOX whose centre or extent is not all literal constants. Unlike
     * CIRCLE/POLYGON, this needs no dedicated join function -- box(...)
     * returns a plain skyregion (a four-corner polygon under the hood, see
     * box_region() in adql.c), and skycell_region_support's non-constant
     * branch already covers any skyregion value, however it was built, via
     * skycell_region_bound. So the redirect target is exactly the
     * {@code <@} form CONTAINS already falls back to; this only matters for
     * INTERSECTS, whose own function has no support function at all. Null
     * if the shape doesn't match, or the box is a literal -- the same
     * fallback already covers that case.
     */
    private String crossmatchBox(String[] cols, String region) {
        if (cols == null) return null;
        Matcher bm = Pattern.compile("\\s*BOX\\s*\\((.*)\\)\\s*",
                Pattern.CASE_INSENSITIVE | Pattern.DOTALL).matcher(region);
        if (!bm.matches()) return null;
        List<String> a = stripFrame(splitArgs(bm.group(1)), "BOX");
        if (a.size() == 4 && !allConst(a)) {
            return "(point('ICRS', " + cols[0] + ", " + cols[1] + ") <@ box('ICRS', "
                    + a.get(0) + ", " + a.get(1) + ", " + a.get(2) + ", " + a.get(3) + "))";
        }
        return null;
    }

    /** skycell_radial_query/skycell_poly_join/&lt;@ for whichever cross-match
     * shape {@code region} is, else null. */
    private String crossmatch(String[] cols, String region) {
        String xm = crossmatchRadial(cols, region);
        if (xm != null) return xm;
        xm = crossmatchPoly(cols, region);
        return xm != null ? xm : crossmatchBox(cols, region);
    }

    /**
     * {@code inner <@ outer} for the skycell dialect, or the cross-match
     * redirect if {@code inner} is a point over plain columns and
     * {@code outer} is a non-constant CIRCLE/POLYGON/BOX. Shared between
     * CONTAINS and INTERSECTS: a point has no area, so intersecting a
     * region is exactly containment (see skycell_intersects_pos's own
     * comment), whether the region is literal or per-row.
     */
    private String containsSkycell(String inner, String outer) {
        String xm = crossmatch(pointOfColumns(inner), outer);
        if (xm != null) return xm;
        return "(" + translateExpression(inner) + " <@ " + translateExpression(outer) + ")";
    }

    /** POINT('ICRS', ra, dec) over plain columns -&gt; {ra, dec}, else null. */
    private static String[] pointOfColumns(String inner) {
        Matcher m = Pattern.compile("\\s*POINT\\s*\\((.*)\\)\\s*",
                Pattern.CASE_INSENSITIVE | Pattern.DOTALL).matcher(inner);
        if (!m.matches()) return null;
        List<String> a = splitArgs(m.group(1));
        if (!a.isEmpty() && a.get(0).startsWith("'")) a = a.subList(1, a.size());
        return a.size() == 2 ? new String[] {a.get(0), a.get(1)} : null;
    }

    private static final Pattern CONST_NUM = Pattern.compile(
            "[-+]?(?:\\d+\\.\\d*(?:[eE][-+]?\\d+)?|\\.\\d+(?:[eE][-+]?\\d+)?|\\d+(?:[eE][-+]?\\d+)?)");

    /** True if every argument (a CIRCLE's centre/radius, a POLYGON's
     * vertices) is a literal number, not a column or expression. */
    private static boolean allConst(List<String> a) {
        for (String x : a) {
            if (!CONST_NUM.matcher(x.trim()).matches()) return false;
        }
        return true;
    }

    // ------------------------------------------------------------- predicates

    private static final Pattern EQ1 =
            Pattern.compile("(\\((?:[^()]|\\([^()]*\\))*\\))\\s*=\\s*([01])\\b");
    private static final Pattern EQ2 =
            Pattern.compile("\\b([01])\\s*=\\s*(\\((?:[^()]|\\([^()]*\\))*\\))");

    /** {@code CONTAINS(...) = 1} is already boolean in PostgreSQL. */
    private static String foldPredicates(String sql) {
        String prev = null;
        while (!sql.equals(prev)) {
            prev = sql;
            sql = replaceAll(EQ1.matcher(sql), true);
            sql = replaceAll(EQ2.matcher(sql), false);
        }
        return sql;
    }

    private static String replaceAll(Matcher m, boolean exprFirst) {
        StringBuffer sb = new StringBuffer();
        while (m.find()) {
            String expr = exprFirst ? m.group(1) : m.group(2);
            String val = exprFirst ? m.group(2) : m.group(1);
            m.appendReplacement(sb, Matcher.quoteReplacement("1".equals(val) ? expr : "NOT " + expr));
        }
        m.appendTail(sb);
        return sb.toString();
    }

    public static void main(String[] args) {
        Dialect d = args.length > 1 && args[1].equalsIgnoreCase("pgsphere")
                ? Dialect.PGSPHERE : Dialect.SKYCELL;
        System.out.println(new SkycellAdql(d, "2.1").translate(args[0]));
    }
}
