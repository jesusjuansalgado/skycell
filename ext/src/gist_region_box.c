/*
 * gist_region_box.c -- EXPERIMENTAL alternative GiST opclass for skyregion:
 * a plain axis-aligned 3D box key (6 doubles), no spherical caps at all,
 * deliberately modelled on pgSphere's own Box3D key rather than this
 * extension's multi-cap opclass (gist_region.c).
 *
 * WHY THIS EXISTS: gist_region.c's multi-cap key wins && and <@ against
 * pgSphere but loses @>(region,region) and @>(region,point), and three
 * independent rounds (14, 26, 41 -- see GIST_REGION_DESIGN.md) found no
 * way to close those two without either regressing something else or
 * hitting the same key-bloat tax every size-growing attempt in that file
 * has paid. The two losing strategies share the same root cause: neither
 * has a tight, sound per-strategy test to build from this key shape (round
 * six's own finding), so they fall back to the *overlap* test as a
 * necessary condition -- meaning the multi-cap key's extra richness over a
 * plain box mostly goes unused for exactly the strategies it's currently
 * losing. This opclass tests the natural question directly: if the
 * richness isn't paying for itself there anyway, does trading it away
 * entirely for a cheaper, smaller box -- accepting whatever && / <@ lose
 * in the process -- land closer to pgSphere everywhere, the way pgSphere's
 * own box key does?
 *
 * Not DEFAULT: `skyregion_gist_ops` (the multi-cap opclass) remains the
 * shipped default. Select this one explicitly: `CREATE INDEX ... USING
 * gist (col skyregion_box_gist_ops)`.
 *
 * KEY: six doubles (xmin,ymin,zmin,xmax,ymax,zmax) in Cartesian space, no
 * varlena padding tricks -- deliberately as close to pgSphere's own
 * Box3D as this extension's region type allows.
 *
 * BUILDING THE BOX: rather than re-deriving a from-scratch bounding-box
 * formula per region kind, reuses this extension's own, already-tested
 * `sc_region_bounding_cap()` (the same function gist_region.c's
 * region_to_multicap() calls) and converts *that* cap into an exact,
 * closed-form axis-aligned box -- provably a safe (non-lossy in the
 * "never too small" sense) over-approximation of the region itself, since
 * the region is entirely contained in its own bounding cap and the box
 * is computed to entirely contain that cap. This costs a little tightness
 * (the box is only as tight as the bounding cap, not the tightest
 * possible box for an odd-shaped polygon) in exchange for reusing
 * correctness this extension already has, rather than writing and
 * re-verifying a second bounding-volume computation from scratch.
 *
 * The per-axis cap -> box formula: for a cap with centre unit vector c
 * and angular radius r, parametrising its boundary circle in an
 * orthonormal (c, u, v) frame gives boundary points p(theta) = c*cos(r) +
 * (u*cos(theta) + v*sin(theta))*sin(r); since (c, u, v) is an orthonormal
 * basis, c_k^2 + u_k^2 + v_k^2 = 1 for any fixed axis k, so component k's
 * amplitude around its boundary is exactly sqrt(1 - c_k^2). That gives
 * max/min over the *boundary* as c_k*cos(r) +/- sqrt(1-c_k^2)*sin(r) --
 * exact, not sampled -- with one more check needed: if the cap swallows
 * axis k's own pole (+-e_k) entirely (angle(c, +-e_k) <= r, i.e. +-c_k >=
 * cos(r)), the true extreme on that side is the pole itself (+-1), which
 * the boundary-only formula would under-report.
 *
 * CONSISTENT: box-vs-box interleave (six comparisons, no trig at all) for
 * &&/@>(region,region)/<@(region,region) -- the same "reuse overlap as a
 * necessary condition" shortcut both this extension's own multi-cap
 * opclass and pgSphere itself use for these strategies, so this isn't
 * giving the box key an unsound shortcut the cap key couldn't also use;
 * it's testing whether the *cost* of evaluating that shared shortcut is
 * where the real difference lives. @>(region,point) is a direct box-
 * contains-point test. recheck is always true (lossy either way, same as
 * gist_region.c).
 *
 * See GIST_REGION_DESIGN.md's "Round forty-two" for the measured result.
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

#define GIST_REGION_BOX_STRATEGY_OVERLAP 1
#define GIST_REGION_BOX_STRATEGY_CONTAINS_POINT 2
#define GIST_REGION_BOX_STRATEGY_CONTAINS_REGION 3
#define GIST_REGION_BOX_STRATEGY_CONTAINED_BY_REGION 4

typedef struct
{
	double		xmin,
				ymin,
				zmin,
				xmax,
				ymax,
				zmax;
} GistBox3D;

static void
axis_extent(double ck, double cr, double sr, double *lo, double *hi)
{
	double		root = sqrt(fmax(0.0, 1.0 - ck * ck));
	double		bmax = ck * cr + root * sr;
	double		bmin = ck * cr - root * sr;

	*hi = (ck >= cr) ? 1.0 : bmax;
	*lo = (-ck >= cr) ? -1.0 : bmin;
}

/* cap -> box3d, exact (see file header for the derivation) */
static void
cap_to_box3d(sc_vec3 c, double r, GistBox3D *out)
{
	double		cr,
				sr;

	if (r < 0)
	{
		/* empty region: an inverted box so union()/overlap behave like
		 * the empty set. */
		out->xmin = out->ymin = out->zmin = 1.0;
		out->xmax = out->ymax = out->zmax = -1.0;
		return;
	}
	if (r >= M_PI)
	{
		out->xmin = out->ymin = out->zmin = -1.0;
		out->xmax = out->ymax = out->zmax = 1.0;
		return;
	}
	cr = cos(r);
	sr = sin(r);
	axis_extent(c.x, cr, sr, &out->xmin, &out->xmax);
	axis_extent(c.y, cr, sr, &out->ymin, &out->ymax);
	axis_extent(c.z, cr, sr, &out->zmin, &out->zmax);
}

static void
region_to_box3d(sc_region *r, GistBox3D *out)
{
	sc_vec3		c;
	double		rad;

	sc_region_bounding_cap(r, &c, &rad);
	cap_to_box3d(c, rad, out);
}

static void
box3d_union(const GistBox3D *a, const GistBox3D *b, GistBox3D *out)
{
	out->xmin = Min(a->xmin, b->xmin);
	out->ymin = Min(a->ymin, b->ymin);
	out->zmin = Min(a->zmin, b->zmin);
	out->xmax = Max(a->xmax, b->xmax);
	out->ymax = Max(a->ymax, b->ymax);
	out->zmax = Max(a->zmax, b->zmax);
}

static double
box3d_volume(const GistBox3D *b)
{
	double		dx = Max(0.0, b->xmax - b->xmin);
	double		dy = Max(0.0, b->ymax - b->ymin);
	double		dz = Max(0.0, b->zmax - b->zmin);

	return dx * dy * dz;
}

static bool
box3d_overlaps(const GistBox3D *a, const GistBox3D *b)
{
	return a->xmin <= b->xmax && b->xmin <= a->xmax &&
		a->ymin <= b->ymax && b->ymin <= a->ymax &&
		a->zmin <= b->zmax && b->zmin <= a->zmax;
}

static bool
box3d_contains_point(const GistBox3D *b, sc_vec3 p)
{
	return p.x >= b->xmin && p.x <= b->xmax &&
		p.y >= b->ymin && p.y <= b->ymax &&
		p.z >= b->zmin && p.z <= b->zmax;
}

static bytea *
box3d_to_bytea(const GistBox3D *b)
{
	Size		sz = VARHDRSZ + sizeof(GistBox3D);
	bytea	   *out = (bytea *) palloc(sz);

	SET_VARSIZE(out, sz);
	memcpy(VARDATA(out), b, sizeof(GistBox3D));
	return out;
}

static void
bytea_to_box3d(bytea *b, GistBox3D *out)
{
	memcpy(out, VARDATA_ANY(b), sizeof(GistBox3D));
}

PG_FUNCTION_INFO_V1(skyregion_box_gist_compress);
Datum
skyregion_box_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		sc_region	r;
		GistBox3D	b;
		bytea	   *k;

		skycell_region_from_datum(entry->key, &r);
		region_to_box3d(&r, &b);
		sc_region_free(&r);
		k = box3d_to_bytea(&b);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(k), entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(skyregion_box_gist_decompress);
Datum
skyregion_box_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(skyregion_box_gist_union);
Datum
skyregion_box_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *sizep = (int *) PG_GETARG_POINTER(1);
	GistBox3D	out,
				b;
	bytea	   *outb;

	bytea_to_box3d(DatumGetByteaP(entryvec->vector[0].key), &out);
	for (int i = 1; i < entryvec->n; i++)
	{
		bytea_to_box3d(DatumGetByteaP(entryvec->vector[i].key), &b);
		box3d_union(&out, &b, &out);
	}
	outb = box3d_to_bytea(&out);
	*sizep = VARSIZE(outb);
	PG_RETURN_POINTER(outb);
}

PG_FUNCTION_INFO_V1(skyregion_box_gist_penalty);
Datum
skyregion_box_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	GistBox3D	o,
				n,
				u;

	bytea_to_box3d(DatumGetByteaP(origentry->key), &o);
	bytea_to_box3d(DatumGetByteaP(newentry->key), &n);
	box3d_union(&o, &n, &u);
	*result = (float) (box3d_volume(&u) - box3d_volume(&o));
	PG_RETURN_POINTER(result);
}

typedef struct
{
	double		key;
	OffsetNumber idx;
}			box3d_sort_entry;

static int
box3d_sort_cmp(const void *a, const void *b)
{
	double		ka = ((const box3d_sort_entry *) a)->key;
	double		kb = ((const box3d_sort_entry *) b)->key;

	return (ka > kb) - (ka < kb);
}

/*
 * Deliberately the simplest reasonable split, not gist_region.c's R*-tree-
 * style sweep: pick whichever axis has the widest spread of entry
 * midpoints, sort on it, cut at the median. A box key's whole premise
 * here is cheapness; re-implementing the multi-cap opclass's own more
 * elaborate split logic on top of it would muddy what's actually being
 * compared.
 */
PG_FUNCTION_INFO_V1(skyregion_box_gist_picksplit);
Datum
skyregion_box_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = (OffsetNumber) (entryvec->n - 1);
	int			n = maxoff - FirstOffsetNumber + 1;
	GistBox3D  *bx = (GistBox3D *) palloc(sizeof(GistBox3D) * (maxoff + 1));
	box3d_sort_entry *sorted = (box3d_sort_entry *) palloc(sizeof(box3d_sort_entry) * n);
	int			bestAxis = 0;
	double		bestSpread = -1;
	int			mid = n / 2;
	GistBox3D	lu,
				ru;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		bytea_to_box3d(DatumGetByteaP(entryvec->vector[i].key), &bx[i]);

	for (int axis = 0; axis < 3; axis++)
	{
		double		lo = HUGE_VAL,
					hi = -HUGE_VAL;

		for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		{
			double		c = (axis == 0) ? (bx[i].xmin + bx[i].xmax) / 2.0
				: (axis == 1) ? (bx[i].ymin + bx[i].ymax) / 2.0
				: (bx[i].zmin + bx[i].zmax) / 2.0;

			lo = Min(lo, c);
			hi = Max(hi, c);
		}
		if (hi - lo > bestSpread)
		{
			bestSpread = hi - lo;
			bestAxis = axis;
		}
	}

	for (int k = 0; k < n; k++)
	{
		OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
		double		c = (bestAxis == 0) ? (bx[idx].xmin + bx[idx].xmax) / 2.0
			: (bestAxis == 1) ? (bx[idx].ymin + bx[idx].ymax) / 2.0
			: (bx[idx].zmin + bx[idx].zmax) / 2.0;

		sorted[k].key = c;
		sorted[k].idx = idx;
	}
	qsort(sorted, n, sizeof(box3d_sort_entry), box3d_sort_cmp);

	v->spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	v->spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * n);
	v->spl_nleft = v->spl_nright = 0;

	lu = bx[sorted[0].idx];
	v->spl_left[v->spl_nleft++] = sorted[0].idx;
	for (int k = 1; k < mid; k++)
	{
		box3d_union(&lu, &bx[sorted[k].idx], &lu);
		v->spl_left[v->spl_nleft++] = sorted[k].idx;
	}

	ru = bx[sorted[mid].idx];
	v->spl_right[v->spl_nright++] = sorted[mid].idx;
	for (int k = mid + 1; k < n; k++)
	{
		box3d_union(&ru, &bx[sorted[k].idx], &ru);
		v->spl_right[v->spl_nright++] = sorted[k].idx;
	}

	v->spl_ldatum = PointerGetDatum(box3d_to_bytea(&lu));
	v->spl_rdatum = PointerGetDatum(box3d_to_bytea(&ru));

	pfree(bx);
	pfree(sorted);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skyregion_box_gist_same);
Datum
skyregion_box_gist_same(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_P(0);
	bytea	   *b = PG_GETARG_BYTEA_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	GistBox3D	ba,
				bb;

	bytea_to_box3d(a, &ba);
	bytea_to_box3d(b, &bb);
	*result = (memcmp(&ba, &bb, sizeof(GistBox3D)) == 0);
	PG_RETURN_POINTER(result);
}

/*
 * Query-side box cache: the same wasted-work shape (and the same value-
 * based, not pointer-based, cache-key fix) as gist_region.c's own
 * consistent()/round one and spgist_region.c's inner_consistent()/round
 * thirty-nine. Cheaper here still: the cache only needs to hold the
 * query's own 48-byte box, not a full sc_region (no polygon v[]/n[]
 * lifetime to manage).
 */
typedef struct
{
	bytea	   *last_query;
	Size		last_query_size;
	GistBox3D	qbox;
}			box_query_cache;

static const GistBox3D *
box_cached_query(FunctionCallInfo fcinfo, Datum queryDatum)
{
	box_query_cache *qcache = (box_query_cache *) fcinfo->flinfo->fn_extra;
	bytea	   *qb = DatumGetByteaP(queryDatum);
	Size		qsz = VARSIZE(qb);

	if (qcache == NULL)
	{
		qcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(box_query_cache));
		fcinfo->flinfo->fn_extra = qcache;
	}
	if (qcache->last_query == NULL || qcache->last_query_size != qsz ||
		memcmp(qcache->last_query, qb, qsz) != 0)
	{
		sc_region	qreg;

		skycell_region_from_datum(queryDatum, &qreg);
		region_to_box3d(&qreg, &qcache->qbox);
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
	return &qcache->qbox;
}

PG_FUNCTION_INFO_V1(skyregion_box_gist_consistent);
Datum
skyregion_box_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Datum		queryDatum = PG_GETARG_DATUM(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	GistBox3D	eb;
	bool		result;

	bytea_to_box3d(DatumGetByteaP(entry->key), &eb);

	switch (strategy)
	{
		case GIST_REGION_BOX_STRATEGY_OVERLAP:
		case GIST_REGION_BOX_STRATEGY_CONTAINS_REGION:
		case GIST_REGION_BOX_STRATEGY_CONTAINED_BY_REGION:
			result = box3d_overlaps(&eb, box_cached_query(fcinfo, queryDatum));
			*recheck = true;
			break;
		case GIST_REGION_BOX_STRATEGY_CONTAINS_POINT:
			result = box3d_contains_point(&eb, skycell_pos_from_datum(queryDatum));
			*recheck = true;
			break;
		default:
			elog(ERROR, "skyregion_box_gist_consistent: unsupported strategy %d", strategy);
			result = false;
	}
	PG_RETURN_BOOL(result);
}
