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
 * slower) for this opclass, found empirically before it was reasoned out: a
 * physically HEALPix-sorted table missed 2 of 3 true matches on a cone
 * query that an unsorted build of the identical rows answered correctly.
 * SP-GiST's own core marks a freshly created inner tuple "all-the-same"
 * whenever picksplit reports only one node, and once marked, the core
 * refuses to let choose() add a new node to it (spgAddNode on an
 * all-the-same tuple is a hard error) -- choose() is instead expected to
 * dump anything that arrives there into that one node regardless of its
 * real label, on the documented assumption that "the eventual PickSplit
 * call will re-sort things out". That assumption only holds if "one node"
 * genuinely means "provably indistinguishable forever", which a *fixed*
 * one-order-at-a-time split cannot promise: a small overflow batch at some
 * deep order is often locally homogeneous (one digit) purely because of
 * insertion order or a dense cluster, not because every point that could
 * ever land there shares that digit. A later, genuinely different point
 * then has no legal way to be routed correctly, and is forced into that
 * node's subtree anyway -- inner_consistent's classify() calls downstream
 * of that node then run against the WRONG assumed pixel and can prune a
 * leaf a correct pixel would not have pruned.
 *
 * The real, general fix is exactly the standard PATRICIA-trie discipline
 * (the same one the built-in inet_ops spgist opclass uses for address
 * bits): a tuple is only ever created at a point where the current batch
 * *actually* branches (picksplit searches forward, order by order, for the
 * first point of real disagreement, however far that is -- see below), so
 * "one node" is only ever reported when the batch is truly identical all
 * the way to SC_MAX_ORDER. That in turn means choose() must be able to
 * detect a later point whose true ancestor diverges from a tuple's stored
 * prefix *before* reaching its decision order, and split the prefix itself
 * (spgSplitTuple) rather than just picking a node -- the part a "no
 * compression" design gets to skip, and the part that turned out not to be
 * optional.
 *
 * PICKSPLIT: given a batch of points that must be organised under a new
 * inner tuple, start at `candidate_order = in->level` and compute each
 * point's digit there. If they all agree, that digit is common to the
 * whole batch and gives no information; bump candidate_order and try
 * again, until either two points disagree (the real branch point) or
 * SC_MAX_ORDER is exhausted (true duplicates, one node, all-the-same is
 * correct). This is an ordinary longest-common-prefix search, computed
 * fresh from the batch's actual coordinates each time -- no state carried
 * in from the caller. Starting from in->level (rather than reading a
 * parent prefix picksplit's own arguments have no room for) can only ever
 * *under*-estimate how much is already guaranteed common to this batch,
 * since prefixes only ever get deeper: any such slack just means a few of
 * the loop's early iterations re-confirm agreement the caller already
 * established, before it reaches the real branch point. Never wrong, just
 * occasionally a few redundant sc_vec2pix() calls.
 *
 * CHOOSE: reads the current tuple's own (order, pix) prefix directly
 * (never from in->level, which is only a hop counter once prefixes can
 * span more than one order) and checks the incoming point's ancestor at
 * that prefix against the stored pix. A match proceeds exactly like the
 * fixed-order design (compute the next digit, look up or add a node -- now
 * always legal, since a real branch point never has only one label). A
 * mismatch means this point's true ancestor diverges from the prefix
 * somewhere shallower than expected: find the deepest order the two still
 * agree on and issue spgSplitTuple, inserting a new, shorter-prefixed tuple
 * above the existing one at exactly that point -- the framework re-invokes
 * choose() on the result, which then adds the new point as a sibling node.
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

	Assert(in->hasPrefix);
	order = UNPACK_ORDER(in->prefixDatum);
	pix = UNPACK_PIX(in->prefixDatum);
	v = skycell_pos_from_datum(in->datum);

	if (order >= TERMINAL_ORDER)
	{
		/* genuinely exhausted: nothing left to distinguish, ever */
		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = 0;
		out->result.matchNode.levelAdd = 1;
		out->result.matchNode.restDatum = in->leafDatum;
		PG_RETURN_VOID();
	}

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

		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum =
			Int64GetDatum(PACK_PREFIX(common_order, common_pix));
		out->result.splitTuple.prefixNNodes = 1;
		out->result.splitTuple.prefixNodeLabels = (Datum *) palloc(sizeof(Datum));
		out->result.splitTuple.prefixNodeLabels[0] =
			Int16GetDatum(pix_digit_at(pix, order, common_order + 1));
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
	 * Not found: legal here (unlike a fixed-order-per-level design) because
	 * a tuple only ever reaches this point with nNodes==1 via the genuine
	 * SC_MAX_ORDER exhaustion above, which never falls through to here --
	 * every other tuple was created (by picksplit or spgSplitTuple) at an
	 * order where the batch that built it actually disagreed, so it is
	 * never marked all-the-same and spgAddNode against it is always valid.
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
	int32		candidate_order = in->level;
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

	Assert(in->hasPrefix);
	order = UNPACK_ORDER(in->prefixDatum);
	pix = UNPACK_PIX(in->prefixDatum);
	terminal = (order >= TERMINAL_ORDER);

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
