/*
 * gist_point_cap.c -- EXPERIMENTAL GiST opclass for skypos: a single
 * spherical-cap key per entry, competing directly with pgSphere's native
 * spoint GiST (a float-precision axis-aligned 3D box, confirmed empirically
 * via pageinspect -- see GIST_REGION_DESIGN.md) rather than with skycell's
 * own B-tree-rewrite path or its SP-GiST opclass (skypos_spgist_ops).
 *
 * WHY A CAP, NOT A BOX: skycell already has a box-shaped key for a different
 * type (gist_region_box.c, for skyregion), built because a box beats a
 * multi-cap key for *regions* from a few degrees on -- a box's extra
 * corners are wasted precision on a near-circular footprint. The argument
 * runs the other way for *points*: the thing being bounded here is not one
 * region's own shape but an accumulated cluster of many catalogue points,
 * and a real catalogue's clusters are typically isotropic (a globular
 * cluster, a density peak, a HEALPix-sorted run of nearby sources) --
 * exactly the shape a circle bounds tightly and a square bounds loosely
 * (the same circle-in-a-square argument as gist_region_box.c's own file
 * header, just working in the opposite direction here: tighter cap, not
 * tighter box, because the underlying shape being bounded has flipped).
 *
 * LEAF KEYS ARE EXACT: a point is cap_make(point, 0.0) -- no precision lost
 * converting it to this key, unlike pgSphere's own leaf key, which pageinspect
 * showed to be a box, which is lossy (hence recheck=true) even at the leaf,
 * on a single point, for no reason but following the same box-key struct
 * its internal nodes use. consistent() exploits this: leaf entries get the
 * *exact* sc_region_contains() test (recheck=false, no accepted candidate is
 * ever a false positive); only internal nodes use the lossy cap_overlaps()
 * necessary-condition test against the query region's own bounding cap,
 * purely to decide which children to descend into.
 *
 * A float-precision storage variant (pgSphere-sized key, 16 bytes instead of
 * this file's 32) was tried and measured, not just argued: it shrank the
 * index (1031MB -> 711MB) and *improved* buffer counts further at every
 * radius from 1' on (e.g. -16% at 3 degrees, widening the margin over
 * pgSphere there), exactly as the smaller-key hypothesis predicted. But it
 * had to give up the exact, recheck=false leaf test above -- a float-
 * rounded point has no slop margin sc_region_contains() can safely use, so
 * every leaf candidate needed recheck=true instead, which re-invokes the
 * real <@ operator on every matching row, not just the pruning
 * comparisons. That cost scales with the *result* row count, not the
 * buffer count a plan reports, and at the radii where this opclass's
 * pruning edge over pgSphere actually shows up, result counts are large
 * enough that this made every query 3-5x slower in wall-clock despite
 * touching fewer pages (confirmed per-probe, not just in the average: e.g.
 * a ~100-buffer probe went from 0.7ms to ~2.5ms, and the single densest
 * probe tested went from 28.8ms to 141ms, the same ~5x ratio at both
 * scales). Reverted; this file keeps the double-precision, recheck=false-
 * at-leaf design. See GIST_REGION_DESIGN.md for the full numbers.
 *
 * REUSED CODE: the GistCap struct and its cap_make/cap_center/cap_union2/
 * cap_union_caps/cap_area_proxy/cap_overlaps/cap_overlap_amount primitives
 * are copied from gist_region.c (they are `static` there, not exposed via a
 * header -- duplicated rather than risking a change to that file's own
 * visibility, which is already proven across forty-plus rounds of work).
 * sc_region_contains() (cover.h, not static) gives the exact leaf test, and
 * skycell_pos_from_datum()/skycell_region_from_datum() (skycell_internal.h)
 * do the Datum extraction, exactly as every other opclass in this directory
 * uses them.
 *
 * PICKSPLIT: the same R*-tree-style sweep as skyregion_gist_picksplit()
 * (gist_region.c) -- sort entries by cap centre along each Cartesian axis,
 * track running forward/backward cap unions, choose the axis minimising
 * total cap area across candidate split points, then the split point on it
 * minimising cap *overlap* (cap_overlap_amount(), not cap_area_proxy()) --
 * but simpler throughout, since each entry here already holds exactly one
 * cap (no multicap union-of-up-to-4-sub-caps bookkeeping is needed for a
 * point, which has no internal structure to decompose).
 *
 * ROUND THREE -- A SAFER SHRINK: round two's mistake was narrowing the
 * centre, the one field the exact leaf test depends on. The radius field is
 * a different story: internal nodes already use it only through the lossy,
 * already-recheck=true cap_overlaps() test, and at a leaf it is always
 * exactly 0 regardless of what precision could represent it in. So this
 * round shrinks storage without touching cx/cy/cz's precision at all: leaf
 * tuples drop the radius field entirely (24 bytes: just the three double
 * coordinates, radius implicit 0) and internal tuples keep double centres
 * but store radius as a float (28 bytes, down from 32). The on-disk format
 * is self-describing (VARSIZE tells bytea_to_cap which shape it's reading),
 * since a single compress()/union()/same()/penalty()/consistent() call must
 * handle whichever shape a given page's entries carry. cap_to_bytea()
 * always emits the leaf shape from compress() (entry->leafkey) and the
 * internal shape everywhere else (union()'s output and picksplit()'s two
 * split data, both of which become a parent page's entry, never a leaf's).
 * This keeps the exact, recheck=false leaf test fully intact -- cx/cy/cz
 * never lose a bit -- while still cutting leaf tuples (the large majority
 * of entries in any GiST tree) by 25%. See GIST_REGION_DESIGN.md for the
 * measured result.
 *
 * STATUS: correctness-verified (0 mismatches, 210 brute-force probes across
 * 1" to 3 degrees, this version, round one's double-precision-everywhere
 * version, and the reverted round-two float variant). Measured against
 * pgSphere's native spoint GiST and skycell's own skypos_spgist_ops on
 * buffer counts and wall-clock -- see GIST_REGION_DESIGN.md for the full
 * numbers before relying on this for anything beyond experimentation. Not
 * yet wired into any versioned SQL file -- register it ad hoc (CREATE
 * FUNCTION ... AS 'MODULE_PATHNAME'; CREATE OPERATOR CLASS ...) against a
 * scratch database until a shipping decision is made.
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

#define SKYPOS_CAP_STRATEGY_CONTAINED_BY_REGION 1

typedef struct
{
	double		cx,
				cy,
				cz;
	double		radius;			/* radians; 0 for an exact point, negative = empty */
} GistCap;

static inline GistCap
cap_make(sc_vec3 c, double r)
{
	GistCap		k;

	k.cx = c.x;
	k.cy = c.y;
	k.cz = c.z;
	k.radius = r;
	return k;
}

static inline sc_vec3
cap_center(GistCap k)
{
	sc_vec3		c;

	c.x = k.cx;
	c.y = k.cy;
	c.z = k.cz;
	return c;
}

/* identical to gist_region.c's cap_union2(); see its own comment for the
 * d + r1 + r2 < pi caveat (not reachable here at the leaf, since leaf caps
 * are always zero-radius; only a chain of unions of already-close points
 * could approach it, same residual limitation as gist_region.c). */
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
	theta = *ro - r1;
	sinD = sin(d);
	if (sinD < 1e-9)
	{
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
			*co = c1;
	}
}

static inline GistCap
cap_union_caps(GistCap a, GistCap b)
{
	sc_vec3		c;
	double		r;

	cap_union2(cap_center(a), a.radius, cap_center(b), b.radius, &c, &r);
	return cap_make(c, r);
}

static double
cap_area_proxy(double radius)
{
	if (radius < 0)
		return 0.0;
	return 1.0 - cos(fmin(radius, M_PI));
}

static bool
cap_overlaps(GistCap a, GistCap b)
{
	double		sum;

	if (a.radius < 0 || b.radius < 0)
		return false;
	sum = a.radius + b.radius;
	if (sum >= M_PI)
		return true;		/* any two points are within pi of each other */
	/*
	 * round 27's trig-free pattern (gist_region.c's cap_contains_point):
	 * "angle(a,b) <= sum" is exactly "dot(a,b) >= cos(sum)" for sum in
	 * [0,pi] (cos is monotonic decreasing there) -- one dot product and one
	 * cos(), instead of sc_angle()'s cross product + sqrt + atan2. This is
	 * consistent()'s hottest per-call path (every internal node visited
	 * during a descent calls it once), confirmed by a median-wall-clock
	 * regression this opclass had despite winning on buffer counts: pgSphere's
	 * native box-vs-box test is six plain comparisons, no trig at all, so
	 * sc_angle()'s cost here was a real per-node tax ours was paying and
	 * pgSphere's wasn't. See GIST_REGION_DESIGN.md for the measured effect.
	 */
	return sc_dot(cap_center(a), cap_center(b)) >= cos(sum);
}

static double
cap_overlap_amount(GistCap a, GistCap b)
{
	if (a.radius < 0 || b.radius < 0)
		return 0.0;
	return fmax(0.0, a.radius + b.radius - sc_angle(cap_center(a), cap_center(b)));
}

/*
 * On-disk shapes (see round three's comment in the file header): a leaf
 * entry's radius is always exactly 0, so it isn't stored at all; an
 * internal entry's radius only ever feeds a lossy, already-rechecked test,
 * so it is stored as a float. Which shape a given bytea holds is read back
 * from its own VARSIZE, not tracked out-of-band -- compress()/union()/
 * picksplit()'s callers already know which to WRITE (is_leaf), but same()/
 * penalty()/consistent() must handle either shape of whatever the index
 * hands them.
 */
typedef struct
{
	double		cx, cy, cz;
}			GistCapLeaf;

/*
 * packed: a plain {double,double,double,float} would round up to 32 bytes
 * under the platform's natural 8-byte struct alignment (three 8-byte
 * doubles plus one 4-byte float is 28 bytes of data but a 32-byte struct,
 * padded to a multiple of its 8-byte alignment) -- silently erasing this
 * round's entire saving over round one's plain 32-byte double key. Member
 * access on a packed struct costs an unaligned load/store instead of a
 * fast aligned one, immaterial here since every access goes through a
 * local copy via memcpy, not a cast pointer into a page.
 */
typedef struct
{
	double		cx, cy, cz;
	float		radius;
}			__attribute__((packed)) GistCapInternal;

static bytea *
cap_to_bytea(const GistCap *c, bool is_leaf)
{
	if (is_leaf)
	{
		Size		sz = VARHDRSZ + sizeof(GistCapLeaf);
		bytea	   *out = (bytea *) palloc(sz);
		GistCapLeaf l;

		Assert(c->radius == 0.0);
		l.cx = c->cx;
		l.cy = c->cy;
		l.cz = c->cz;
		SET_VARSIZE(out, sz);
		memcpy(VARDATA(out), &l, sizeof(GistCapLeaf));
		return out;
	}
	else
	{
		Size		sz = VARHDRSZ + sizeof(GistCapInternal);
		bytea	   *out = (bytea *) palloc(sz);
		GistCapInternal n;

		n.cx = c->cx;
		n.cy = c->cy;
		n.cz = c->cz;
		n.radius = (float) c->radius;
		SET_VARSIZE(out, sz);
		memcpy(VARDATA(out), &n, sizeof(GistCapInternal));
		return out;
	}
}

static void
bytea_to_cap(bytea *b, GistCap *out)
{
	Size		sz = VARSIZE_ANY_EXHDR(b);

	if (sz == sizeof(GistCapLeaf))
	{
		GistCapLeaf l;

		memcpy(&l, VARDATA_ANY(b), sizeof(GistCapLeaf));
		out->cx = l.cx;
		out->cy = l.cy;
		out->cz = l.cz;
		out->radius = 0.0;
	}
	else
	{
		GistCapInternal n;

		Assert(sz == sizeof(GistCapInternal));
		memcpy(&n, VARDATA_ANY(b), sizeof(GistCapInternal));
		out->cx = n.cx;
		out->cy = n.cy;
		out->cz = n.cz;
		out->radius = n.radius;
	}
}

PG_FUNCTION_INFO_V1(skypos_cap_gist_compress);
Datum
skypos_cap_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		sc_vec3		p = skycell_pos_from_datum(entry->key);
		GistCap		k = cap_make(p, 0.0);
		bytea	   *b = cap_to_bytea(&k, true);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(b), entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(skypos_cap_gist_decompress);
Datum
skypos_cap_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(skypos_cap_gist_union);
Datum
skypos_cap_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	GistCap		out,
				c;
	bytea	   *outb;

	bytea_to_cap(DatumGetByteaP(entryvec->vector[0].key), &out);
	for (int i = 1; i < entryvec->n; i++)
	{
		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &c);
		out = cap_union_caps(out, c);
	}
	outb = cap_to_bytea(&out, false);
	*sizep = VARSIZE(outb);
	PG_RETURN_POINTER(outb);
}

PG_FUNCTION_INFO_V1(skypos_cap_gist_penalty);
Datum
skypos_cap_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	GistCap		o,
				n,
				u;

	bytea_to_cap(DatumGetByteaP(origentry->key), &o);
	bytea_to_cap(DatumGetByteaP(newentry->key), &n);
	u = cap_union_caps(o, n);
	*result = (float) (cap_area_proxy(u.radius) - cap_area_proxy(o.radius));
	PG_RETURN_POINTER(result);
}

typedef struct
{
	double		key;			/* cap centre's coordinate along the axis being tried */
	OffsetNumber idx;
}			cap_sort_entry;

static int
cap_sort_cmp(const void *a, const void *b)
{
	double		ka = ((const cap_sort_entry *) a)->key;
	double		kb = ((const cap_sort_entry *) b)->key;

	return (ka > kb) - (ka < kb);
}

/*
 * R*-tree-style split (Beckmann et al. 1990), the single-cap analogue of
 * skyregion_gist_picksplit() (gist_region.c): since each entry here is
 * exactly one GistCap (not a multicap union of up to 4 sub-caps), the
 * running forward/backward unions and the per-split-point cost are plain
 * cap_union_caps()/cap_area_proxy()/cap_overlap_amount() calls, no
 * MAX_SUBCAPS loop anywhere. Loop bounds are OffsetNumber/FirstOffsetNumber
 * throughout, matching gist_region.c's own convention (and the bug that
 * convention exists to prevent -- see its file header).
 */
PG_FUNCTION_INFO_V1(skypos_cap_gist_picksplit);
Datum
skypos_cap_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = (OffsetNumber) (entryvec->n - 1);
	int			n = maxoff - FirstOffsetNumber + 1;
	GistCap    *caps = (GistCap *) palloc(sizeof(GistCap) * (maxoff + 1));
	int			minfill = Max(1, n * 3 / 10);
	cap_sort_entry *sorted = (cap_sort_entry *) palloc(sizeof(cap_sort_entry) * n);
	GistCap    *fwd = (GistCap *) palloc(sizeof(GistCap) * n);
	GistCap    *bwd = (GistCap *) palloc(sizeof(GistCap) * n);
	int			bestAxis = 0;
	double		bestAxisMargin = HUGE_VAL;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		bytea_to_cap(DatumGetByteaP(entryvec->vector[i].key), &caps[i]);

	for (int axis = 0; axis < 3; axis++)
	{
		double		marginSum = 0;

		for (int k = 0; k < n; k++)
		{
			OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
			sc_vec3		c = cap_center(caps[idx]);

			sorted[k].idx = idx;
			sorted[k].key = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
		}
		qsort(sorted, n, sizeof(cap_sort_entry), cap_sort_cmp);

		fwd[0] = caps[sorted[0].idx];
		for (int k = 1; k < n; k++)
			fwd[k] = cap_union_caps(fwd[k - 1], caps[sorted[k].idx]);
		bwd[n - 1] = caps[sorted[n - 1].idx];
		for (int k = n - 2; k >= 0; k--)
			bwd[k] = cap_union_caps(bwd[k + 1], caps[sorted[k].idx]);

		for (int m = minfill; m <= n - minfill; m++)
			marginSum += cap_area_proxy(fwd[m - 1].radius) + cap_area_proxy(bwd[m].radius);

		if (marginSum < bestAxisMargin)
		{
			bestAxisMargin = marginSum;
			bestAxis = axis;
		}
	}

	/* redo the winning axis's sort + cumulative unions */
	for (int k = 0; k < n; k++)
	{
		OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
		sc_vec3		c = cap_center(caps[idx]);

		sorted[k].idx = idx;
		sorted[k].key = (bestAxis == 0) ? c.x : (bestAxis == 1) ? c.y : c.z;
	}
	qsort(sorted, n, sizeof(cap_sort_entry), cap_sort_cmp);
	fwd[0] = caps[sorted[0].idx];
	for (int k = 1; k < n; k++)
		fwd[k] = cap_union_caps(fwd[k - 1], caps[sorted[k].idx]);
	bwd[n - 1] = caps[sorted[n - 1].idx];
	for (int k = n - 2; k >= 0; k--)
		bwd[k] = cap_union_caps(bwd[k + 1], caps[sorted[k].idx]);

	{
		int			bestM = minfill;
		double		bestOverlap = HUGE_VAL;
		double		bestArea = HUGE_VAL;

		for (int m = minfill; m <= n - minfill; m++)
		{
			double		overlap = cap_overlap_amount(fwd[m - 1], bwd[m]);
			double		area = cap_area_proxy(fwd[m - 1].radius) + cap_area_proxy(bwd[m].radius);

			if (overlap < bestOverlap || (overlap == bestOverlap && area < bestArea))
			{
				bestOverlap = overlap;
				bestArea = area;
				bestM = m;
			}
		}

		v->spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * bestM);
		v->spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * (n - bestM));
		v->spl_nleft = v->spl_nright = 0;
		for (int k = 0; k < bestM; k++)
			v->spl_left[v->spl_nleft++] = sorted[k].idx;
		for (int k = bestM; k < n; k++)
			v->spl_right[v->spl_nright++] = sorted[k].idx;

		v->spl_ldatum = PointerGetDatum(cap_to_bytea(&fwd[bestM - 1], false));
		v->spl_rdatum = PointerGetDatum(cap_to_bytea(&bwd[bestM], false));
	}

	pfree(caps);
	pfree(sorted);
	pfree(fwd);
	pfree(bwd);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skypos_cap_gist_same);
Datum
skypos_cap_gist_same(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_P(0);
	bytea	   *b = PG_GETARG_BYTEA_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	GistCap		ca,
				cb;

	bytea_to_cap(a, &ca);
	bytea_to_cap(b, &cb);
	*result = (memcmp(&ca, &cb, sizeof(GistCap)) == 0);
	PG_RETURN_POINTER(result);
}

/*
 * Query-side cache: the same value-keyed fn_extra discipline as gist_region.
 * c's consistent()/gist_region_box.c's box_cached_query()/spgist_region.c's
 * spg_cached_region() -- keyed on the query Datum's bytes, not its pointer,
 * for the same reason all three give (a join's per-tuple memory context is
 * reset and reused between outer rows, so pointer identity alone cannot
 * tell two different rows' regions apart). Caches the full parsed sc_region
 * (for the leaf-level exact test) and its bounding cap (for internal-node
 * pruning), computed together whenever the query bytes change.
 */
typedef struct
{
	bytea	   *last_query;
	Size		last_query_size;
	sc_region	reg;
	GistCap		bound;
}			cap_query_cache;

static void
cap_cached_query(FunctionCallInfo fcinfo, Datum queryDatum, sc_region **reg_out, GistCap *bound_out)
{
	cap_query_cache *qcache = (cap_query_cache *) fcinfo->flinfo->fn_extra;
	bytea	   *qb = DatumGetByteaP(queryDatum);
	Size		qsz = VARSIZE(qb);

	if (qcache == NULL)
	{
		qcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(cap_query_cache));
		fcinfo->flinfo->fn_extra = qcache;
	}
	if (qcache->last_query == NULL || qcache->last_query_size != qsz ||
		memcmp(qcache->last_query, qb, qsz) != 0)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
		sc_vec3		c;
		double		r;

		if (qcache->last_query != NULL)
			sc_region_free(&qcache->reg);	/* frees the old poly v[]/n[], if any */
		skycell_region_from_datum(queryDatum, &qcache->reg);
		MemoryContextSwitchTo(oldcxt);

		sc_region_bounding_cap(&qcache->reg, &c, &r);
		qcache->bound = cap_make(c, r);

		if (qcache->last_query == NULL || qcache->last_query_size < qsz)
		{
			if (qcache->last_query != NULL)
				pfree(qcache->last_query);
			qcache->last_query = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, qsz);
		}
		memcpy(qcache->last_query, qb, qsz);
		qcache->last_query_size = qsz;
	}
	*reg_out = &qcache->reg;
	*bound_out = qcache->bound;
}

/*
 * Leaf entries get the EXACT test (sc_region_contains against the point
 * this cap was built from, recheck=false): the structural edge this
 * opclass has over pgSphere's own leaf key, which pageinspect confirmed is
 * a lossy box even at the leaf. Internal entries have no single point left
 * to test exactly -- only a bounding cap -- so they fall back to
 * cap_overlaps() against the query's own bounding cap, a sound (necessary,
 * not sufficient) pruning condition: if a point under this subtree could be
 * in the query region, the subtree's bounding cap must overlap some cap
 * containing that region, and the region's own bounding cap is one such
 * cap. *recheck* is meaningless for a non-leaf result (GiST ignores it
 * there) but set anyway for clarity.
 *
 * A float-precision storage variant tried dropping this leaf-exact branch
 * in favour of a smaller on-disk key; it measured 3-5x slower in wall-clock
 * at every radius despite fewer buffers, because recheck=true at the leaf
 * re-invokes the real <@ operator on every matching row, a cost that scales
 * with result cardinality rather than with pages touched. Keeping the exact
 * leaf test here is load-bearing, not a nicety -- see the file header.
 */
PG_FUNCTION_INFO_V1(skypos_cap_gist_consistent);
Datum
skypos_cap_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	GistCap		ek;
	sc_region  *qreg;
	GistCap		qbound;
	bool		result;

	if (strategy != SKYPOS_CAP_STRATEGY_CONTAINED_BY_REGION)
		elog(ERROR, "skypos_cap_gist_consistent: unsupported strategy %d", strategy);

	bytea_to_cap(DatumGetByteaP(entry->key), &ek);
	cap_cached_query(fcinfo, queryDatum, &qreg, &qbound);

	if (GIST_LEAF(entry))
	{
		result = sc_region_contains(qreg, cap_center(ek)) != 0;
		*recheck = false;
	}
	else
	{
		result = cap_overlaps(ek, qbound);
		*recheck = true;
	}
	PG_RETURN_BOOL(result);
}
