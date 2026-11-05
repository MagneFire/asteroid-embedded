/* The turn eases out (a quadratic), as the launcher's RotationAnimation
 * with Easing.OutQuad. Whole quarter turns are exact, so the picture
 * lands on the pixel grid. */
#include "rotation.h"

#define HALF_PI 1.5707963f

static int size, angle;
static float from, to;
static uint32_t start;
static bool turning;
static int held = -1;

void rotation_init(int n)
{
	size = n;
}

static float shown(uint32_t now)
{
	if (!turning)
		return (float)angle;
	uint32_t t = now - start;
	if (t >= ROTATION_MS) {
		turning = false;
		return (float)angle;
	}
	float p = (float)t / ROTATION_MS;
	return from + (to - from) * (1.0f - (1.0f - p) * (1.0f - p));
}

void rotation_set(int degrees, uint32_t now)
{
	if (!size)
		return;
	if (held >= 0)
		degrees = held;
	degrees = ((degrees % 360) + 360) % 360;
	if (degrees == angle)
		return;
	float a = shown(now), d = (float)degrees - a;
	while (d > 180.0f)
		d -= 360.0f;
	while (d <= -180.0f)
		d += 360.0f;
	from = a;
	to = a + d;
	start = now;
	turning = true;
	angle = degrees;
}

int rotation_angle(void)
{
	return angle;
}

void rotation_hold(int degrees, uint32_t now)
{
	held = -1;
	if (degrees >= 0)
		rotation_set(degrees, now);
	held = degrees < 0 ? -1 : angle;
}

bool rotation_animating(uint32_t now)
{
	return turning && now - start < ROTATION_MS;
}

/* The angle in whole quarter turns and what is left, at most a quarter
 * turn, for which a short series is close enough. */
void rotation_turn(uint32_t now, float *c, float *s)
{
	float a = shown(now);
	while (a < 0.0f)
		a += 360.0f;
	while (a >= 360.0f)
		a -= 360.0f;
	int q = (int)(a / 90.0f);
	float r = (a - 90.0f * q) * (HALF_PI / 90.0f), r2 = r * r;
	float sr = r * (1.0f - r2 / 6.0f * (1.0f - r2 / 20.0f * (1.0f - r2 / 42.0f)));
	float cr = 1.0f - r2 / 2.0f * (1.0f - r2 / 12.0f * (1.0f - r2 / 30.0f * (1.0f - r2 / 56.0f)));
	static const int qc[4] = { 1, 0, -1, 0 }, qs[4] = { 0, 1, 0, -1 };
	*c = cr * qc[q & 3] - sr * qs[q & 3];
	*s = sr * qc[q & 3] + cr * qs[q & 3];
}

void rotation_touch(int *x, int *y)
{
	int sx = *x, sy = *y, m = size - 1;
	switch (angle) {
	case 90:
		*x = sy;
		*y = m - sx;
		break;
	case 180:
		*x = m - sx;
		*y = m - sy;
		break;
	case 270:
		*x = m - sy;
		*y = sx;
		break;
	}
}

/* Four pixels at a time go round the quarter turn: the one at (x, y)
 * to (m - y, x) for a clockwise one. */
void rotation_buffer(uint32_t *buf, uint32_t stride)
{
	int m = size - 1;
	if (angle == 180) {
		for (int y = 0; y < (size + 1) / 2; y++)
			for (int x = 0; x < (y == m - y ? (size + 1) / 2 : size); x++) {
				uint32_t *a = &buf[y * stride + x], *b = &buf[(m - y) * stride + (m - x)], t = *a;
				*a = *b;
				*b = t;
			}
		return;
	}
	if (angle != 90 && angle != 270)
		return;
	for (int y = 0; y < size / 2; y++)
		for (int x = 0; x < (size + 1) / 2; x++) {
			uint32_t *p0 = &buf[y * stride + x], *p1 = &buf[x * stride + (m - y)];
			uint32_t *p2 = &buf[(m - y) * stride + (m - x)], *p3 = &buf[(m - x) * stride + y];
			uint32_t t = *p3;
			if (angle == 90) {
				*p3 = *p2;
				*p2 = *p1;
				*p1 = *p0;
				*p0 = t;
			} else {
				t = *p0;
				*p0 = *p1;
				*p1 = *p2;
				*p2 = *p3;
				*p3 = t;
			}
		}
}
