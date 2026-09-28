/*
 * gist_region.c -- a GiST opclass for skyregion, indexing region-region
 * INTERSECTS (&&) directly.
 *
 * skyregion has no per-row covering the way a point column does (skycell_
 * cone_bound and friends): both sides of a region-region test are extended
 * shapes, so there is no single outer row to build a covering from.  The
 * MOC-ranges recipe (skycell_region_moc_ranges, see skycell--0.9.sql) works
 * around that with a hand-maintained side table.  This file instead gives
 * skyregion a real index: a GiST opclass over &&, with no side table and no
 * hand-written join.
 *
 * KEY DESIGN, round two (see GIST_REGION_DESIGN.md's "Picksplit, round two"
 * for the round-one story: a single bounding cap per region, tuned with two
 * successive split algorithms, found to scale badly -- 5x slower than
 * pgSphere's native && at 50,000 rows despite being competitive at 5,000,
 * with EXPLAIN (ANALYZE, BUFFERS) showing the tree itself being walked too
 * much, not a recheck-stage problem). A single cap is too coarse a key once
 * enough same-scale regions accumulate: this file replaces it with up to
 * MAX_SUBCAPS (4) sub-caps per region, mirroring how the MOC-ranges recipe
 * gets its own precision from several `[lo,hi]` ranges per region rather
 * than one:
 *
 *   - a cone still gets exactly one sub-cap (itself -- already exact, a
 *     second cap would add nothing);
 *   - a polygon is decomposed via the extension's own MOC builder
 *     (moc_for_region(), shared with skycell_region_moc()) into up to
 *     MAX_SUBCAPS HEALPix cells, each converted to its own (centre, radius)
 *     cap via sc_pix2vec()/sc_pixrad() -- an adaptive, boundary-aware
 *     decomposition already built for a different purpose (indexing stored
 *     footprints in a B-tree), reused here instead of reinvented.
 *
 * Each key also keeps one "overall" cap -- the plain union of its sub-caps,
 * the same single-cap key round one used throughout -- purely as a cheap
 * summary for picksplit's axis-sort heuristic; it plays no part in
 * consistent()'s actual pruning decision, which uses the sub-caps only.
 *
 * consistent(): two multi-cap keys "overlap" iff any sub-cap of one overlaps
 * any sub-cap of the other (an O(MAX_SUBCAPS^2) check, with the overall caps
 * checked first as a cheap short-circuit reject, valid because every
 * sub-cap lies within its region's own overall cap by construction). Still
 * lossy (recheck stays true), but a tighter lossy than one big cap.
 *
 * Storage type is plain bytea, a fixed-size packed struct (~164 bytes: an
 * overall cap plus MAX_SUBCAPS sub-caps, empty slots marked with a negative
 * radius) -- bigger than round one's single-cap key (36 bytes), which lowers
 * per-page fanout; whether the pruning improvement is worth that trade is
 * exactly what this redesign has to answer empirically (see
 * GIST_REGION_DESIGN.md).
 *
 * union() and penalty() merge sub-caps from multiple keys down to at most
 * MAX_SUBCAPS via greedy nearest-cluster absorption (merge_caps_greedy): the
 * first MAX_SUBCAPS input caps seed one cluster each, then every further cap
 * joins whichever existing cluster it would enlarge least. Not a globally
 * optimal clustering, but O(n * MAX_SUBCAPS), cheap enough to run on every
 * union/penalty call the way a full agglomerative merge would not be.
 *
 * picksplit is unchanged in spirit from round one (R*-tree-style: choose the
 * axis -- among the overall cap centre's x/y/z coordinate -- with the
 * smallest total margin, then the split point on it minimising overlap), but
 * now operates on each entry's *overall* cap for the axis-sort/margin/
 * overlap heuristic (a deliberate simplification: split quality is a
 * node-level partitioning question, not the same thing consistent()'s
 * per-sub-cap precision is for), while the final left/right keys are built
 * from the *full* multi-cap union of whichever entries land on each side --
 * this is what lets the tighter representation propagate through internal
 * nodes, not just leaves, which is where round one's actual bottleneck was
 * (too many internal pages visited, not too many false leaf candidates).
 *
 * picksplit's loop bounds are OffsetNumber/FirstOffsetNumber, not plain
 * 0-based indices, on purpose: an earlier version treated
 * entryvec->vector[0] as a real entry there (it is not -- that slot is
 * reserved, unlike in union(), which really is 0-based) and crashed the
 * server building an index over as few as ~160 rows, the first row count
 * that forces a page split. See GIST_REGION_DESIGN.md for how that was
 * diagnosed; a correctness test too small to force a split will not catch a
 * regression here.
 *
 * STATUS: correctness-verified (a fresh 2923-row/104,215-pair mixed circle/
 * polygon self-join stress test, 0 false positives/negatives, plus both
 * benchmark scales below agreeing exactly with brute force) and the
 * redesign paid off: ~6.8ms vs pgSphere's ~25ms at 200 probes x 5,000
 * footprints (faster than pgSphere, not just competitive), and ~110ms vs
 * pgSphere's ~90ms at 500 x 50,000 (round one was ~470ms there, ~5.3x
 * slower than pgSphere -- this is ~1.2x). See GIST_REGION_DESIGN.md's
 * "Round three" for the full numbers and what's still untested (>50,000
 * rows, concurrent writes).
 *
 * A second strategy, @>(skyregion,skypos) (round four: "does this region
 * contain this point", the commutator of the existing <@(skypos,skyregion)
 * operator), was added the same way pgSphere's own scircle_ops registers
 * scircle @> spoint alongside scircle && scircle in one opclass: the query
 * argument is a skypos, not a skyregion, so consistent() dispatches on
 * strategy number to tell which type it's holding. The pruning test reuses
 * the same sub-caps as OVERLAP: a point can only be in the region if it's in
 * at least one sub-cap, since the sub-caps cover the region by construction
 * (moc_for_region's decomposition, built to bound a region for indexing, is
 * never a gappy approximation). No new geometry needed, as expected -- but
 * one real bug: consistent() used to read the query argument as a bytea
 * unconditionally, before dispatching on strategy, left over from when &&
 * was the only one; a skypos has no varlena header at all, so that surfaced
 * immediately as "compressed lz4 data is corrupt" the moment a correctness
 * test exercised this strategy. Fixed by moving that read into the &&
 * case where it belongs -- caught before any benchmark number was trusted,
 * unlike the two bugs in earlier rounds.
 *
 * STATUS (round four): correctness-verified (59,342/59,342 against a fresh
 * 2923-row/4000-point stress test, 0 false positives/negatives) and a clear
 * win over the only baseline that existed for this direction before it
 * (brute-force skycell_in_region): ~110x faster at 200 probes x 5,000
 * regions, ~160x at 500 x 50,000. Against pgSphere's native containment the
 * gap widens with scale (~2x -> ~5.5x) for a specific, checked reason (not
 * a new pruning regression -- see GIST_REGION_DESIGN.md's "Round four"):
 * this opclass's own absolute time barely differs from OVERLAP's at the
 * same scale, but pgSphere's native point-in-shape test is cheaper than its
 * native shape-shape overlap test, so pgSphere's <@ pulls further ahead of
 * its own && than this opclass's <@ does of its own &&.
 *
 * STATUS (round five): tried making picksplit's own cost functions sub-cap
 * aware instead of overall-cap-only (see skyregion_gist_picksplit's own
 * comment below for the design). A real, if modest, win: ~10-15% fewer
 * index pages visited per probe at both benchmark scales, for both
 * strategies (measured via EXPLAIN BUFFERS, not wall-clock time, which was
 * too noisy on this machine to trust directly at these sub-100ms query
 * times -- the buffer count is deterministic and moved the same direction
 * every time it was measured, unlike a handful of individual timing runs).
 * Kept over the overall-cap-only version: strictly better on the one metric
 * that isolates the split algorithm's own effect, same O(n) per axis
 * complexity, and it actually simplifies picksplit's own code (the final
 * spl_ldatum/spl_rdatum fall out of the same sweep that chose the split
 * point, instead of a separate multicap_union_many() pass over the result).
 * See GIST_REGION_DESIGN.md's "Round five" for the full numbers.
 *
 * A third strategy, @>(skyregion,skyregion) (round six: "does this region
 * wholly contain that region", the full-containment test README.md and the
 * paper both used to describe as "not yet indexed, evaluated by sequential
 * scan"), was added the cheap way: it reuses OVERLAP's own consistent()
 * test rather than a new one. That is a deliberate, sound choice, not a
 * placeholder -- see the comment at the top of skyregion_gist_consistent's
 * OVERLAP/CONTAINS_REGION case for why a tighter per-sub-cap test would
 * risk a false negative (pruning a true match) where this one cannot: "A
 * contains B" implies "A and B overlap" whenever B is nonempty, and that
 * implication survives replacing both sides with their (superset) cap
 * covers, so non-overlapping covers are still a sound proof of
 * non-containment. What this buys is exactly OVERLAP's own selectivity,
 * not tighter -- a row can pass this strategy's index test by merely
 * touching the query region, with recheck (skycell_region_covers) doing
 * the actual containment decision -- but that is still a real filter
 * against a full sequential scan whenever most of a catalogue's footprints
 * don't touch the query region at all, which is the common case. See
 * bench/24_region_contains_region.sql for the measured numbers.
 *
 * A fourth strategy, <@(skyregion,skyregion) (round seven: "is this row's
 * region wholly contained within that region", CONTAINS_REGION's mirror --
 * "which of my candidate footprints fit inside this one" rather than "which
 * of my candidates contain this one"), closes the direction gap round six's
 * own comment flagged: @>(skyregion,skyregion) had no COMMUTATOR, so only
 * `indexed_col @> probe` could use the index, never `probe @> indexed_col`.
 * Needs no new pruning logic at all -- the same overlap-of-covers necessary
 * condition CONTAINS_REGION already established holds regardless of which
 * side is "the container" and which is "the contained", so this strategy
 * shares CONTAINS_REGION's whole case body in consistent(), differing only
 * in which exact function GiST's own recheck falls back to (determined by
 * which operator the query actually used, not by anything this file does).
 * See GIST_REGION_DESIGN.md's "Round seven" for the numbers.
 */
#include "postgres.h"

#include <math.h>
#include <string.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "varatt.h"

#include "cover.h"
#include "healpix.h"
#include "skycell_internal.h"

#define GIST_REGION_STRATEGY_OVERLAP 1	/* && (skyregion, skyregion) */
#define GIST_REGION_STRATEGY_CONTAINS_POINT 2	/* @> (skyregion, skypos) */
#define GIST_REGION_STRATEGY_CONTAINS_REGION 3	/* @> (skyregion, skyregion) */
#define GIST_REGION_STRATEGY_CONTAINED_BY_REGION 4	/* <@ (skyregion, skyregion) */
#define MAX_SUBCAPS 4

typedef struct
{
	double		cx,
				cy,
				cz;
	double		radius;			/* radians; negative marks the empty/unused cap */
} GistCap;

typedef struct
{
	GistCap		overall;		/* union of sub[]; picksplit's axis-sort heuristic only */
	GistCap		sub[MAX_SUBCAPS];
} GistMultiCap;

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

/*
 * Smallest cap (not necessarily minimal, but valid and cheap) covering both
 * inputs.  d + r1 + r2 < pi is assumed for the general branch: a single
 * region's own cap cannot violate that (skyregion forbids a hemisphere-or-
 * larger cone and a polygon spanning one), but repeated unions of far-apart
 * small regions could in principle approach it -- clamped rather than
 * exactly handled, a known limitation (see file header).
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

static inline GistCap
cap_union_caps(GistCap a, GistCap b)
{
	sc_vec3		c;
	double		r;

	cap_union2(cap_center(a), a.radius, cap_center(b), b.radius, &c, &r);
	return cap_make(c, r);
}

/* monotonic in the cap's true area (2*pi*(1-cos(r))); the constant does not
 * matter since penalty/margin only compare differences. */
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
	if (a.radius < 0 || b.radius < 0)
		return false;
	return sc_angle(cap_center(a), cap_center(b)) <= a.radius + b.radius;
}

/* how much two caps overlap, not just whether they do: 0 when disjoint (or
 * merely touching), growing with how far the sum of radii exceeds the
 * centre distance -- a cheap angular proxy, not a real lens-shaped overlap
 * area, in the same spirit as cap_area_proxy() above. */
static double
cap_overlap_amount(GistCap a, GistCap b)
{
	if (a.radius < 0 || b.radius < 0)
		return 0.0;
	return fmax(0.0, a.radius + b.radius - sc_angle(cap_center(a), cap_center(b)));
}

/*
 * Greedy nearest-cluster merge: reduce an arbitrary list of (non-empty) caps
 * down to at most MAX_SUBCAPS, by seeding one cluster per input cap up to
 * MAX_SUBCAPS, then absorbing every further cap into whichever existing
 * cluster it would enlarge least (smallest area(union) - area(cluster) -
 * area(cap) "waste", the same metric Guttman's Quadratic split used for
 * seed selection in the single-cap round one). O(n * MAX_SUBCAPS), not a
 * globally optimal clustering, but cheap enough to run on every union()/
 * penalty() call.
 */
static void
merge_caps_greedy(const GistCap *caps, int n, GistCap *out, int *nout)
{
	int			k = Min(n, MAX_SUBCAPS);

	for (int i = 0; i < k; i++)
		out[i] = caps[i];
	for (int i = k; i < n; i++)
	{
		int			best = 0;
		double		bestWaste = HUGE_VAL;

		for (int j = 0; j < k; j++)
		{
			GistCap		u = cap_union_caps(out[j], caps[i]);
			double		waste = cap_area_proxy(u.radius) - cap_area_proxy(out[j].radius) - cap_area_proxy(caps[i].radius);

			if (waste < bestWaste)
			{
				bestWaste = waste;
				best = j;
			}
		}
		out[best] = cap_union_caps(out[best], caps[i]);
	}
	*nout = k;
	for (int i = k; i < MAX_SUBCAPS; i++)
		out[i].radius = -1;
}

static double
multicap_total_area(const GistMultiCap *m)
{
	double		a = 0;

	for (int i = 0; i < MAX_SUBCAPS; i++)
		if (m->sub[i].radius >= 0)
			a += cap_area_proxy(m->sub[i].radius);
	return a;
}

/*
 * Round nine (reverted) tried to shrink the *stored* key for a plain
 * circle to match pgSphere's own scircle size and measured backwards even
 * under a genuinely cold cache: the CPU cost of reconstructing a decoded
 * key from a variable-length encoding outweighed the I/O saved by the
 * smaller pages, in every regime tested. This is the narrower version of
 * that same idea, scoped to avoid that failure mode entirely: no format
 * change, no decode-time branching, nothing touching union()/penalty()/
 * picksplit() or a single byte of what gets written to disk -- only a
 * fast path inside these two comparison functions, for the case where a
 * key (leaf or internal) has collapsed to a single cap. That happens for
 * every leaf that is a plain circle (region_to_multicap sets sub[0] equal
 * to overall for a cone -- "already exact, a second cap would add
 * nothing", this file's own header) and for any internal node whose
 * merge_caps_greedy union happened to collapse multiple children into
 * one cluster. In that case the overall-cap check just above -- which
 * both functions already had to pay for as their first line -- *is* the
 * exact sub-cap test, since sub[0] and overall are the same cap; the
 * O(MAX_SUBCAPS^2) (or O(MAX_SUBCAPS)) loop below it is redundant work
 * re-deriving an answer already known. This is the same per-comparison
 * cost pgSphere's own scircle_ops pays for a circle and nothing else;
 * skycell now pays that same cost for the same case, instead of the flat
 * sub-cap-loop cost round four's own design notes named as the reason
 * pgSphere's cheap native point-in-shape test pulls further ahead of
 * skycell's than pgSphere's own && does of its own.
 *
 * Sound because sub[] is always packed contiguously from index 0 in this
 * file (region_to_multicap, merge_caps_greedy) -- sub[1] unused implies
 * sub[2]/sub[3] are too, so checking just sub[1] is enough to know there
 * is at most one real sub-cap, without an explicit stored count.
 */
static inline bool
multicap_is_single_cap(const GistMultiCap *m)
{
	return m->sub[1].radius < 0;
}

static bool
multicap_overlaps(const GistMultiCap *a, const GistMultiCap *b)
{
	if (!cap_overlaps(a->overall, b->overall))
		return false;			/* every sub-cap lies within its own overall cap */
	if (multicap_is_single_cap(a) && multicap_is_single_cap(b))
		return true;			/* overall caps overlap, and each *is* its one sub-cap */
	for (int i = 0; i < MAX_SUBCAPS; i++)
	{
		if (a->sub[i].radius < 0)
			continue;
		for (int j = 0; j < MAX_SUBCAPS; j++)
		{
			if (b->sub[j].radius < 0)
				continue;
			if (cap_overlaps(a->sub[i], b->sub[j]))
				return true;
		}
	}
	return false;
}

/*
 * A point can only lie inside the region if it lies inside at least one of
 * the region's sub-caps -- the sub-caps cover the region by construction
 * (moc_for_region()'s decomposition is always a covering, never an
 * approximation that leaves gaps, since it exists elsewhere to bound a
 * region for indexing, not to draw it), so "in no sub-cap" is a sound
 * rejection. Same overall-cap short-circuit as multicap_overlaps(), and
 * the same single-cap fast path (see multicap_is_single_cap()'s comment
 * above multicap_overlaps()).
 */
static bool
multicap_contains_point(const GistMultiCap *m, sc_vec3 p)
{
	GistCap		pc = cap_make(p, 0.0);

	if (!cap_overlaps(m->overall, pc))
		return false;
	if (multicap_is_single_cap(m))
		return true;
	for (int i = 0; i < MAX_SUBCAPS; i++)
	{
		if (m->sub[i].radius < 0)
			continue;
		if (cap_overlaps(m->sub[i], pc))
			return true;
	}
	return false;
}

/* union of N multi-cap keys: overall caps reduce pairwise as before; sub-caps
 * flatten into one list and merge_caps_greedy back down to MAX_SUBCAPS. */
static void
multicap_union_many(const GistMultiCap **entries, int n, GistMultiCap *out)
{
	GistCap		flat[MAX_SUBCAPS * 64];		/* bounded by caller's fanout; see Assert below */
	int			nflat = 0;
	int			nout;

	Assert(n <= 64);
	out->overall = entries[0]->overall;
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
			out->overall = cap_union_caps(out->overall, entries[i]->overall);
		for (int j = 0; j < MAX_SUBCAPS; j++)
			if (entries[i]->sub[j].radius >= 0 && nflat < (int) lengthof(flat))
				flat[nflat++] = entries[i]->sub[j];
	}
	merge_caps_greedy(flat, nflat, out->sub, &nout);
}

/* union of exactly two multi-cap keys -- a thin wrapper so picksplit's
 * incremental sweep (below) can fold one more entry into a running multi-cap
 * at a time, reusing multicap_union_many's own overall-cap and greedy
 * sub-cap merge logic rather than duplicating it. */
static void
multicap_union2(const GistMultiCap *a, const GistMultiCap *b, GistMultiCap *out)
{
	const GistMultiCap *ptrs[2];

	ptrs[0] = a;
	ptrs[1] = b;
	multicap_union_many(ptrs, 2, out);
}

/* how much two multi-caps overlap, not just whether: the sum of every
 * pairwise sub-cap overlap amount (cap_overlap_amount(), the same angular
 * proxy && and picksplit's single-cap version already use), zero when no
 * sub-cap pair overlaps at all. A sum rather than a max: picksplit uses this
 * only to *compare* candidate split points against each other, so what
 * matters is a monotonic proxy for "how much total overlap this split
 * leaves," not a physically exact overlap volume. */
static double
multicap_overlap_amount(const GistMultiCap *a, const GistMultiCap *b)
{
	double		total = 0;

	for (int i = 0; i < MAX_SUBCAPS; i++)
	{
		if (a->sub[i].radius < 0)
			continue;
		for (int j = 0; j < MAX_SUBCAPS; j++)
		{
			if (b->sub[j].radius < 0)
				continue;
			total += cap_overlap_amount(a->sub[i], b->sub[j]);
		}
	}
	return total;
}

static double
multicap_penalty(const GistMultiCap *orig, const GistMultiCap *newc)
{
	GistCap		flat[2 * MAX_SUBCAPS];
	GistCap		merged[MAX_SUBCAPS];
	int			nflat = 0,
				nmerged;

	for (int i = 0; i < MAX_SUBCAPS; i++)
		if (orig->sub[i].radius >= 0)
			flat[nflat++] = orig->sub[i];
	for (int i = 0; i < MAX_SUBCAPS; i++)
		if (newc->sub[i].radius >= 0)
			flat[nflat++] = newc->sub[i];
	merge_caps_greedy(flat, nflat, merged, &nmerged);

	{
		double		mergedArea = 0,
					origArea = multicap_total_area(orig);

		for (int i = 0; i < nmerged; i++)
			mergedArea += cap_area_proxy(merged[i].radius);
		return mergedArea - origArea;
	}
}

/*
 * VARDATA_ANY, not VARDATA: an index tuple GiST hands back here may have
 * been repacked with a 1-byte varlena header (it easily fits under the
 * short-header limit), and VARDATA alone assumes the 4-byte form we
 * ourselves palloc'd it with -- reading through the wrong offset there is
 * exactly the kind of thing that segfaults deep in a page split. memcpy
 * sidesteps any alignment assumption on top of that.
 */
static bytea *
multicap_to_bytea(const GistMultiCap *m)
{
	Size		sz = VARHDRSZ + sizeof(GistMultiCap);
	bytea	   *out = (bytea *) palloc(sz);

	SET_VARSIZE(out, sz);
	memcpy(VARDATA(out), m, sizeof(GistMultiCap));
	return out;
}

static void
bytea_to_multicap(bytea *b, GistMultiCap *m)
{
	memcpy(m, VARDATA_ANY(b), sizeof(GistMultiCap));
}

/*
 * Decompose a region into its GiST key: a cone is already exactly one cap,
 * a polygon is decomposed via the extension's own MOC builder (shared with
 * skycell_region_moc()) into up to MAX_SUBCAPS HEALPix cells.
 */
static void
region_to_multicap(sc_region *r, GistMultiCap *out)
{
	sc_vec3		oc;
	double		orad;

	sc_region_bounding_cap(r, &oc, &orad);
	out->overall = cap_make(oc, orad);

	if (r->kind == SC_REGION_CONE)
	{
		out->sub[0] = out->overall;
		for (int i = 1; i < MAX_SUBCAPS; i++)
			out->sub[i].radius = -1;
	}
	else
	{
		ArrayType  *moc = moc_for_region(r, MAX_SUBCAPS, 20);
		Datum	   *elems;
		bool	   *nulls;
		int			n;

		deconstruct_array(moc, INT8OID, sizeof(int64), true, TYPALIGN_DOUBLE, &elems, &nulls, &n);
		for (int i = 0; i < MAX_SUBCAPS; i++)
		{
			if (i < n)
			{
				int64		pix;
				int			order = sc_nuniq_decode(DatumGetInt64(elems[i]), &pix);

				out->sub[i] = cap_make(sc_pix2vec(order, pix), sc_pixrad(order));
			}
			else
				out->sub[i].radius = -1;
		}
		pfree(elems);
		pfree(nulls);
		pfree(moc);
	}
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
		GistMultiCap m;
		bytea	   *k;

		skycell_region_from_datum(entry->key, &r);
		region_to_multicap(&r, &m);
		sc_region_free(&r);
		k = multicap_to_bytea(&m);

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
	int			n = Min(entryvec->n, 64);	/* matches multicap_union_many's own cap */
	GistMultiCap *mc = palloc(sizeof(GistMultiCap) * n);
	const GistMultiCap **ptrs = palloc(sizeof(GistMultiCap *) * n);
	GistMultiCap out;
	bytea	   *outb;

	for (int i = 0; i < n; i++)
	{
		bytea_to_multicap(DatumGetByteaP(entryvec->vector[i].key), &mc[i]);
		ptrs[i] = &mc[i];
	}
	multicap_union_many(ptrs, n, &out);
	outb = multicap_to_bytea(&out);
	*sizep = VARSIZE(outb);
	pfree(mc);
	pfree(ptrs);
	PG_RETURN_POINTER(outb);
}

PG_FUNCTION_INFO_V1(skyregion_gist_penalty);
Datum
skyregion_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	GistMultiCap morig,
				mnew;

	bytea_to_multicap(DatumGetByteaP(origentry->key), &morig);
	bytea_to_multicap(DatumGetByteaP(newentry->key), &mnew);
	*result = (float) multicap_penalty(&morig, &mnew);
	PG_RETURN_POINTER(result);
}

typedef struct
{
	double		key;			/* overall cap centre's coordinate along the axis being tried */
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
 * R*-tree-style split (Beckmann et al. 1990), adapted to spherical caps.
 * Entries are still *ordered* along a candidate axis by their overall cap
 * centre's x/y/z coordinate (a cheap, single-number sort key -- sub-caps
 * have no one natural per-axis coordinate the way a single cap's centre
 * does), but the *cost* of a candidate split -- both the per-axis margin
 * sum used to choose an axis, and the per-split-point overlap used to
 * choose where to cut on it -- is now computed from the *full* multi-cap
 * union of each side (multicap_union2(), multicap_overlap_amount()), not
 * just the overall caps. This is round five's answer to a question the
 * round three/four file header left open: since the tighter sub-cap
 * representation already showed up as a genuine win when used for
 * consistent()'s pruning, does using it for the split *decision* itself
 * (not just the final left/right keys, which round three already built
 * this way) help further? The running fwdMC[k]/bwdMC[k] arrays below fold
 * one more entry's full multi-cap into the running union per step
 * (O(MAX_SUBCAPS^2), a small constant), keeping the whole sweep O(n) per
 * axis, same complexity class as the overall-cap-only version it replaces.
 * See GIST_REGION_DESIGN.md's "Round five" for whether it actually helped.
 *
 * A useful side effect: since fwdMC[bestM-1]/bwdMC[bestM] are already the
 * exact full multi-cap union of everything picksplit assigned to each side,
 * they're used directly as spl_ldatum/spl_rdatum -- no separate final
 * multicap_union_many() pass needed, unlike the overall-cap-only version.
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
	GistMultiCap *mc = palloc(sizeof(GistMultiCap) * (maxoff + 1));
	int			minfill = Max(1, n * 3 / 10);
	axis_sort_entry *sorted = palloc(sizeof(axis_sort_entry) * n);
	GistMultiCap *fwdMC = palloc(sizeof(GistMultiCap) * n);	/* fwdMC[k]: full multi-cap union of sorted[0..k] */
	GistMultiCap *bwdMC = palloc(sizeof(GistMultiCap) * n);	/* bwdMC[k]: full multi-cap union of sorted[k..n-1] */
	int			bestAxis = 0;
	double		bestAxisMargin = HUGE_VAL;

	for (OffsetNumber i = FirstOffsetNumber; i <= maxoff; i++)
		bytea_to_multicap(DatumGetByteaP(entryvec->vector[i].key), &mc[i]);

	for (int axis = 0; axis < 3; axis++)
	{
		double		marginSum = 0;

		for (int k = 0; k < n; k++)
		{
			OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
			sc_vec3		c = cap_center(mc[idx].overall);

			sorted[k].idx = idx;
			sorted[k].key = (axis == 0) ? c.x : (axis == 1) ? c.y : c.z;
		}
		qsort(sorted, n, sizeof(axis_sort_entry), axis_sort_cmp);

		fwdMC[0] = mc[sorted[0].idx];
		for (int k = 1; k < n; k++)
			multicap_union2(&fwdMC[k - 1], &mc[sorted[k].idx], &fwdMC[k]);
		bwdMC[n - 1] = mc[sorted[n - 1].idx];
		for (int k = n - 2; k >= 0; k--)
			multicap_union2(&bwdMC[k + 1], &mc[sorted[k].idx], &bwdMC[k]);

		/* total sub-cap area, not a single cap's radius, as the "margin" a
		 * multi-cap union has no single linear extent to measure the way one
		 * cap's radius did in the overall-cap-only version this replaces --
		 * area is the closest available proxy for "how big is this side",
		 * used the same way here: to compare axes against each other, not as
		 * an absolute quantity. */
		for (int m = minfill; m <= n - minfill; m++)
			marginSum += multicap_total_area(&fwdMC[m - 1]) + multicap_total_area(&bwdMC[m]);

		if (marginSum < bestAxisMargin)
		{
			bestAxisMargin = marginSum;
			bestAxis = axis;
		}
	}

	/* redo the winning axis's sort + cumulative unions (cheap: one more
	 * O(n) pass, and keeps the loop above simple) */
	for (int k = 0; k < n; k++)
	{
		OffsetNumber idx = (OffsetNumber) (k + FirstOffsetNumber);
		sc_vec3		c = cap_center(mc[idx].overall);

		sorted[k].idx = idx;
		sorted[k].key = (bestAxis == 0) ? c.x : (bestAxis == 1) ? c.y : c.z;
	}
	qsort(sorted, n, sizeof(axis_sort_entry), axis_sort_cmp);
	fwdMC[0] = mc[sorted[0].idx];
	for (int k = 1; k < n; k++)
		multicap_union2(&fwdMC[k - 1], &mc[sorted[k].idx], &fwdMC[k]);
	bwdMC[n - 1] = mc[sorted[n - 1].idx];
	for (int k = n - 2; k >= 0; k--)
		multicap_union2(&bwdMC[k + 1], &mc[sorted[k].idx], &bwdMC[k]);

	{
		int			bestM = minfill;
		double		bestOverlap = HUGE_VAL;
		double		bestArea = HUGE_VAL;

		for (int m = minfill; m <= n - minfill; m++)
		{
			double		overlap = multicap_overlap_amount(&fwdMC[m - 1], &bwdMC[m]);
			double		area = multicap_total_area(&fwdMC[m - 1]) + multicap_total_area(&bwdMC[m]);

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

		v->spl_ldatum = PointerGetDatum(multicap_to_bytea(&fwdMC[bestM - 1]));
		v->spl_rdatum = PointerGetDatum(multicap_to_bytea(&bwdMC[bestM]));
	}

	pfree(mc);
	pfree(sorted);
	pfree(fwdMC);
	pfree(bwdMC);
	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(skyregion_gist_same);
Datum
skyregion_gist_same(PG_FUNCTION_ARGS)
{
	bytea	   *a = PG_GETARG_BYTEA_P(0);
	bytea	   *b = PG_GETARG_BYTEA_P(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	GistMultiCap ma,
				mb;

	/* a full-byte comparison of the (fixed-size, deterministically packed)
	 * key is the safe choice: a false "not same" just misses a rare
	 * optimisation, a false "same" would be a real bug. */
	bytea_to_multicap(a, &ma);
	bytea_to_multicap(b, &mb);
	*result = (memcmp(&ma, &mb, sizeof(GistMultiCap)) == 0);
	PG_RETURN_POINTER(result);
}

/*
 * consistent() is called once per index entry visited during a scan -- for
 * one probe row's index scan that can easily be dozens of internal-page
 * entries plus every matching leaf, all with the *same* query argument (the
 * one outer row's region, unchanged for the scan's lifetime). Re-parsing
 * that region and recomputing its multi-cap key on every single call is
 * wasted work worth caching in fn_extra, the same pattern skycell.c's
 * cone/poly/hist caches already use elsewhere in this extension -- but
 * keyed on the query Datum's own *bytes*, not its pointer: in a join (this
 * opclass's main use case), each outer row gets a fresh per-tuple memory
 * context that is reset and reused for the next row, so two genuinely
 * different rows' region values can legitimately land at the same address.
 * A pointer-identity cache took that as "same value, skip recomputing" and
 * silently reused a stale, wrong key -- consistent() returning a false "no
 * overlap" from it prunes a subtree outright, with no recheck to catch it
 * afterwards (unlike a false positive, which recheck still filters).
 * Caught by the large self-join correctness test after this cache first
 * went in (round one of this opclass, before the multi-cap redesign): 61 of
 * 64 true matches missing, 0 spurious ones -- exactly what a stale cache
 * causing false pruning looks like, not a geometry bug. See
 * GIST_REGION_DESIGN.md for the full story.
 */
typedef struct
{
	bytea	   *last_query;		/* palloc'd copy in fn_mcxt, or NULL */
	Size		last_query_size;
	GistMultiCap qmc;
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
	GistMultiCap em;
	bool		result;

	bytea_to_multicap(DatumGetByteaP(entry->key), &em);

	switch (strategy)
	{
		case GIST_REGION_STRATEGY_OVERLAP:
		case GIST_REGION_STRATEGY_CONTAINS_REGION:
		case GIST_REGION_STRATEGY_CONTAINED_BY_REGION:
			{
				/* the query is a skyregion (bytea-backed) for all three of
				 * these strategies -- STRATEGY_CONTAINS_POINT's query is a
				 * skypos instead, a fixed-size by-reference struct with no
				 * varlena header at all, so DatumGetByteaP/VARSIZE on it
				 * would misread its raw bytes as a (possibly toasted)
				 * varlena, corrupting whatever that garbage "pointer"
				 * happens to land on. Computed here, not above the switch,
				 * so it only ever runs for the strategies it's valid for.
				 *
				 * CONTAINS_REGION (round six) reuses OVERLAP's own test
				 * rather than a tighter one: "A covers B" (B nonempty)
				 * implies "A and B overlap", so CoverA and CoverB (each a
				 * superset of its region, by the same covering guarantee
				 * OVERLAP and CONTAINS_POINT already rely on) must overlap
				 * too whenever A actually covers B -- a valid necessary
				 * condition, sound to prune on. A tighter test -- e.g.
				 * requiring every sub-cap of B to sit inside a single
				 * sub-cap of A -- would be a SUFFICIENT condition for
				 * containment, not a necessary one, so it cannot be used to
				 * prune (a query region straddling two adjacent sub-caps of
				 * a genuinely containing A would be wrongly rejected: a
				 * false negative, unlike a spurious candidate that recheck
				 * still catches). This costs pruning precision -- rows that
				 * merely overlap the query region pass through to recheck
				 * alongside rows that truly contain it -- but is the same
				 * bbox-then-recheck shape PostGIS's own GiST opclass uses
				 * for ST_Contains, and still prunes every row whose cover
				 * doesn't even touch the query region, which is most rows
				 * on a real catalogue. See bench/24_region_contains_region.sql
				 * for the measured selectivity and speed this buys over
				 * the sequential scan it replaces.
				 *
				 * CONTAINED_BY_REGION (round seven) is CONTAINS_REGION's
				 * mirror: "is the indexed row's region wholly contained
				 * *within* this query region" (<@, not @>), the direction
				 * round six's own opclass comment flagged as needing its
				 * own operator and strategy rather than a different query
				 * spelling. The necessary condition is exactly the same
				 * overlap test, unchanged: "B contains A" implies "A and B
				 * overlap" regardless of which of A/B is the indexed row and
				 * which is the query, so this needs no new pruning logic at
				 * all -- only a new strategy number for the planner to
				 * dispatch a differently-named operator to, and a different
				 * exact function (skycell_region_covered_by) for recheck to
				 * fall back to, which GiST's own recheck machinery already
				 * handles by calling whatever operator the query actually
				 * used. */
				bytea	   *qb = DatumGetByteaP(queryDatum);
				Size		qsz = VARSIZE(qb);

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
					region_to_multicap(&qreg, &qcache->qmc);
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
				result = multicap_overlaps(&em, &qcache->qmc);
				*recheck = true;	/* the cap test is lossy either way */
			}
			break;
		case GIST_REGION_STRATEGY_CONTAINS_POINT:
			/* a skypos is a fixed-size by-reference Datum, not a region --
			 * no fn_extra caching needed here (unlike the OVERLAP case
			 * above): extracting its unit vector is two doubles and a bit
			 * of trig, far cheaper than the region-parsing + MOC
			 * decomposition that OVERLAP's cache exists to avoid repeating. */
			result = multicap_contains_point(&em, skycell_pos_from_datum(queryDatum));
			*recheck = true;	/* sub-caps cover the region; recheck confirms
								 * the point is actually inside its true shape */
			break;
		default:
			elog(ERROR, "skyregion_gist_consistent: unsupported strategy %d", strategy);
			result = false;	/* unreachable */
	}
	PG_RETURN_BOOL(result);
}
