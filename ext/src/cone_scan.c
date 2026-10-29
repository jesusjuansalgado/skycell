/*
 * cone_scan.c -- a custom scan for constant cones (skycell.custom_scan).
 *
 * The rewrite in skycell.c turns skycell_cone(cell, ra, dec, ra0, dec0, r)
 * into an OR of `cell >= lo AND cell <= hi` arms plus the exact test, and
 * leaves the rest to PostgreSQL.  The planner then builds an index path per
 * arm, estimates every arm against the column's histogram and costs a
 * BitmapOr over them: measured at 10-12 us per range on the paper's
 * catalogue, more than skycell's own covering, and the whole of its warm
 * deficit against pgSphere at small radii.
 *
 * With skycell.custom_scan on (the default), a constant cone on a single base relation is
 * left as the opaque function call (skycell_support declines to rewrite it),
 * so the planner sees one restriction clause whose selectivity skycell
 * itself supplies.  set_rel_pathlist_hook then adds one CustomPath per such
 * clause that a B-tree index on the cell expression can serve, costed from
 * the covering; the executor walks the covering's ranges in cell order with
 * a plain index scan each when the heap follows the cell order, or collects
 * their TIDs into a bitmap and reads each heap page once when it does not --
 * the same choice PostgreSQL makes between an index and a bitmap scan, from
 * the same correlation statistic -- and rechecks rows with
 * skycell_in_cone(ra, dec, ...), which, unlike skycell_cone(), does not need
 * the cell expression evaluated per row.
 *
 * A cross-match cone (centre or radius from another relation's row) gets the
 * same node parameterized by that row, computing each row's covering on
 * rescan; a cone over parameters alone (a generic plan, a correlated
 * sub-select) is covered per rescan as well.  skycell_join and
 * skycell_radial_query take these paths as the skycell_cone they stand for.
 *
 * Limits: only a cone that is a top-level AND term of a WHERE or JOIN ON (one
 * under an OR keeps the rewrite, which a BitmapOr can serve); and before
 * PostgreSQL 18 the bitmap mode does not prefetch (18's table AM reads the
 * bitmap through a read stream, which does).
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "executor/tuptable.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_statistic.h"
#include "commands/explain.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"	/* ExplainProperty*() moved here in 18 */
#endif
#include "miscadmin.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/tidbitmap.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/guc.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"
#include "catalog/pg_operator_d.h"

#include "cover.h"
#include "skycell_internal.h"

static bool skycell_custom_scan = true;
static bool skycell_index_filter = true;
static bool skycell_nearest_scan = true;

/* the six-argument skycell_cone, as last seen by cone_scan_keep() */
static Oid	cone_funcid = InvalidOid;

/* skycell_in_cone(float8 x 6), looked up once per cone_funcid */
static Oid	in_cone_funcid = InvalidOid;
static Oid	in_cone_for = InvalidOid;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static Plan *cone_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					   List *tlist, List *clauses, List *custom_plans);
static Node *cone_create_state(CustomScan *cscan);
static void cone_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *cone_exec(CustomScanState *node);
static void cone_end(CustomScanState *node);
static void cone_rescan(CustomScanState *node);
static void cone_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static void add_nearest_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte);
static Plan *nearest_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
						  List *tlist, List *clauses, List *custom_plans);
static Node *nearest_create_state(CustomScan *cscan);
static void nearest_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *nearest_exec(CustomScanState *node);
static void nearest_end(CustomScanState *node);
static void nearest_rescan(CustomScanState *node);
static void nearest_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static const CustomPathMethods nearest_path_methods = {
	.CustomName = "SkycellNearest",
	.PlanCustomPath = nearest_plan,
};

static const CustomScanMethods nearest_scan_methods = {
	.CustomName = "SkycellNearest",
	.CreateCustomScanState = nearest_create_state,
};

static const CustomExecMethods nearest_exec_methods = {
	.CustomName = "SkycellNearest",
	.BeginCustomScan = nearest_begin,
	.ExecCustomScan = nearest_exec,
	.EndCustomScan = nearest_end,
	.ReScanCustomScan = nearest_rescan,
	.ExplainCustomScan = nearest_explain,
};

static const CustomPathMethods cone_path_methods = {
	.CustomName = "SkycellCone",
	.PlanCustomPath = cone_plan,
};

static const CustomScanMethods cone_scan_methods = {
	.CustomName = "SkycellCone",
	.CreateCustomScanState = cone_create_state,
};

static const CustomExecMethods cone_exec_methods = {
	.CustomName = "SkycellCone",
	.BeginCustomScan = cone_begin,
	.ExecCustomScan = cone_exec,
	.EndCustomScan = cone_end,
	.ReScanCustomScan = cone_rescan,
	.ExplainCustomScan = cone_explain,
};

typedef struct ConeScanState
{
	CustomScanState css;
	Oid			indexoid;
	Relation	index;
	IndexScanDesc iscan;
	int			nranges;
	const int64 *bounds;		/* lo0, hi0, lo1, hi1, ... */
	int			cur;			/* ordered: range being scanned, -1 before the first */
	ScanKeyData keys[2];
	bool		bitmap;			/* bitmap mode, else ordered */
	TableScanDesc hscan;		/* bitmap: heap scan over the TID bitmap */
	TIDBitmap  *tbm;			/* bitmap: NULL until the first fetch */
#if PG_VERSION_NUM < 180000
	TBMIterator *it;
	TBMIterateResult *tbmres;	/* bitmap: page being returned, or NULL */
#endif

	/* a cross-match cone: the covering is computed per outer row */
	bool		runtime;
	bool		joined;			/* parameterized by an outer row */
	bool		need_cover;		/* on the first fetch after a (re)start */
	List	   *args;			/* ExprStates of ra0, dec0, radius */
	Oid			relid,
				statrel;
	AttrNumber	attnum;
	bool		have_dens;
	uint64		dens_generation;	/* skycell_density_generation() at the load */
	sc_density	dens;
	int64	   *rt_bounds;		/* bounds' storage, rt_cap ranges */
	int			rt_cap;
	double		ncoverings,		/* for EXPLAIN ANALYZE: this process's own */
				nranges_total;

	/*
	 * The index filter: when the index also holds the cone's ra and dec
	 * columns (INCLUDE), the cone is tested on each index entry and only the
	 * entries inside it are fetched from the heap.  ira/idec are their index
	 * attribute numbers, 0 when the index holds neither.
	 */
	AttrNumber	ira,
				idec;
	bool		have_centre;
	double		cra,
				cosdec0,
				cdec,
				thr;
} ConeScanState;

/* ------------------------------------------------------------------ */
/* planning                                                           */
/* ------------------------------------------------------------------ */

/*
 * What the centre and radius (arguments 3-5) make of a cone over relation
 * varno: a constant cone (all three constants), a cross-match cone (none of
 * them reads varno, at least one reads another relation of this query level,
 * so the scan is parameterized by the outer row and computes its covering
 * per row), a run-time cone (no other relation, but parameters: a generic
 * plan, or a correlated sub-select's outer columns, so the covering is
 * computed per rescan), or neither -- left to the rewrite, as is anything
 * volatile, set-returning or containing a sub-select or an aggregate.
 */
typedef enum
{
	CONE_NONE,
	CONE_CONST,
	CONE_JOIN,
	CONE_RUNTIME
} ConeKind;

typedef struct
{
	Index		varno;
	bool		outer;			/* reads another relation */
	bool		param;			/* reads a parameter or an outer query's column */
} cone_arg_ctx;

static bool
cone_arg_unsafe(Node *node, cone_arg_ctx *c)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink) || IsA(node, SubPlan) || IsA(node, Aggref) ||
		IsA(node, WindowFunc) || IsA(node, GroupingFunc) || IsA(node, PlaceHolderVar))
		return true;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		if (v->varlevelsup == 0)
		{
			if (v->varno == c->varno)
				return true;
			c->outer = true;
		}
		else
			c->param = true;
		return false;
	}
	if (IsA(node, Param))
		c->param = true;
	return expression_tree_walker(node, cone_arg_unsafe, (void *) c);
}

static ConeKind
cone_kind(List *args, Index varno)
{
	cone_arg_ctx c = {varno, false, false};
	bool		all_const = true;

	for (int i = 3; i <= 5; i++)
	{
		Node	   *a = list_nth(args, i);

		if (IsA(a, Const))
		{
			if (((Const *) a)->constisnull)
				return CONE_NONE;
			continue;
		}
		all_const = false;
		if (cone_arg_unsafe(a, &c) || contain_volatile_functions(a) || expression_returns_set(a))
			return CONE_NONE;
	}
	if (all_const)
		return CONE_CONST;
	return c.outer ? CONE_JOIN : c.param ? CONE_RUNTIME : CONE_NONE;
}

/*
 * Does the qual contain fcall as one of its top-level AND terms, written as the
 * call or as an operator over it?  The call the
 * support function sees is a rebuilt copy whose arguments are already folded,
 * so the query's own terms are compared by value: an argument matches if it is
 * equal as written or once folded the same way.
 */
/*
 * The common unfolded argument is a literal under a one-argument cast, such
 * as the numeric literal 12.5 passed as float8.  Folding it is calling that
 * cast on the constant, which is what eval_const_expressions() does for an
 * immutable strict function, without the executor state evaluate_expr()
 * builds for every argument.  Returns false, leaving *v unset, for anything
 * else; then the caller folds the argument the general way.
 */
static bool
fold_cast_of_const(Node *x, Datum *v)
{
	FuncExpr   *f;
	Const	   *c;

	if (!IsA(x, FuncExpr))
		return false;
	f = (FuncExpr *) x;
	if (list_length(f->args) != 1 || f->funcretset || !IsA(linitial(f->args), Const))
		return false;
	c = (Const *) linitial(f->args);
	if (c->constisnull || func_volatile(f->funcid) != PROVOLATILE_IMMUTABLE ||
		!func_strict(f->funcid))
		return false;
	*v = OidFunctionCall1Coll(f->funcid, f->inputcollid, c->constvalue);
	return true;
}

static bool
call_matches(PlannerInfo *root, Oid funcid, List *args, FuncExpr *fcall)
{
	ListCell   *a,
			   *b;

	if (funcid != fcall->funcid ||
		list_length(args) != list_length(fcall->args))
		return false;
	forboth(a, args, b, fcall->args)
	{
		Node	   *x = (Node *) lfirst(a);
		Node	   *y = (Node *) lfirst(b);
		Datum		v;

		if (equal(x, y))
			continue;
		if (IsA(x, Var) || IsA(x, Const))
			return false;
		if (IsA(y, Const) && !((Const *) y)->constisnull &&
			exprType(x) == ((Const *) y)->consttype &&
			exprTypmod(x) == ((Const *) y)->consttypmod &&
			exprCollation(x) == ((Const *) y)->constcollid && fold_cast_of_const(x, &v))
		{
			Const	   *k = (Const *) y;

			if (!datumIsEqual(v, k->constvalue, k->constbyval, k->constlen))
				return false;
			continue;
		}
		if (!equal(eval_const_expressions(root, copyObject(x)), y))
			return false;
	}
	return true;
}

static bool
qual_has_term(PlannerInfo *root, Node *qual, FuncExpr *fcall)
{
	ListCell   *lc;

	if (qual == NULL)
		return false;
	if (IsA(qual, List))
	{
		foreach(lc, (List *) qual)
			if (qual_has_term(root, lfirst(lc), fcall))
				return true;
		return false;
	}
	if (is_andclause(qual))
		return qual_has_term(root, (Node *) ((BoolExpr *) qual)->args, fcall);
	if (IsA(qual, FuncExpr))
		return call_matches(root, ((FuncExpr *) qual)->funcid, ((FuncExpr *) qual)->args, fcall);
	/* an operator (the ADQL <@ and @>): the support function sees its function */
	if (IsA(qual, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) qual;

		return call_matches(root, OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno),
							op->args, fcall);
	}
	return false;
}

/* ...in any WHERE or JOIN ON of the join tree (pulled-up subqueries included) */
static bool
jointree_has_term(PlannerInfo *root, Node *jt, FuncExpr *fcall)
{
	ListCell   *lc;

	if (jt == NULL)
		return false;
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;

		if (qual_has_term(root, f->quals, fcall))
			return true;
		foreach(lc, f->fromlist)
			if (jointree_has_term(root, lfirst(lc), fcall))
				return true;
		return false;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		return qual_has_term(root, j->quals, fcall) ||
			jointree_has_term(root, j->larg, fcall) ||
			jointree_has_term(root, j->rarg, fcall);
	}
	return false;
}

/*
 * Called from skycell_support's SupportRequestSimplify: true to leave this
 * skycell_cone call unrewritten for the custom path.  Only a cone whose
 * parameters are constants, whose cell/ra/dec come from one base relation, and
 * which is a top-level AND term of a WHERE or JOIN ON: only there can it become
 * a restriction a scan answers.  A cross-match cone (centre or radius from
 * another relation, cone_kind()) is kept as well, for the parameterized path
 * of cone_set_rel_pathlist().  Under an OR, a NOT, a CASE or in a target list
 * it is rewritten as before, which a BitmapOr can still serve.  Whether an
 * index can serve it is only known once the planner has the relation's index
 * list, and a relation without one gets a sequential scan either way.
 */
static bool
keep_cone(PlannerInfo *root, FuncExpr *written, FuncExpr *fcall)
{
	List	   *args = fcall->args;
	List	   *vars;
	ListCell   *lc;
	Index		varno = 0;

	if (!skycell_custom_scan || root == NULL || root->parse == NULL ||
		list_length(args) != 6)
		return false;
	vars = pull_var_clause((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
						   PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return false;
		varno = v->varno;
	}
	if (varno == 0 || varno > list_length(root->parse->rtable) ||
		rt_fetch(varno, root->parse->rtable)->rtekind != RTE_RELATION)
		return false;
	if (cone_kind(args, varno) == CONE_NONE)
		return false;
	/* the scan reads heap tuples; any other table AM keeps the rewrite */
	{
		RangeTblEntry *rte = rt_fetch(varno, root->parse->rtable);

		if (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_MATVIEW)
		{
			Relation	r = table_open(rte->relid, NoLock);	/* locked by the parser */
			bool		heap = table_slot_callbacks(r) == &TTSOpsBufferHeapTuple;

			table_close(r, NoLock);
			if (!heap)
				return false;
		}
		else if (rte->relkind != RELKIND_PARTITIONED_TABLE)
			return false;
	}
	if (!jointree_has_term(root, (Node *) root->parse->jointree, written))
		return false;
	cone_funcid = fcall->funcid;
	return true;
}

bool
cone_scan_keep(PlannerInfo *root, FuncExpr *fcall)
{
	return keep_cone(root, fcall, fcall);
}

/*
 * The same for a Q3C-shaped spelling (skycell_join, skycell_radial_query):
 * cone6 is the six-argument skycell_cone it stands for, with the cell
 * expression synthesised; written is the call as the query wrote it, which is
 * what has to be a top-level AND term.
 */
bool
cone_scan_keep_as(PlannerInfo *root, FuncExpr *written, FuncExpr *cone6)
{
	return keep_cone(root, written, cone6);
}

/* a B-tree on the cell expression, comparing int8; the smallest if several */
static IndexOptInfo *
cell_index(RelOptInfo *rel, Node *cell)
{
	IndexOptInfo *best = NULL;
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ix = (IndexOptInfo *) lfirst(lc);

		if (ix->relam != BTREE_AM_OID || ix->ncolumns < 1 || ix->hypothetical ||
			ix->opcintype[0] != INT8OID || (ix->indpred != NIL && !ix->predOK))
			continue;
		if (ix->indexkeys[0] != 0)
		{
			if (!IsA(cell, Var) || ((Var *) cell)->varno != rel->relid ||
				((Var *) cell)->varattno != ix->indexkeys[0])
				continue;
		}
		else if (ix->indexprs == NIL || !equal(linitial(ix->indexprs), cell))
			continue;
		if (!OidIsValid(get_opfamily_member(ix->opfamily[0], INT8OID, INT8OID,
											BTGreaterEqualStrategyNumber)) ||
			!OidIsValid(get_opfamily_member(ix->opfamily[0], INT8OID, INT8OID,
											BTLessEqualStrategyNumber)))
			continue;
		if (best == NULL || ix->pages < best->pages)
			best = ix;
	}
	return best;
}

/*
 * How well the heap follows the cell order: the correlation ANALYZE keeps for
 * the cell column (or, for an expression, for the index's own column), as
 * btcostestimate reads it.  0 -- the pessimistic answer -- when there are no
 * statistics.
 */
static double
cell_correlation(RangeTblEntry *rte, IndexOptInfo *ix, Node *cell)
{
	Oid			statrel = ix->indexoid;
	AttrNumber	attnum = 1;
	HeapTuple	tp;
	double		corr = 0.0;

	if (ix->indexkeys[0] != 0)
	{
		statrel = rte->relid;
		attnum = ((Var *) cell)->varattno;
	}
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(statrel),
						 Int16GetDatum(attnum), BoolGetDatum(false));
	if (HeapTupleIsValid(tp))
	{
		AttStatsSlot sslot;

		if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers == 1)
				corr = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
		ReleaseSysCache(tp);
	}
	return corr;
}

/*
 * What a cross-match cone costs per outer row, estimated before any outer row
 * is known: coverings computed at sample centres and averaged.  The centres
 * are taken evenly through the cell column's histogram, so they fall where
 * the rows are, as the targets of a cross-match usually do (uniform centres
 * when there is no histogram).  The radius is the planner's estimate of the
 * radius argument, or 1 arcsec when it has none.
 */
#define JOIN_SAMPLES 16

typedef struct
{
	double		nranges;		/* ranges per covering */
	double		cand;			/* rows the ranges hold */
	double		matches;		/* rows inside the cone */
	double		steps;			/* cell classifications per covering */
	double		ntotal;			/* rows in the relation */
	Oid			statrel;		/* where its density comes from */
	bool		sampled;		/* measured on sampled probes (join_cone_sample) */
} join_estimate;

/*
 * A column, or a plain cast of one, of a numeric type: safe to evaluate at
 * plan time (no cast here can fail on a value the column holds).
 */
static bool
plain_column(Node *n)
{
	Oid			t;

	if (IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	else if (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcformat == COERCE_IMPLICIT_CAST &&
			 list_length(((FuncExpr *) n)->args) == 1)
		n = linitial(((FuncExpr *) n)->args);
	if (!IsA(n, Var) || ((Var *) n)->varlevelsup != 0 || ((Var *) n)->varattno <= 0)
		return false;
	t = ((Var *) n)->vartype;
	return t == FLOAT8OID || t == FLOAT4OID || t == NUMERICOID ||
		t == INT2OID || t == INT4OID || t == INT8OID;
}

/*
 * A centre coordinate the sample can read: a plain column, or one inside the
 * CASE that guards the centre of a cone added for distance() (guard_centre()
 * in adql.c), whose first branch is the column itself. The sample evaluates
 * the expression as written, so a row the guard turns to NULL is skipped.
 */
static bool
centre_column(Node *n)
{
	if (IsA(n, CaseExpr) && ((CaseExpr *) n)->arg == NULL &&
		list_length(((CaseExpr *) n)->args) >= 1)
		n = (Node *) ((CaseWhen *) linitial(((CaseExpr *) n)->args))->result;
	return plain_column(n);
}

static bool
sampleable_rel(PlannerInfo *root, int varno, RangeTblEntry **rte)
{
	*rte = planner_rt_fetch(varno, root);
	return (*rte)->rtekind == RTE_RELATION && !(*rte)->inh &&
		((*rte)->relkind == RELKIND_RELATION || (*rte)->relkind == RELKIND_MATVIEW);
}

/*
 * What a cross-match's cones hold, measured: the density model cannot know
 * that the targets of a cross-match usually sit on catalogue sources (a
 * probe drawn from another survey has its counterpart in the cone), so it
 * underestimates the matches by orders of magnitude for small radii.  When
 * the centre (and radius, unless constant) are plain columns of one outer
 * table and the cones are small, read PROBE_SAMPLES of its rows spread over
 * its pages, cover each probe as the scan will, and count the inner rows
 * the ranges hold and the cone keeps -- as get_actual_variable_range() reads
 * an index to estimate a range.  Anything else keeps the density estimate.
 *
 * The sample reads at most PROBE_MAX_CAND inner rows.  One the budget cuts
 * short has still measured cones holding more than the model said -- the
 * case of probes sitting on clusters, which the model, spreading each cell's
 * rows evenly over it, misses most -- so it raises the estimate to what it
 * read, and never lowers it.  The budget bounds what planning reads, cold on
 * a heap out of cell order a random page a row: it is not raised to sample
 * larger cones.
 */
#define PROBE_SAMPLES	32
#define PROBE_SLOTS		(2 * PROBE_SAMPLES)	/* rows a block share rounds up to */
#define PROBE_MAX_CAND	4000	/* inner rows read in all */

typedef struct
{
	double		ra0,
				dec0,
				radius;
	bool		null;
} probe_centre;

static bool
join_cone_sample(PlannerInfo *root, List *args, const sc_density *dens, join_estimate *e)
{
	Node	   *rarg = (Node *) list_nth(args, 5);
	bool		rconst = IsA(rarg, Const);
	Relids		ov,
				iv;
	int			ovarno,
				ivarno;
	RangeTblEntry *orte,
			   *irte;
	RelOptInfo *irelinfo;
	IndexOptInfo *ix;
	Relation	orel,
				irel,
				ind;
	BlockNumber nblocks;
	MemoryContext cxt,
				old;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *x_ra0,
			   *x_dec0,
			   *x_r = NULL,
			   *x_ra,
			   *x_dec;
	TupleTableSlot *oslot,
			   *islot;
	TableScanDesc tscan = NULL;
	IndexScanDesc iscan;
	ScanKeyData keys[2];
	Oid			opfamily;
	sc_cover_params p;
	probe_centre probes[PROBE_SLOTS];
	int			nprobes = 0;
	double		sum_n = 0,
				sum_r = 0,
				sum_c = 0,
				sum_m = 0;
	int			nblk,
				per_blk;
	bool		overrun = false;

	/* centre and radius: plain columns of one table; cell, ra, dec: of another */
	if (!centre_column(list_nth(args, 3)) || !centre_column(list_nth(args, 4)) ||
		!(rconst ? !((Const *) rarg)->constisnull : plain_column(rarg)) ||
		!plain_column(lsecond(args)) || !plain_column(lthird(args)))
		return false;
	ov = pull_varnos(root, (Node *) list_make3(list_nth(args, 3), list_nth(args, 4), rarg));
	iv = pull_varnos(root, (Node *) list_make3(linitial(args), lsecond(args), lthird(args)));
	if (!bms_get_singleton_member(ov, &ovarno) || !bms_get_singleton_member(iv, &ivarno) ||
		ovarno == ivarno || !sampleable_rel(root, ovarno, &orte) || !sampleable_rel(root, ivarno, &irte))
		return false;
	irelinfo = find_base_rel(root, ivarno);
	if ((ix = cell_index(irelinfo, linitial(args))) == NULL || !ActiveSnapshotSet())
		return false;

	cxt = AllocSetContextCreate(CurrentMemoryContext, "skycell probe sample", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	orel = table_open(orte->relid, NoLock);	/* locked by the parser */
	irel = table_open(irte->relid, NoLock);
	nblocks = RelationGetNumberOfBlocks(orel);
	if (nblocks == 0 || orel->rd_tableam->scan_set_tidrange == NULL ||
		table_slot_callbacks(irel) != &TTSOpsBufferHeapTuple)
	{
		table_close(irel, NoLock);
		table_close(orel, NoLock);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
		return false;
	}
	ind = index_open(ix->indexoid, AccessShareLock);
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	x_ra0 = ExecPrepareExpr((Expr *) copyObject(list_nth(args, 3)), estate);
	x_dec0 = ExecPrepareExpr((Expr *) copyObject(list_nth(args, 4)), estate);
	if (!rconst)
		x_r = ExecPrepareExpr((Expr *) copyObject(rarg), estate);
	x_ra = ExecPrepareExpr((Expr *) copyObject(lsecond(args)), estate);
	x_dec = ExecPrepareExpr((Expr *) copyObject(lthird(args)), estate);
	oslot = table_slot_create(orel, NULL);
	islot = table_slot_create(irel, NULL);

	opfamily = ind->rd_opfamily[0];
	ScanKeyInit(&keys[0], 1, BTGreaterEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTGreaterEqualStrategyNumber)), Int64GetDatum(0));
	ScanKeyInit(&keys[1], 1, BTLessEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTLessEqualStrategyNumber)), Int64GetDatum(0));
#if PG_VERSION_NUM >= 180000
	iscan = index_beginscan(irel, ind, GetActiveSnapshot(), NULL, 2, 0);
#else
	iscan = index_beginscan(irel, ind, GetActiveSnapshot(), 2, 0);
#endif
	skycell_scan_params(&p, dens);

	/* the probes first: cheap, a few outer pages */
	nblk = (int) Min((BlockNumber) PROBE_SAMPLES, nblocks);
	per_blk = (PROBE_SAMPLES + nblk - 1) / nblk;
	for (int k = 0; k < nblk; k++)
	{
		BlockNumber b = (BlockNumber) (((uint64) k * nblocks) / nblk);
		ItemPointerData lo,
					hi;
		int			got = 0;

		ItemPointerSet(&lo, b, FirstOffsetNumber);
		ItemPointerSet(&hi, b, MaxOffsetNumber);
		if (tscan == NULL)
			tscan = table_beginscan_tidrange(orel, GetActiveSnapshot(), &lo, &hi);
		else
			table_rescan_tidrange(tscan, &lo, &hi);
		while (got < per_blk &&
			   table_scan_getnextslot_tidrange(tscan, ForwardScanDirection, oslot))
		{
			bool		n1,
						n2,
						n3 = false;
			probe_centre *pc = &probes[nprobes++];

			got++;
			ResetExprContext(econtext);
			econtext->ecxt_scantuple = oslot;
			pc->ra0 = DatumGetFloat8(ExecEvalExprSwitchContext(x_ra0, econtext, &n1));
			pc->dec0 = DatumGetFloat8(ExecEvalExprSwitchContext(x_dec0, econtext, &n2));
			pc->radius = rconst ? DatumGetFloat8(((Const *) rarg)->constvalue)
				: DatumGetFloat8(ExecEvalExprSwitchContext(x_r, econtext, &n3));
			pc->null = n1 || n2 || n3;
		}
	}

	/*
	 * Then their cones, in bit-reversed order, so that a sample the budget
	 * cuts short is still spread over the outer table, not its first pages.
	 */
	for (int k = 0; k < PROBE_SLOTS && !overrun; k++)
	{
		int			j = 0;
		const probe_centre *pc;
		double		thr,
					cosdec0;
		sc_region	reg;
		sc_cover	cov;

		for (int bit = 1, rk = k; bit < PROBE_SLOTS; bit <<= 1, rk >>= 1)
			j = (j << 1) | (rk & 1);
		if (j >= nprobes)
			continue;
		pc = &probes[j];
		sum_n += 1;				/* a NULL probe is a probe that matches nothing */
		if (pc->null || sc_region_cone(&reg, pc->ra0, pc->dec0, pc->radius, true) != NULL)
			continue;
		sc_cover_compute(&reg, dens, &p, &cov);
		thr = pc->radius < 0 ? -1.0
			: (pc->radius >= 180.0 ? 2.0 : pow(sin(pc->radius * M_PI / 360.0), 2));
		cosdec0 = cos(pc->dec0 * M_PI / 180.0);
		sum_r += cov.n;
		for (int i = 0; i < cov.n && !overrun; i++)
		{
			keys[0].sk_argument = Int64GetDatum(cov.r[i].lo);
			keys[1].sk_argument = Int64GetDatum(cov.r[i].hi);
			index_rescan(iscan, keys, 2, NULL, 0);
			while (index_getnext_slot(iscan, ForwardScanDirection, islot))
			{
				bool		m1,
							m2;
				double		ra,
							dec,
							sdd,
							sda;

				econtext->ecxt_scantuple = islot;
				ra = DatumGetFloat8(ExecEvalExprSwitchContext(x_ra, econtext, &m1));
				dec = DatumGetFloat8(ExecEvalExprSwitchContext(x_dec, econtext, &m2));
				if (++sum_c > PROBE_MAX_CAND)
				{
					overrun = true;
					break;
				}
				if (m1 || m2)
					continue;
				/* skycell_in_cone's haversine */
				sdd = sin((dec - pc->dec0) * M_PI / 360.0);
				sda = sin((ra - pc->ra0) * M_PI / 360.0);
				if (sdd * sdd + cos(dec * M_PI / 180.0) * cosdec0 * sda * sda <= thr)
					sum_m += 1;
			}
		}
		sc_cover_free(&cov);
	}

	index_endscan(iscan);
	if (tscan)
		table_endscan(tscan);
	ExecDropSingleTupleTableSlot(oslot);
	ExecDropSingleTupleTableSlot(islot);
	FreeExecutorState(estate);
	index_close(ind, NoLock);
	table_close(irel, NoLock);
	table_close(orel, NoLock);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);

	if (sum_n == 0)
		return false;
	if (overrun)
	{
		/*
		 * The cones held more than the model expected: what was read --
		 * whole probes and the one the budget stopped -- is a lower bound
		 * on the probes it reached, so it may only raise the estimate.
		 */
		e->cand = fmax(e->cand, sum_c / sum_n);
		e->matches = fmax(e->matches, sum_m / sum_n);
		return true;
	}
	e->nranges = sum_r / sum_n;
	e->cand = sum_c / sum_n;
	e->matches = sum_m / sum_n;
	e->sampled = true;
	return true;
}

/*
 * join_cone_estimate() is asked for the same clause by the selectivity
 * request, the parameterized path's row count and its cost: keep the last
 * few answers for the query being planned (planner_hook bumps the
 * generation, so nothing carries over to the next query).
 */
#define EST_CACHE 8
static uint64 plan_generation = 0;
static struct
{
	uint64		gen;
	List	   *args;
	join_estimate e;
}			est_cache[EST_CACHE];
static int	est_next = 0;
static planner_hook_type prev_planner_hook = NULL;

static get_relation_info_hook_type prev_get_relation_info_hook = NULL;
static PlannerInfo *distances_root = NULL;
static uint64 distances_gen = 0;

/*
 * A query level's first relation is looked up after its quals are simplified
 * and before they are distributed: the moment to make ADQL's integer
 * predicates and distance comparisons indexable
 * (skycell_index_adql_predicates() in adql.c).
 */
static void
cone_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relid, inhparent, rel);
	if (root != distances_root || plan_generation != distances_gen)
	{
		distances_root = root;
		distances_gen = plan_generation;
		skycell_index_adql_predicates(root);
	}
}

static PlannedStmt *
cone_planner(Query *parse, const char *query_string, int cursorOptions, ParamListInfo boundParams)
{
	plan_generation++;
	if (prev_planner_hook)
		return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
	return standard_planner(parse, query_string, cursorOptions, boundParams);
}

static bool join_cone_estimate_uncached(PlannerInfo *root, List *args, join_estimate *e);

static bool
join_cone_estimate(PlannerInfo *root, List *args, join_estimate *e)
{
	bool		ok;

	for (int i = 0; i < EST_CACHE; i++)
		if (est_cache[i].gen == plan_generation && plan_generation != 0 && est_cache[i].args == args)
		{
			*e = est_cache[i].e;
			return true;
		}
	ok = join_cone_estimate_uncached(root, args, e);
	if (ok && plan_generation != 0)
	{
		est_cache[est_next].gen = plan_generation;
		est_cache[est_next].args = args;
		est_cache[est_next].e = *e;
		est_next = (est_next + 1) % EST_CACHE;
	}
	return ok;
}

static bool
join_cone_estimate_uncached(PlannerInfo *root, List *args, join_estimate *e)
{
	Node	   *cell = linitial(args);
	Node	   *rarg = estimate_expression_value(root, (Node *) list_nth(args, 5));
	double		radius = 1.0 / 3600.0;
	sc_density	dens;
	sc_cover_params p;
	bool		uses_cell_ops;
	int			n = 0;

	memset(e, 0, sizeof(*e));
	if (IsA(rarg, Const) && !((Const *) rarg)->constisnull)
		radius = DatumGetFloat8(((Const *) rarg)->constvalue);
	density_for_var(root, cell, &dens, &e->statrel, &uses_cell_ops);
	if (!(dens.ntotal > 0))
		return false;
	e->ntotal = dens.ntotal;
	skycell_scan_params(&p, &dens);
	for (int k = 0; k < JOIN_SAMPLES; k++)
	{
		double		ra,
					dec;
		sc_region	reg;
		sc_cover	cov;

		if (dens.nbounds >= 2)
		{
			sc_vec3		v = sc_pix2vec(29, dens.bounds[(int) ((k + 0.5) * dens.nbounds / JOIN_SAMPLES)]);

			ra = atan2(v.y, v.x) * 180.0 / M_PI;
			dec = asin(fmax(-1.0, fmin(1.0, v.z))) * 180.0 / M_PI;
		}
		else
		{
			/* a Fibonacci sphere */
			ra = fmod(k * 137.50776405003785, 360.0);
			dec = asin(1.0 - 2.0 * (k + 0.5) / JOIN_SAMPLES) * 180.0 / M_PI;
		}
		if (ra < 0)
			ra += 360.0;
		if (sc_region_cone(&reg, ra, dec, radius, true) != NULL)
			continue;
		sc_cover_compute(&reg, &dens, &p, &cov);
		e->nranges += cov.n;
		e->cand += cov.exp_rows;
		e->matches += (cov.area > 0) ? cov.exp_rows * fmin(1.0, reg.area / cov.area) : 0;
		e->steps += cov.steps;
		sc_cover_free(&cov);
		n++;
	}
	if (n == 0)
		return false;
	e->nranges /= n;
	e->cand /= n;
	e->matches /= n;
	e->steps /= n;
	/* small cones: measure what sampled probes' cones actually hold */
	if (e->cand * PROBE_SAMPLES <= PROBE_MAX_CAND)
		(void) join_cone_sample(root, args, &dens, e);
	return true;
}

/*
 * Selectivity of a cross-match or run-time cone, for skycell_support's
 * SupportRequestSelectivity: the rows one outer row's cone is expected to
 * hold over the rows in the relation, so that the join's estimated size is
 * the outer rows times the matches per row.  -1 when the cone is not one the
 * custom scan takes (skycell_support then declines).
 */
double
cone_join_selectivity(PlannerInfo *root, List *args)
{
	List	   *vars;
	Index		varno = 0;
	ListCell   *lc;
	join_estimate e;

	if (!skycell_custom_scan || list_length(args) != 6)
		return -1;
	vars = pull_var_clause((Node *) list_make3(linitial(args), lsecond(args), lthird(args)),
						   PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varlevelsup != 0 || (varno != 0 && v->varno != varno))
			return -1;
		varno = v->varno;
	}
	if (varno == 0)
		return -1;
	switch (cone_kind(args, varno))
	{
		case CONE_JOIN:
		case CONE_RUNTIME:
			break;
		default:
			return -1;
	}
	if (!join_cone_estimate(root, args, &e))
		return -1;
	return e.matches / e.ntotal;
}

/*
 * How many times a scan parameterized by these outer relations runs: the
 * smallest row estimate among them, as get_loop_count() (static in
 * indxpath.c) has it, without its semijoin adjustment.
 */
static double
cone_loop_count(PlannerInfo *root, Relids outer)
{
	double		result = 0.0;
	int			relid = -1;

	while ((relid = bms_next_member(outer, relid)) >= 0)
	{
		RelOptInfo *orel;

		if (relid >= root->simple_rel_array_size ||
			(orel = root->simple_rel_array[relid]) == NULL ||
			IS_DUMMY_REL(orel) || orel->rows <= 0.0)
			continue;
		if (result == 0.0 || orel->rows < result)
			result = orel->rows;
	}
	return result > 1.0 ? result : 1.0;
}

/*
 * Cost of walking the covering, in the planner's units, for both ways the
 * executor can do it, mirroring what PostgreSQL charges for the same work:
 * a descent per range (btcostestimate), the leaf pages and index tuples the
 * ranges hold, the heap, then the recheck on every candidate row and the
 * projection on the ones that pass.  Only the heap differs:
 *
 *   ordered  one index scan per range, heap fetched in cell order;
 *   bitmap   the ranges' TIDs collected first, each heap page then read once
 *            in physical order (cost_bitmap_heap_scan's page count).
 *
 * *bitmap reports which is cheaper; that is the mode the scan runs in.
 *
 * loops is how many times the scan runs: the outer rows of a cross-match
 * (cone_loop_count()), 1 otherwise.  As cost_index(), compute_bitmap_pages()
 * and genericcostestimate() have it, the pages the scans read together are
 * then estimated across all of them (index_pages_fetched(), which knows
 * effective_cache_size) and charged per scan: a page an earlier probe read is
 * not a random read again.  Without this every probe of a cross-match paid
 * for its pages cold, about ten times what the scan costs, and a parallel
 * sequential scan that runs ten times slower was planned instead.
 *
 * recheck is the per-row cost of the cross-match's join clause, which the
 * scan evaluates on every candidate, as cost_index() charges a path's join
 * clauses; 0 for a cone with constant or run-time bounds.
 */
static void
cone_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ix, double nr, double exp_rows,
		  double rows, double corr, double loops, Cost recheck,
		  Cost *startup, Cost *total, bool *bitmap)
{
	double		cand = fmax(exp_rows, rows);
	double		ituples = fmax(ix->tuples, 1.0);
	double		ipages = fmax((double) ix->pages, 1.0);
	double		T = fmax((double) rel->pages, 1.0);
	double		rpp = (rel->tuples > 0 && rel->pages > 0) ? rel->tuples / rel->pages : 100.0;
	double		descent = (ceil(log2(ituples)) + (Max(ix->tree_height, 0) + 1) * 50.0)
		* cpu_operator_cost;
	double		index_pages = nr + cand * ipages / ituples;
	Cost		common,
				index_io,
				ordered,
				bm;

	/* a descent per range, then the leaf pages the ranges hold */
	if (loops > 1.0)
		index_io = index_pages_fetched(index_pages * loops, ix->pages, ipages, root)
			* random_page_cost / loops;
	else
		index_io = index_pages * random_page_cost;
	common = nr * descent
		+ index_io
		+ cand * (cpu_index_tuple_cost + 2.0 * cpu_operator_cost)
		+ cand * (cpu_tuple_cost + rel->baserestrictcost.per_tuple + recheck)
		+ rows * rel->reltarget->cost.per_tuple;

	/*
	 * Both modes read the same few pages, in order, when the heap follows the
	 * cell order (io_clustered: one random page to enter each range, then
	 * sequential); they part when it does not -- an ordered scan then pays a
	 * page per row, a bitmap scan each distinct page once.  Each is
	 * interpolated from its uncorrelated cost towards io_clustered by
	 * correlation squared, as cost_index does; the bitmap also pays for being
	 * built, so on a clustered heap the ordered scan is the cheaper one.
	 */
	{
		double		min_pages = fmin(T, nr + cand / rpp);
		Cost		io_clustered,
					ordered_uncorr,
					bitmap_uncorr;
		double		pages;
		double		per_page;
		double		c2 = corr * corr;

		if (loops > 1.0)
		{
			/* the loops' pages together, charged per loop, at random cost */
			io_clustered = index_pages_fetched(min_pages * loops, rel->pages, ipages, root)
				* random_page_cost / loops;
			ordered_uncorr = index_pages_fetched(cand * loops, rel->pages, ipages, root)
				* random_page_cost / loops;
			pages = index_pages_fetched(cand * loops, rel->pages, ipages, root) / loops;
		}
		else
		{
			io_clustered = nr * random_page_cost + fmax(0.0, min_pages - nr) * seq_page_cost;
			ordered_uncorr = index_pages_fetched(cand, rel->pages, ipages, root) * random_page_cost;
			pages = fmin(T, (2.0 * T * cand) / (2.0 * T + cand));
		}
		per_page = (pages >= 2.0)
			? random_page_cost - (random_page_cost - seq_page_cost) * sqrt(pages / T)
			: random_page_cost;
		bitmap_uncorr = pages * per_page;

		ordered = ordered_uncorr + c2 * (io_clustered - ordered_uncorr);
		bm = bitmap_uncorr + c2 * (io_clustered - bitmap_uncorr)
			+ cand * 0.1 * cpu_operator_cost;
	}

	*bitmap = bm < ordered;
	*startup = rel->baserestrictcost.startup + rel->reltarget->cost.startup;
	*total = *startup + common + Min(ordered, bm);
}

/*
 * A path whose covering is computed at execution: per outer row for a
 * cross-match cone (ppi, the parameterization by the outer relations), per
 * rescan for a run-time one (ppi NULL).  Costed from coverings sampled at
 * plan time (join_cone_estimate()).
 */
static void
add_runtime_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte, FuncExpr *f,
				 ParamPathInfo *ppi)
{
	Node	   *cell = linitial(f->args);
	IndexOptInfo *ix;
	join_estimate e;
	CustomPath *cp;
	bool		bitmap;
	QualCost	recheck = {0, 0};
	double		rows = ppi ? ppi->ppi_rows : rel->rows;
	AttrNumber	attnum = IsA(cell, Var) ? ((Var *) cell)->varattno : 1;

	if ((ix = cell_index(rel, cell)) == NULL || !join_cone_estimate(root, f->args, &e))
		return;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = ppi;
	cp->path.parallel_aware = false;

	/*
	 * Not parallel aware -- one scan does not split its cone among workers
	 * -- but parallel safe: under a Parallel Seq Scan of the probes, each
	 * worker covers and scans the cones of its own outer rows, as it would
	 * an inner index scan.  The executor only reads: the index, the heap,
	 * and the density from the catalogue and the density map.
	 */
	cp->path.parallel_safe = rel->consider_parallel &&
		is_parallel_safe(root, (Node *) f) &&
		(ppi == NULL || is_parallel_safe(root, (Node *) ppi->ppi_clauses));
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.pathkeys = NIL;
	/* the cone itself is rechecked on every candidate, as a join clause */
	if (ppi)
		cost_qual_eval(&recheck, ppi->ppi_clauses, root);
	cone_cost(root, rel, ix, e.nranges, e.cand, rows, cell_correlation(rte, ix, cell),
			  ppi ? cone_loop_count(root, ppi->ppi_req_outer) : 1.0, recheck.per_tuple,
			  &cp->path.startup_cost, &cp->path.total_cost, &bitmap);
	cp->path.startup_cost += recheck.startup;
	cp->path.total_cost += recheck.startup;
	/* the covering itself, each time: ten operator calls a classified cell */
	cp->path.total_cost += e.steps * 10.0 * cpu_operator_cost;
	cp->flags = 0;
	cp->custom_paths = NIL;
	/* no bounds: cone_begin() then computes the covering each time */
	cp->custom_private = list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
											  ObjectIdGetDatum(ix->indexoid), false, true),
									makeNullConst(INT8ARRAYOID, -1, InvalidOid),
									makeBoolConst(bitmap, false), f);
	cp->custom_private = lappend(cp->custom_private,
								 list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
													  ObjectIdGetDatum(rte->relid), false, true),
											makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
													  ObjectIdGetDatum(e.statrel), false, true),
											makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
													  Int16GetDatum(attnum), false, true),
											makeBoolConst(ppi != NULL, false)));
	cp->methods = &cone_path_methods;
	add_path(rel, &cp->path);
}

static void
cone_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	if (!skycell_custom_scan || rel->indexlist == NIL)
		return;
	if (rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	if (rte->rtekind != RTE_RELATION || rte->inh ||
		(rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW))
		return;

	/*
	 * The scan slot's type is fixed when the scan state is created, before
	 * the relation is open, and the quals are compiled for it: heap only.
	 */
	{
		Relation	r = table_open(rte->relid, NoLock);	/* locked by the parser */
		bool		heap = table_slot_callbacks(r) == &TTSOpsBufferHeapTuple;

		table_close(r, NoLock);
		if (!heap)
			return;
	}

	add_nearest_path(root, rel, rte);
	if (!OidIsValid(cone_funcid))
		return;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		FuncExpr   *f = (FuncExpr *) ri->clause;
		IndexOptInfo *ix;
		sc_region	reg;
		sc_cover	cov;
		sc_density	dens;
		CustomPath *cp;
		Datum	   *b;
		Const	   *bounds;
		bool		bitmap;

		if (ri->pseudoconstant || !IsA(f, FuncExpr) || f->funcid != cone_funcid ||
			list_length(f->args) != 6)
			continue;
		if (cone_kind(f->args, rel->relid) == CONE_RUNTIME)
		{
			add_runtime_path(root, rel, rte, f, NULL);
			continue;
		}
		if ((ix = cell_index(rel, linitial(f->args))) == NULL)
			continue;
		if (!skycell_const_cone_cover(root, f->args, &reg, &cov, &dens))
			continue;

		/* copy the ranges out of the covering memo straight away */
		b = palloc(sizeof(Datum) * Max(2 * cov.n, 1));
		for (int i = 0; i < cov.n; i++)
		{
			b[2 * i] = Int64GetDatum(cov.r[i].lo);
			b[2 * i + 1] = Int64GetDatum(cov.r[i].hi);
		}
		bounds = makeConst(INT8ARRAYOID, -1, InvalidOid, -1,
						   PointerGetDatum(construct_array_builtin(b, 2 * cov.n, INT8OID)),
						   false, false);

		cp = makeNode(CustomPath);
		cp->path.pathtype = T_CustomScan;
		cp->path.parent = rel;
		cp->path.pathtarget = rel->reltarget;
		cp->path.param_info = NULL;
		cp->path.parallel_aware = false;
		/* a constant cone is a restriction clause: consider_parallel judged it */
		cp->path.parallel_safe = rel->consider_parallel;
		cp->path.parallel_workers = 0;
		cp->path.rows = rel->rows;
		cp->path.pathkeys = NIL;
		cone_cost(root, rel, ix, cov.n, cov.exp_rows, rel->rows,
				  cell_correlation(rte, ix, linitial(f->args)), 1.0, 0,
				  &cp->path.startup_cost, &cp->path.total_cost, &bitmap);
		cp->flags = 0;
		cp->custom_paths = NIL;
		/* the clause this path answers rides along for cone_plan(), which drops it */
		cp->custom_private = list_make4(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
												  ObjectIdGetDatum(ix->indexoid), false, true),
										bounds, makeBoolConst(bitmap, false), f);
		cp->methods = &cone_path_methods;
		add_path(rel, &cp->path);
	}

	/*
	 * A cross-match cone is a join clause: offer the scan parameterized by the
	 * relations its centre and radius read, for the inner side of a nested
	 * loop, where each outer row gets its own covering.
	 */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);
		FuncExpr   *f = (FuncExpr *) ri->clause;
		Relids		inner,
					outer;

		if (ri->pseudoconstant || !IsA(f, FuncExpr) || f->funcid != cone_funcid ||
			list_length(f->args) != 6 || !join_clause_is_movable_to(ri, rel))
			continue;
		inner = pull_varnos(root, (Node *) list_make3(linitial(f->args), lsecond(f->args),
													 lthird(f->args)));
		outer = pull_varnos(root, (Node *) list_make3(list_nth(f->args, 3), list_nth(f->args, 4),
													 list_nth(f->args, 5)));
		if (bms_is_empty(inner) || !bms_is_subset(inner, rel->relids) ||
			bms_overlap(outer, rel->relids))
			continue;
		outer = bms_union(bms_difference(ri->clause_relids, rel->relids), rel->lateral_relids);
		if (bms_is_empty(outer))
			continue;
		add_runtime_path(root, rel, rte, f, get_baserel_parampathinfo(root, rel, outer));
	}
}

/* ------------------------------------------------------------------ */
/* nearest neighbours: ORDER BY distance(...) [LIMIT k]               */
/* ------------------------------------------------------------------ */

/*
 * A B-tree on the cell has no notion of distance, so a query ordered by the
 * distance to a point gets no index path from PostgreSQL: it sorts the whole
 * relation.  SkycellNearest returns the rows in distance order from the cell
 * index itself, ring by ring.  It covers the cone of radius r1 around the
 * centre, reads its rows, keeps those with distance <= r1, sorts them and
 * returns them; then the cone of radius r2 > r1, keeping r1 < distance <= r2;
 * and so on.  Each ring's rows all come after the previous ring's, so the
 * output is in order, and a LIMIT above stops the scan at the ring that
 * completes it.  The first radius is the one the density expects to hold the
 * rows the query asks for; each later one grows the area by what the rings so
 * far found.  A ring that would reach most of the relation is the last one,
 * read sequentially: it returns every row not yet returned, those with a NULL
 * or NaN distance included, in the order ORDER BY gives them.
 *
 * The distance kept is the ORDER BY expression itself, evaluated per row, so
 * the order is the one a Sort would produce.  The cones are covered a hair
 * wider than the ring, as for a distance comparison (adql.c), so no row on a
 * ring's edge can be missed, and less the cells wholly inside the previous
 * ring's cone, whose rows were all returned: a ring reads its own band and
 * the cells the previous edge crossed.  The query's other conditions are
 * tested as a ring is read, so the rows they reject are never sorted.
 *
 * Rows come out of the ring's buffer, not the heap, so a query that reads a
 * system column of the relation (ctid, a row lock) is left to PostgreSQL.
 */

/* does the query read a system column of rel? */
static bool
reads_system_column(RelOptInfo *rel)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;
	int			x = -1;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach(lc, rel->baserestrictinfo)
		pull_varattnos((Node *) lfirst_node(RestrictInfo, lc)->clause, rel->relid, &attrs);
	while ((x = bms_next_member(attrs, x)) >= 0)
		if (x + FirstLowInvalidHeapAttributeNumber < 0)
			return true;
	return false;
}

/* ascending, NULLs last, by float8's own ordering: what the scan produces */
static bool
nearest_pathkey(PathKey *pk)
{
#if PG_VERSION_NUM >= 180000
	if (pk->pk_cmptype != COMPARE_LT)
		return false;
#else
	if (pk->pk_strategy != BTLessStrategyNumber)
		return false;
#endif
	return !pk->pk_nulls_first &&
		get_opfamily_member(pk->pk_opfamily, FLOAT8OID, FLOAT8OID,
							BTLessStrategyNumber) == Float8LessOperator;
}

/* the fewest rows a first ring is sized for */
#define NEAREST_MIN_ROWS 16.0
/* a ring is sized for this many times the rows the query is expected to read */
#define NEAREST_MARGIN 1.25
/* the rows a LIMIT is taken to ask for when its count is a parameter */
#define NEAREST_UNKNOWN_LIMIT 100.0

/*
 * The cost of one ring of c candidate rows: the cone scan's own cost for
 * them, the distance on each, and their sort.
 */
static void
nearest_ring_cost(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ix, double corr,
				  Cost per_row, double c, Cost *cost, bool *bitmap)
{
	Cost		startup,
				total;

	/* a ring is a few ranges, whatever its size: covering steps and descents */
	cone_cost(root, rel, ix, 4.0, c, c, corr, 1.0, per_row, &startup, &total, bitmap);
	*cost = total + 30.0 * 10.0 * cpu_operator_cost
		+ c * (cpu_tuple_cost + 2.0 * cpu_operator_cost * log2(fmax(c, 2.0)));
}

/*
 * The radius the relation's conditions confine the answer to, when they say
 * the distance ordered by is below one: the comparison itself (as
 * skycell_distance_below(), adql.c's spelling of it once a cone carries the
 * estimate, or as written), or a cone round the same centre.  The radius
 * expressions, each read once by the scan, which then never reads past the
 * least of them: without that, the 10 nearest within 1 arcmin of a patch that
 * holds fewer would have the rings widen to the whole relation.
 */
static double cap_area(double r);

static bool
radius_expr_ok(PlannerInfo *root, RelOptInfo *rel, Node *r)
{
	return exprType(r) == FLOAT8OID && !contain_volatile_functions(r) &&
		!contain_subplans(r) && !bms_is_member(rel->relid, pull_varnos(root, r));
}

static List *
nearest_bounds(PlannerInfo *root, RelOptInfo *rel, Node *dist, Node *ra0, Node *dec0)
{
	List	   *bounds = NIL;
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		Node	   *q = (Node *) lfirst_node(RestrictInfo, lc)->clause;
		Node	   *r = NULL;

		if (IsA(q, FuncExpr))
		{
			FuncExpr   *f = (FuncExpr *) q;
			char	   *name = get_func_name(f->funcid);
			int			n = list_length(f->args);

			if (name == NULL)
				continue;
			if (strcmp(name, "skycell_distance_below") == 0 && n == 3 &&
				equal(linitial(f->args), dist))
				r = lsecond(f->args);
			else if (strcmp(name, "skycell_cone") == 0 && n == 6 &&
					 equal(list_nth(f->args, 3), ra0) && equal(list_nth(f->args, 4), dec0))
				r = list_nth(f->args, 5);
			else if (strcmp(name, "skycell_in_cone") == 0 && n >= 5 &&
					 equal(lthird(f->args), ra0) && equal(lfourth(f->args), dec0))
				r = list_nth(f->args, 4);
		}
		else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
		{
			OpExpr	   *op = (OpExpr *) q;
			Oid			fn = OidIsValid(op->opfuncid) ? op->opfuncid : get_opcode(op->opno);

			if ((fn == F_FLOAT8LT || fn == F_FLOAT8LE) && equal(linitial(op->args), dist))
				r = lsecond(op->args);
			else if ((fn == F_FLOAT8GT || fn == F_FLOAT8GE) && equal(lsecond(op->args), dist))
				r = linitial(op->args);
		}
		if (r != NULL && radius_expr_ok(root, rel, r))
			bounds = lappend(bounds, copyObject(r));
	}
	return bounds;
}

static void
add_nearest_path(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	PathKey    *pk;
	ListCell   *lc;

	if (!skycell_nearest_scan || root->query_pathkeys == NIL)
		return;
	pk = linitial_node(PathKey, root->query_pathkeys);
	if (!nearest_pathkey(pk) || reads_system_column(rel))
		return;

	foreach(lc, pk->pk_eclass->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Node	   *expr = (Node *) em->em_expr;
		Node	   *cell,
				   *ra0,
				   *dec0;
		IndexOptInfo *ix;
		sc_density	dens;
		Oid			statrel = InvalidOid;
		bool		uses_cell_ops;
		CustomPath *cp;
		QualCost	dist_cost;
		double		n = fmax(rel->tuples, 1.0),
					sel = fmin(1.0, fmax(rel->rows / n, 1e-9)),
					want,
					k0,
					corr;
		Cost		first,
					all;
		bool		bitmap,
					bitmap_all;
		List	   *bounds;
		ListCell   *lb;
		double		bound = -1;

		if (em->em_is_const || !bms_equal(em->em_relids, rel->relids))
			continue;
		while (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		/* the cell expression, or a stored column holding it (README, setup 2) */
		if (!skycell_distance_order(expr, rel->relid, &cell, &ra0, &dec0) ||
			!skycell_cell_density(root, &cell, &dens, &statrel, &uses_cell_ops) ||
			(ix = cell_index(rel, cell)) == NULL)
			continue;

		/*
		 * The first ring holds the rows the query reads -- the LIMIT, over the
		 * fraction of rows the other conditions keep -- with a margin, so that it
		 * usually completes the query alone; a LIMIT whose count is a
		 * parameter is taken as NEAREST_UNKNOWN_LIMIT rows, and the rings
		 * grow from there if it asks for more.  Without a LIMIT it is the
		 * whole relation, read through the index and sorted: never cheaper
		 * than the sequential scan and sort PostgreSQL plans itself.
		 */
		if (root->limit_tuples > 0)
			want = root->limit_tuples;
		else if (root->parse->limitCount != NULL &&
				 !(IsA(root->parse->limitCount, Const) &&
				   ((Const *) root->parse->limitCount)->constisnull))
			want = NEAREST_UNKNOWN_LIMIT;	/* LIMIT $1: the rings will grow */
		else
			want = rel->rows;
		k0 = fmin(n, fmax(NEAREST_MIN_ROWS, NEAREST_MARGIN * want / sel));

		/*
		 * A radius the conditions bound the answer to caps the first ring --
		 * by the rows a cone of it holds at the relation's mean density, when
		 * it is a constant -- and there is no reading past it.
		 */
		bounds = nearest_bounds(root, rel, expr, ra0, dec0);
		foreach(lb, bounds)
		{
			Const	   *b = (Const *) lfirst(lb);

			if (IsA(b, Const) && !b->constisnull &&
				(bound < 0 || DatumGetFloat8(b->constvalue) < bound))
				bound = fmax(0.0, DatumGetFloat8(b->constvalue));
		}
		if (bound >= 0)
			k0 = fmin(k0, fmax(NEAREST_MIN_ROWS, n * cap_area(bound) / (4.0 * M_PI)));
		corr = cell_correlation(rte, ix, cell);
		cost_qual_eval_node(&dist_cost, expr, root);
		nearest_ring_cost(root, rel, ix, corr, dist_cost.per_tuple, k0, &first, &bitmap);
		nearest_ring_cost(root, rel, ix, corr, dist_cost.per_tuple, n, &all, &bitmap_all);

		cp = makeNode(CustomPath);
		cp->path.pathtype = T_CustomScan;
		cp->path.parent = rel;
		cp->path.pathtarget = rel->reltarget;
		cp->path.param_info = NULL;
		cp->path.parallel_aware = false;
		cp->path.parallel_safe = rel->consider_parallel && is_parallel_safe(root, expr);
		cp->path.parallel_workers = 0;
		cp->path.rows = rel->rows;
		cp->path.pathkeys = list_make1(pk);
		/*
		 * Nothing comes out before the first ring is sorted.  Reading on
		 * past it costs about the whole relation's ring again, twice over
		 * for the cells each ring's edge shares with the next.
		 */
		cp->path.startup_cost = first;
		cp->path.total_cost = (k0 >= n || bound >= 0) ? first : first + 2.0 * all;
		/* ExecScan projects, so a computed target needs no Result above */
		cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
		cp->custom_paths = NIL;
		cp->custom_private = list_make5(makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
												  ObjectIdGetDatum(ix->indexoid), false, true),
										makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
												  ObjectIdGetDatum(statrel), false, true),
										makeBoolConst(bitmap, false),
										list_make3(float8_const(k0), float8_const(want),
												   makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
															 Int16GetDatum(IsA(cell, Var) ? ((Var *) cell)->varattno : 1),
															 false, true)),
										list_concat(list_make3(expr, ra0, dec0), bounds));
		cp->methods = &nearest_path_methods;
		add_path(rel, &cp->path);
		return;
	}
}

/*
 * The plan: every condition on the relation stays a qual, applied to the rows
 * as they come out in order.  custom_private is laid out as cone_begin()
 * reads a run-time cone's -- (index, NULL bounds, mode, no index filter,
 * relation, statistics relation, attribute, not joined) -- then the first
 * ring's rows and the rows the query was planned to read; custom_exprs holds
 * the centre, the ORDER BY expression and the radii the answer is bounded by.
 */
static Plan *
nearest_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *p = best_path->custom_private;
	List	   *e = (List *) list_nth(p, 4);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = extract_actual_clauses(clauses, false);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_private = list_make5(linitial(p),
									makeNullConst(INT8ARRAYOID, -1, InvalidOid),
									lthird(p),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(0), false, true),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(0), false, true));
	cs->custom_private = lappend(cs->custom_private,
								 makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
										   ObjectIdGetDatum(rte->relid), false, true));
	cs->custom_private = lappend(cs->custom_private, lsecond(p));
	cs->custom_private = lappend(cs->custom_private, lthird((List *) lfourth(p)));
	cs->custom_private = lappend(cs->custom_private, makeBoolConst(false, false));
	cs->custom_private = lappend(cs->custom_private, linitial((List *) lfourth(p)));
	cs->custom_private = lappend(cs->custom_private, lsecond((List *) lfourth(p)));
	cs->custom_exprs = list_concat(list_make3(copyObject(lsecond(e)), copyObject(lthird(e)),
											  copyObject(linitial(e))),
								   copyObject(list_copy_tail(e, 3)));
	cs->custom_scan_tlist = NIL;
	cs->methods = &nearest_scan_methods;
	return &cs->scan.plan;
}

static Oid
in_cone_oid(void)
{
	if (in_cone_for != cone_funcid || !OidIsValid(in_cone_funcid))
	{
		Oid			types[6] = {FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID};

		in_cone_funcid = lookup_sibling_func(cone_funcid, "skycell_in_cone", 6, types);
		in_cone_for = cone_funcid;
	}
	return in_cone_funcid;
}

/*
 * The cone this scan is driven by is rechecked with skycell_in_cone(ra, dec,
 * ...): the ranges never return a row whose cell is NULL, which is all
 * skycell_cone's strictness on that argument would add, and the cell
 * expression need not be computed per row.  Any other cone qual on the same
 * relation is kept as written.
 */

/*
 * The index attribute holding column arg of relation rel (a plain float8
 * column, as skycell_in_cone reads it), or 0.  Any column past the cell key
 * will do -- an INCLUDE column or a further key -- as long as the index can
 * return it.
 */
static AttrNumber
index_column_of(RelOptInfo *rel, Oid indexoid, Node *arg)
{
	Var		   *v = (Var *) arg;
	ListCell   *lc;

	if (!IsA(arg, Var) || v->varno != rel->relid || v->varlevelsup != 0 ||
		v->varattno <= 0 || v->vartype != FLOAT8OID)
		return 0;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ix = (IndexOptInfo *) lfirst(lc);

		if (ix->indexoid != indexoid)
			continue;
		for (int k = 1; k < ix->ncolumns; k++)
			if (ix->indexkeys[k] == v->varattno && ix->canreturn[k])
				return (AttrNumber) (k + 1);
	}
	return 0;
}

static Plan *
cone_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		  List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	FuncExpr   *driving = (FuncExpr *) lfourth(best_path->custom_private);
	Oid			indexoid = DatumGetObjectId(linitial_node(Const, best_path->custom_private)->constvalue);
	AttrNumber	ira = 0,
				idec = 0;
	List	   *quals = NIL;
	ListCell   *lc;

	if (skycell_index_filter)
	{
		ira = index_column_of(rel, indexoid, lsecond(driving->args));
		idec = index_column_of(rel, indexoid, lthird(driving->args));
		if (ira == 0 || idec == 0)
			ira = idec = 0;
	}

	foreach(lc, clauses)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

		if (ri->pseudoconstant)
			continue;
		if ((Node *) ri->clause == (Node *) driving)
		{
			List	   *a = list_copy_tail(copyObject(driving->args), 1);

			a = lappend(a, makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
									 Float8GetDatum(-1.0), false, true));
			quals = lappend(quals, makeFuncExpr(in_cone_oid(), BOOLOID, a, InvalidOid,
												InvalidOid, COERCE_EXPLICIT_CALL));
		}
		else
			quals = lappend(quals, ri->clause);
	}

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = quals;
	cs->scan.scanrelid = rel->relid;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = NIL;
	cs->custom_private = list_make5(linitial(best_path->custom_private),
									lsecond(best_path->custom_private),
									lthird(best_path->custom_private),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(ira), false, true),
									makeConst(INT2OID, -1, InvalidOid, sizeof(int16),
											  Int16GetDatum(idec), false, true));
	/* a constant cone's centre and radius, for the index filter */
	if (ira != 0)
		cs->custom_exprs = list_copy_tail(copyObject(driving->args), 3);
	if (list_length(best_path->custom_private) > 4)
	{
		/*
		 * A cross-match cone: centre and radius are evaluated per outer row,
		 * createplan having turned the outer relation's columns into nestloop
		 * parameters, and the density they are covered against is loaded at
		 * execution from (relid, statistics relation, attnum).
		 */
		cs->custom_exprs = list_copy_tail(copyObject(driving->args), 3);
		cs->custom_private = list_concat(cs->custom_private,
										 (List *) list_nth(best_path->custom_private, 4));
	}
	cs->custom_scan_tlist = NIL;
	cs->methods = &cone_scan_methods;
	return &cs->scan.plan;
}

/* ------------------------------------------------------------------ */
/* execution                                                          */
/* ------------------------------------------------------------------ */

static Node *
cone_create_state(CustomScan *cscan)
{
	ConeScanState *st = (ConeScanState *) newNode(sizeof(ConeScanState), T_CustomScanState);

	st->css.methods = &cone_exec_methods;
	st->css.slotOps = &TTSOpsBufferHeapTuple;	/* checked heap at plan time */
	return (Node *) st;
}

static void
cone_begin(CustomScanState *node, EState *estate, int eflags)
{
	ConeScanState *st = (ConeScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Const	   *oidc = linitial_node(Const, cscan->custom_private);
	Const	   *bc = lsecond_node(Const, cscan->custom_private);
	Const	   *modec = lthird_node(Const, cscan->custom_private);
	Relation	heap = node->ss.ss_currentRelation;
	LOCKMODE	lockmode = exec_rt_fetch(cscan->scan.scanrelid, estate)->rellockmode;
	Oid			opfamily;

	st->indexoid = DatumGetObjectId(oidc->constvalue);
	if (bc->constisnull)
	{
		/* a cross-match cone: covered per outer row, by cone_cover_row() */
		st->runtime = true;
		st->need_cover = true;
		st->nranges = 0;
		st->bounds = NULL;
		st->relid = DatumGetObjectId(list_nth_node(Const, cscan->custom_private, 5)->constvalue);
		st->statrel = DatumGetObjectId(list_nth_node(Const, cscan->custom_private, 6)->constvalue);
		st->attnum = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 7)->constvalue);
		st->joined = DatumGetBool(list_nth_node(Const, cscan->custom_private, 8)->constvalue);
	}
	else
	{
		ArrayType  *arr = DatumGetArrayTypeP(bc->constvalue);

		st->nranges = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr)) / 2;
		st->bounds = (const int64 *) ARR_DATA_PTR(arr);
	}
	st->args = ExecInitExprList(cscan->custom_exprs, &node->ss.ps);
	st->ira = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 3)->constvalue);
	st->idec = DatumGetInt16(list_nth_node(Const, cscan->custom_private, 4)->constvalue);
	st->bitmap = DatumGetBool(modec->constvalue);
	st->cur = -1;
	st->index = index_open(st->indexoid, lockmode);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	opfamily = st->index->rd_opfamily[0];
	ScanKeyInit(&st->keys[0], 1, BTGreaterEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTGreaterEqualStrategyNumber)),
				Int64GetDatum(0));
	ScanKeyInit(&st->keys[1], 1, BTLessEqualStrategyNumber,
				get_opcode(get_opfamily_member(opfamily, INT8OID, INT8OID,
											   BTLessEqualStrategyNumber)),
				Int64GetDatum(0));
	if (table_slot_callbacks(heap) != &TTSOpsBufferHeapTuple)
		elog(ERROR, "skycell: custom cone scan on a non-heap relation");
	/*
	 * PostgreSQL 18 gave index_beginscan()/_bitmap() an instrumentation
	 * argument (EXPLAIN's index search counts) and has the table AM walk a
	 * bitmap itself, so its heap scan is begun with the first bitmap, as
	 * BitmapHeapScan does.
	 */
	/*
	 * With the index filter the entries are read one by one, with their
	 * included columns, in either mode: the bitmap mode then collects only
	 * the TIDs that pass, instead of every TID of every range.
	 */
#if PG_VERSION_NUM >= 180000
	if (st->bitmap && st->ira == 0)
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, NULL, 2);
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, NULL, 2, 0);
#else
	if (st->bitmap && st->ira == 0)
		st->iscan = index_beginscan_bitmap(st->index, estate->es_snapshot, 2);
	else
		st->iscan = index_beginscan(heap, st->index, estate->es_snapshot, 2, 0);
	/* PostgreSQL 17 alone asks whether the scan returns tuples: it does */
	if (st->bitmap)
#if PG_VERSION_NUM >= 170000
		st->hscan = table_beginscan_bm(heap, estate->es_snapshot, 0, NULL, true);
#else
		st->hscan = table_beginscan_bm(heap, estate->es_snapshot, 0, NULL);
#endif
#endif
	if (st->ira != 0)
		st->iscan->xs_want_itup = true;
}

/* the cone the index filter tests, from its centre and radius in degrees */
static void
cone_set_centre(ConeScanState *st, double ra0, double dec0, double radius)
{
	double		thr = radius < 0 ? -1.0
		: (radius >= 180.0 ? 2.0 : pow(sin(radius * M_PI / 360.0), 2));

	st->cra = ra0;
	st->cdec = dec0;
	st->cosdec0 = cos(dec0 * M_PI / 180.0);

	/*
	 * A hair wider than skycell_in_cone's threshold, so that rounding can
	 * only let a borderline entry through, never hold one back: the plan's
	 * qual still applies the exact test to every row the filter passes.
	 */
	st->thr = thr < 0 ? thr : thr * (1.0 + 1e-9) + 1e-300;
	st->have_centre = true;
}

/*
 * The index filter's test of the current index entry: skycell_in_cone's
 * haversine on the included ra and dec.  A NULL coordinate fails, as the
 * strict exact test would.
 */
static bool
cone_entry_passes(ConeScanState *st)
{
	IndexScanDesc s = st->iscan;
	bool		n1,
				n2;
	double		ra,
				dec,
				sdd,
				sda;

	ra = DatumGetFloat8(index_getattr(s->xs_itup, st->ira, s->xs_itupdesc, &n1));
	dec = DatumGetFloat8(index_getattr(s->xs_itup, st->idec, s->xs_itupdesc, &n2));
	if (!n1 && !n2)
	{
		sdd = sin((dec - st->cdec) * M_PI / 360.0);
		sda = sin((ra - st->cra) * M_PI / 360.0);
		if (sdd * sdd + cos(dec * M_PI / 180.0) * st->cosdec0 * sda * sda <= st->thr)
			return true;
	}
	/*
	 * Counted in the node's instrumentation, not the scan state: a parallel
	 * query adds its workers' counts into the leader's (InstrAggNode), so
	 * EXPLAIN ANALYZE reports them all however the probes were shared out.
	 */
	InstrCountFiltered2(&st->css.ss.ps, 1);
	return false;
}

/* the next TID of the current range whose entry passes the index filter */
static ItemPointer
cone_next_tid(ConeScanState *st)
{
	ItemPointer tid;

	while ((tid = index_getnext_tid(st->iscan, ForwardScanDirection)) != NULL)
		if (cone_entry_passes(st))
			return tid;
	return NULL;
}

static void
cone_load_density(ConeScanState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old;

	/*
	 * The density points into the backend's cache, which an invalidation
	 * processed since the last row -- by the outer side, say -- may have
	 * freed.  Evaluating the arguments can do that too, so callers check
	 * after them; nothing between this and the covering takes a lock.
	 */
	if (st->have_dens && st->dens_generation == skycell_density_generation())
		return;
	old = MemoryContextSwitchTo(estate->es_query_cxt);
	if (st->have_dens && st->dens.bounds)
		pfree((void *) st->dens.bounds);	/* a copy in es_query_cxt */
	st->dens_generation = skycell_density_generation();
	skycell_load_density(st->relid, st->statrel, st->attnum, &st->dens);
	MemoryContextSwitchTo(old);
	st->have_dens = true;
}

/*
 * Cover the cone (ra0, dec0, radius) into the scan state's ranges, and return
 * the rows the density expects inside the cone itself.  The covering is built
 * in the per-tuple context, which ExecScan resets before every fetch, and the
 * ranges are copied out of it.
 */
static double
cone_cover_at(ConeScanState *st, double ra0, double dec0, double radius)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old;
	sc_region	reg;
	sc_cover	cov;
	sc_cover_params p;
	double		rows;

	cone_load_density(st);
	old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	check_err(sc_region_cone(&reg, ra0, dec0, radius, true));
	skycell_scan_params(&p, &st->dens);
	sc_cover_compute(&reg, &st->dens, &p, &cov);
	MemoryContextSwitchTo(old);
	if (cov.n > st->rt_cap)
	{
		st->rt_cap = Max(cov.n, 2 * st->rt_cap);
		st->rt_bounds = (st->rt_bounds == NULL)
			? MemoryContextAlloc(estate->es_query_cxt, sizeof(int64) * 2 * st->rt_cap)
			: repalloc(st->rt_bounds, sizeof(int64) * 2 * st->rt_cap);
	}
	for (int r = 0; r < cov.n; r++)
	{
		st->rt_bounds[2 * r] = cov.r[r].lo;
		st->rt_bounds[2 * r + 1] = cov.r[r].hi;
	}
	st->bounds = st->rt_bounds;
	st->nranges = cov.n;
	st->ncoverings += 1;
	st->nranges_total += cov.n;
	rows = (cov.area > 0) ? cov.exp_rows * fmin(1.0, reg.area / cov.area) : 0.0;
	sc_cover_free(&cov);
	return rows;
}

/*
 * A cross-match cone's covering for the current outer row: centre and radius
 * evaluated (the outer row's columns are nestloop parameters by now) and
 * covered against the relation's density with the custom scan's parameters.
 * NULL arguments match nothing, as the strict skycell_cone would; an invalid
 * centre is an error, as it is in the rewrite's run-time bounds and in
 * skycell_cone_ranges().
 */
static void
cone_cover_row(ConeScanState *st)
{
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	double		v[3];
	int			i = 0;
	ListCell   *lc;

	st->need_cover = false;
	st->nranges = 0;
	foreach(lc, st->args)
	{
		bool		isnull;
		Datum		d = ExecEvalExpr((ExprState *) lfirst(lc), econtext, &isnull);

		if (isnull)
			return;
		v[i++] = DatumGetFloat8(d);
	}
	if (st->ira != 0)
		cone_set_centre(st, v[0], v[1], v[2]);
	(void) cone_cover_at(st, v[0], v[1], v[2]);
}

/* point the scan keys at range i and restart the index scan there */
static void
cone_range(ConeScanState *st, int i)
{
	st->keys[0].sk_argument = Int64GetDatum(st->bounds[2 * i]);
	st->keys[1].sk_argument = Int64GetDatum(st->bounds[2 * i + 1]);
	index_rescan(st->iscan, st->keys, 2, NULL, 0);
}

/*
 * The next visible row of the current range whose entry passes the index
 * filter: index_getnext_slot() with the filter between the entry and the
 * heap, following a HOT chain as it does.
 */
static bool
cone_next_filtered(ConeScanState *st, TupleTableSlot *slot)
{
	for (;;)
	{
		if (!st->iscan->xs_heap_continue && cone_next_tid(st) == NULL)
			return false;
		if (index_fetch_heap(st->iscan, slot))
			return true;
	}
}

/* ordered: the next row of the current range, moving on when it runs out */
static TupleTableSlot *
cone_next_ordered(ConeScanState *st, TupleTableSlot *slot)
{
	for (;;)
	{
		if (st->cur >= st->nranges)
			return ExecClearTuple(slot);
		if (st->cur >= 0 &&
			(st->ira != 0 ? cone_next_filtered(st, slot)
			 : index_getnext_slot(st->iscan, ForwardScanDirection, slot)))
			return slot;
		if (++st->cur >= st->nranges)
			return ExecClearTuple(slot);
		cone_range(st, st->cur);
	}
}

/*
 * bitmap: on the first call collect every range's TIDs, then return the rows
 * page by page in physical order.  A lossy page returns all its rows; the
 * plan's quals hold the exact test, and the ranges cover the cone, so that
 * test alone decides -- no range condition needs rechecking.
 */
static void
cone_build_bitmap(ConeScanState *st)
{
	st->tbm = tbm_create(work_mem * (Size) 1024, NULL);
	for (int i = 0; i < st->nranges; i++)
	{
		cone_range(st, i);
		if (st->ira == 0)
			index_getbitmap(st->iscan, st->tbm);
		else
		{
			ItemPointer tid;

			while ((tid = cone_next_tid(st)) != NULL)
				tbm_add_tuples(st->tbm, tid, 1, false);
		}
	}
}

#if PG_VERSION_NUM >= 180000
/* PostgreSQL 18: the table AM walks the bitmap itself, through a read stream */
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	bool		recheck;
	uint64		lossy = 0,
				exact = 0;

	if (st->tbm == NULL)
	{
		TBMIterator it;

		cone_build_bitmap(st);
		it = tbm_begin_iterate(st->tbm, NULL, InvalidDsaPointer);
		if (st->hscan == NULL)
			st->hscan = table_beginscan_bm(st->css.ss.ss_currentRelation,
										   st->css.ss.ps.state->es_snapshot, 0, NULL);
		st->hscan->st.rs_tbmiterator = it;
	}
	if (table_scan_bitmap_next_tuple(st->hscan, slot, &recheck, &lossy, &exact))
		return slot;
	return ExecClearTuple(slot);
}
#else
static TupleTableSlot *
cone_next_bitmap(ConeScanState *st, TupleTableSlot *slot)
{
	if (st->tbm == NULL)
	{
		cone_build_bitmap(st);
		st->it = tbm_begin_iterate(st->tbm);
		st->tbmres = NULL;
	}
	for (;;)
	{
		if (st->tbmres == NULL)
		{
			if ((st->tbmres = tbm_iterate(st->it)) == NULL)
				return ExecClearTuple(slot);
			if (!table_scan_bitmap_next_block(st->hscan, st->tbmres))
			{
				st->tbmres = NULL;
				continue;
			}
		}
		if (table_scan_bitmap_next_tuple(st->hscan, st->tbmres, slot))
			return slot;
		st->tbmres = NULL;
	}
}
#endif

static TupleTableSlot *
cone_next(ScanState *ss)
{
	ConeScanState *st = (ConeScanState *) ss;

	if (st->need_cover)
		cone_cover_row(st);
	else if (st->ira != 0 && !st->have_centre)
	{
		/* a constant cone: its centre and radius are evaluated once */
		ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
		double		v[3];
		int			i = 0;
		ListCell   *lc;

		foreach(lc, st->args)
		{
			bool		isnull;

			v[i++] = DatumGetFloat8(ExecEvalExpr((ExprState *) lfirst(lc), econtext, &isnull));
			if (isnull)
				v[2] = -1.0;	/* matches nothing, as the strict test does */
		}
		cone_set_centre(st, v[0], v[1], v[2]);
	}
	return st->bitmap ? cone_next_bitmap(st, ss->ss_ScanTupleSlot)
		: cone_next_ordered(st, ss->ss_ScanTupleSlot);
}

/* the plan's quals hold the exact test; ExecScan applies them to EPQ rows too */
static bool
cone_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
cone_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, cone_next, cone_recheck);
}

/*
 * Releasing the bitmap mode, in the order each version's own BitmapHeapScan
 * uses.  On PostgreSQL 18 the iterator lives in the heap scan, and the heap
 * scan's read stream may still reference the bitmap's pages, so the iterator
 * is ended and the scan reset or ended before the bitmap is freed.
 */
static void
cone_release_bitmap(ConeScanState *st, bool end)
{
#if PG_VERSION_NUM >= 180000
	if (st->hscan)
	{
		if (!tbm_exhausted(&st->hscan->st.rs_tbmiterator))
			tbm_end_iterate(&st->hscan->st.rs_tbmiterator);
		if (end)
			table_endscan(st->hscan);
		else
			table_rescan(st->hscan, NULL);
	}
	if (st->tbm)
		tbm_free(st->tbm);
#else
	if (st->it)
		tbm_end_iterate(st->it);
	if (st->tbm)
		tbm_free(st->tbm);
	st->it = NULL;
	st->tbmres = NULL;
	if (st->hscan)
	{
		if (end)
			table_endscan(st->hscan);
		else
			table_rescan(st->hscan, NULL);
	}
#endif
	st->tbm = NULL;
	if (end)
		st->hscan = NULL;
}

static void
cone_end(CustomScanState *node)
{
	ConeScanState *st = (ConeScanState *) node;

	cone_release_bitmap(st, true);
	if (st->iscan)
		index_endscan(st->iscan);
	if (st->index)
		index_close(st->index, NoLock);
}

static void
cone_rescan(CustomScanState *node)
{
	ConeScanState *st = (ConeScanState *) node;

	st->cur = -1;
	st->need_cover = st->runtime;	/* new outer row, new covering */
	cone_release_bitmap(st, false);
	ExecScanReScan(&node->ss);
}

static void
cone_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ConeScanState *st = (ConeScanState *) node;

	ExplainPropertyText("Index", RelationGetRelationName(st->index), es);
	if (!st->runtime)
		ExplainPropertyInteger("Ranges", NULL, st->nranges, es);
	else if (es->analyze && st->ncoverings > 0)
	{
		/*
		 * An average, from the coverings this process computed: in a parallel
		 * query the workers' are not sent back (only a parallel-aware node
		 * gets shared memory), so it is the leader's share of the probes.
		 */
		ExplainPropertyFloat(node->ss.ps.worker_instrument ? "Ranges per Covering (leader)"
							 : "Ranges per Covering", NULL,
							 st->nranges_total / st->ncoverings, 1, es);
	}
	else
		ExplainPropertyText("Ranges", st->joined ? "per outer row" : "per rescan", es);
	ExplainPropertyText("Mode", st->bitmap ? "bitmap" : "ordered", es);
	if (st->ira != 0)
	{
		TupleDesc	itd = RelationGetDescr(st->index);
		char	   *cols = psprintf("%s, %s",
									NameStr(TupleDescAttr(itd, st->ira - 1)->attname),
									NameStr(TupleDescAttr(itd, st->idec - 1)->attname));

		ExplainPropertyText("Index Filter", cols, es);
		if (es->analyze && node->ss.ps.instrument)
			ExplainPropertyFloat("Rows Removed by Index Filter", NULL,
								 node->ss.ps.instrument->nfiltered2, 0, es);
	}
}

/* ------------------------------------------------------------------ */
/* nearest neighbours: execution                                      */
/* ------------------------------------------------------------------ */

typedef struct NearestRow
{
	double		d;
	bool		isnull;
	HeapTuple	tup;
} NearestRow;

typedef struct NearestScanState
{
	ConeScanState cone;			/* must be first: the ranges and their scans */
	ExprState  *dist;			/* the ORDER BY expression */
	ExprState  *prequal;		/* the conditions safe to test in the ring */
	TupleTableSlot *fetch;		/* a row as the heap returns it */
	double		k0;				/* rows the first ring should hold */
	double		want;			/* rows the query was planned to read */
	double		n_inside,		/* rows read nearer than r, and of them */
				n_passed;		/* those the conditions kept */
	bool		started,
				final,			/* the next ring is the rest of the relation */
				done;			/* no ring after the current one */
	double		ra0,
				dec0;
	double		rprev,			/* the current ring is (rprev, r]; rprev < 0 */
				r;				/* for the first, which starts at the centre */
	double		bound;			/* no ring reads past this radius; < 0: none */
	bool		last;			/* the current ring reaches the bound */

	/*
	 * The current ring's rows: copies of their heap tuples with their
	 * distance, sorted in memory, while they fit in work_mem; past that, a
	 * tuplesort of the relation's columns and the distance, which can spill.
	 */
	MemoryContext ringcxt;
	NearestRow *rows;
	int			nrows,
				cap,
				next;
	Size		mem;
	Tuplesortstate *sort;
	TupleDesc	sortdesc;		/* the relation's columns, then the distance */
	TupleTableSlot *put,
			   *got;

	double		nrings,			/* for EXPLAIN ANALYZE */
				nread;
} NearestScanState;

static Node *
nearest_create_state(CustomScan *cscan)
{
	NearestScanState *ns = (NearestScanState *) newNode(sizeof(NearestScanState),
														T_CustomScanState);

	ns->cone.css.methods = &nearest_exec_methods;
	/* rows come out of the ring's buffer as heap tuples, not from a buffer page */
	ns->cone.css.slotOps = &TTSOpsHeapTuple;
	return (Node *) ns;
}

static void
nearest_begin(CustomScanState *node, EState *estate, int eflags)
{
	NearestScanState *ns = (NearestScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	heap = node->ss.ss_currentRelation;
	TupleDesc	hd = RelationGetDescr(heap);

	cone_begin(node, estate, eflags);
	ns->k0 = DatumGetFloat8(list_nth_node(Const, cscan->custom_private, 9)->constvalue);
	ns->want = DatumGetFloat8(list_nth_node(Const, cscan->custom_private, 10)->constvalue);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	/*
	 * Without a parent, so that it reads whichever slot it is handed -- the
	 * heap's, not the scan's own -- the way PostgreSQL compiles an index's
	 * expressions.  It holds no sub-plan (adql.c checks).
	 */
	ns->dist = ExecInitExpr((Expr *) lthird(cscan->custom_exprs), NULL);

	/*
	 * The relation's conditions, tested as a row is read: a row they reject
	 * is never copied or sorted, which matters when they reject the nearest
	 * rows -- each ring would otherwise sort rows only to have the scan's
	 * filter drop them.  Not a volatile one, which would then run twice on
	 * the rows the scan returns, and not one with a sub-plan, which needs the
	 * plan state; ExecScan still tests them all on the rows returned.
	 */
	{
		List	   *safe = NIL;
		ListCell   *lc;

		foreach(lc, cscan->scan.plan.qual)
		{
			Node	   *q = (Node *) lfirst(lc);

			if (!contain_volatile_functions(q) && !contain_subplans(q))
				safe = lappend(safe, q);
		}
		ns->prequal = safe ? ExecInitQual(safe, NULL) : NULL;
	}
	ns->fetch = table_slot_create(heap, &estate->es_tupleTable);
	ns->ringcxt = AllocSetContextCreate(estate->es_query_cxt, "skycell nearest ring",
										ALLOCSET_DEFAULT_SIZES);
	ns->sortdesc = CreateTemplateTupleDesc(hd->natts + 1);
	for (int i = 1; i <= hd->natts; i++)
		TupleDescCopyEntry(ns->sortdesc, i, hd, i);
	TupleDescInitEntry(ns->sortdesc, hd->natts + 1, "distance", FLOAT8OID, -1, 0);
	ns->put = ExecInitExtraTupleSlot(estate, ns->sortdesc, &TTSOpsVirtual);
	ns->got = ExecInitExtraTupleSlot(estate, ns->sortdesc, &TTSOpsMinimalTuple);
}

/* solid angle of a cone of radius r degrees */
static double
cap_area(double r)
{
	return 2.0 * M_PI * (1.0 - cos(fmin(r, 180.0) * M_PI / 180.0));
}

/* radius in degrees of a cone of solid angle a, 180 for the sky and beyond */
static double
cap_radius(double a)
{
	double		c = 1.0 - a / (2.0 * M_PI);

	return (c <= -1.0) ? 180.0 : acos(c) * 180.0 / M_PI;
}

/* one row into the tuplesort, as the relation's columns and the distance */
static void
nearest_put_sorted(NearestScanState *ns, HeapTuple tup, double d, bool isnull)
{
	int			natts = ns->sortdesc->natts - 1;

	ExecClearTuple(ns->put);
	heap_deform_tuple(tup, RelationGetDescr(ns->cone.css.ss.ss_currentRelation),
					  ns->put->tts_values, ns->put->tts_isnull);
	ns->put->tts_values[natts] = Float8GetDatum(d);
	ns->put->tts_isnull[natts] = isnull;
	ExecStoreVirtualTuple(ns->put);
	tuplesort_puttupleslot(ns->sort, ns->put);
}

/* the ring outgrew work_mem: what it holds so far goes into a tuplesort */
static void
nearest_spill(NearestScanState *ns)
{
	AttrNumber	key = ns->sortdesc->natts;
	Oid			lt = Float8LessOperator;
	Oid			coll = InvalidOid;
	bool		nulls_first = false;

	ns->sort = tuplesort_begin_heap(ns->sortdesc, 1, &key, &lt, &coll, &nulls_first,
									work_mem, NULL, TUPLESORT_NONE);
	for (int i = 0; i < ns->nrows; i++)
		nearest_put_sorted(ns, ns->rows[i].tup, ns->rows[i].d, ns->rows[i].isnull);
	ns->nrows = 0;
	MemoryContextReset(ns->ringcxt);
	ns->rows = NULL;
	ns->cap = 0;
}

/* keep a candidate row if it belongs to the current ring */
static void
nearest_consider(NearestScanState *ns, TupleTableSlot *row)
{
	ExprContext *econtext = ns->cone.css.ss.ps.ps_ExprContext;
	bool		isnull;
	double		d;
	Datum		v;
	HeapTuple	tup;
	MemoryContext old;

	ResetExprContext(econtext);
	econtext->ecxt_scantuple = row;
	v = ExecEvalExprSwitchContext(ns->dist, econtext, &isnull);
	d = isnull ? 0.0 : DatumGetFloat8(v);
	ns->nread += 1;
	if (!ns->final)
	{
		/* a NULL or NaN distance sorts last: the final ring's */
		if (isnull || isnan(d) || d > ns->r)
			return;
		if (ns->rprev >= 0 && !(d > ns->rprev))
			return;				/* an inner ring's, returned already */
		ns->n_inside += 1;
	}
	else if (!isnull && !isnan(d) && ns->rprev >= 0 && !(d > ns->rprev))
		return;
	if (ns->prequal && !ExecQual(ns->prequal, econtext))
	{
		InstrCountFiltered1(&ns->cone.css.ss.ps, 1);
		return;
	}
	ns->n_passed += 1;

	if (ns->sort == NULL)
	{
		old = MemoryContextSwitchTo(ns->ringcxt);
		tup = ExecCopySlotHeapTuple(row);
		ns->mem += HEAPTUPLESIZE + tup->t_len + sizeof(NearestRow);
		if (ns->nrows == ns->cap)
		{
			ns->cap = ns->cap ? 2 * ns->cap : 256;
			ns->rows = ns->rows ? repalloc(ns->rows, sizeof(NearestRow) * ns->cap)
				: palloc(sizeof(NearestRow) * ns->cap);
		}
		ns->rows[ns->nrows].d = d;
		ns->rows[ns->nrows].isnull = isnull;
		ns->rows[ns->nrows].tup = tup;
		ns->nrows++;
		MemoryContextSwitchTo(old);
		if (ns->mem > (Size) work_mem * 1024)
			nearest_spill(ns);
		return;
	}
	tup = ExecFetchSlotHeapTuple(row, false, NULL);
	nearest_put_sorted(ns, tup, d, isnull);
}

/*
 * By distance, as float8's ordering has it (NaN after every number, and equal
 * to NaN), NULL last.  A sort specialised for the rows, with the comparison
 * inline: through qsort()'s function pointer, sorting was a third of a ring.
 */
static inline int
nearest_row_cmp(const NearestRow *x, const NearestRow *y)
{
	if (x->isnull || y->isnull)
		return (int) x->isnull - (int) y->isnull;
	if (isnan(x->d) || isnan(y->d))
		return (int) isnan(x->d) - (int) isnan(y->d);
	return (x->d > y->d) - (x->d < y->d);
}

#define ST_SORT nearest_sort_rows
#define ST_ELEMENT_TYPE NearestRow
#define ST_COMPARE(a, b) nearest_row_cmp(a, b)
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

/*
 * The radius whose cone the density expects to hold k rows around the centre:
 * the density of the finest HEALPix cell round the centre that still holds a
 * few times k rows -- a crowded field gets a small first ring, empty sky a
 * wide one.  A lookup per order, where computing coverings to size the ring
 * cost more than reading it.
 */
static double
nearest_first_radius(NearestScanState *ns, double k, double n)
{
	const sc_density *d = &ns->cone.dens;
	int64_t		pix = sc_ang2pix(SC_MAX_ORDER, ns->ra0, ns->dec0);
	double		rho = n / (4.0 * M_PI);

	for (int m = 0; m <= SC_MAX_ORDER; m++)
	{
		int64_t		p = pix >> (2 * (SC_MAX_ORDER - m));
		double		rows = sc_density_rows(d, sc_pix_lo(m, p), sc_pix_hi(m, p));

		if (rows < 4.0 * k)
			break;
		rho = rows / (4.0 * M_PI / (12.0 * pow(4.0, m)));
	}
	return cap_radius(k / fmax(rho, 1e-300));
}

/*
 * The cells wholly inside the cone (ra0, dec0, r), as sorted, disjoint ranges
 * of order-29 cells (lo, hi pairs, palloc'd into *out): from the twelve base
 * cells down, refining only the cells the cone's edge crosses, to the order
 * whose cells are a sixteenth of r across.  The cap test's IN is never wrong,
 * so every row of these cells is nearer the centre than r.  Stops refining
 * after NEAREST_INTERIOR_STEPS classifications: fewer cells, still all inside.
 */
#define NEAREST_INTERIOR_STEPS 4096

static int
cone_interior(double ra0, double dec0, double r, int64 **out)
{
	sc_region	reg;
	struct
	{
		int			order;
		int64_t		pix;
	}			stack[12 + 3 * (SC_MAX_ORDER + 1)];
	int			top = 0,
				n = 0,
				cap = 64,
				steps = 0,
				maxo;
	int64	   *b = palloc(sizeof(int64) * 2 * cap);

	if (sc_region_cone(&reg, ra0, dec0, r, true) != NULL)
	{
		*out = b;
		return 0;
	}
	maxo = (int) floor(log2(1.0233267079464885 / fmax(r * M_PI / 180.0 / 16.0, 1e-12)));
	maxo = Max(0, Min(maxo, SC_MAX_ORDER));
	/* children pushed last to first, so that they come off in cell order */
	for (int i = 11; i >= 0; i--)
	{
		stack[top].order = 0;
		stack[top++].pix = i;
	}
	while (top > 0)
	{
		int			order = stack[--top].order;
		int64_t		pix = stack[top].pix;
		double		fo;
		sc_class	cls = sc_region_classify_cap(&reg, order, pix, &fo);

		steps++;
		if (cls == SC_IN)
		{
			int64		lo = sc_pix_lo(order, pix),
						hi = sc_pix_hi(order, pix);

			if (n > 0 && b[2 * n - 1] + 1 == lo)
				b[2 * n - 1] = hi;
			else
			{
				if (n == cap)
				{
					cap *= 2;
					b = repalloc(b, sizeof(int64) * 2 * cap);
				}
				b[2 * n] = lo;
				b[2 * n + 1] = hi;
				n++;
			}
		}
		else if (cls == SC_PARTIAL && order < maxo && steps < NEAREST_INTERIOR_STEPS)
		{
			for (int c = 3; c >= 0; c--)
			{
				stack[top].order = order + 1;
				stack[top++].pix = 4 * pix + c;
			}
		}
	}
	*out = b;
	return n;
}

/* ranges a less ranges x, both sorted, disjoint, inclusive (lo, hi) pairs */
static int
ranges_minus(const int64 *a, int na, const int64 *x, int nx, int64 *out)
{
	int			n = 0,
				j = 0;

	for (int i = 0; i < na; i++)
	{
		int64		lo = a[2 * i],
					hi = a[2 * i + 1];

		while (j < nx && x[2 * j + 1] < lo)
			j++;
		for (int k = j; k < nx && x[2 * k] <= hi && lo <= hi; k++)
		{
			if (x[2 * k] > lo)
			{
				out[2 * n] = lo;
				out[2 * n + 1] = x[2 * k] - 1;
				n++;
			}
			if (x[2 * k + 1] >= hi)
				lo = hi + 1;	/* the rest of the range is excluded */
			else
				lo = Max(lo, x[2 * k + 1] + 1);
		}
		if (lo <= hi)
		{
			out[2 * n] = lo;
			out[2 * n + 1] = hi;
			n++;
		}
	}
	return n;
}

/* drop the current ring's rows */
static void
nearest_clear_ring(NearestScanState *ns)
{
	ExecClearTuple(ns->cone.css.ss.ss_ScanTupleSlot);
	if (ns->sort)
		tuplesort_end(ns->sort);
	ns->sort = NULL;
	if (ns->ringcxt)
		MemoryContextReset(ns->ringcxt);
	ns->rows = NULL;
	ns->nrows = ns->cap = ns->next = 0;
	ns->mem = 0;
}

/* collect and sort the next ring's rows, and choose the one after it */
static void
nearest_ring(NearestScanState *ns)
{
	ConeScanState *st = &ns->cone;
	ExprContext *econtext = st->css.ss.ps.ps_ExprContext;
	EState	   *estate = st->css.ss.ps.state;
	double		n;

	nearest_clear_ring(ns);
	cone_load_density(st);
	n = fmax(st->dens.ntotal, 1.0);
	if (!ns->started)
	{
		bool		null1,
					null2;
		Datum		a = ExecEvalExpr((ExprState *) linitial(st->args), econtext, &null1);
		Datum		b = ExecEvalExpr((ExprState *) lsecond(st->args), econtext, &null2);

		ns->started = true;
		ns->rprev = -1.0;
		ns->n_inside = ns->n_passed = 0;
		ns->last = false;

		/*
		 * The radii the conditions bound the answer to, the least of them a
		 * hair wider, as the rings are: rows the conditions keep are no
		 * farther.  A NULL one keeps no row at all.
		 */
		ns->bound = -1.0;
		for (int i = 3; i < list_length(st->args); i++)
		{
			bool		isnull;
			Datum		v = ExecEvalExpr((ExprState *) list_nth(st->args, i), econtext, &isnull);
			double		b = isnull ? -1.0 : DatumGetFloat8(v);

			if (isnull || isnan(b))
			{
				ns->done = true;
				return;
			}
			b = fmin(180.0, fmax(0.0, b) * (1.0 + 1e-9) + 1e-12);
			if (ns->bound < 0 || b < ns->bound)
				ns->bound = b;
		}
		ns->ra0 = null1 ? 0.0 : DatumGetFloat8(a);
		ns->dec0 = null2 ? 0.0 : DatumGetFloat8(b);
		/*
		 * No centre to search around: every distance is NULL, or whatever
		 * the expression makes of it, and the one ring is the relation.
		 */
		if (null1 || null2 || !isfinite(ns->ra0) || !(ns->dec0 >= -90.0 && ns->dec0 <= 90.0))
			ns->final = true;
		else
		{
			ns->r = nearest_first_radius(ns, ns->k0, n);
			ns->final = ns->r >= 180.0 || ns->k0 >= 0.5 * n;
			if (ns->bound >= 0 && (ns->final || ns->r >= ns->bound))
			{
				ns->r = ns->bound;
				ns->final = false;
				ns->last = true;
			}
		}
	}

	if (ns->final)
	{
		TableScanDesc seq = table_beginscan(st->css.ss.ss_currentRelation,
											estate->es_snapshot, 0, NULL);

		while (table_scan_getnextslot(seq, ForwardScanDirection, ns->fetch))
			nearest_consider(ns, ns->fetch);
		table_endscan(seq);
		ns->done = true;
	}
	else
	{
		double		grow;

		/* a hair wider than the ring, as for a distance comparison */
		(void) cone_cover_at(st, ns->ra0, ns->dec0, fmin(180.0, ns->r * (1.0 + 1e-9) + 1e-12));

		/*
		 * Less the cells wholly inside the previous ring's cone -- a hair
		 * narrower than it -- whose rows were all returned already: a ring
		 * then reads its own band, and the cells the previous edge crossed.
		 */
		if (ns->rprev > 0 && ns->rprev * (1.0 - 1e-9) - 1e-12 > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
			int64	   *inner;
			int			ni = cone_interior(ns->ra0, ns->dec0,
										   ns->rprev * (1.0 - 1e-9) - 1e-12, &inner);

			if (ni > 0 && st->nranges > 0)
			{
				int64	   *b = palloc(sizeof(int64) * 2 * (st->nranges + ni));
				int			nb = ranges_minus(st->bounds, st->nranges, inner, ni, b);

				MemoryContextSwitchTo(old);
				if (nb > st->rt_cap)
				{
					st->rt_cap = Max(nb, 2 * st->rt_cap);
					st->rt_bounds = repalloc(st->rt_bounds, sizeof(int64) * 2 * st->rt_cap);
				}
				memcpy(st->rt_bounds, b, sizeof(int64) * 2 * nb);
				st->bounds = st->rt_bounds;
				st->nranges = nb;
			}
			MemoryContextSwitchTo(old);
		}
		st->cur = -1;
		cone_release_bitmap(st, false);
		for (;;)
		{
			TupleTableSlot *row = st->bitmap ? cone_next_bitmap(st, ns->fetch)
				: cone_next_ordered(st, ns->fetch);

			if (TupIsNull(row))
				break;
			nearest_consider(ns, row);
		}

		/*
		 * The next cone: grown to where, at the density and the share the
		 * conditions kept so far, the rows kept would be what the query was
		 * planned to read, with a margin -- at least twice the area, and
		 * twice while the conditions have kept nothing: a ring too wide
		 * costs every row it reads past the ones needed, one too narrow only
		 * a covering and a few descents.  Sixteen times while the cones have
		 * held nothing.  Once that reaches most of the relation, the next
		 * ring is the rest of it.
		 */
		if (ns->n_inside <= 0)
			grow = 16.0;
		else if (ns->n_passed <= 0)
			grow = 2.0;
		else
			grow = fmin(64.0, fmax(2.0, NEAREST_MARGIN * ns->want / ns->n_passed));
		ns->rprev = ns->r;
		ns->r = cap_radius(cap_area(ns->r) * grow);
		ns->final = ns->r >= 180.0 || ns->n_inside * grow >= 0.5 * n;
		if (ns->last)
			ns->done = true;	/* that ring reached the bound */
		else if (ns->bound >= 0 && (ns->final || ns->r >= ns->bound))
		{
			ns->r = ns->bound;
			ns->final = false;
			ns->last = true;
		}
	}
	ExecClearTuple(ns->fetch);
	ResetExprContext(econtext);
	if (ns->sort)
		tuplesort_performsort(ns->sort);
	else if (ns->nrows > 1)
		nearest_sort_rows(ns->rows, ns->nrows);
	ns->nrings += 1;
}

static TupleTableSlot *
nearest_next(ScanState *ss)
{
	NearestScanState *ns = (NearestScanState *) ss;
	TupleTableSlot *out = ss->ss_ScanTupleSlot;

	for (;;)
	{
		if (ns->sort == NULL && ns->next < ns->nrows)
		{
			ExecStoreHeapTuple(ns->rows[ns->next++].tup, out, false);
			return out;
		}
		if (ns->sort != NULL && tuplesort_gettupleslot(ns->sort, true, false, ns->got, NULL))
		{
			slot_getallattrs(ns->got);
			ExecStoreHeapTuple(heap_form_tuple(out->tts_tupleDescriptor, ns->got->tts_values,
											   ns->got->tts_isnull), out, true);
			return out;
		}
		if (ns->done)
			return ExecClearTuple(out);
		nearest_ring(ns);
	}
}

static TupleTableSlot *
nearest_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, nearest_next, cone_recheck);
}

static void
nearest_reset(NearestScanState *ns)
{
	nearest_clear_ring(ns);
	ns->started = ns->final = ns->done = false;
	ns->cone.cur = -1;
}

static void
nearest_end(CustomScanState *node)
{
	nearest_reset((NearestScanState *) node);
	cone_end(node);
}

static void
nearest_rescan(CustomScanState *node)
{
	NearestScanState *ns = (NearestScanState *) node;

	nearest_reset(ns);
	cone_release_bitmap(&ns->cone, false);
	ExecScanReScan(&node->ss);
}

static void
nearest_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	NearestScanState *ns = (NearestScanState *) node;

	ExplainPropertyText("Index", RelationGetRelationName(ns->cone.index), es);
	ExplainPropertyText("Mode", ns->cone.bitmap ? "bitmap" : "ordered", es);
	if (es->analyze)
	{
		double		loops = (node->ss.ps.instrument && node->ss.ps.instrument->nloops > 0)
			? node->ss.ps.instrument->nloops : 1.0;

		ExplainPropertyFloat("Rings", NULL, ns->nrings / loops, 1, es);
		ExplainPropertyFloat("Rows Read by Rings", NULL, ns->nread / loops, 0, es);
	}
}

/* ------------------------------------------------------------------ */

void
cone_scan_init(void)
{
	DefineCustomBoolVariable("skycell.custom_scan",
							 "Answer cones and cross-matches with skycell's own scan node "
							 "instead of rewriting them into B-tree range conditions.",
							 "The planner sees one clause and one path, instead of building "
							 "and estimating a path per range; the scan walks the ranges in "
							 "cell order, or through a TID bitmap when the heap is not "
							 "ordered by cell. Off restores the rewrite.",
							 &skycell_custom_scan, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.index_filter",
							 "Test cones on the index entries when the cell index also "
							 "holds the position columns.",
							 "With an index such as (skycell_ang2cell(ra, dec)) INCLUDE "
							 "(ra, dec), the custom scan reads ra and dec from each index "
							 "entry and fetches from the table only the rows inside the "
							 "cone. Off fetches every row of the covering cells.",
							 &skycell_index_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("skycell.nearest_scan",
							 "Return rows ordered by their distance to a point from the cell "
							 "index, for ORDER BY distance(...) LIMIT k.",
							 "The scan reads widening rings around the point through the "
							 "cell index and sorts each ring, so a LIMIT stops it after the "
							 "ring that completes it. Off leaves such a query to a sort of "
							 "the whole relation. Needs skycell.custom_scan.",
							 &skycell_nearest_scan, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	RegisterCustomScanMethods(&cone_scan_methods);
	RegisterCustomScanMethods(&nearest_scan_methods);

	/*
	 * The library is usually loaded by the first skycell call a query makes,
	 * which is while that query is being planned, before cone_planner() has
	 * run for it: count that planning as a generation of its own.
	 */
	plan_generation = 1;
	prev_planner_hook = planner_hook;
	planner_hook = cone_planner;
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = cone_set_rel_pathlist;
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = cone_get_relation_info;
}
