/*
 * Standalone checks for healpix.c:
 *   1. known pixel centres at order 0
 *   2. pix -> centre -> pix round trip at every order
 *   3. nesting: pix(k) == pix(29) >> 2(29-k)
 *   4. every point lies within max_pixrad of its pixel centre (drives the
 *      safety factor used by the covering code -- a violation would be a
 *      false negative in queries)
 *   5. equal-area: uniform points spread evenly over order-3 pixels
 * Build: cc -O2 -o healpix_selftest healpix_selftest.c ../src/healpix.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/healpix.h"

static int	failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; if (failures < 20) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng_state = 88172645463325252ULL;
static double
urand(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (rng_state >> 11) * (1.0 / 9007199254740992.0);
}

static sc_vec3
random_point(int mode)
{
	double		ra = 360.0 * urand(),
				dec;

	switch (mode)
	{
		case 0:				/* uniform on the sphere */
			dec = asin(2 * urand() - 1) * 180 / M_PI;
			break;
		case 1:				/* very close to the poles */
			dec = (urand() < 0.5 ? 1 : -1) * (90.0 - 1e-3 * pow(urand(), 3));
			break;
		case 2:				/* the z = +-2/3 polar/equatorial boundary */
			dec = (urand() < 0.5 ? 1 : -1) * (asin(2.0 / 3.0) * 180 / M_PI + (urand() - 0.5) * 1e-6);
			break;
		default:			/* multiples of 45 deg in RA (face edges) */
			ra = 45.0 * (int) (8 * urand()) + (urand() - 0.5) * 1e-7;
			dec = asin(2 * urand() - 1) * 180 / M_PI;
			break;
	}
	return sc_radec2vec(ra, dec);
}

int
main(void)
{
	/* 1. known centres */
	{
		struct
		{
			int64_t		pix;
			double		ra,
						dec;
		}			known[] = {
			{0, 45, 41.8103148958}, {3, 315, 41.8103148958},
			{4, 0, 0}, {6, 180, 0}, {8, 45, -41.8103148958}, {11, 315, -41.8103148958}
		};

		for (int i = 0; i < 6; i++)
		{
			sc_vec3		c = sc_pix2vec(0, known[i].pix);
			sc_vec3		e = sc_radec2vec(known[i].ra, known[i].dec);
			double		d = sc_angle(c, e) * 180 / M_PI;

			CHECK(d < 1e-8, "order0 pix %lld centre off by %g deg", (long long) known[i].pix, d);
			CHECK(sc_ang2pix(0, known[i].ra, known[i].dec) == known[i].pix, "ang2pix known %lld", (long long) known[i].pix);
		}
	}

	/* 2. round trip */
	for (int order = 0; order <= SC_MAX_ORDER; order++)
	{
		int64_t		npix = (int64_t) 12 << (2 * order);

		for (int i = 0; i < 20000; i++)
		{
			int64_t		pix = (int64_t) (urand() * (double) npix);
			int64_t		back;

			if (pix >= npix)
				pix = npix - 1;
			back = sc_vec2pix(order, sc_pix2vec(order, pix));
			CHECK(back == pix, "roundtrip order %d pix %lld -> %lld", order, (long long) pix, (long long) back);
		}
	}

	/* 3 + 4. nesting and containment radius */
	{
		double		worst[SC_MAX_ORDER + 1] = {0};

		for (int mode = 0; mode < 4; mode++)
			for (int i = 0; i < 400000; i++)
			{
				sc_vec3		p = random_point(mode);
				int64_t		p29 = sc_vec2pix(SC_MAX_ORDER, p);

				for (int order = 0; order <= SC_MAX_ORDER; order++)
				{
					int64_t		pk = sc_vec2pix(order, p);
					double		ratio;

					CHECK(pk == (p29 >> (2 * (SC_MAX_ORDER - order))),
						  "nesting order %d mode %d", order, mode);
					ratio = sc_angle(p, sc_pix2vec(order, pk)) / sc_pixrad_raw(order);
					if (ratio > worst[order])
						worst[order] = ratio;
				}
			}
		printf("max distance(point, centre)/max_pixrad per order:\n");
		for (int order = 0; order <= SC_MAX_ORDER; order++)
		{
			printf("  %2d: %.6f%s", order, worst[order], (order % 5 == 4) ? "\n" : "");
			CHECK(worst[order] * 1.0 <= 1.0 + 1e-6,
				  "order %d: point farther from centre than max_pixrad (ratio %.6f)", order, worst[order]);
		}
		printf("\n");
	}

	/* 5. equal area at order 3 (768 pixels) */
	{
		static int	counts[768];
		int			n = 768 * 2000;
		double		chi2 = 0;

		for (int i = 0; i < n; i++)
			counts[sc_vec2pix(3, random_point(0))]++;
		for (int i = 0; i < 768; i++)
			chi2 += (counts[i] - 2000.0) * (counts[i] - 2000.0) / 2000.0;
		printf("equal-area chi2/dof = %.3f (expect ~1)\n", chi2 / 767);
		CHECK(chi2 / 767 < 1.3, "equal-area chi2/dof %.3f", chi2 / 767);
	}

	/* nuniq round trip */
	for (int order = 0; order <= SC_MAX_ORDER; order++)
	{
		int64_t		pix = ((int64_t) 12 << (2 * order)) - 1,
					back;

		CHECK(sc_nuniq_decode(sc_nuniq(order, pix), &back) == order && back == pix, "nuniq order %d", order);
		CHECK(sc_nuniq_decode(sc_nuniq(order, 0), &back) == order && back == 0, "nuniq0 order %d", order);
	}

	/*
	 * Cell corners and the edge "bulge": how far an edge departs from the
	 * great-circle chord between its corners. The covering's OUT test uses
	 * chords, so it needs an upper bound on that; it scales as (cell size)^2,
	 * and this measures the constant.
	 */
	{
		double		worst_ratio = 0,
					worst_corner = 0;

		for (int order = 0; order <= 20; order++)
		{
			int64_t		npix = (int64_t) 12 << (2 * order);
			double		s = sqrt(4 * M_PI / (double) npix);	/* nominal cell size */

			for (int i = 0; i < 4000; i++)
			{
				int64_t		pix = (int64_t) (urand() * (double) npix);
				sc_vec3		c[4],
							centre;
				double		maxcorner = 0;

				if (pix >= npix)
					pix = npix - 1;
				sc_pix_corners(order, pix, c);
				centre = sc_pix2vec(order, pix);
				for (int k = 0; k < 4; k++)
					maxcorner = fmax(maxcorner, sc_angle(centre, c[k]));
				/* every boundary point within (corner cap + bulge) */
				for (int e = 0; e < 4; e++)
				{
					sc_vec3		a = c[e],
								b = c[(e + 1) & 3];
					sc_vec3		n = sc_cross(a, b);
					double		nn = sqrt(sc_dot(n, n));

					if (nn < 1e-300)
						continue;
					n.x /= nn; n.y /= nn; n.z /= nn;
					for (int j = 1; j < 16; j++)
					{
						sc_vec3		p = sc_pix_edge_point(order, pix, e, j / 16.0);
						double		dev = fabs(asin(fmin(1.0, fabs(sc_dot(n, p)))));

						worst_ratio = fmax(worst_ratio, dev / (s * s));
						worst_corner = fmax(worst_corner, sc_angle(centre, p) / fmax(maxcorner, 1e-300));
					}
				}
			}
		}
		printf("edge bulge <= %.4f * size^2; boundary point / max-corner distance <= %.4f\n",
			   worst_ratio, worst_corner);
		/*
		 * What the covering relies on (cover.c, classify_cone_exact): a
		 * cell's corners bound it, so no boundary point is farther from the
		 * centre than its farthest corner. The bulge itself is measured per
		 * cell there rather than modelled, and is only reported here.
		 */
		CHECK(worst_corner <= 1.0, "a boundary point lies %.4f x farther from the centre"
			  " than the farthest corner", worst_corner);
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
	return failures ? 1 : 0;
}
