/* The screen orientation from accelerometer samples. */
#include <stdio.h>
#include "sensors/orient.h"

static int failures;

static void expect(const char *what, int got, int want)
{
	if (got != want) {
		fprintf(stderr, "%s: got %d, want %d\n", what, got, want);
		failures++;
	}
}

/* Feeds the sample every 100 ms for ms and returns the last angle. */
static int hold(struct orient *o, int16_t x, int16_t y, int16_t z, uint32_t *t, uint32_t ms)
{
	const int16_t v[3] = { x, y, z };
	int a = o->angle;
	for (uint32_t i = 0; i <= ms / 100; i++, *t += 100)
		a = orient_update(o, v, *t);
	return a;
}

int main(void)
{
	struct orient o;
	uint32_t t = 1000;
	orient_reset(&o);
	expect("start", o.angle, 0);
	expect("upright", hold(&o, 0, 2048, 0, &t, 1000), 0);
	expect("right edge up, too soon", hold(&o, 2048, 0, 0, &t, ORIENT_HOLD_MS - 100), 0);
	expect("right edge up", hold(&o, 2048, 0, 0, &t, 200), 90);
	expect("upside down", hold(&o, 0, -2048, 0, &t, 1000), 180);
	expect("left edge up", hold(&o, -2048, 0, 0, &t, 1000), 270);
	expect("diagonal keeps it", hold(&o, 1400, 1400, 0, &t, 2000), 270);
	expect("face up, too soon", hold(&o, 100, 200, 2048, &t, ORIENT_HOLD_MS - 100), 270);
	expect("face up is upright", hold(&o, 100, 200, 2048, &t, 200), 0);
	expect("tilted 45 degrees towards the right", hold(&o, 1448, 0, 1448, &t, 1000), 90);
	expect("barely tilted is upright", hold(&o, 1000, 0, 2048, &t, 1000), 0);
	hold(&o, 0, -2048, 0, &t, 1000);
	expect("face down is upright", hold(&o, 0, 100, -2048, &t, 1000), 0);

	/* A brief turn does not count: the hold starts over. */
	orient_reset(&o);
	t = 0;
	hold(&o, 2048, 0, 0, &t, 200);
	hold(&o, 0, 2048, 0, &t, 100);
	expect("brief turn", hold(&o, 2048, 0, 0, &t, 200), 0);

	/* The hold is timed across the millisecond counter's wrap. */
	orient_reset(&o);
	t = UINT32_MAX - 199;
	expect("across the wrap", hold(&o, 2048, 0, 0, &t, ORIENT_HOLD_MS), 90);

	if (failures)
		return 1;
	printf("orient_test: ok\n");
	return 0;
}
