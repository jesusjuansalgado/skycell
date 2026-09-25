/*
 * astro.c -- the astronomy functions a TAP service is asked for beside the
 * geometry: epoch propagation, proper motion, coordinate frames, and the
 * HEALPix helpers of the IVOA UDF registry.
 *
 * Names follow the registry (ivo_epoch_prop, ivo_epoch_prop_pos,
 * ivo_apply_pm, ivo_healpix_index, ivo_healpix_center) so that ADQL written
 * for GAVO DaCHS or the Gaia archives runs here unchanged.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "cover.h"
#include "skycell_internal.h"

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)
#define MAS2RAD (DEG2RAD / 3600000.0)
#define RAD2MAS (1.0 / MAS2RAD)

/*
 * The astronomical unit in km yr / s: v_r [km/s] = AU_KMYS * mu_r [mas/yr] /
 * parallax [mas].  It is what ties a radial velocity to the rate at which a
 * star's distance changes, and so to how its proper motion evolves.
 */
#define AU_KMYS 4.740470446

typedef struct
{
	double		ra;
	double		dec;
} SkyPos;

static SkyPos *
pos_make(double ra, double dec)
{
	SkyPos	   *p = (SkyPos *) palloc(sizeof(SkyPos));

	p->ra = ra - floor(ra / 360.0) * 360.0;
	p->dec = dec;
	return p;
}

#define PG_GETARG_SKYPOS(n) ((SkyPos *) DatumGetPointer(PG_GETARG_DATUM(n)))

/* ------------------------------------------------------------------ */
/* epoch propagation                                                   */
/* ------------------------------------------------------------------ */

/*
 * Propagate a six-parameter astrometric solution from one epoch to another,
 * rigorously: the star moves on a straight line in space, so its direction
 * moves on a great circle only to first order, and its proper motion,
 * parallax and radial velocity all change as it does.  This is the
 * formulation of the Hipparcos catalogue (ESA SP-1200, vol. 1, sect. 1.5.5),
 * also used by the Gaia archives.
 *
 *   f      = 1 / sqrt(1 + 2 mu_r t + (mu^2 + mu_r^2) t^2)
 *   r(t)   = [r (1 + mu_r t) + mu t] f
 *   mu(t)  = [mu (1 + mu_r t) - r mu^2 t] f^3
 *   mu_r(t)= [mu_r + (mu^2 + mu_r^2) t] f^2
 *   plx(t) = plx f
 *
 * With a zero parallax and radial velocity the radial motion mu_r vanishes
 * and this reduces to great-circle propagation of the proper motion, which
 * is what ivo_apply_pm() does directly.
 *
 * Angles in degrees, parallax in mas, proper motions in mas/yr (pmra
 * including the cos(dec) factor), radial velocity in km/s, epochs in Julian
 * years.  out[] receives ra, dec, parallax, pmra, pmdec, radial velocity.
 */
static void
epoch_prop(double ra, double dec, double plx, double pmra, double pmdec,
		   double rv, double t, double out[6])
{
	double		ca = cos(ra * DEG2RAD),
				sa = sin(ra * DEG2RAD),
				cd = cos(dec * DEG2RAD),
				sd = sin(dec * DEG2RAD);
	/* the normal triad at the starting position */
	double		r0[3] = {cd * ca, cd * sa, sd};
	double		p0[3] = {-sa, ca, 0};
	double		q0[3] = {-sd * ca, -sd * sa, cd};
	double		mu0[3];
	double		mu_a = pmra * MAS2RAD,
				mu_d = pmdec * MAS2RAD;
	double		mu2,
				mu_r,
				f,
				f2,
				r1[3],
				mu1[3],
				p1[3],
				q1[3];
	double		ra1,
				dec1,
				plx1,
				mu_r1,
				ca1,
				sa1,
				cd1,
				sd1;

	for (int i = 0; i < 3; i++)
		mu0[i] = p0[i] * mu_a + q0[i] * mu_d;
	mu2 = mu0[0] * mu0[0] + mu0[1] * mu0[1] + mu0[2] * mu0[2];
	/* radial proper motion: zero unless both parallax and rv are known */
	mu_r = (plx != 0.0) ? (rv * plx / AU_KMYS) * MAS2RAD : 0.0;

	f = 1.0 / sqrt(fmax(1e-300, 1.0 + 2.0 * mu_r * t + (mu2 + mu_r * mu_r) * t * t));
	f2 = f * f;
	for (int i = 0; i < 3; i++)
	{
		r1[i] = (r0[i] * (1.0 + mu_r * t) + mu0[i] * t) * f;
		mu1[i] = (mu0[i] * (1.0 + mu_r * t) - r0[i] * mu2 * t) * f2 * f;
	}
	mu_r1 = (mu_r + (mu2 + mu_r * mu_r) * t) * f2;
	plx1 = plx * f;

	dec1 = asin(fmax(-1.0, fmin(1.0, r1[2]))) * RAD2DEG;
	ra1 = atan2(r1[1], r1[0]) * RAD2DEG;
	ra1 -= floor(ra1 / 360.0) * 360.0;

	ca1 = cos(ra1 * DEG2RAD);
	sa1 = sin(ra1 * DEG2RAD);
	cd1 = cos(dec1 * DEG2RAD);
	sd1 = sin(dec1 * DEG2RAD);
	p1[0] = -sa1;
	p1[1] = ca1;
	p1[2] = 0;
	q1[0] = -sd1 * ca1;
	q1[1] = -sd1 * sa1;
	q1[2] = cd1;

	out[0] = ra1;
	out[1] = dec1;
	out[2] = plx1;
	out[3] = (mu1[0] * p1[0] + mu1[1] * p1[1] + mu1[2] * p1[2]) * RAD2MAS;
	out[4] = (mu1[0] * q1[0] + mu1[1] * q1[1] + mu1[2] * q1[2]) * RAD2MAS;
	out[5] = (plx1 != 0.0) ? mu_r1 * RAD2MAS * AU_KMYS / plx1 : rv;
}

PG_FUNCTION_INFO_V1(ivo_epoch_prop);
Datum
ivo_epoch_prop(PG_FUNCTION_ARGS)
{
	double		out[6];
	Datum		d[6];

	epoch_prop(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
			   PG_GETARG_FLOAT8(3), PG_GETARG_FLOAT8(4), PG_GETARG_FLOAT8(5),
			   PG_GETARG_FLOAT8(7) - PG_GETARG_FLOAT8(6), out);
	for (int i = 0; i < 6; i++)
		d[i] = Float8GetDatum(out[i]);
	PG_RETURN_ARRAYTYPE_P(construct_array(d, 6, FLOAT8OID, sizeof(float8), true, TYPALIGN_DOUBLE));
}

PG_FUNCTION_INFO_V1(ivo_epoch_prop_pos);
Datum
ivo_epoch_prop_pos(PG_FUNCTION_ARGS)
{
	double		out[6];

	epoch_prop(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2),
			   PG_GETARG_FLOAT8(3), PG_GETARG_FLOAT8(4), PG_GETARG_FLOAT8(5),
			   PG_GETARG_FLOAT8(7) - PG_GETARG_FLOAT8(6), out);
	PG_RETURN_POINTER(pos_make(out[0], out[1]));
}

/* the same without a parallax or radial velocity: ra, dec, pmra, pmdec, epochs */
PG_FUNCTION_INFO_V1(ivo_epoch_prop_pos_pm);
Datum
ivo_epoch_prop_pos_pm(PG_FUNCTION_ARGS)
{
	double		out[6];

	epoch_prop(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), 0.0,
			   PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3), 0.0,
			   PG_GETARG_FLOAT8(5) - PG_GETARG_FLOAT8(4), out);
	PG_RETURN_POINTER(pos_make(out[0], out[1]));
}

/* ivo_apply_pm: great-circle proper motion over epdiff years */
PG_FUNCTION_INFO_V1(ivo_apply_pm);
Datum
ivo_apply_pm(PG_FUNCTION_ARGS)
{
	double		out[6];

	epoch_prop(PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), 0.0,
			   PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3), 0.0,
			   PG_GETARG_FLOAT8(4), out);
	PG_RETURN_POINTER(pos_make(out[0], out[1]));
}

/*
 * How far a star with this proper motion can move in dt years, in degrees.
 * A cone search on catalogue positions that must not miss a moving star
 * widens its radius by this much (see the README, "Proper motion").
 */
PG_FUNCTION_INFO_V1(skycell_pm_margin);
Datum
skycell_pm_margin(PG_FUNCTION_ARGS)
{
	double		pmra = PG_GETARG_FLOAT8(0),
				pmdec = PG_GETARG_FLOAT8(1),
				dt = PG_GETARG_FLOAT8(2);

	PG_RETURN_FLOAT8(sqrt(pmra * pmra + pmdec * pmdec) * fabs(dt) / 3600000.0);
}

/* ------------------------------------------------------------------ */
/* coordinate frames                                                   */
/* ------------------------------------------------------------------ */

/* rows: the galactic axes expressed in ICRS (Hipparcos, ESA SP-1200 1.5.3) */
static const double gal_matrix[3][3] = {
	{-0.0548755604162154, -0.8734370902348850, -0.4838350155487132},
	{0.4941094278755837, -0.4448296299600112, 0.7469822444972189},
	{-0.8676661490190047, -0.1980763734312015, 0.4559837761750669}
};

/* obliquity of the ecliptic at J2000 */
#define ECL_OBLIQUITY (23.4392911111 * DEG2RAD)

static Datum
rotate(PG_FUNCTION_ARGS, int mode)
{
	SkyPos	   *p = PG_GETARG_SKYPOS(0);
	double		ca = cos(p->ra * DEG2RAD),
				sa = sin(p->ra * DEG2RAD),
				cd = cos(p->dec * DEG2RAD),
				sd = sin(p->dec * DEG2RAD);
	double		v[3] = {cd * ca, cd * sa, sd},
				o[3];
	double		ce = cos(ECL_OBLIQUITY),
				se = sin(ECL_OBLIQUITY);

	switch (mode)
	{
		case 0:					/* ICRS -> galactic */
			for (int i = 0; i < 3; i++)
				o[i] = gal_matrix[i][0] * v[0] + gal_matrix[i][1] * v[1] + gal_matrix[i][2] * v[2];
			break;
		case 1:					/* galactic -> ICRS */
			for (int i = 0; i < 3; i++)
				o[i] = gal_matrix[0][i] * v[0] + gal_matrix[1][i] * v[1] + gal_matrix[2][i] * v[2];
			break;
		case 2:					/* ICRS -> ecliptic */
			o[0] = v[0];
			o[1] = v[1] * ce + v[2] * se;
			o[2] = -v[1] * se + v[2] * ce;
			break;
		default:				/* ecliptic -> ICRS */
			o[0] = v[0];
			o[1] = v[1] * ce - v[2] * se;
			o[2] = v[1] * se + v[2] * ce;
			break;
	}
	PG_RETURN_POINTER(pos_make(atan2(o[1], o[0]) * RAD2DEG,
							   asin(fmax(-1.0, fmin(1.0, o[2]))) * RAD2DEG));
}

PG_FUNCTION_INFO_V1(icrs2gal);
Datum
icrs2gal(PG_FUNCTION_ARGS)
{
	return rotate(fcinfo, 0);
}

PG_FUNCTION_INFO_V1(gal2icrs);
Datum
gal2icrs(PG_FUNCTION_ARGS)
{
	return rotate(fcinfo, 1);
}

PG_FUNCTION_INFO_V1(icrs2ecl);
Datum
icrs2ecl(PG_FUNCTION_ARGS)
{
	return rotate(fcinfo, 2);
}

PG_FUNCTION_INFO_V1(ecl2icrs);
Datum
ecl2icrs(PG_FUNCTION_ARGS)
{
	return rotate(fcinfo, 3);
}

/* ------------------------------------------------------------------ */
/* the registry's HEALPix helpers                                      */
/* ------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(ivo_healpix_index);
Datum
ivo_healpix_index(PG_FUNCTION_ARGS)
{
	int32		order = PG_GETARG_INT32(0);

	if (order < 0 || order > SC_MAX_ORDER)
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("HEALPix order must be within [0, %d]", SC_MAX_ORDER)));
	PG_RETURN_INT64(sc_ang2pix(order, PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2)));
}

PG_FUNCTION_INFO_V1(ivo_healpix_index_pos);
Datum
ivo_healpix_index_pos(PG_FUNCTION_ARGS)
{
	int32		order = PG_GETARG_INT32(0);
	SkyPos	   *p = PG_GETARG_SKYPOS(1);

	if (order < 0 || order > SC_MAX_ORDER)
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("HEALPix order must be within [0, %d]", SC_MAX_ORDER)));
	PG_RETURN_INT64(sc_ang2pix(order, p->ra, p->dec));
}

PG_FUNCTION_INFO_V1(ivo_healpix_center);
Datum
ivo_healpix_center(PG_FUNCTION_ARGS)
{
	int32		order = PG_GETARG_INT32(0);
	int64		index = PG_GETARG_INT64(1);
	sc_vec3		v;

	if (order < 0 || order > SC_MAX_ORDER)
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("HEALPix order must be within [0, %d]", SC_MAX_ORDER)));
	if (index < 0 || index >= ((int64) 12 << (2 * order)))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						errmsg("HEALPix index out of range for order %d", order)));
	v = sc_pix2vec(order, index);
	PG_RETURN_POINTER(pos_make(atan2(v.y, v.x) * RAD2DEG,
							   asin(fmax(-1.0, fmin(1.0, v.z))) * RAD2DEG));
}
