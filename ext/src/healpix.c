/*
 * healpix.c -- minimal HEALPix NESTED scheme for skycell (see healpix.h).
 */
#include <math.h>
#include "healpix.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Safety factor applied to the HEALPix max_pixrad() estimate.  The test
 * program (test/healpix_selftest.c) samples millions of points and checks
 * distance(point, centre of its pixel) / max_pixrad <= 1; we keep a margin so
 * that floating point noise can never produce a false negative.
 */
#define SC_PIXRAD_SAFETY 1.01

static const int jrll[12] = {2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4};
static const int jpll[12] = {1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7};

static inline uint64_t
spread_bits(uint64_t x)
{
	x &= 0xFFFFFFFFULL;
	x = (x | (x << 16)) & 0x0000FFFF0000FFFFULL;
	x = (x | (x << 8)) & 0x00FF00FF00FF00FFULL;
	x = (x | (x << 4)) & 0x0F0F0F0F0F0F0F0FULL;
	x = (x | (x << 2)) & 0x3333333333333333ULL;
	x = (x | (x << 1)) & 0x5555555555555555ULL;
	return x;
}

static inline uint64_t
compress_bits(uint64_t x)
{
	x &= 0x5555555555555555ULL;
	x = (x | (x >> 1)) & 0x3333333333333333ULL;
	x = (x | (x >> 2)) & 0x0F0F0F0F0F0F0F0FULL;
	x = (x | (x >> 4)) & 0x00FF00FF00FF00FFULL;
	x = (x | (x >> 8)) & 0x0000FFFF0000FFFFULL;
	x = (x | (x >> 16)) & 0x00000000FFFFFFFFULL;
	return x;
}

static inline int64_t
xyf2nest(int order, int64_t ix, int64_t iy, int face)
{
	return ((int64_t) face << (2 * order))
		+ (int64_t) spread_bits((uint64_t) ix)
		+ (int64_t) (spread_bits((uint64_t) iy) << 1);
}

static inline void
nest2xyf(int order, int64_t pix, int64_t *ix, int64_t *iy, int *face)
{
	int64_t		npface = (int64_t) 1 << (2 * order);
	uint64_t	p;

	*face = (int) (pix >> (2 * order));
	p = (uint64_t) (pix & (npface - 1));
	*ix = (int64_t) compress_bits(p);
	*iy = (int64_t) compress_bits(p >> 1);
}

/* v1 mod v2, result in [0, v2) */
static inline double
fmodulo(double v1, double v2)
{
	double		tmp;

	if (v1 >= 0)
		return (v1 < v2) ? v1 : fmod(v1, v2);
	tmp = fmod(v1, v2) + v2;
	return (tmp == v2) ? 0.0 : tmp;
}

static int64_t
loc2pix(int order, double z, double phi, double sth, int have_sth)
{
	int64_t		nside = (int64_t) 1 << order;
	double		za = fabs(z);
	double		tt = fmodulo(phi * (2.0 / M_PI), 4.0);	/* in [0,4) */

	if (za <= 2.0 / 3.0)
	{
		/* equatorial region */
		double		temp1 = nside * (0.5 + tt);
		double		temp2 = nside * (z * 0.75);
		int64_t		jp = (int64_t) (temp1 - temp2); /* ascending edge line */
		int64_t		jm = (int64_t) (temp1 + temp2); /* descending edge line */
		int64_t		ifp = jp >> order;
		int64_t		ifm = jm >> order;
		int			face;
		int64_t		ix,
					iy;

		if (ifp == ifm)
			face = (int) (ifp | 4);
		else if (ifp < ifm)
			face = (int) ifp;
		else
			face = (int) (ifm + 8);
		ix = jm & (nside - 1);
		iy = nside - (jp & (nside - 1)) - 1;
		return xyf2nest(order, ix, iy, face);
	}
	else
	{
		/* polar caps */
		int			ntt = (int) tt;
		double		tp,
					tmp;
		int64_t		jp,
					jm;

		if (ntt > 3)
			ntt = 3;
		tp = tt - ntt;
		if (za < 0.99 || !have_sth)
			tmp = nside * sqrt(3.0 * (1.0 - za));
		else
			tmp = nside * sth / sqrt((1.0 + za) / 3.0);
		jp = (int64_t) (tp * tmp);
		jm = (int64_t) ((1.0 - tp) * tmp);
		if (jp > nside - 1)
			jp = nside - 1;
		if (jm > nside - 1)
			jm = nside - 1;
		if (z >= 0)
			return xyf2nest(order, nside - jm - 1, nside - jp - 1, ntt);
		return xyf2nest(order, jp, jm, ntt + 8);
	}
}

static void
pix2loc(int order, int64_t pix, double *z, double *phi, double *sth, int *have_sth)
{
	int64_t		nside = (int64_t) 1 << order;
	double		fact2 = 4.0 / (12.0 * (double) nside * (double) nside);
	double		fact1 = (double) (nside << 1) * fact2;
	int64_t		ix,
				iy,
				jr,
				nr,
				tmp;
	int			face;

	*have_sth = 0;
	nest2xyf(order, pix, &ix, &iy, &face);
	jr = ((int64_t) jrll[face] << order) - ix - iy - 1;

	if (jr < nside)
	{
		double		t;

		nr = jr;
		t = (double) (nr * nr) * fact2;
		*z = 1.0 - t;
		if (*z > 0.99)
		{
			*sth = sqrt(t * (2.0 - t));
			*have_sth = 1;
		}
	}
	else if (jr > 3 * nside)
	{
		double		t;

		nr = nside * 4 - jr;
		t = (double) (nr * nr) * fact2;
		*z = t - 1.0;
		if (*z < -0.99)
		{
			*sth = sqrt(t * (2.0 - t));
			*have_sth = 1;
		}
	}
	else
	{
		nr = nside;
		*z = (double) (2 * nside - jr) * fact1;
	}

	tmp = (int64_t) jpll[face] * nr + ix - iy;
	if (tmp < 0)
		tmp += 8 * nr;
	*phi = (nr == nside) ? 0.75 * (M_PI / 2) * (double) tmp * fact1
		: (0.5 * (M_PI / 2) * (double) tmp) / (double) nr;
}

/*
 * Continuous face coordinates (x, y in [0,1] within face) -> (z, phi).  The
 * discrete pix2loc above is this map at the pixel centre; here it is needed
 * at the pixel *corners*, which bound the cell exactly rather than by the
 * worst-case cap over the whole sphere.
 */
static void
xyf2loc(double x, double y, int face, double *z, double *phi,
		double *sth, int *have_sth)
{
	double		jr = (double) jrll[face] - x - y;
	double		nr,
				t;

	*have_sth = 0;
	if (jr < 1.0)
	{
		nr = jr;
		t = nr * nr / 3.0;
		*z = 1.0 - t;
		if (*z > 0.99)
		{
			*sth = sqrt(t * (2.0 - t));
			*have_sth = 1;
		}
	}
	else if (jr > 3.0)
	{
		nr = 4.0 - jr;
		t = nr * nr / 3.0;
		*z = t - 1.0;
		if (*z < -0.99)
		{
			*sth = sqrt(t * (2.0 - t));
			*have_sth = 1;
		}
	}
	else
	{
		nr = 1.0;
		*z = (2.0 - jr) * (2.0 / 3.0);
	}

	t = (double) jpll[face] * nr + x - y;
	if (t < 0)
		t += 8.0;
	if (t >= 8.0)
		t -= 8.0;
	*phi = (nr < 1e-15) ? 0.0 : (0.5 * (M_PI / 2) * t) / nr;
}

static sc_vec3
loc2vec(double z, double phi, double sth, int have_sth)
{
	sc_vec3		v;

	if (!have_sth)
		sth = sqrt((1.0 - z) * (1.0 + z));
	v.x = sth * cos(phi);
	v.y = sth * sin(phi);
	v.z = z;
	return v;
}

static sc_vec3
xyf2vec(double x, double y, int face)
{
	double		z,
				phi,
				sth = 0;
	int			have_sth;

	xyf2loc(x, y, face, &z, &phi, &sth, &have_sth);
	return loc2vec(z, phi, sth, have_sth);
}

void
sc_pix_corners(int order, int64_t pix, sc_vec3 out[4])
{
	int64_t		ix,
				iy,
				nside = (int64_t) 1 << order;
	int			face;
	double		x0,
				x1,
				y0,
				y1;

	nest2xyf(order, pix, &ix, &iy, &face);
	x0 = (double) ix / (double) nside;
	x1 = (double) (ix + 1) / (double) nside;
	y0 = (double) iy / (double) nside;
	y1 = (double) (iy + 1) / (double) nside;

	out[0] = xyf2vec(x0, y0, face);
	out[1] = xyf2vec(x1, y0, face);
	out[2] = xyf2vec(x1, y1, face);
	out[3] = xyf2vec(x0, y1, face);
}

sc_vec3
sc_pix_edge_point(int order, int64_t pix, int e, double t)
{
	int64_t		ix,
				iy,
				nside = (int64_t) 1 << order;
	int			face;
	double		x0,
				x1,
				y0,
				y1;

	nest2xyf(order, pix, &ix, &iy, &face);
	x0 = (double) ix / (double) nside;
	x1 = (double) (ix + 1) / (double) nside;
	y0 = (double) iy / (double) nside;
	y1 = (double) (iy + 1) / (double) nside;

	switch (e & 3)
	{
		case 0:
			return xyf2vec(x0 + t * (x1 - x0), y0, face);
		case 1:
			return xyf2vec(x1, y0 + t * (y1 - y0), face);
		case 2:
			return xyf2vec(x1 - t * (x1 - x0), y1, face);
		default:
			return xyf2vec(x0, y1 - t * (y1 - y0), face);
	}
}

sc_vec3
sc_radec2vec(double ra_deg, double dec_deg)
{
	double		ra = ra_deg * (M_PI / 180.0);
	double		dec = dec_deg * (M_PI / 180.0);
	double		cd = cos(dec);
	sc_vec3		v;

	v.x = cd * cos(ra);
	v.y = cd * sin(ra);
	v.z = sin(dec);
	return v;
}

int64_t
sc_xyf2nest(int order, int64_t ix, int64_t iy, int face)
{
	return xyf2nest(order, ix, iy, face);
}

int64_t
sc_ang2pix(int order, double ra_deg, double dec_deg)
{
	double		dec = dec_deg * (M_PI / 180.0);

	return loc2pix(order, sin(dec), ra_deg * (M_PI / 180.0), cos(dec), 1);
}

int64_t
sc_vec2pix(int order, sc_vec3 v)
{
	double		xy = sqrt(v.x * v.x + v.y * v.y);
	double		norm = sqrt(xy * xy + v.z * v.z);

	return loc2pix(order, v.z / norm, atan2(v.y, v.x), xy / norm, 1);
}

sc_vec3
sc_pix2vec(int order, int64_t pix)
{
	double		z,
				phi,
				sth = 0;
	int			have_sth;
	sc_vec3		v;

	pix2loc(order, pix, &z, &phi, &sth, &have_sth);
	if (!have_sth)
		sth = sqrt((1.0 - z) * (1.0 + z));
	v.x = sth * cos(phi);
	v.y = sth * sin(phi);
	v.z = z;
	return v;
}

double
sc_angle(sc_vec3 a, sc_vec3 b)
{
	sc_vec3		c = sc_cross(a, b);

	return atan2(sqrt(sc_dot(c, c)), sc_dot(a, b));
}

static sc_vec3
vec_from_zphi(double z, double phi)
{
	double		sth = sqrt((1.0 - z) * (1.0 + z));
	sc_vec3		v;

	v.x = sth * cos(phi);
	v.y = sth * sin(phi);
	v.z = z;
	return v;
}

/* healpix_base max_pixrad(): max distance pixel centre -> corner */
static double
max_pixrad_raw(int order)
{
	double		nside = (double) ((int64_t) 1 << order);
	double		t1 = 1.0 - 1.0 / nside;
	sc_vec3		va = vec_from_zphi(2.0 / 3.0, M_PI / (4.0 * nside));
	sc_vec3		vb;

	t1 *= t1;
	vb = vec_from_zphi(1.0 - t1 / 3.0, 0.0);
	return sc_angle(va, vb);
}

double
sc_pixrad(int order)
{
	static double table[SC_MAX_ORDER + 1];
	static int	ready = 0;

	if (!ready)
	{
		for (int k = 0; k <= SC_MAX_ORDER; k++)
			table[k] = max_pixrad_raw(k) * SC_PIXRAD_SAFETY + 1e-15;
		ready = 1;
	}
	return table[order];
}

double
sc_pixrad_raw(int order)
{
	return max_pixrad_raw(order);
}

int
sc_nuniq_decode(int64_t nuniq, int64_t *pix)
{
	int			msb = 63 - __builtin_clzll((unsigned long long) nuniq);
	int			order = msb / 2 - 1;

	*pix = nuniq - ((int64_t) 4 << (2 * order));
	return order;
}
