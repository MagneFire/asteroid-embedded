/* The orientation against Android's conventions at 50 Hz: the cardinal
 * headings, the top and the right edge raised, a turn the gyroscope
 * sees while the field cannot be trusted, a field that turns without
 * the watch (held, as the vendor's library holds it, while it is
 * unreliable, and followed slowly once it can be trusted), a magnet's
 * field the compass still trusts, and the gyroscope's zero rate learnt
 * while still. */
#include <math.h>
#include <stdio.h>
#include "sensors/fusion.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define DT	0.02f
#define PI	3.14159265f

static struct fusion f;
static float out[3];

/* The world's field and gravity in the watch's axes, for a heading of
 * its y axis (clockwise from north) with the watch flat. */
static void flat(float heading, float a[3], float m[3])
{
	float h = heading * PI / 180;
	a[0] = 0, a[1] = 0, a[2] = 9.8f;
	m[0] = -30 * sinf(h), m[1] = 30 * cosf(h), m[2] = -36;
}

static void run(const float a[3], const float w[3], const float m[3], int accuracy, int n)
{
	for (int k = 0; k < n; k++)
		fusion_update(&f, a, w, m, accuracy, DT);
	fusion_angles(&f, out);
}

static float off(float az, float want)
{
	float d = fmodf(fabsf(az - want), 360);
	return d > 180 ? 360 - d : d;
}

int main(void)
{
	float a[3], m[3], w[3] = { 0, 0, 0 };

	fusion_init(&f);
	CHECK(!fusion_angles(&f, out));
	static const float headings[] = { 0, 90, 180, 270 };
	for (int i = 0; i < 4; i++) {
		fusion_init(&f);
		flat(headings[i], a, m);
		run(a, w, m, 3, 10);
		CHECK(off(out[0], headings[i]) < 0.5f && fabsf(out[1]) < 0.5f && fabsf(out[2]) < 0.5f);
		CHECK(out[0] >= 0 && out[0] < 360);
	}

	/* The top edge raised 30° is pitch -30; the right edge, roll 30. */
	static const float field[3] = { 0, 30, -36 };
	float top[3] = { 0, 9.8f * sinf(PI / 6), 9.8f * cosf(PI / 6) };
	float tm[3] = { 0, field[1] * cosf(PI / 6) + field[2] * sinf(PI / 6), -field[1] * sinf(PI / 6) + field[2] * cosf(PI / 6) };
	fusion_init(&f);
	run(top, w, tm, 3, 10);
	CHECK(fabsf(out[1] + 30) < 0.5f && fabsf(out[2]) < 0.5f && off(out[0], 0) < 0.5f);
	float right[3] = { 9.8f * sinf(PI / 6), 0, 9.8f * cosf(PI / 6) };
	float rm[3] = { field[2] * sinf(PI / 6), field[1], field[2] * cosf(PI / 6) };
	fusion_init(&f);
	run(right, w, rm, 3, 10);
	CHECK(fabsf(out[2] - 30) < 0.5f && fabsf(out[1]) < 0.5f && off(out[0], 0) < 0.5f);

	/* Turned 90° clockwise in 1 s on a magnet's field, which cannot be
	 * trusted: the gyroscope carries the heading round. */
	fusion_init(&f);
	flat(0, a, m);
	run(a, w, m, 3, 10);
	static const float magnet[3] = { 600, -250, -230 };
	w[2] = -PI / 2;
	run(a, w, magnet, 0, 50);
	w[2] = 0;
	run(a, w, magnet, 0, 100);
	CHECK(off(out[0], 90) < 2);

	/* The field turning 90° on its own, the watch still: held while it
	 * cannot be trusted; once it can, followed over seconds. */
	fusion_init(&f);
	flat(0, a, m);
	run(a, w, m, 3, 10);
	flat(270, a, m);
	run(a, w, m, 0, 100);
	CHECK(off(out[0], 0) < 0.5f);
	run(a, w, m, 3, 25);
	CHECK(off(out[0], 0) > 5 && off(out[0], 0) < 45);
	run(a, w, m, 3, 500);
	CHECK(off(out[0], 270) < 1);

	/* Started on a field that is not yet trusted, the first one that
	 * is sets the heading at once, and so does a more accurate one. */
	fusion_init(&f);
	flat(0, a, m);
	run(a, w, magnet, 0, 10);
	flat(30, a, m);
	run(a, w, m, 1, 1);
	CHECK(off(out[0], 30) < 1);
	flat(0, a, m);
	run(a, w, m, 1, 10);
	CHECK(off(out[0], 30) < 5);
	run(a, w, m, 3, 1);
	CHECK(off(out[0], 0) < 1);

	/* A field of another strength or dip than the trusted one is not
	 * the Earth's, whatever its accuracy. */
	static const float stronger[3] = { -45, 0, -54 }, steeper[3] = { -15, 0, -44 };
	run(a, w, stronger, 3, 200);
	CHECK(off(out[0], 0) < 0.5f);
	run(a, w, steeper, 3, 200);
	CHECK(off(out[0], 0) < 0.5f);

	/* A gyroscope 1°/s off while still, the field not trusted: the
	 * heading hardly drifts once its zero rate is learnt. */
	fusion_init(&f);
	flat(0, a, m);
	run(a, w, m, 3, 10);
	w[2] = PI / 180;
	run(a, w, m, 0, 50 * 60);
	CHECK(off(out[0], 0) < 5 && fabsf(f.bias[2] - PI / 180) < 0.002f);
	w[2] = 0;

	/* Straight up on its top edge, no number is out of range. */
	fusion_init(&f);
	float up[3] = { 0, 9.8f, 0 }, um[3] = { 0, -36, -30 };
	run(up, w, um, 3, 10);
	CHECK(fabsf(out[1] + 90) < 1 && out[0] >= 0 && out[0] < 360 && out[0] == out[0]);

	if (failures)
		return 1;
	printf("fusion_test: ok\n");
	return 0;
}
