/* Each sample is kept in one of 26 directions around the current
 * centre, the latest in each, and a sphere is fitted through them by
 * least squares: its centre is the hard-iron offset. The accuracy grows
 * with the directions covered and how well they lie on the sphere. A
 * field that stays far off the sphere's radius means the iron around
 * the watch changed: the directions start over. */
#include <string.h>
#include "sensors/compass.h"

#define MIN_BINS	8
#define MEDIUM_BINS	12
#define HIGH_BINS	18
/* The root mean square distance from the sphere, as a part of its
 * radius, for each accuracy from low up. */
#define LOW_FIT		0.12f
#define MEDIUM_FIT	0.07f
#define HIGH_FIT	0.04f
/* Around the Earth's 22-67 µT. */
#define RADIUS_MIN	15.0f
#define RADIUS_MAX	120.0f
#define REFIT_UPDATES	8
/* The spread's determinant over the cube of its mean variance: 1 for
 * samples all round, 0 for samples in one plane, which any sphere
 * through their circle fits. */
#define FLAT_MIN	0.2f
#define OFF_FIELD	0.3f
#define OFF_FIELD_MAX	16
/* µT, summed over the axes: a refit that moves the offset less is not
 * worth a flash write. */
#define SAVE_MOVE	1.0f
#define PI		3.14159265f
#define DEG		(180 / PI)

enum { UNRELIABLE, LOW, MEDIUM, HIGH };

static float root(float v)
{
#if defined(__arm__) && defined(__ARM_FP)
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(v));
	return r;
#else
	return __builtin_sqrtf(v);
#endif
}

static float absf(float v) { return v < 0 ? -v : v; }

/* Within 1e-5 rad. */
static float arctan2(float y, float x)
{
	float ax = absf(x), ay = absf(y);
	if (ax == 0 && ay == 0)
		return 0;
	bool swap = ay > ax;
	float t = swap ? ax / ay : ay / ax, t2 = t * t;
	float a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	if (swap)
		a = PI / 2 - a;
	if (x < 0)
		a = PI - a;
	return y < 0 ? -a : a;
}

static float len3(const float v[3]) { return root(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

void compass_init(struct compass *c, const struct compass_cal *saved)
{
	memset(c, 0, sizeof *c);
	if (saved && saved->radius >= RADIUS_MIN && saved->radius <= RADIUS_MAX && saved->status > UNRELIABLE) {
		c->cal = *saved;
		/* Until it fits samples of its own. */
		c->cal.status = LOW;
		c->suspect = true;
	}
}

unsigned compass_bins(const struct compass *c) { return (unsigned)__builtin_popcount(c->filled); }

static int bin_of(const float d[3])
{
	float n = len3(d);
	if (n < 1e-3f)
		return -1;
	int k = 0;
	for (int i = 0; i < 3; i++)
		k = k * 3 + (d[i] > 0.5f * n ? 2 : d[i] < -0.5f * n ? 0 : 1);
	/* 13 is the centre, which a unit vector never quantises to. */
	return k > 13 ? k - 1 : k;
}

/* Solves the 4x4 system m·x = m[.][4] in place; false if singular. */
static bool solve(float m[4][5], float x[4])
{
	float scale = 0;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			if (absf(m[i][j]) > scale)
				scale = absf(m[i][j]);
	for (int col = 0; col < 4; col++) {
		int p = col;
		for (int r = col + 1; r < 4; r++)
			if (absf(m[r][col]) > absf(m[p][col]))
				p = r;
		if (absf(m[p][col]) <= scale * 1e-6f)
			return false;
		if (p != col)
			for (int j = 0; j < 5; j++) {
				float t = m[col][j];
				m[col][j] = m[p][j];
				m[p][j] = t;
			}
		for (int r = col + 1; r < 4; r++) {
			float f = m[r][col] / m[col][col];
			for (int j = col; j < 5; j++)
				m[r][j] -= f * m[col][j];
		}
	}
	for (int i = 3; i >= 0; i--) {
		float s = m[i][4];
		for (int j = i + 1; j < 4; j++)
			s -= m[i][j] * x[j];
		x[i] = s / m[i][i];
	}
	return true;
}

/* |p|² = 2a·p + k for p taken from their mean, which keeps the sums
 * small enough for floats. */
static bool fit(const struct compass *c, struct compass_cal *out)
{
	float mean[3] = { 0 }, n = 0;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (c->filled & 1u << b) {
			for (int i = 0; i < 3; i++)
				mean[i] += c->bin[b][i];
			n++;
		}
	if (n < MIN_BINS)
		return false;
	for (int i = 0; i < 3; i++)
		mean[i] /= n;
	float m[4][5] = { { 0 } };
	for (int b = 0; b < COMPASS_BINS; b++) {
		if (!(c->filled & 1u << b))
			continue;
		float r[5] = { c->bin[b][0] - mean[0], c->bin[b][1] - mean[1], c->bin[b][2] - mean[2], 1, 0 };
		r[4] = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 5; j++)
				m[i][j] += r[i] * r[j];
	}
	float tr = (m[0][0] + m[1][1] + m[2][2]) / 3;
	float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	float x[4];
	if (tr <= 0 || det < FLAT_MIN * tr * tr * tr || !solve(m, x))
		return false;
	float centre[3] = { x[0] / 2, x[1] / 2, x[2] / 2 };
	float r2 = x[3] + centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2];
	if (r2 < RADIUS_MIN * RADIUS_MIN || r2 > RADIUS_MAX * RADIUS_MAX)
		return false;
	float radius = root(r2), err = 0;
	for (int b = 0; b < COMPASS_BINS; b++) {
		if (!(c->filled & 1u << b))
			continue;
		float d[3];
		for (int i = 0; i < 3; i++)
			d[i] = c->bin[b][i] - mean[i] - centre[i];
		float e = len3(d) - radius;
		err += e * e;
	}
	float part = root(err / n) / radius;
	unsigned bins = compass_bins(c);
	int status = bins >= HIGH_BINS && part < HIGH_FIT	  ? HIGH
		     : bins >= MEDIUM_BINS && part < MEDIUM_FIT ? MEDIUM
		     : part < LOW_FIT				  ? LOW
								  : UNRELIABLE;
	if (status == UNRELIABLE)
		return false;
	for (int i = 0; i < 3; i++)
		out->offset[i] = mean[i] + centre[i];
	out->radius = radius;
	out->status = status;
	return true;
}

int compass_feed(struct compass *c, const float raw[3], float out[3])
{
	float d[3];
	bool had = c->cal.status > UNRELIABLE;
	/* Samples wider apart than any field's sphere are from a magnet
	 * that came or went, such as the charger's: start over from this
	 * one. */
	for (int i = 0; i < 3 && c->seen; i++)
		if (raw[i] - c->lo[i] > 2 * RADIUS_MAX || c->hi[i] - raw[i] > 2 * RADIUS_MAX) {
			c->seen = false;
			c->filled = 0;
		}
	for (int i = 0; i < 3; i++) {
		if (!c->seen || raw[i] < c->lo[i])
			c->lo[i] = raw[i];
		if (!c->seen || raw[i] > c->hi[i])
			c->hi[i] = raw[i];
	}
	c->seen = true;
	for (int i = 0; i < 3; i++)
		d[i] = raw[i] - (had ? c->cal.offset[i] : (c->lo[i] + c->hi[i]) / 2);
	int b = bin_of(d);
	if (b >= 0) {
		bool fresh = !(c->filled & 1u << b);
		memcpy(c->bin[b], raw, sizeof c->bin[b]);
		c->filled |= 1u << b;
		struct compass_cal f;
		if ((fresh || ++c->updates >= REFIT_UPDATES) && fit(c, &f)) {
			c->updates = 0;
			if (c->suspect || f.status >= c->cal.status) {
				float moved = 0;
				for (int i = 0; i < 3; i++)
					moved += absf(f.offset[i] - c->cal.offset[i]);
				c->changed |= f.status != c->cal.status || moved > SAVE_MOVE;
				c->cal = f;
				c->suspect = false;
			}
		}
	}
	for (int i = 0; i < 3; i++)
		out[i] = raw[i] - c->cal.offset[i];
	if (c->cal.status == UNRELIABLE)
		return UNRELIABLE;
	float off = absf(len3(out) - c->cal.radius) / c->cal.radius;
	c->off_field = off > OFF_FIELD ? c->off_field + 1 : 0;
	if (c->off_field >= OFF_FIELD_MAX) {
		c->off_field = 0;
		c->filled = 0;
		c->seen = false;
		c->suspect = true;
		c->cal.status = UNRELIABLE;
		return UNRELIABLE;
	}
	return off > OFF_FIELD && c->cal.status > LOW ? LOW : c->cal.status;
}

bool compass_orientation(const float g[3], const float m[3], float out[3])
{
	float h[3], a[3], n[3];
	float gl = len3(g);
	if (gl == 0)
		return false;
	for (int i = 0; i < 3; i++)
		a[i] = g[i] / gl;
	/* East, then north, in the device's axes. */
	cross(m, a, h);
	float hl = len3(h), ml = len3(m);
	if (hl < 0.1f * ml || hl == 0)
		return false;
	for (int i = 0; i < 3; i++)
		h[i] /= hl;
	cross(a, h, n);
	float az = arctan2(h[1], n[1]) * DEG;
	out[0] = az < 0 ? az + 360 : az;
	out[1] = arctan2(-a[1], a[2]) * DEG;
	out[2] = arctan2(a[0], root(a[1] * a[1] + a[2] * a[2])) * DEG;
	return true;
}
