/* Lying within 30 degrees of flat, face up or down, the screen is
 * upright, as asteroid-launcher takes Qt's FaceUp and FaceDown readings
 * for the primary orientation. Tilted further, gravity has to be within
 * about 27 degrees of one of the edges; held at a diagonal the angle
 * stays. A new angle is taken once it has held for ORIENT_HOLD_MS. */
#include "orient.h"

void orient_reset(struct orient *o)
{
	o->angle = 0;
	o->candidate = -1;
	o->since = 0;
}

static int side(const int16_t v[3])
{
	int32_t x = v[0], y = v[1], z = v[2];
	int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
	if (3 * (x * x + y * y) <= z * z)
		return 0;
	if (ay >= 2 * ax)
		return y > 0 ? 0 : 180;
	if (ax >= 2 * ay)
		return x > 0 ? 90 : 270;
	return -1;
}

int orient_update(struct orient *o, const int16_t v[3], uint32_t now)
{
	int s = side(v);
	if (s < 0 || s == o->angle) {
		o->candidate = -1;
		return o->angle;
	}
	if (s != o->candidate) {
		o->candidate = s;
		o->since = now;
	} else if (now - o->since >= ORIENT_HOLD_MS) {
		o->angle = s;
		o->candidate = -1;
	}
	return o->angle;
}
