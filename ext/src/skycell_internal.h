/*
 * skycell_internal.h -- what the ADQL layer (adql.c) borrows from the
 * planner glue in skycell.c: the density model, the covering memo and the
 * pieces that build B-tree range conditions out of a covering.
 */
#ifndef SKYCELL_INTERNAL_H
#define SKYCELL_INTERNAL_H

#include "nodes/pathnodes.h"
#include "nodes/primnodes.h"
#include "cover.h"

extern void check_err(const char *err);
extern void current_params(sc_cover_params *p, int max_ranges, const sc_density *d);

extern int	skycell_join_slots;

extern Const *int8_const(int64 v);
extern Const *float8_const(double v);
extern Const *int4_const(int32 v);
extern Expr *int8_cmp(int strategy, Node *left, Expr *right);
extern Expr *range_arm(Node *cell, Expr *lo, Expr *hi);
extern Expr *range_arm_family(Node *cell, Expr *lo, Expr *hi, Oid opfamily);
extern Node *ranges_and_exact(sc_cover *cov, Node *cell, Expr *exact, bool uses_cell_ops);
extern Oid	lookup_sibling_func(Oid funcid, const char *name, int nargs, const Oid *argtypes);
extern Oid	cell_ops_opfamily(void);

extern void cover_cached(const sc_region *reg, const sc_density *d,
						 const sc_cover_params *p, Oid statrel,
						 double ra0, double dec0, double radius, sc_cover *out);
extern bool density_for_expr(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
							  bool *uses_cell_ops);
extern void density_for_var(PlannerInfo *root, Node *arg, sc_density *d, Oid *statrel,
							 bool *uses_cell_ops);

/* adql.c: the region behind a skyregion datum, as cover.c understands it */
extern void skycell_region_from_datum(Datum d, sc_region *r);

#endif
