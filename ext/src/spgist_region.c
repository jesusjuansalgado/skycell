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
 * A prefix packs (order, pix) into one int8: order in the high bits (0..30,
 * where 30 is the "exhausted, genuinely all-the-same" sentinel one past
 * SC_MAX_ORDER), pix (up to 58 bits, order-29 pixel ids need 2*29=58) in the
 * low bits. order=-1 ("no ancestor established yet", the very first tuple)
 * is stored as 0 with pix ignored -- order is offset by +1 throughout so
 * the packed value is never negative.
 */
#define PIX_BITS 58
#define PACK_PREFIX(order, pix) \
	((((int64) ((order) + 1)) << PIX_BITS) | ((pix) & ((INT64CONST(1) << PIX_BITS) - 1)))
#define UNPACK_ORDER(p)	 (((int32) (((uint64) (p)) >> PIX_BITS)) - 1)
#define UNPACK_PIX(p)	 ((int64) (p) & ((INT64CONST(1) << PIX_BITS) - 1))
#define TERMINAL_ORDER	 (SC_MAX_ORDER + 1)

static inline int16
healpix_digit(int order, sc_vec3 v)
{
	int64		pix = sc_vec2pix(order, v);

	return (order == 0) ? (int16) pix : (int16) (pix & 3);
}

/* the digit at at_order implied by full_pix, a pixel value captured at the
 * (deeper-or-equal) stored_order */
static inline int16
pix_digit_at(int64 full_pix, int stored_order, int at_order)
{
	int64		shifted = full_pix >> (2 * (stored_order - at_order));

	return (at_order == 0) ? (int16) shifted : (int16) (shifted & 3);
}

PG_FUNCTION_INFO_V1(spg_healpix_config);
Datum
spg_healpix_config(PG_FUNCTION_ARGS)
{
	spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0);
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = INT8OID;
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
	order = UNPACK_ORDER(in->prefixDatum);
	pix = UNPACK_PIX(in->prefixDatum);
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
		 */
		old_label = pix_digit_at(pix, order, common_order + 1);
		new_label = healpix_digit(common_order + 1, v);
		Assert(new_label != old_label);

		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum =
			Int64GetDatum(PACK_PREFIX(common_order, common_pix));
		out->result.splitTuple.prefixNNodes = 2;
		out->result.splitTuple.prefixNodeLabels = (Datum *) palloc(sizeof(Datum) * 2);
		out->result.splitTuple.prefixNodeLabels[0] = Int16GetDatum(old_label);
		out->result.splitTuple.prefixNodeLabels[1] = Int16GetDatum(new_label);
		out->result.splitTuple.childNodeN = 0;
		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum =
			Int64GetDatum(PACK_PREFIX(order, pix));
		PG_RETURN_VOID();
	}

	label = healpix_digit(order + 1, v);

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

		first_label = healpix_digit(candidate_order, v0);
		labels[0] = first_label;
		diverged = false;
		for (int i = 1; i < in->nTuples; i++)
		{
			sc_vec3		v = skycell_pos_from_datum(in->datums[i]);

			labels[i] = healpix_digit(candidate_order, v);
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
		out->prefixDatum = Int64GetDatum(PACK_PREFIX(TERMINAL_ORDER, 0));
		out->nNodes = 1;
		out->nodeLabels = (Datum *) palloc(sizeof(Datum));
		out->nodeLabels[0] = Int16GetDatum(0);
		out->mapTuplesToNodes = (int *) palloc0(sizeof(int) * in->nTuples);
		pfree(labels);
		PG_RETURN_VOID();
	}

	{
		sc_vec3		v0 = skycell_pos_from_datum(in->datums[0]);
		int64		ancestor_pix = (candidate_order > 0)
			? sc_vec2pix(candidate_order - 1, v0) : 0;
		int16		distinct[12];
		int			ndistinct = 0;

		out->hasPrefix = true;
		out->prefixDatum = Int64GetDatum(PACK_PREFIX(candidate_order - 1, ancestor_pix));

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

PG_FUNCTION_INFO_V1(spg_healpix_inner_consistent);
Datum
spg_healpix_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int32		order;
	int64		pix;
	bool		terminal;
	sc_region	reg;
	bool		have_reg = false;
	int		   *keep;
	int			nkeep = 0;

	/*
	 * allTheSame tuples' (order, pix, node labels) cannot be trusted -- see
	 * choose()'s comment on the same point -- so treat them exactly like
	 * this opclass's own genuine SC_MAX_ORDER-exhausted case: visit every
	 * node, unconditionally, with no classify()-based pruning.
	 */
	Assert(in->hasPrefix);
	order = UNPACK_ORDER(in->prefixDatum);
	pix = UNPACK_PIX(in->prefixDatum);
	terminal = in->allTheSame || (order >= TERMINAL_ORDER);

	if (!terminal && in->nkeys > 0)
	{
		skycell_region_from_datum(in->scankeys[0].sk_argument, &reg);
		have_reg = true;
	}

	keep = (int *) palloc(sizeof(int) * in->nNodes);

	{
		int64	   *child_pixes = (int64 *) palloc(sizeof(int64) * in->nNodes);

		for (int i = 0; i < in->nNodes; i++)
		{
			int64		child_pix = (order < 0)
				? DatumGetInt16(in->nodeLabels[i])
				: (pix << 2) | DatumGetInt16(in->nodeLabels[i]);

			if (!terminal && have_reg)
			{
				double		fo;
				sc_class	cls = sc_region_classify(&reg, order + 1, child_pix, &fo);

				if (cls == SC_OUT)
					continue;
			}
			child_pixes[nkeep] = child_pix;
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

		pfree(child_pixes);
	}

	if (have_reg)
		sc_region_free(&reg);
	pfree(keep);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(spg_healpix_leaf_consistent);
Datum
spg_healpix_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	sc_region	reg;
	sc_vec3		v;
	bool		res;

	out->recheck = false;
	out->leafValue = (Datum) 0;

	if (in->nkeys == 0)
		PG_RETURN_BOOL(true);

	skycell_region_from_datum(in->scankeys[0].sk_argument, &reg);
	v = skycell_pos_from_datum(in->leafDatum);
	res = sc_region_contains(&reg, v) != 0;
	sc_region_free(&reg);
	PG_RETURN_BOOL(res);
}
