#include <string.h>
#include "sensors/fmath.h"
#include "sensors/fusion.h"

/* In 1/s: how fast the estimate turns toward gravity and toward the
 * field's north, and both without a gyroscope. */
#define LEVEL_GAIN	1.0f
#define NORTH_GAIN	0.5f
#define BLIND_GAIN	5.0f
/* Gravity within this part of its length, as learnt slowly. */
#define LEVEL_SLACK	0.15f
#define GRAVITY_GAIN	0.01f
/* Still enough for the gyroscope's zero rate: rad/s, then the part of
 * gravity off the estimate, for STILL_SAMPLES; learnt at BIAS_GAIN of
 * the difference a sample. */
#define STILL_RATE	0.05f
#define STILL_TILT	0.02f
#define STILL_SAMPLES	25
#define BIAS_GAIN	0.02f
/* A field off the strength and the dip it had while trusted by more
 * than these is a magnet's; they follow it at FIELD_GAIN a sample. */
#define STRENGTH_SLACK	0.15f
#define DIP_SLACK	0.1f
#define FIELD_GAIN	0.02f
/* Longer without an update starts over. */
#define GAP_S		0.5f
#define STEP_MAX_S	0.1f

static float len3(const float v[3]) { return fm_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void matrix(const float q[4], float r[3][3])
{
	float w = q[0], x = q[1], y = q[2], z = q[3];
	r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - w * z), r[0][2] = 2 * (x * z + w * y);
	r[1][0] = 2 * (x * y + w * z), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - w * x);
	r[2][0] = 2 * (x * z - w * y), r[2][1] = 2 * (y * z + w * x), r[2][2] = 1 - 2 * (x * x + y * y);
}

static void normalise(float q[4])
{
	float n = fm_sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (int i = 0; i < 4; i++)
		q[i] /= n;
}

/* The rows of the rotation are east, north and up in the watch's axes. */
static bool start(struct fusion *f, const float a[3], const float m[3])
{
	float up[3], east[3], north[3], al = len3(a);
	if (al == 0)
		return false;
	for (int i = 0; i < 3; i++)
		up[i] = a[i] / al;
	cross(m, up, east);
	float el = len3(east);
	if (el < 0.1f * len3(m) || el == 0)
		return false;
	for (int i = 0; i < 3; i++)
		east[i] /= el;
	cross(up, east, north);
	float r[3][3] = { { east[0], east[1], east[2] }, { north[0], north[1], north[2] }, { up[0], up[1], up[2] } };
	float t = r[0][0] + r[1][1] + r[2][2], *q = f->q;
	if (t > 0) {
		float s = fm_sqrt(t + 1) * 2;
		q[0] = s / 4, q[1] = (r[2][1] - r[1][2]) / s, q[2] = (r[0][2] - r[2][0]) / s, q[3] = (r[1][0] - r[0][1]) / s;
	} else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
		float s = fm_sqrt(1 + r[0][0] - r[1][1] - r[2][2]) * 2;
		q[0] = (r[2][1] - r[1][2]) / s, q[1] = s / 4, q[2] = (r[0][1] + r[1][0]) / s, q[3] = (r[0][2] + r[2][0]) / s;
	} else if (r[1][1] > r[2][2]) {
		float s = fm_sqrt(1 + r[1][1] - r[0][0] - r[2][2]) * 2;
		q[0] = (r[0][2] - r[2][0]) / s, q[1] = (r[0][1] + r[1][0]) / s, q[2] = s / 4, q[3] = (r[1][2] + r[2][1]) / s;
	} else {
		float s = fm_sqrt(1 + r[2][2] - r[0][0] - r[1][1]) * 2;
		q[0] = (r[1][0] - r[0][1]) / s, q[1] = (r[0][2] + r[2][0]) / s, q[2] = (r[1][2] + r[2][1]) / s, q[3] = s / 4;
	}
	normalise(q);
	f->started = true;
	return true;
}

void fusion_init(struct fusion *f) { memset(f, 0, sizeof *f); }

/* The field's horizontal part, east and north, with the estimate. */
static bool north_of(const float r[3][3], const float m[3], float *e, float *n)
{
	*e = r[0][0] * m[0] + r[0][1] * m[1] + r[0][2] * m[2];
	*n = r[1][0] * m[0] + r[1][1] * m[1] + r[1][2] * m[2];
	return fm_sqrt(*e * *e + *n * *n) >= 0.1f * len3(m);
}

/* Turned about the world's z until the field points north. */
static void turn_north(struct fusion *f, float e, float n)
{
	float h = fm_sqrt(e * e + n * n), c = n / h;
	float z[4] = { fm_sqrt((1 + c) / 2), 0, 0, fm_sqrt((1 - c) / 2) }, *q = f->q, t[4];
	if (e < 0)
		z[3] = -z[3];
	t[0] = z[0] * q[0] - z[3] * q[3];
	t[1] = z[0] * q[1] - z[3] * q[2];
	t[2] = z[0] * q[2] + z[3] * q[1];
	t[3] = z[0] * q[3] + z[3] * q[0];
	memcpy(q, t, sizeof t);
	normalise(q);
}

/* Whether the field is the one trusted before, and the north it says
 * can turn the estimate. */
static bool trusted(struct fusion *f, const float m[3], const float up[3], int accuracy, bool raised)
{
	float ml = len3(m);
	if (!accuracy || ml == 0)
		return false;
	float dip = (m[0] * up[0] + m[1] * up[1] + m[2] * up[2]) / ml;
	if (raised) {
		f->strength = ml;
		f->dip = dip;
		return true;
	}
	if (fm_abs(ml - f->strength) > STRENGTH_SLACK * f->strength || fm_abs(dip - f->dip) > DIP_SLACK)
		return false;
	f->strength += FIELD_GAIN * (ml - f->strength);
	f->dip += FIELD_GAIN * (dip - f->dip);
	return true;
}

void fusion_update(struct fusion *f, const float a[3], const float w[3], const float m[3], int accuracy, float dt)
{
	if (dt > GAP_S)
		f->started = false;
	if (!f->started) {
		if (m && start(f, a, m))
			f->level = 0;
		return;
	}
	if (dt <= 0)
		return;
	if (dt > STEP_MAX_S)
		dt = STEP_MAX_S;
	float r[3][3], rate[3] = { 0, 0, 0 }, up[3];
	matrix(f->q, r);
	up[0] = r[2][0], up[1] = r[2][1], up[2] = r[2][2];
	float level = w ? LEVEL_GAIN : BLIND_GAIN, north = w ? NORTH_GAIN : BLIND_GAIN;
	if (w)
		for (int i = 0; i < 3; i++)
			rate[i] = w[i] - f->bias[i];
	float al = len3(a), tilt = 1;
	if (al > 0) {
		float an[3], e[3];
		for (int i = 0; i < 3; i++)
			an[i] = a[i] / al;
		cross(an, up, e);
		tilt = len3(e);
		/* The arm's own acceleration is not gravity. */
		if (!f->gravity)
			f->gravity = al;
		bool steady = fm_abs(al - f->gravity) < LEVEL_SLACK * f->gravity;
		f->gravity += GRAVITY_GAIN * (al - f->gravity);
		if (steady)
			for (int i = 0; i < 3; i++)
				rate[i] += level * e[i];
	}
	/* A field more accurate than any before sets the heading at once:
	 * the one it started from may have had the offset in it, and a
	 * newer offset is a better one. */
	float e, n;
	bool raised = accuracy > f->level;
	if (m && north_of(r, m, &e, &n) && trusted(f, m, up, accuracy, raised)) {
		if (raised) {
			turn_north(f, e, n);
			f->level = accuracy;
		} else
			for (int i = 0; i < 3; i++)
				rate[i] += north * fm_atan2(e, n) * up[i];
	}
	if (w) {
		float d[3] = { w[0] - f->bias[0], w[1] - f->bias[1], w[2] - f->bias[2] };
		f->still = len3(d) < STILL_RATE && tilt < STILL_TILT ? f->still + 1 : 0;
		if (f->still >= STILL_SAMPLES)
			for (int i = 0; i < 3; i++)
				f->bias[i] += BIAS_GAIN * d[i];
	}
	float *q = f->q, h = dt / 2;
	float dq[4] = { -q[1] * rate[0] - q[2] * rate[1] - q[3] * rate[2], q[0] * rate[0] + q[2] * rate[2] - q[3] * rate[1],
			q[0] * rate[1] - q[1] * rate[2] + q[3] * rate[0], q[0] * rate[2] + q[1] * rate[1] - q[2] * rate[0] };
	for (int i = 0; i < 4; i++)
		q[i] += h * dq[i];
	normalise(q);
}

bool fusion_angles(const struct fusion *f, float out[3])
{
	if (!f->started)
		return false;
	float r[3][3];
	matrix(f->q, r);
	float az = fm_atan2(r[0][1], r[1][1]) * 180 / FM_PI;
	out[0] = az < 0 ? az + 360 : az;
	out[1] = fm_atan2(-r[2][1], r[2][2]) * 180 / FM_PI;
	out[2] = fm_atan2(r[2][0], fm_sqrt(r[2][1] * r[2][1] + r[2][2] * r[2][2])) * 180 / FM_PI;
	return true;
}
