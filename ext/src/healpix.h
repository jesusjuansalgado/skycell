/*
 * healpix.h -- minimal HEALPix NESTED scheme (orders 0..29) for skycell.
 *
 * Only what the index needs: point -> pixel, pixel -> centre, and a safe
 * per-order bound on the angular radius of any pixel.  Pure C, no PostgreSQL
 * dependencies, so it can be unit-tested standalone.
 *
 * Algorithms follow Gorski et al. 2005 (ApJ 622, 759) and the reference
 * healpix_base implementation.
 */
#ifndef SKYCELL_HEALPIX_H
#define SKYCELL_HEALPIX_H

#include <stdint.h>

#define SC_MAX_ORDER 29
/* number of order-29 pixels on the sphere: 12 * 4^29 */
#define SC_NPIX29 ((int64_t) 12 << 58)

typedef struct
{
	double		x,
				y,
				z;
} sc_vec3;

/* unit vector from RA/Dec in degrees */
sc_vec3		sc_radec2vec(double ra_deg, double dec_deg);

/* pixel index (NESTED) at the given order */
int64_t		sc_ang2pix(int order, double ra_deg, double dec_deg);
int64_t		sc_vec2pix(int order, sc_vec3 v);

/* the four corners of a pixel, counter-clockwise in face coordinates */
void		sc_pix_corners(int order, int64_t pix, sc_vec3 out[4]);

/* a point on a pixel's boundary; 0 <= t <= 1 along edge e (0..3). For tests. */
sc_vec3		sc_pix_edge_point(int order, int64_t pix, int e, double t);

/* pixel index from face coordinates (0 <= ix, iy < 2^order, face 0..11) */
int64_t		sc_xyf2nest(int order, int64_t ix, int64_t iy, int face);

/* unit vector of the pixel centre */
sc_vec3		sc_pix2vec(int order, int64_t pix);

/*
 * Upper bound (radians) on the angular distance between a pixel centre and
 * any point of that pixel, valid for every pixel at this order.
 */
double		sc_pixrad(int order);
/* the unpadded HEALPix max_pixrad() value (for tests) */
double		sc_pixrad_raw(int order);

/* first and last order-29 pixel covered by pixel (order, pix) */
static inline int64_t
sc_pix_lo(int order, int64_t pix)
{
	return pix << (2 * (SC_MAX_ORDER - order));
}

static inline int64_t
sc_pix_hi(int order, int64_t pix)
{
	return ((pix + 1) << (2 * (SC_MAX_ORDER - order))) - 1;
}

/* IVOA MOC "NUNIQ" encoding of (order, pix): 4 * 4^order + pix */
static inline int64_t
sc_nuniq(int order, int64_t pix)
{
	return ((int64_t) 4 << (2 * order)) + pix;
}

/* inverse of sc_nuniq; returns order, stores pix */
int			sc_nuniq_decode(int64_t nuniq, int64_t *pix);

/* angle between two unit vectors, accurate for tiny and large angles */
double		sc_angle(sc_vec3 a, sc_vec3 b);

static inline double
sc_dot(sc_vec3 a, sc_vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline sc_vec3
sc_cross(sc_vec3 a, sc_vec3 b)
{
	sc_vec3		r;

	r.x = a.y * b.z - a.z * b.y;
	r.y = a.z * b.x - a.x * b.z;
	r.z = a.x * b.y - a.y * b.x;
	return r;
}

/* squared chord length |a-b|^2 = 4 sin^2(theta/2); precise for small angles */
static inline double
sc_chord2(sc_vec3 a, sc_vec3 b)
{
	double		dx = a.x - b.x,
				dy = a.y - b.y,
				dz = a.z - b.z;

	return dx * dx + dy * dy + dz * dz;
}

#endif
