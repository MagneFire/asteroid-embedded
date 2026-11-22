/* Wrist raises from synthetic 26 Hz accelerometer samples: the arm
 * lowered and raised, the face turned away, arm swings while walking,
 * a raise still moving when the check starts, and arming with the face
 * up. The hub's trigger is called where its 6D position would change. */
#include <stdio.h>
#include <stdlib.h>
#include "sensors/raise.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define SAMPLE_MS	38

static struct raise r;
static uint32_t now;
static unsigned raises;
static uint32_t raised_at;

static const int16_t down[3] = { -1000, 0, 0 }, view[3] = { -100, 400, 900 }, steep[3] = { 0, 780, 620 },
		     face_down[3] = { 0, 0, -1000 }, side[3] = { 0, -1000, 0 };

static void sample(const int16_t v[3])
{
	now += SAMPLE_MS;
	if (raise_sample(&r, v, now)) {
		raises++;
		raised_at = now;
	}
}

static void hold(const int16_t v[3], uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += SAMPLE_MS)
		sample(v);
}

/* From a to b in ms, with a jolt of extra acceleration on the way; the
 * hub triggers when the pose is halfway. */
static void turn(const int16_t a[3], const int16_t b[3], uint32_t ms, bool trigger)
{
	unsigned n = ms / SAMPLE_MS;
	for (unsigned k = 1; k <= n; k++) {
		int16_t v[3];
		for (int i = 0; i < 3; i++)
			v[i] = (int16_t)(a[i] + (b[i] - a[i]) * (int)k / (int)n + (k % 2 ? 350 : -350));
		if (trigger && k == n / 2)
			raise_check(&r, now);
		sample(v);
	}
}

int main(void)
{
	raise_arm(&r, now);
	hold(view, 2000);
	CHECK(raises == 0);

	turn(view, down, 400, true);
	hold(down, 1000);
	CHECK(raises == 0 && !r.in_view);
	turn(down, view, 400, true);
	uint32_t still = now;
	hold(view, 1000);
	CHECK(raises == 1 && raised_at - still <= 2 * SAMPLE_MS);

	/* Turned away, back facing up from the side, and steeply up. */
	turn(view, side, 300, true);
	hold(side, 1000);
	CHECK(raises == 1);
	turn(side, view, 300, true);
	hold(view, 800);
	CHECK(raises == 2);
	turn(view, down, 300, true);
	hold(down, 800);
	turn(down, steep, 300, true);
	hold(steep, 800);
	CHECK(raises == 3);
	turn(steep, face_down, 300, true);
	hold(face_down, 800);
	CHECK(raises == 3 && !r.in_view);

	/* Swings at the side while walking, the face never up. */
	raise_check(&r, now);
	for (int k = 0; k < 40; k++) {
		int16_t v[3] = { (int16_t)(-1000 + (k % 4) * 150), (int16_t)((k % 2 ? 1 : -1) * 500), 80 };
		if (k % 6 == 0)
			raise_check(&r, now);
		sample(v);
	}
	hold(down, RAISE_CHECK_MS);
	CHECK(raises == 3);

	/* Facing up without the hub seeing a change says nothing; a check
	 * that starts before the arm is at rest waits for it. */
	turn(down, view, 300, false);
	hold(view, 1000);
	CHECK(raises == 3);
	turn(view, down, 300, true);
	hold(down, 600);
	raise_check(&r, now);
	for (int k = 0; k < 6; k++) {
		int16_t v[3] = { view[0], view[1], (int16_t)(view[2] + (k % 2 ? 700 : -200)) };
		sample(v);
	}
	CHECK(raises == 3);
	hold(view, 300);
	CHECK(raises == 4);

	/* Lowered with a long swing before it rests, then raised. */
	turn(view, down, 1600, true);
	hold(down, 1000);
	turn(down, view, 400, true);
	hold(view, 500);
	CHECK(raises == 5);
	/* Lowered while walking, never at rest, then raised. */
	raise_check(&r, now);
	for (int k = 0; k < 60; k++) {
		int16_t v[3] = { (int16_t)(-1000 + (k % 4) * 150), (int16_t)((k % 2 ? 1 : -1) * 500), 80 };
		if (k % 6 == 0)
			raise_check(&r, now);
		sample(v);
	}
	turn(down, view, 400, true);
	hold(view, 500);
	CHECK(raises == 6);
	/* A jolt while looking at it is not a raise. */
	raise_check(&r, now);
	for (int k = 0; k < 4; k++) {
		int16_t v[3] = { view[0], view[1], (int16_t)(k % 2 ? 1400 : 200) };
		sample(v);
	}
	hold(view, 500);
	CHECK(raises == 6);

	/* A check runs out. */
	turn(view, down, 300, true);
	hold(down, RAISE_CHECK_MS + 200);
	CHECK(!raise_checking(&r, now));
	hold(view, 500);
	CHECK(raises == 6);

	if (failures)
		return 1;
	printf("raise_test: ok\n");
	return 0;
}
