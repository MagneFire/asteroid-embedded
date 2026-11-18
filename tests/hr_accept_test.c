/* Which catfish hub heart rates are kept: none before the window is full
 * and steady, a trusted one after 20 s, an untrusted one after 40 s, the
 * climb from 110 never, and a gain jump setting the window back. */
#include <stdio.h>
#include "sensors/hr_accept.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* Feeds n rates a second apart from t, returning the last kept one. */
static unsigned feed(struct hr_accept *a, const float *bpm, const float *y, unsigned n, uint32_t *t)
{
	unsigned kept = 0;
	for (unsigned i = 0; i < n; i++, *t += 1000) {
		unsigned k = hr_accept_take(a, bpm[i], y[i], 0, *t);
		if (k)
			kept = k;
	}
	return kept;
}

int main(void)
{
	struct hr_accept a;
	uint32_t t = 1000;
	float bpm[60], y[60];

	/* Steady at 78 with no trust: kept only from 40 s on. */
	hr_accept_reset(&a);
	for (unsigned i = 0; i < 60; i++) {
		bpm[i] = 78 + (i % 3);
		y[i] = i % 7 ? 1 : 0;
	}
	CHECK(feed(&a, bpm, y, 39, &t) == 0);
	CHECK(feed(&a, bpm + 39, y + 39, 3, &t) == 79);

	/* Trusted once in the window: kept from 20 s on. */
	hr_accept_reset(&a);
	t = 1000;
	y[15] = 2;
	CHECK(feed(&a, bpm, y, 19, &t) == 0);
	CHECK(feed(&a, bpm + 19, y + 19, 2, &t) != 0);

	/* The climb from 110 is never steady enough. */
	hr_accept_reset(&a);
	t = 1000;
	for (unsigned i = 0; i < 60; i++) {
		bpm[i] = 110 + i;
		y[i] = 2;
	}
	CHECK(feed(&a, bpm, y, 60, &t) == 0);

	/* A jump in the gain starts the window over. */
	hr_accept_reset(&a);
	t = 1000;
	for (unsigned i = 0; i < 60; i++) {
		bpm[i] = 80;
		y[i] = 3;
	}
	CHECK(feed(&a, bpm, y, 25, &t) == 80);
	CHECK(hr_accept_take(&a, 80, 3, 1, t) == 0);
	t += 1000;
	CHECK(feed(&a, bpm, y, 11, &t) == 0);
	CHECK(feed(&a, bpm, y, 1, &t) == 80);

	/* Out of range, or the library not ready. */
	CHECK(hr_accept_take(&a, 250, 3, 0, t) == 0);
	CHECK(hr_accept_take(&a, 80, -1, 0, t) == 0);

	if (failures)
		return 1;
	printf("hr_accept_test: ok\n");
	return 0;
}
