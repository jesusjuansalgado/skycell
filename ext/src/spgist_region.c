/*
 * spgist_region.c -- an SP-GiST opclass for skypos, indexing the *point*
 * catalogue directly against skypos <@ skyregion (round: "SP-GiST over the
 * HEALPix hierarchy", the direction paper/response-to-referee-2.md and
 * bench/tap-ab/RESULTS.md already flagged as future work for a different
 * reason -- removing the OR-ed-B-tree-ranges planning cost -- tried here for
 * both that reason and this session's own GiST fanout wall).
 *
 * Unlike gist_region.c (which indexes the *region* column, for "many
 * candidate regions, one point" queries), this indexes the *point* column,
 * for "one query region, many candidate points" -- cone search and
 * cross-match, skycell's actual headline workload, currently answered by a
 * planner rewrite into OR-ed B-tree ranges (skycell.c/adql.c's
 * SupportRequestSimplify machinery), not a real index descent.
 *
 * TREE SHAPE: the index tree is a PATRICIA trie over the HEALPix NESTED
 * pixel hierarchy -- one inner tuple per branch point, not one per order.
 * Every inner tuple carries an explicit prefix, (order, pix): "every leaf
 * under this tuple is known to lie in HEALPix pixel `pix` at `order`" (order
 * -1, pix irrelevant, for the very first tuple ever created). The tuple's
 * own nodes then decide the NEXT order's digit: up to 12 nodes if order is
 * -1 (order 0 has exactly 12 pixels, the label *is* the pixel id), else up
 * to 4 (the label is the 2-bit digit extending pix from `order` to
 * `order+1`). A prefix can and normally does span more than one order at a
 * time -- see PICKSPLIT below for why that is required, not just an
 * optimisation.
 *
 * Beyond SC_MAX_ORDER (29, sub-mas precision) no finer order exists, so a
 * batch that still hasn't diverged there is a real duplicate/near-duplicate
 * cluster: picksplit reports one node and the SP-GiST core marks the tuple
 * "all-the-same", which is the correct, framework-documented use of that
 * mechanism (a bucket the index genuinely cannot resolve further).
 *
 * WHY A PLAIN "ONE ORDER PER LEVEL, NO PREFIX" TREE IS UNSOUND (not just
 * slower) for this opclass, found empirically before any of this was
 * reasoned out: a physically HEALPix-sorted table missed 2 of 3 true
 * matches on a cone query that an unsorted build of the identical rows
 * answered correctly. SP-GiST's own core marks a freshly created inner
 * tuple "all-the-same" whenever picksplit reports only one node, and once
 * marked, the core refuses to let choose() add a new node to it (spgAddNode
 * against an all-the-same tuple is a hard error). A *fixed* one-order-at-a-
 * time split cannot avoid reporting "one node" for batches that are not
 * genuinely indistinguishable: a small overflow batch at some deep order is
 * often locally homogeneous (one digit) purely by chance of insertion order
 * or a dense cluster, not because every point that could ever land there
 * shares that digit -- and once that happens, a later, genuinely different
 * point has no legal way to be routed correctly.
 *
 * The general fix is the standard PATRICIA-trie discipline the built-in
 * inet_ops spgist opclass uses for address bits: picksplit always finds a
 * batch's true first point of disagreement, however deep that is (a
 * longest-common-prefix search -- see PICKSPLIT below), so "one node" is
 * only reported when a batch is genuinely identical all the way to
 * SC_MAX_ORDER, and choose() can split a tuple's own prefix (spgSplitTuple)
 * when a later point's true ancestor diverges from it before reaching its
 * decision order.
 *
 * That still isn't the whole story. Even with a fully correct picksplit,
 * the SP-GiST core will, on its own, synthesise a placeholder one-node
 * inner tuple -- also marked all-the-same, also refusing spgAddNode -- to
 * hold a freshly spgAddNode-created node's first leaf(ves), without ever
 * calling this opclass's picksplit. This is invisible at small scale (an
 * empty node rarely accumulates enough leaves to matter before the whole
 * page splits some other way) and was only caught at 10M rows, where dense
 * clusters make it common: elog tracing showed such a tuple with 8 node
 * slots, all labelled 0, a shape this opclass's own picksplit can never
 * produce (it only ever emits nodes with genuinely distinct labels). Since
 * neither its (order, pix) prefix nor its node labels can be trusted in
 * this case, the only universally safe response -- covering both this and
 * the genuine SC_MAX_ORDER-exhausted case identically -- is the one the
 * access method's own documentation describes: whenever in->allTheSame is
 * set, match the (single) existing node unconditionally and trust that the
 * next real picksplit call on that bucket, now a true from-scratch
 * longest-common-prefix search rather than a per-level guess, will untangle
 * whatever accumulated there. choose() and inner_consistent() both check
 * in->allTheSame before touching the prefix or node labels at all, ahead of
 * every other branch.
 *
 * PICKSPLIT: given a batch of points that must be organised under a new
 * inner tuple, always start the search at order 0 -- never at in->level,
 * which is a hop count, not an order, and the two permanently diverge once
 * spgSplitTuple is in play (a split inserts an extra hop whose own decision
 * can land at a much shallower common order than one hop's worth of
 * progress would suggest, so hop count can run ahead of, or fall behind,
 * the batch's true established order after enough splits -- both
 * directions were tried and both produced this same "cannot add a node"
 * failure, at large enough scale for the drift to matter). Compute each
 * point's digit at the current candidate order; if they all agree, that
 * digit is common to the whole batch and gives no information, so bump the
 * order and try again, until either two points disagree (the real branch
 * point) or SC_MAX_ORDER is exhausted (true duplicates, one node,
 * all-the-same is correct). A full from-scratch search costs at most
 * SC_MAX_ORDER+1 harmless re-checks of orders every ancestor already
 * confirmed the batch agrees on -- negligible next to one page read -- and
 * is the only version of this immune to however in->level has drifted.
 *
 * CHOOSE: reads the current tuple's own (order, pix) prefix directly
 * (never from in->level, for the same reason picksplit no longer does) and
 * checks the incoming point's ancestor at that prefix against the stored
 * pix. A match proceeds exactly like the fixed-order design (compute the
 * next digit, look up or add a node -- always legal here, since every
 * non-all-the-same tuple was created with genuinely distinct labels). A
 * mismatch means this point's true ancestor diverges from the prefix
 * somewhere shallower than expected: find the deepest order the two still
 * agree on and issue spgSplitTuple, inserting a new, shorter-prefixed tuple
 * above the existing one at exactly that point. Its one real subtlety: the
 * new upper tuple must be given *both* labels atomically (the old content's
 * digit and this new point's own, necessarily different one) rather than
 * created with just the old one and a plan to spgAddNode the new one on the
 * re-invocation choose() gets right after -- a fresh nNodes==1 tuple is
 * all-the-same whether picksplit or spgSplitTuple created it, so that
 * second call would hit the exact same wall this whole design works around.
 *
 * PRUNING (inner_consistent): each candidate child's (order, pix) is
 * classified against the query region with the exact same sc_region_classify
 * cover.c already uses for the B-tree covering and the GiST region opclass --
 * SC_OUT children are not descended into at all. Skipping several orders in
 * one prefix hop only means classify() is not separately consulted at the
 * skipped, intermediate orders; since each is a superset of the next, that
 * costs some pruning opportunity, never correctness.
 *
 * EXACTNESS (leaf_consistent): every candidate leaf point gets the same
 * exact sc_region_contains() test skycell_pos_in_region() uses -- recheck is
 * always false, this opclass is exact, not lossy (unlike gist_region.c's
 * multi-cap key, which is intentionally an approximation with recheck=true).
 */
#include "postgres.h"

#include "access/spgist.h"
#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include "cover.h"
#include "healpix.h"
#include "skycell_internal.h"

/*
 * A prefix is (order, width, pix): "every leaf under this tuple is known to
 * lie in HEALPix pixel `pix` at `order`" (order -1, pix irrelevant, for the
 * very first tuple ever created; order TERMINAL_ORDER, one past
 * SC_MAX_ORDER, for a genuinely exhausted all-the-same tuple), and this
 * tuple's own nodes decide the next `width` orders as one combined digit
 * (see WIDE-RADIX SPLITTING below). `pix` is the plain, full HEALPix NESTED
 * pixel id sc_vec2pix() itself returns -- face included -- so classify()
 * can use it directly with no reconstruction.
 *
 * This needs more than one int8's worth of bits: a NESTED id can carry a
 * face value up to 11 in bits above its 2*order interleaved position bits,
 * so a full id at order 29 needs up to 2*29+4 = 62 bits on its own, before
 * `order` and `width` even get a look-in -- no single int8 field holds
 * (order, width, pix) together for every order this opclass reaches. A
 * previous version of this file packed (order, pix) into one int8 with
 * pix capped at 58 bits; that silently truncated real pixel ids at
 * order >= 28, a latent bug never triggered because no reproduction used
 * before it was replaced ever built a tree that deep. Rather than continue
 * squeezing bits, the prefix is a small bytea holding the three fields
 * verbatim (SpgPrefixData below) -- a few bytes bigger per inner tuple, but
 * with no ceiling to silently blow through.
 */
typedef struct SpgPrefixData
{
	int32		order;
	int32		width;
	int64		pix;
} SpgPrefixData;

#define TERMINAL_ORDER	 (SC_MAX_ORDER + 1)

static inline Datum
pack_prefix(int32 order, int32 width, int64 pix)
{
	bytea	   *b = (bytea *) palloc(VARHDRSZ + sizeof(SpgPrefixData));
	SpgPrefixData pd;

	pd.order = order;
	pd.width = width;
	pd.pix = pix;
	SET_VARSIZE(b, VARHDRSZ + sizeof(SpgPrefixData));
	memcpy(VARDATA(b), &pd, sizeof(SpgPrefixData));
	return PointerGetDatum(b);
}

static inline void
unpack_prefix(Datum d, int32 *order, int32 *width, int64 *pix)
{
	bytea	   *b = DatumGetByteaPP(d);
	SpgPrefixData pd;

	/* memcpy rather than casting VARDATA_ANY: nothing guarantees the int64
	 * inside lands 8-byte aligned in memory. */
	memcpy(&pd, VARDATA_ANY(b), sizeof(SpgPrefixData));
	*order = pd.order;
	*width = pd.width;
	*pix = pd.pix;
}

/*
 * WIDE-RADIX SPLITTING (tried, measured, not kept -- SPLIT_WIDTH is 1
 * below): a node's decision does not have to be one HEALPix order (a 2-bit,
 * up-to-4-way digit) at a time. Combining several consecutive orders into
 * one combined digit (up to 4^width-way) makes each inner tuple decide that
 * many orders at once, which does shrink tree depth by roughly `width`, on
 * the theory that fewer, wider levels means fewer page reads per descent --
 * classify() rejecting the extra candidates is pure in-memory trigonometry,
 * no I/O, unlike round nine's variable-length GiST key where the added cost
 * was itself on the critical path.
 *
 * Measured at 10M rows with SPLIT_WIDTH=3 against the same cone-search
 * harness round eleven used (EXPLAIN (ANALYZE, BUFFERS)), buffer touches
 * came out *higher* than SPLIT_WIDTH=1, and increasingly so as the query
 * area grew: 15.6 vs 14.5 at 1", 15.9 vs 14.7 at 1', 99.5 vs 47.1 at 30'
 * (2.1x), 278.1 vs 98.8 at 1deg (2.8x), 1625.5 vs 472.3 at 3deg (3.4x) --
 * confirmed to be wide-radix's own effect, not the switch to a bytea prefix
 * (rebuilding the SAME bytea-prefix code with SPLIT_WIDTH=1 reproduced
 * round eleven's int8-prefix buffer counts almost exactly). The likely
 * cause: a PATRICIA trie's own branching already tracks exactly where the
 * data disagrees, at whatever order that happens to be; forcing every
 * decision to cover `width` orders regardless inflates picksplit's node
 * count (and thus the index's node/page count) past what the data actually
 * needs, and that extra breadth costs more, for boundary-crossing range
 * queries, than the shallower depth saves -- growing with query area
 * because a bigger query crosses more of that inflated boundary. Left at
 * SPLIT_WIDTH=1 (effectively single-order splits, the pre-experiment
 * behaviour) rather than removing the mechanism entirely, since the
 * explicit-width-storage fix below (SpgPrefixData.width) is worth keeping
 * regardless of SPLIT_WIDTH -- it's what makes wide splitting *safe* to
 * revisit, should a smarter version (e.g. width chosen per-tuple from
 * local density rather than one constant) turn out to pay off later.
 *
 * split_width() below is only ever used to pick the *requested* width for a
 * genuinely new, uncapped decision (picksplit's own new tuple). It is NOT a
 * substitute for storing the width actually used: spgSplitTuple's upper
 * tuple can be forced narrower than this by the old tuple's own established
 * order (see choose() below), and once a tuple exists, whatever width it
 * was actually built with is written into its prefix (SpgPrefixData.width
 * above) and read back from there by every later choose()/inner_consistent()
 * call on it -- never re-derived from `order` alone. An earlier version of
 * this file tried exactly that re-derivation ("choose() has nowhere else to
 * learn it, only the tuple's own base order, so split_width() is the single
 * deterministic source of truth") and it was wrong: a capped split silently
 * produces a tuple whose real width doesn't match what split_width(order)
 * would predict, so reads using that prediction compute the wrong digit
 * span and miss real matches. Caught at 50,000 rows via a sorted-insertion
 * stress test (2869 false negatives) before this file ever claimed the
 * technique worked.
 */
#define SPLIT_WIDTH 1
#define MAX_NODES 64			/* max(12, 4^SPLIT_WIDTH); bump with SPLIT_WIDTH */

static inline int
split_width(int32 base_order)
{
	return (base_order < 0) ? 1 : SPLIT_WIDTH;
}

/* the combined digit spanning orders [from_order, to_order], computed fresh
 * from a point's real coordinates */
static inline int32
wide_digit(int from_order, int to_order, sc_vec3 v)
{
	int64		pix = sc_vec2pix(to_order, v);

	if (from_order == 0)
		return (int32) pix;	/* order 0's full value already includes the
								 * face, nothing to mask off */
	return (int32) (pix & ((INT64CONST(1) << (2 * (to_order - from_order + 1))) - 1));
}

/* the same combined digit, extracted from a pix value already known at
 * stored_order (>= to_order) rather than recomputed from a point */
static inline int32
wide_digit_from_pix(int64 full_pix, int stored_order, int from_order, int to_order)
{
	int64		shifted = full_pix >> (2 * (stored_order - to_order));

	if (from_order == 0)
		return (int32) shifted;
	return (int32) (shifted & ((INT64CONST(1) << (2 * (to_order - from_order + 1))) - 1));
}

PG_FUNCTION_INFO_V1(spg_healpix_config);
Datum
spg_healpix_config(PG_FUNCTION_ARGS)
{
	spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0);
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BYTEAOID;
	cfg->labelType = INT2OID;
	cfg->leafType = cfgin->attType;
	cfg->canReturnData = false;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(spg_healpix_choose);
Datum
spg_healpix_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	sc_vec3		v;
	int32		order;
	int32		width;
	int64		pix;
	int16		label;

	/*
	 * allTheSame covers two cases, and neither one's (order, pix, node
	 * labels) can be trusted: the genuine one this opclass asks for
	 * (picksplit's own SC_MAX_ORDER-exhausted branch, real duplicates,
	 * order packed as TERMINAL_ORDER) and one this opclass never asked
	 * for -- observed directly (elog tracing) at 10M rows: a freshly
	 * spgAddNode-created node's first leaf(ves), represented as a
	 * placeholder inner tuple the core synthesises on its own, marked
	 * allTheSame by default with node labels that do not reflect real
	 * content (seen as literally 8 copies of label 0). Trying to read a
	 * digit out of this tuple's own prefix, or search its node labels for
	 * a match, is meaningless in the second case; spgAddNode against it is
	 * a hard error in both. The only universally safe move is the
	 * documented one: match whatever single node is there and trust
	 * picksplit -- now a real, from-scratch longest-common-prefix search,
	 * not a per-level guess -- to untangle any resulting mixture the next
	 * time that bucket actually overflows.
	 */
	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = 0;
		out->result.matchNode.levelAdd = 1;
		out->result.matchNode.restDatum = in->leafDatum;
		PG_RETURN_VOID();
	}

	Assert(in->hasPrefix);
	unpack_prefix(in->prefixDatum, &order, &width, &pix);
	v = skycell_pos_from_datum(in->datum);

	/* this tuple's own prefix order must be < SC_MAX_ORDER (see picksplit's
	 * from-scratch-search comment) so order+1 below stays in-bounds. */
	Assert(order < SC_MAX_ORDER);

	if (order >= 0 && sc_vec2pix(order, v) != pix)
	{
		/*
		 * This point's true ancestor diverges from the tuple's prefix
		 * before reaching its decision order: split the prefix at the
		 * deepest order where they still agree (possibly -1, i.e. no
		 * agreement at all beyond "somewhere on the sphere").
		 */
		int32		common_order;
		int64		common_pix = 0;
		int32		upper_width;
		int32		upper_to_order;
		int16		old_label;
		int16		new_label;

		for (common_order = order - 1; common_order >= 0; common_order--)
		{
			int64		want = pix >> (2 * (order - common_order));

			if (sc_vec2pix(common_order, v) == want)
			{
				common_pix = want;
				break;
			}
		}
		/* common_order is now the deepest match, or -1 if none */

		/*
		 * The new upper tuple must carry BOTH labels from the start (the
		 * old content's digit, and this new point's own, necessarily
		 * different one -- that's what "common_order" means) rather than
		 * just the old one with a plan to spgAddNode the new one on the
		 * re-invocation choose() gets right after a split: the framework
		 * marks *any* freshly created nNodes==1 inner tuple all-the-same,
		 * spgSplitTuple-created ones included, and then refuses spgAddNode
		 * against it exactly as it would a picksplit-created singleton --
		 * caught at 10M rows, where dense clusters make this path common
		 * enough to hit (a smaller/sparser test can pass for a long time
		 * without ever exercising a real split at all).
		 *
		 * Its requested width follows the same rule every other freshly
		 * created tuple uses (split_width(common_order)), but gets capped by
		 * `order`: the old content's digits beyond its own established order
		 * live in ITS nodes, not in this prefix, so this tuple cannot decide
		 * anything deeper than `order` either. Whatever width actually
		 * results from that cap is what gets stored -- never re-derived
		 * later from `common_order` alone (see the wide-radix comment above
		 * split_width()).
		 */
		upper_width = Min(split_width(common_order), order - common_order);
		upper_to_order = common_order + upper_width;
		old_label = (int16) wide_digit_from_pix(pix, order, common_order + 1, upper_to_order);
		new_label = (int16) wide_digit(common_order + 1, upper_to_order, v);
		Assert(new_label != old_label);

		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum =
			pack_prefix(common_order, upper_width, common_pix);
		out->result.splitTuple.prefixNNodes = 2;
		out->result.splitTuple.prefixNodeLabels = (Datum *) palloc(sizeof(Datum) * 2);
		out->result.splitTuple.prefixNodeLabels[0] = Int16GetDatum(old_label);
		out->result.splitTuple.prefixNodeLabels[1] = Int16GetDatum(new_label);
		out->result.splitTuple.childNodeN = 0;
		out->result.splitTuple.postfixHasPrefix = true;
		/* the old tuple's content, position, and width are all unchanged --
		 * only its place in the tree gains a new parent -- so reuse its
		 * prefix datum verbatim rather than repacking it. */
		out->result.splitTuple.postfixPrefixDatum = in->prefixDatum;
		PG_RETURN_VOID();
	}

	{
		/*
		 * `width` here is this tuple's OWN stored width, read at function
		 * entry -- not split_width(order). It was fixed when this tuple was
		 * created (by picksplit or by the spgSplitTuple branch above, in
		 * either this call or an earlier one) and already reflects any
		 * capping, so order+width is guaranteed <= SC_MAX_ORDER with no
		 * further clamping needed here.
		 */
		int32		to_order = order + width;

		label = (int16) wide_digit(order + 1, to_order, v);
	}

	for (int i = 0; i < in->nNodes; i++)
	{
		if (DatumGetInt16(in->nodeLabels[i]) == label)
		{
			out->resultType = spgMatchNode;
			out->result.matchNode.nodeN = i;
			out->result.matchNode.levelAdd = 1;
			out->result.matchNode.restDatum = in->leafDatum;
			PG_RETURN_VOID();
		}
	}

	/*
	 * Not found: legal here because the allTheSame check above has already
	 * returned for every tuple the core could refuse spgAddNode against --
	 * anything reaching this line has nNodes >= 2 (a real picksplit or
	 * spgSplitTuple decision, both of which only ever create tuples whose
	 * batch actually disagreed) or is a core-synthesised placeholder that
	 * was already handled by matching, not falling through to here.
	 */
	out->resultType = spgAddNode;
	out->result.addNode.nodeLabel = Int16GetDatum(label);
	out->result.addNode.nodeN = in->nNodes;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(spg_healpix_picksplit);
Datum
spg_healpix_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	/*
	 * Always start the longest-common-prefix search from scratch at order 0,
	 * never from in->level. in->level is a hop count, not an order, and
	 * spgSplitTuple makes the two permanently diverge: a split inserts an
	 * extra tree level (+1 hop) whose own decision can land at a much
	 * shallower common order than 1 real order's worth of progress, so hop
	 * count can run ahead of the true established order after enough
	 * splits -- both directions were tried and both are real bugs, not just
	 * a performance nuance: clamping in->level to SC_MAX_ORDER stopped an
	 * out-of-bounds sc_vec2pix() call (hop count overshooting order) but
	 * left the opposite case -- hop count *undershooting*, so this search
	 * starts already past an order the batch actually disagrees on -- to
	 * silently manufacture a bogus "all identical" tuple no real duplicate
	 * data justified, which is exactly what later collides with a genuinely
	 * different point routed there by an ancestor and produces "cannot add
	 * a node to an all-the-same inner tuple". A full from-scratch search
	 * costs at most SC_MAX_ORDER+1 harmless re-checks of orders every
	 * ancestor already confirmed the batch agrees on -- negligible next to
	 * one page read -- and is the only version of this that cannot be
	 * fooled by however in->level has drifted.
	 */
	int32		candidate_order = 0;
	int16	   *labels;
	int16		first_label;
	bool		diverged = false;

	Assert(in->nTuples > 0);
	labels = (int16 *) palloc(sizeof(int16) * in->nTuples);

	for (;;)
	{
		sc_vec3		v0 = skycell_pos_from_datum(in->datums[0]);

		first_label = (int16) wide_digit(candidate_order, candidate_order, v0);
		labels[0] = first_label;
		diverged = false;
		for (int i = 1; i < in->nTuples; i++)
		{
			sc_vec3		v = skycell_pos_from_datum(in->datums[i]);

			labels[i] = (int16) wide_digit(candidate_order, candidate_order, v);
			if (labels[i] != first_label)
				diverged = true;
		}
		if (diverged || candidate_order >= SC_MAX_ORDER)
			break;
		candidate_order++;
	}

	out->leafTupleDatums = (Datum *) palloc(sizeof(Datum) * in->nTuples);
	for (int i = 0; i < in->nTuples; i++)
		out->leafTupleDatums[i] = in->datums[i];

	if (!diverged)
	{
		/* every point agrees all the way to SC_MAX_ORDER: real duplicates */
		out->hasPrefix = true;
		out->prefixDatum = pack_prefix(TERMINAL_ORDER, 0, 0);
		out->nNodes = 1;
		out->nodeLabels = (Datum *) palloc(sizeof(Datum));
		out->nodeLabels[0] = Int16GetDatum(0);
		out->mapTuplesToNodes = (int *) palloc0(sizeof(int) * in->nTuples);
		pfree(labels);
		PG_RETURN_VOID();
	}

	{
		sc_vec3		v0 = skycell_pos_from_datum(in->datums[0]);
		int32		base_order = candidate_order - 1;
		int64		ancestor_pix = (base_order >= 0)
			? sc_vec2pix(base_order, v0) : 0;
		int32		nominal_width = split_width(base_order);
		int32		to_order = Min(candidate_order + nominal_width - 1, SC_MAX_ORDER);
		int32		actual_width = to_order - base_order;
		int16		distinct[MAX_NODES];
		int			ndistinct = 0;

		/*
		 * labels[] currently holds the single-order digit at candidate_order
		 * (just enough to find where the batch disagrees, above); the
		 * actual node labels combine every order from there out to
		 * to_order (see the wide-radix comment above choose()), so
		 * recompute them now that the span is known.
		 */
		for (int i = 0; i < in->nTuples; i++)
		{
			sc_vec3		v = skycell_pos_from_datum(in->datums[i]);

			labels[i] = (int16) wide_digit(candidate_order, to_order, v);
		}

		out->hasPrefix = true;
		out->prefixDatum = pack_prefix(base_order, actual_width, ancestor_pix);

		out->mapTuplesToNodes = (int *) palloc(sizeof(int) * in->nTuples);
		for (int i = 0; i < in->nTuples; i++)
		{
			int			nodeidx = -1;

			for (int j = 0; j < ndistinct; j++)
			{
				if (distinct[j] == labels[i])
				{
					nodeidx = j;
					break;
				}
			}
			if (nodeidx < 0)
			{
				Assert(ndistinct < MAX_NODES);
				nodeidx = ndistinct;
				distinct[ndistinct++] = labels[i];
			}
			out->mapTuplesToNodes[i] = nodeidx;
		}

		out->nNodes = ndistinct;
		out->nodeLabels = (Datum *) palloc(sizeof(Datum) * ndistinct);
		for (int j = 0; j < ndistinct; j++)
			out->nodeLabels[j] = Int16GetDatum(distinct[j]);
	}

	pfree(labels);
	PG_RETURN_VOID();
}

/*
 * inner_consistent()/leaf_consistent() are each called once per node/leaf
 * visited during a single index descent -- dozens of times per query, all
 * with the *same* query argument (one scan's skyregion, unchanged for the
 * scan's lifetime) -- yet each was re-parsing it from scratch on every call
 * via skycell_region_from_datum(), the same wasted-work shape gist_region.c's
 * consistent() found and fixed for the region GiST opclass (round one of
 * that opclass; see GIST_REGION_DESIGN.md). Here it's worse than a parse:
 * sc_region_cone()/sc_region_poly() redo sc_radec2vec()/sc_ang2pix() (a
 * cone) or the full vertex/area/sin_rho[] setup (a polygon) from scratch,
 * at every one of a query's node visits rather than once per scan -- a
 * cone's out_c2[]/in_c2[] are filled lazily, order by order, so a fresh,
 * uncached reconstruction also throws away whatever this descent's earlier
 * node visits had already filled, paying for the same order's two sin()/pow()
 * calls again at the next visit instead of once for the whole scan.
 * Cached here in fn_extra, the same value-based cache-key discipline
 * gist_region.c's own fix established: keyed on the query Datum's *bytes*,
 * not pointer identity, because a join's per-tuple memory context is reset
 * and reused between outer rows, so two different rows' regions can
 * legitimately land at the same address -- a pointer-identity cache would
 * silently reuse a stale, wrong region and prune (inner_consistent) or
 * reject (leaf_consistent) incorrectly, with nothing downstream to catch
 * either mistake (this opclass has no recheck to fall back on at all).
 * inner_consistent() and leaf_consistent() are separate catalog functions,
 * each with their own fn_extra slot, so each gets its own cache instance;
 * they are never called from the same fcinfo.
 */
typedef struct
{
	bytea	   *last_query;		/* palloc'd copy in fn_mcxt, or NULL */
	Size		last_query_size;
	sc_region	reg;			/* reg.v/reg.n, if set, also live in fn_mcxt */
}			spg_region_query_cache;

static sc_region *
spg_cached_region(FunctionCallInfo fcinfo, Datum queryDatum)
{
	spg_region_query_cache *qcache = (spg_region_query_cache *) fcinfo->flinfo->fn_extra;
	bytea	   *qb = DatumGetByteaP(queryDatum);
	Size		qsz = VARSIZE(qb);

	if (qcache == NULL)
	{
		qcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(spg_region_query_cache));
		fcinfo->flinfo->fn_extra = qcache;
	}
	if (qcache->last_query == NULL || qcache->last_query_size != qsz ||
		memcmp(qcache->last_query, qb, qsz) != 0)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		if (qcache->last_query != NULL)
			sc_region_free(&qcache->reg);	/* frees the old poly v[]/n[], if any */
		skycell_region_from_datum(queryDatum, &qcache->reg);
		MemoryContextSwitchTo(oldcxt);

		if (qcache->last_query == NULL || qcache->last_query_size < qsz)
		{
			if (qcache->last_query != NULL)
				pfree(qcache->last_query);
			qcache->last_query = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, qsz);
		}
		memcpy(qcache->last_query, qb, qsz);
		qcache->last_query_size = qsz;
	}
	return &qcache->reg;
}

PG_FUNCTION_INFO_V1(spg_healpix_inner_consistent);
Datum
spg_healpix_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int32		order;
	int32		width;
	int64		pix;
	bool		terminal;
	sc_region  *reg = NULL;
	int		   *keep;
	int			nkeep = 0;

	/*
	 * allTheSame tuples' (order, pix, node labels) cannot be trusted -- see
	 * choose()'s comment on the same point -- so treat them exactly like
	 * this opclass's own genuine SC_MAX_ORDER-exhausted case: visit every
	 * node, unconditionally, with no classify()-based pruning.
	 */
	Assert(in->hasPrefix);
	unpack_prefix(in->prefixDatum, &order, &width, &pix);
	terminal = in->allTheSame || (order >= TERMINAL_ORDER);

	if (!terminal && in->nkeys > 0)
		reg = spg_cached_region(fcinfo, in->scankeys[0].sk_argument);

	keep = (int *) palloc(sizeof(int) * in->nNodes);

	{
		/*
		 * to_order/shift are only meaningful -- and order is only a real
		 * order, not the TERMINAL_ORDER sentinel or an untrustworthy
		 * allTheSame value -- when !terminal; computing them unconditionally
		 * for a terminal tuple (order could be 30, past SC_MAX_ORDER) would
		 * make to_order < order and shift negative, undefined behaviour in
		 * C. Every node is kept unconditionally when terminal regardless
		 * (see above), so child_pix is simply not needed in that case.
		 *
		 * `width` is this tuple's own stored width (read above), already
		 * reflecting whatever cap applied when it was created -- not
		 * split_width(order) -- so to_order needs no further clamping
		 * either (see the wide-radix comment above choose()).
		 */
		int32		to_order = terminal ? 0 : order + width;
		int32		shift = terminal ? 0 : 2 * (to_order - order);

		for (int i = 0; i < in->nNodes; i++)
		{
			if (!terminal && reg != NULL)
			{
				int64		child_pix = (order < 0)
					? DatumGetInt16(in->nodeLabels[i])
					: (pix << shift) | DatumGetInt16(in->nodeLabels[i]);
				double		fo;
				sc_class	cls = sc_region_classify(reg, to_order, child_pix, &fo);

				if (cls == SC_OUT)
					continue;
			}
			keep[nkeep++] = i;
		}

		out->nNodes = nkeep;
		out->nodeNumbers = (int *) palloc(sizeof(int) * nkeep);
		out->levelAdds = (int *) palloc(sizeof(int) * nkeep);
		out->traversalValues = (void **) palloc(sizeof(void *) * nkeep);

		for (int j = 0; j < nkeep; j++)
		{
			out->nodeNumbers[j] = keep[j];
			out->levelAdds[j] = 1;
			out->traversalValues[j] = NULL;
		}
	}

	pfree(keep);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(spg_healpix_leaf_consistent);
Datum
spg_healpix_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	sc_region  *reg;
	sc_vec3		v;
	bool		res;

	out->recheck = false;
	out->leafValue = (Datum) 0;

	if (in->nkeys == 0)
		PG_RETURN_BOOL(true);

	reg = spg_cached_region(fcinfo, in->scankeys[0].sk_argument);
	v = skycell_pos_from_datum(in->leafDatum);
	res = sc_region_contains(reg, v) != 0;
	PG_RETURN_BOOL(res);
}
