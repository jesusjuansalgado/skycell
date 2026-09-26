/*
 * gist_region.c -- a GiST opclass for skyregion, indexing region-region
 * INTERSECTS (&&) directly.
 *
 * skyregion has no per-row covering the way a point column does (skycell_
 * cone_bound and friends): both sides of a region-region test are extended
 * shapes, so there is no single outer row to build a covering from.  The
 * MOC-ranges recipe (skycell_region_moc_ranges, see skycell--0.9.sql) works
 * around that with a hand-maintained side table.  This file instead gives
 * skyregion a real index: a GiST opclass whose key is a cheap, fixed-size
 * bounding spherical cap (unit-vector centre + angular radius), the same
 * kind of bound PostGIS's box2df or pg_sphere's spherekey use for their own
 * geometry types.  A cap is:
 *
 *   - exact for a cone (it already is one: sc_region_bounding_cap just
 *     returns its own centre and radius);
 *   - a cheap over-approximation for a polygon (centroid + farthest vertex,
 *     via the existing sc_region_centroid -- not the minimal enclosing cap,
 *     but valid, and reuses machinery cover.c already has for other
 *     purposes).
 *
 * Two caps overlap iff the angle between their centres is at most the sum
 * of their radii -- exactly sc_region_overlaps's own cone-cone formula,
 * lifted to bound *any* region kind uniformly.  That makes the cap test a
 * correct (if lossy) pruning condition for &&: every genuinely overlapping
 * pair of regions has overlapping bounding caps, so a leaf whose cap does
 * not overlap the query's cap can be skipped outright, and a leaf whose cap
 * does overlap needs the real intersects() check (GiST's "recheck") to rule
 * out false positives from the two shapes overlapping in bounding-cap space
 * but not in reality.
 *
 * Support functions: standard GiST (compress/decompress/union/penalty/
 * picksplit/same/consistent), storage type plain bytea -- a packed
 * {cx,cy,cz,radius} struct, no custom SQL type needed.  Modelled on
 * pg_sphere's own spherekey convention (verified compatible with this
 * PostgreSQL version: pg_sphere's GiST indexes are exercised throughout this
 * project's own benchmarks), not on PostgreSQL's textbook point/box
 * example, since a unit-sphere cap has no direct textbook analogue.
 *
 * STATUS: correctness-verified (see GIST_REGION_DESIGN.md for the full
 * history: an initial Linear split, then Guttman's Quadratic split, then the
 * current R*-tree-style margin/overlap split below), but the performance
 * verdict is scale-dependent and not a clean win: competitive with pgSphere's
 * native opclass at a 5000-row footprint table, ~5x slower at 50,000 rows,
 * where even the MOC-ranges recipe this opclass was meant to replace pulls
 * ahead of it. Two split algorithms (Quadratic, R*-tree-style) bracket the
 * likely cause as the bounding-cap key itself being too coarse at scale, not
 * the tree-balancing on top of it -- see GIST_REGION_DESIGN.md's "Picksplit,
 * round two" before trying a third split algorithm. Not concurrent-write-
 * tested. cap_union's near-antipodal case (centres almost pi radians apart)
 * is approximated rather than handled exactly; skyregion itself already
 * forbids a cone larger than a hemisphere and a polygon spanning one, so a
 * single region's own cap cannot be near-antipodal internally, but a union
 * of two widely-separated small regions' caps could approach it.
 *
 * picksplit's loop bounds below are OffsetNumber/FirstOffsetNumber, not
 * plain 0-based indices, on purpose: an earlier version treated
 * entryvec->vector[0] as a real entry there (it is not -- that slot is
 * reserved, unlike in union(), which really is 0-based) and crashed the
 * server building an index over as few as ~160 rows, the first row count
 * that forces a page split. See GIST_REGION_DESIGN.md for how that was
 * diagnosed; a correctness test too small to force a split will not catch a
 * regression here.
 */
#include "postgres.h"

#include <math.h>
#include <string.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "cover.h"
#include "healpix.h"
#include "skycell_internal.h"

#define GIST_REGION_STRATEGY_OVERLAP 1	/* && (skyregion, skyregion) */

typedef struct
{
	double		cx,
				cy,
				cz;
	double		radius;			/* radians; negative marks the empty cap */
} GistCap;

/*
 * VARDATA(out) is only 4-byte aligned (right after a short varlena header),
 * not the 8-byte alignment a GistCap's doubles want -- a direct struct
 * pointer dereference there can fault under vectorised load/store codegen.
 * memcpy sidesteps the alignment requirement entirely.
 */
static bytea *
cap_to_bytea(sc_vec3 c, double radius)
{
	Size		sz = VARHDRSZ + sizeof(GistCap);
	bytea	   *out = (bytea *) palloc(sz);
	GistCap		k;

	SET_VARSIZE(out, sz);
	k.cx = c.x;
	k.cy = c.y;
	k.cz = c.z;
	k.radius = radius;
	memcpy(VARDATA(out), &k, sizeof(GistCap));
	return out;
}

static void
bytea_to_cap(bytea *b, sc_vec3 *c, double *radius)
{
	GistCap		k;

	/*
	 * VARDATA_ANY, not VARDATA: an index tuple GiST hands back here may have
	 * been repacked with a 1-byte varlena header (it easily fits under the
	 * short-header limit), and VARDATA alone assumes the 4-byte form we
	 * ourselves palloc'd it with -- reading through the wrong offset there
	 * is exactly the kind of thing that segfaults deep in a page split.
	 */
	memcpy(&k, VARDATA_ANY(b), sizeof(GistCap));
	c->x = k.cx;
	c->y = k.cy;
	c->z = k.cz;
	*radius = k.radius;
}

/*
 * Smallest cap (not necessarily minimal, but valid and cheap) covering both
 * inputs.  d + r1 + r2 < pi is assumed for the general branch: a single
 * region's own cap cannot violate that (skyregion forbids a hemisphere-or-
 * larger cone and a polygon spanning one), but repeated unions of far-apart
 * small regions could in principle approach it -- clamped rather than
 * exactly handled, a known limitation of this spike (see file header).
 */
static void
cap_union2(sc_vec3 c1, double r1, sc_vec3 c2, double r2, sc_vec3 *co, double *ro)
{
	double		d;
	double		theta,
				sinD,
				a,
				b;

	if (r1 < 0)
	{
		*co = c2;
		*ro = r2;
		return;
	}
	if (r2 < 0)
	{
		*co = c1;
		*ro = r1;
		return;
	}

	d = sc_angle(c1, c2);
	if (d + r2 <= r1)
	{
		*co = c1;
		*ro = r1;
		return;
	}
	if (d + r1 <= r2)
	{
		*co = c2;
		*ro = r2;
		return;
	}

	*ro = fmin(M_PI, (d + r1 + r2) / 2.0);
	theta = *ro - r1;			/* angular distance from c1 towards c2 */
	sinD = sin(d);
	if (sinD < 1e-9)
	{
		/* c1 and c2 (near-)coincident but neither cap contains the other:
		 * cannot happen with finite r1,r2 unless d is also ~0, which the
		 * containment checks above already caught -- fall back to c1. */
		*co = c1;
		return;
	}
	a = sin(d - theta) / sinD;
	b = sin(theta) / sinD;
	co->x = a * c1.x + b * c2.x;
	co->y = a * c1.y + b * c2.y;
	co->z = a * c1.z + b * c2.z;
	{
		double		n = sqrt(sc_dot(*co, *co));

		if (n > 1e-15)
		{
			co->x /= n;
			co->y /= n;
			co->z /= n;
		}
		else
			*co = c1;			/* antipodal degenerate case: give up gracefully */
	}
}

/* monotonic in the cap's true area (2*pi*(1-cos(r))); the constant does not
 * matter since penalty only compares differences. */
static double
cap_area_proxy(double radius)
{
	if (radius < 0)
		return 0.0;
	return 1.0 - cos(fmin(radius, M_PI));
}

static bool
cap_overlaps(sc_vec3 c1, double r1, sc_vec3 c2, double r2)
{
	if (r1 < 0 || r2 < 0)
		return false;
	return sc_angle(c1, c2) <= r1 + r2;
}

/* how much two caps overlap, not just whether they do: 0 when disjoint (or
 * merely touching), growing with how far the sum of radii exceeds the
 * centre distance -- a cheap angular proxy, not a real lens-shaped overlap
 * area, in the same spirit as cap_area_proxy() above. */
static double
cap_overlap_amount(sc_vec3 c1, double r1, sc_vec3 c2, double r2)
{
	if (r1 < 0 || r2 < 0)
		return 0.0;
	return fmax(0.0, r1 + r2 - sc_angle(c1, c2));
}

PG_FUNCTION_INFO_V1(skyregion_gist_compress);
Datum
skyregion_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		sc_region	r;
		sc_vec3		c;
		double		radius;
		bytea	   *k;

		skycell_region_from_datum(entry->key, &r);
		sc_region_bounding_cap(&r, &c, &radius);
		sc_region_free(&r);
		k = cap_to_bytea(c, radius);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(k), entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(skyregion_gist_decompress);
Datum
skyregion_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(skyregion_gist_union);
Datum
skyregion_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	sc_vec3		c;
	double		radius;
	bytea	   *out;

	bytea_to_cap(DatumGetByteaP(entryvec->vector[0].key), &c, &radius);
	for (int i = 1; i < entryvec->n; i++)
	{
		sc_vec3		c2;
		double		r2;

		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &c2, &r2);
		cap_union2(c, radius, c2, r2, &c, &radius);
	}
	out = cap_to_bytea(c, radius);
	*sizep = VARSIZE(out);
	PG_RETURN_POINTER(out);
}

PG_FUNCTION_INFO_V1(skyregion_gist_penalty);
Datum
skyregion_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	sc_vec3		c1,
				c2,
				cu;
	double		r1,
				r2,
				ru;

	bytea_to_cap(DatumGetByteaP(origentry->key), &c1, &r1);
	bytea_to_cap(DatumGetByteaP(newentry->key), &c2, &r2);
	cap_union2(c1, r1, c2, r2, &cu, &ru);
	*result = (float) (cap_area_proxy(ru) - cap_area_proxy(r1));
	PG_RETURN_POINTER(result);
}

typedef struct
{
	double		key;			/* cap centre's coordinate along the axis being tried */
	OffsetNumber idx;
}			axis_sort_entry;

static int
axis_sort_cmp(const void *a, const void *b)
{
	double		ka = ((const axis_sort_entry *) a)->key;
	double		kb = ((const axis_sort_entry *) b)->key;

	return (ka > kb) - (ka < kb);
}

/*
 * R*-tree-style split (Beckmann et al. 1990), adapted from axis-aligned
 * boxes to spherical caps -- replaced Guttman's Quadratic split (see git
 * history / GIST_REGION_DESIGN.md for that version and why it was tried
 * first) once benchmarking showed room past it:
 *
 *   ChooseSplitAxis: try sorting the entries by their cap centre's x, y, and
 *   z coordinate in turn (three candidate axes -- a cap has no natural
 *   per-axis bounding box the way a box does, so the centre's own
 *   coordinate stands in for it); for each axis, sum the "margin" (here,
 *   cap radius as a size proxy) of every valid left/right split along it;
 *   the axis with the smallest total margin sum is the one along which the
 *   entries are most naturally separable.
 *
 *   ChooseSplitIndex: along the chosen axis, pick the split point that
 *   minimises the *overlap* between the resulting left and right caps
 *   (cap_overlap_amount, not just whether they overlap), breaking ties by
 *   minimising their combined area -- overlap is what actually costs a GiST
 *   scan extra work later (a query cap intersecting both children's caps
 *   has to descend into both), not raw size.
 *
 * O(n log n) per axis (sort once, then a single forward and backward
 * cumulative-union sweep), so three axes is still O(n log n) overall --
 * cheaper to compute than Quadratic split's O(n^2) seed search, as well as
 * a better split by the numbers (see GIST_REGION_DESIGN.md).
 */
PG_FUNCTION_INFO_V1(skyregion_gist_picksplit);
Datum
skyregion_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	/* real entries are entryvec->vector[FirstOffsetNumber..maxoff]: index 0
	 * is a reserved slot, not a real key (a GiST convention shared with the
	 * OffsetNumber-addressed spl_left/spl_right arrays below, which is why
	 * the loop indices below are used as OffsetNumbers directly, with no
	 * +1/-1 conversion). */
	OffsetNumber maxoff = (OffsetNumber) (entryvec->n - 1);
	int			n = maxoff - FirstOffsetNumber + 1;
	sc_vec3    *c = palloc(sizeof(sc_vec3) * (maxoff + 1));
	double	   *r = palloc(sizeof(double) * (maxoff + 1));
	int			minfill = Max(1, n * 3 / 10);
	axis_sort_entry *sorted = palloc(sizeof(axis_sort_entry) * n);
	sc_vec3    *fwdC = palloc(sizeof(sc_vec3) * n);	/* fwdC[k]/fwdR[k]: union of sorted[0..k] */
	double	   *fwdR = palloc(sizeof(double) * n);
	sc_vec3    *bwdC = palloc(sizeof(sc_vec3) * n);	/* bwdC[k]/bwdR[k]: union of sorted[k..n-1] */
	double	   *bwdR = palloc(sizeof(double) * n);
	int			bestAxis = 0;
	double		bestAxisMargin = HUGE_VAL;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &c[i], &r[i]);

	for (int axis = 0; axis < 3; axis++)
	{
		double		marginSum = 0;

		for (int k = 0; k < n; k++)
		{
			OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);

			sorted[k].idx = idx;
			sorted[k].key = (axis == 0) ? c[idx].x : (axis == 1) ? c[idx].y : c[idx].z;
		}
		qsort(sorted, n, sizeof(axis_sort_entry), axis_sort_cmp);

		fwdC[0] = c[sorted[0].idx];
		fwdR[0] = r[sorted[0].idx];
		for (int k = 1; k < n; k++)
			cap_union2(fwdC[k - 1], fwdR[k - 1], c[sorted[k].idx], r[sorted[k].idx], &fwdC[k], &fwdR[k]);
		bwdC[n - 1] = c[sorted[n - 1].idx];
		bwdR[n - 1] = r[sorted[n - 1].idx];
		for (int k = n - 2; k >= 0; k--)
			cap_union2(bwdC[k + 1], bwdR[k + 1], c[sorted[k].idx], r[sorted[k].idx], &bwdC[k], &bwdR[k]);

		for (int m = minfill; m <= n - minfill; m++)
			marginSum += fwdR[m - 1] + bwdR[m];

		if (marginSum < bestAxisMargin)
		{
			bestAxisMargin = marginSum;
			bestAxis = axis;
		}
	}

	/* redo the winning axis's sort + cumulative unions (cheap: one more
	 * O(n log n) pass, and keeps the loop above simple) */
	for (int k = 0; k < n; k++)
	{
		OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);

		sorted[k].idx = idx;
		sorted[k].key = (bestAxis == 0) ? c[idx].x : (bestAxis == 1) ? c[idx].y : c[idx].z;
	}
	qsort(sorted, n, sizeof(axis_sort_entry), axis_sort_cmp);
	fwdC[0] = c[sorted[0].idx];
	fwdR[0] = r[sorted[0].idx];
	for (int k = 1; k < n; k++)
		cap_union2(fwdC[k - 1], fwdR[k - 1], c[sorted[k].idx], r[sorted[k].idx], &fwdC[k], &fwdR[k]);
	bwdC[n - 1] = c[sorted[n - 1].idx];
	bwdR[n - 1] = r[sorted[n - 1].idx];
	for (int k = n - 2; k >= 0; k--)
		cap_union2(bwdC[k + 1], bwdR[k + 1], c[sorted[k].idx], r[sorted[k].idx], &bwdC[k], &bwdR[k]);

	{
		int			bestM = minfill;
		double		bestOverlap = HUGE_VAL;
		double		bestArea = HUGE_VAL;

		for (int m = minfill; m <= n - minfill; m++)
		{
			double		overlap = cap_overlap_amount(fwdC[m - 1], fwdR[m - 1], bwdC[m], bwdR[m]);
			double		area = cap_area_proxy(fwdR[m - 1]) + cap_area_proxy(bwdR[m]);

			if (overlap < bestOverlap || (overlap == bestOverlap && area < bestArea))
			{
				bestOverlap = overlap;
				bestArea = area;
				bestM = m;
			}
		}

		v->spl_left = palloc(sizeof(OffsetNumber) * bestM);
		v->spl_right = palloc(sizeof(OffsetNumber) * (n - bestM));
		v->spl_nleft = v->spl_nright = 0;
		for (int k = 0; k < bestM; k++)
			v->spl_left[v->spl_nleft++] = sorted[k].idx;
		for (int k = bestM; k < n; k++)
			v->spl_right[v->spl_nright++] = sorted[k].idx;

		v->spl_ldatum = PointerGetDatum(cap_to_bytea(fwdC[bestM - 1], fwdR[bestM - 1]));
		v->spl_rdatum = PointerGetDatum(cap_to_bytea(bwdC[bestM], bwdR[bestM]));
	}

	pfree(c);
	pfree(r);
	pfree(sorted);
	pfree(fwdC);
	pfree(fwdR);
	pfree(bwdC);
	pfree(bwdR);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skyregion_gist_same);
Datum
skyregion_gist_same(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_P(0);
	bytea	   *b = PG_GETARG_BYTEA_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	sc_vec3		ca,
				cb;
	double		ra,
				rb;

	bytea_to_cap(a, &ca, &ra);
	bytea_to_cap(b, &cb, &rb);
	*result = (fabs(ra - rb) < 1e-12 && sc_angle(ca, cb) < 1e-12);
	PG_RETURN_POINTER(result);
}

/*
 * consistent() is called once per index entry visited during a scan -- for
 * one probe row's index scan that can easily be dozens of internal-page
 * entries plus every matching leaf, all with the *same* query argument (the
 * one outer row's region, unchanged for the scan's lifetime). Re-parsing
 * that region and recomputing its bounding cap on every single call is
 * wasted work worth caching in fn_extra, the same pattern skycell.c's
 * cone/poly/hist caches already use elsewhere in this extension -- but
 * keyed on the query Datum's own *bytes*, not its pointer: in a join (this
 * opclass's main use case), each outer row gets a fresh per-tuple memory
 * context that is reset and reused for the next row, so two genuinely
 * different rows' region values can legitimately land at the same address.
 * A pointer-identity cache took that as "same value, skip recomputing" and
 * silently reused a stale, wrong cap -- consistent() returning a false
 * "no overlap" from it prunes a subtree outright, with no recheck to catch
 * it afterwards (unlike a false positive, which recheck still filters).
 * Caught by the large self-join correctness test after this cache first
 * went in: 61 of 64 true matches missing, 0 spurious ones -- exactly what a
 * stale cache causing false pruning looks like, not a geometry bug.
 */
typedef struct
{
	bytea	   *last_query;		/* palloc'd copy in fn_mcxt, or NULL */
	Size		last_query_size;
	sc_vec3		qc;
	double		qr;
}			region_gist_query_cache;

PG_FUNCTION_INFO_V1(skyregion_gist_consistent);
Datum
skyregion_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	region_gist_query_cache *qcache = (region_gist_query_cache *) fcinfo->flinfo->fn_extra;
	bytea	   *qb = DatumGetByteaP(queryDatum);
	Size		qsz = VARSIZE(qb);
	sc_vec3		ec;
	double		er;
	bool		result;

	bytea_to_cap(DatumGetByteaP(entry->key), &ec, &er);

	switch (strategy)
	{
		case GIST_REGION_STRATEGY_OVERLAP:
			if (qcache == NULL)
			{
				qcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(region_gist_query_cache));
				fcinfo->flinfo->fn_extra = qcache;
			}
			if (qcache->last_query == NULL || qcache->last_query_size != qsz ||
				memcmp(qcache->last_query, qb, qsz) != 0)
			{
				sc_region	qreg;

				skycell_region_from_datum(queryDatum, &qreg);
				sc_region_bounding_cap(&qreg, &qcache->qc, &qcache->qr);
				sc_region_free(&qreg);

				if (qcache->last_query == NULL || qcache->last_query_size < qsz)
				{
					if (qcache->last_query != NULL)
						pfree(qcache->last_query);
					qcache->last_query = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, qsz);
				}
				memcpy(qcache->last_query, qb, qsz);
				qcache->last_query_size = qsz;
			}
			result = cap_overlaps(ec, er, qcache->qc, qcache->qr);
			*recheck = true;	/* the cap test is lossy either way */
			break;
		default:
			elog(ERROR, "skyregion_gist_consistent: unsupported strategy %d", strategy);
			result = false;	/* unreachable */
	}
	PG_RETURN_BOOL(result);
}
