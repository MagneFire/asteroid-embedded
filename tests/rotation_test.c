/* The screen rotation: a touch lands on the pixel of the picture that
 * the turned buffer shows there, quarter turns are exact, and the turn
 * goes the shorter way round. */
#include <stdio.h>
#include <math.h>
#include "ui/rotation.h"

#define N 7

static int failures;

static void check(const char *what, int ok)
{
	if (!ok) {
		fprintf(stderr, "%s\n", what);
		failures++;
	}
}

static void turns_and_touches(int degrees)
{
	uint32_t buf[N * (N + 2)];
	rotation_set(degrees, 0);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++)
			buf[y * (N + 2) + x] = (uint32_t)(y * N + x);
	rotation_buffer(buf, N + 2);
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			int px = x, py = y;
			rotation_touch(&px, &py);
			if (buf[y * (N + 2) + x] != (uint32_t)(py * N + px)) {
				fprintf(stderr, "%d degrees: (%d,%d) shows %u, touches (%d,%d)\n", degrees, x, y,
					buf[y * (N + 2) + x], px, py);
				failures++;
				return;
			}
		}
}

int main(void)
{
	float c, s;
	rotation_init(N);
	turns_and_touches(90);
	turns_and_touches(180);
	turns_and_touches(270);
	turns_and_touches(0);

	/* Clockwise: at 90 degrees the picture's top is on the right. */
	rotation_set(90, 0);
	int x = N - 1, y = N / 2;
	rotation_touch(&x, &y);
	check("90: the right edge is the top", x == N / 2 && y == 0);

	rotation_set(0, 1000);
	rotation_set(90, 2000);
	rotation_turn(2000 + ROTATION_MS, &c, &s);
	check("90 exactly", c == 0.0f && s == 1.0f);
	check("done turning", !rotation_animating(2000 + ROTATION_MS));
	rotation_set(180, 3000);
	rotation_turn(3000 + ROTATION_MS, &c, &s);
	check("180 exactly", c == -1.0f && s == 0.0f);

	/* Halfway in time is three quarters of the way, eased out. */
	rotation_set(0, 4000);
	rotation_set(90, 5000);
	check("turning", rotation_animating(5000 + ROTATION_MS / 2));
	rotation_turn(5000 + ROTATION_MS / 2, &c, &s);
	float a = 67.5f * 3.14159265f / 180.0f;
	check("eased", fabsf(c - cosf(a)) < 1e-3f && fabsf(s - sinf(a)) < 1e-3f);

	/* From 270 to 0 the short way: through 315, not 135. */
	rotation_set(270, 6000);
	rotation_set(0, 7000);
	rotation_turn(7000 + ROTATION_MS / 2, &c, &s);
	a = 337.5f * 3.14159265f / 180.0f;
	check("shorter way", fabsf(c - cosf(a)) < 1e-3f && fabsf(s - sinf(a)) < 1e-3f);

	if (failures)
		return 1;
	printf("rotation_test: ok\n");
	return 0;
}
