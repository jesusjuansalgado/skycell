/*
 * removed_opclasses.c -- the C entry points of the three operator classes
 * removed in 0.38, kept as stubs that raise an error.
 *
 *   skyregion_box_gist_ops   (GiST on skyregion, float8 box)  -> the default,
 *                                                                 skyregion_box4_gist_ops
 *   skypos_spgist_ops        (SP-GiST on skypos)              -> a B-tree on skycell_cell(pos)
 *   skypos_cap_gist_ops      (GiST on skypos, spherical cap)  -> a B-tree on skycell_cell(pos)
 *
 * The symbols stay because the install and upgrade scripts of earlier
 * versions create functions over them, and PostgreSQL looks a C function's
 * symbol up when it is created: without them a database could not be built
 * at, or upgraded through, an old version with this library.  An index left
 * on one of these classes after the library is updated fails with the error
 * below until it is dropped; the 0.38 upgrade script refuses to run while one
 * exists.
 */
#include "postgres.h"

#include "fmgr.h"

#define REMOVED(fn, opclass, instead) \
	PG_FUNCTION_INFO_V1(fn); \
	Datum fn(PG_FUNCTION_ARGS); \
	Datum \
	fn(PG_FUNCTION_ARGS) \
	{ \
		ereport(ERROR, \
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
				 errmsg("operator class %s was removed in skycell 0.38", opclass), \
				 errhint("Drop the index and use %s instead.", instead))); \
		PG_RETURN_NULL(); \
	}

#define BOX_GIST(fn) REMOVED(fn, "skyregion_box_gist_ops", "the default region GiST (skyregion_box4_gist_ops)")
#define POS_SPGIST(fn) REMOVED(fn, "skypos_spgist_ops", "a B-tree on skycell_cell(pos)")
#define POS_CAP_GIST(fn) REMOVED(fn, "skypos_cap_gist_ops", "a B-tree on skycell_cell(pos)")

BOX_GIST(skyregion_box_gist_compress)
BOX_GIST(skyregion_box_gist_decompress)
BOX_GIST(skyregion_box_gist_union)
BOX_GIST(skyregion_box_gist_penalty)
BOX_GIST(skyregion_box_gist_picksplit)
BOX_GIST(skyregion_box_gist_same)
BOX_GIST(skyregion_box_gist_consistent)

POS_SPGIST(spg_healpix_config)
POS_SPGIST(spg_healpix_choose)
POS_SPGIST(spg_healpix_picksplit)
POS_SPGIST(spg_healpix_inner_consistent)
POS_SPGIST(spg_healpix_leaf_consistent)

POS_CAP_GIST(skypos_cap_gist_compress)
POS_CAP_GIST(skypos_cap_gist_decompress)
POS_CAP_GIST(skypos_cap_gist_union)
POS_CAP_GIST(skypos_cap_gist_penalty)
POS_CAP_GIST(skypos_cap_gist_picksplit)
POS_CAP_GIST(skypos_cap_gist_same)
POS_CAP_GIST(skypos_cap_gist_consistent)
