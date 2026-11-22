#include <string.h>
#include "sensors/fmath.h"
#include "sensors/raise.h"

#define NEW		0.2f
#define OLD		0.8f
/* The vendor's own, for the same angles. */
#define VENDOR_PI	3.14159f
/* In samples: a raise after a lower and another raise, a lower after
 * either. */
#define RAISE_AFTER_LOWER	15
#define RAISE_AFTER_RAISE	20
#define LOWER_AFTER		15
/* A lower less than this before the window shortens it. */
#define PAD_FROM		30
#define TREND_MIN		3
#define RUN_PART		0.7f
#define HISTORY_FROM		25
#define HISTORY_MAX		75
#define COUNTER_MIN		(-32000)
#define COUNTER_RESET		(-150)

static float at(const struct raise *r, int c, int k)
{
	return r->ring[c][(r->at + RAISE_RING - RAISE_WINDOW + 1 + k) % RAISE_RING];
}

static void extremes(const float *b, int n, int *hi, int *lo)
{
	*hi = *lo = 0;
	for (int k = 1; k < n; k++) {
		if (b[k] > b[*hi])
			*hi = k;
		if (b[k] < b[*lo])
			*lo = k;
	}
}

static void window(const struct raise *r, int c, float sign, float b[RAISE_WINDOW])
{
	for (int k = 0; k < RAISE_WINDOW; k++)
		b[k] = sign * at(r, c, k);
}

static float span(const struct raise *r, int c)
{
	float b[RAISE_WINDOW];
	int hi, lo;
	window(r, c, 1, b);
	extremes(b, RAISE_WINDOW, &hi, &lo);
	return b[hi] - b[lo];
}

static float variance(const struct raise *r, int c)
{
	float s = at(r, c, 0), v = 0;
	for (int k = 1; k < RAISE_WINDOW; k++)
		s = at(r, c, k) + s;
	float mean = s / RAISE_WINDOW;
	for (int k = 0; k < RAISE_WINDOW; k++) {
		float d = at(r, c, k) - mean;
		v = v + d * d;
	}
	return v / RAISE_WINDOW;
}

static float round_half_up(float v)
{
	float i = (float)(int)v;
	return v - i >= 0.5f ? i + 1 : i;
}

/* A rise from the window's lowest point to its highest, trimmed of its
 * flat ends, mostly rising, kept up after its top, by more than least
 * and faster than slope a sample. A pad shortens the window to the
 * samples since a lower. */
static void rise(const struct raise *r, int c, float sign, int pad, float floor, float least, float slope,
		 struct raise_trend *t)
{
	float b[RAISE_WINDOW];
	*t = (struct raise_trend){ 0 };
	window(r, c, sign, b);
	if (pad >= 1 && pad + 1 < RAISE_WINDOW)
		for (int k = 0; k < pad; k++)
			b[k] = b[pad + 1];
	int hi, lo;
	extremes(b, RAISE_WINDOW, &hi, &lo);
	t->start = (int16_t)lo;
	t->end = (int16_t)hi;
	if (lo > hi - TREND_MIN)
		return;
	float steep = 0;
	for (int k = 1; k < RAISE_WINDOW; k++)
		if ((b[k] - b[k - 1]) / 10 > steep)
			steep = (b[k] - b[k - 1]) / 10;
	if (floor > steep)
		steep = floor;
	while (t->start < hi && b[t->start + 1] - b[t->start] < steep)
		t->start++;
	while (t->end > t->start && b[t->end] - b[t->end - 1] < steep)
		t->end--;
	int s = t->start, e = t->end;
	if (s > e - TREND_MIN)
		return;
	int up = 0;
	for (int k = s + 1; k <= e; k++)
		up += b[k] > b[k - 1];
	if (!(round_half_up((float)(e - s) * RUN_PART) <= (float)up))
		return;
	float tail = b[e];
	for (int k = e + 1; k < RAISE_WINDOW; k++)
		if (b[k] < tail)
			tail = b[k];
	float d = b[e] - b[s];
	if (d * RUN_PART <= tail - b[s] && d > least && slope < d / (float)(e - s + 1))
		t->valid = true;
}

/* Not just more of what the arm did since the last lower: the rise is
 * at least factor times that span. */
static bool beyond_history(const struct raise *r, int c, const struct raise_trend *t, float sign, float factor)
{
	int d = r->down;
	if (d >= -HISTORY_FROM)
		return true;
	if (d < -(HISTORY_MAX - 1))
		d = -HISTORY_MAX;
	int from = (r->at + d + RAISE_RING) % RAISE_RING, n = -d - (HISTORY_FROM - 1);
	float hi = r->ring[c][from], lo = hi;
	for (int k = 1; k < n; k++) {
		float v = r->ring[c][(from + k) % RAISE_RING];
		if (v > hi)
			hi = v;
		if (v < lo)
			lo = v;
	}
	return !((at(r, c, t->end) - at(r, c, t->start)) * sign < (hi - lo) * factor);
}

static void fall(const struct raise *r, int c, float least, float slope, struct raise_trend *t)
{
	float b[RAISE_WINDOW];
	*t = (struct raise_trend){ 0 };
	window(r, c, 1, b);
	int hi, lo;
	extremes(b, RAISE_WINDOW, &hi, &lo);
	t->start = (int16_t)hi;
	t->end = (int16_t)lo;
	if (lo - TREND_MIN < hi)
		return;
	while (t->start < lo && b[t->start] - b[t->start + 1] < slope)
		t->start++;
	while (t->end > t->start && b[t->end - 1] - b[t->end] < slope)
		t->end--;
	if (t->start <= t->end - TREND_MIN && least < fm_abs(b[t->start] - b[t->end]))
		t->valid = true;
}

static bool moving(const struct raise *r)
{
	float jolt[3] = { 0, 0, 0 };
	for (int k = 0; k < RAISE_WINDOW; k++)
		for (int c = RAISE_X; c <= RAISE_Z; c++) {
			float d = fm_abs(at(r, c, k) - at(r, c, k - 3));
			if (jolt[c] < d)
				jolt[c] = d;
		}
	for (int i = 0; i < 2; i++)
		for (int j = i + 1; j < 3; j++)
			if (jolt[j] < jolt[i]) {
				float t = jolt[i];
				jolt[i] = jolt[j];
				jolt[j] = t;
			}
	if (jolt[2] < 1.8f && jolt[1] < 0.5f)
		return false;
	float vz = variance(r, RAISE_Z), vy = variance(r, RAISE_Y);
	if (vz < 0.04f || (vy < 0.36f && vz < 0.36f))
		return false;
	return variance(r, RAISE_X) >= 0.4225f || vz >= 0.4225f || vy >= 4.0f;
}

static int raised(struct raise *r)
{
	if (-r->down < RAISE_AFTER_LOWER || -r->up < RAISE_AFTER_RAISE)
		return RAISE_NONE;
	int pad = (int16_t)(r->down + PAD_FROM);
	rise(r, RAISE_Y, 1, pad, 0.2f, 6.1f, 0.25f, &r->y);
	if (r->y.valid)
		r->y.valid = beyond_history(r, RAISE_Y, &r->y, 1, 1.05f);
	rise(r, RAISE_XY, 1, pad, 2.0f, 55.0f, 2.2f, &r->xy);
	if (at(r, RAISE_XY, r->xy.end) < 25.0f)
		r->xy.valid = false;
	if (r->xy.valid)
		r->xy.valid = beyond_history(r, RAISE_XY, &r->xy, 1, 1.10f);
	float sign = 1;
	rise(r, RAISE_AZ, 1, pad, 2.0f, 55.0f, 2.2f, &r->z);
	if (!r->z.valid) {
		sign = -1;
		rise(r, RAISE_AZ, -1, pad, 2.0f, 55.0f, 2.2f, &r->z);
	}
	if (r->z.valid)
		r->z.valid = beyond_history(r, RAISE_AZ, &r->z, sign, 1.10f);
	if (r->z.valid && !r->xy.valid && span(r, RAISE_XY) < 10.0f)
		r->z.valid = false;
	if (r->z.valid && at(r, RAISE_XY, r->z.end) < at(r, RAISE_XY, r->z.start))
		r->z.valid = false;
	if (r->y.valid + r->xy.valid + r->z.valid < 2)
		return RAISE_NONE;
	static const int channel[3] = { RAISE_Y, RAISE_XY, RAISE_AZ };
	const struct raise_trend *won[3] = { &r->y, &r->xy, &r->z };
	for (int i = 0; i < 3; i++)
		if (won[i]->valid)
			r->rose[i] = at(r, channel[i], won[i]->end) - at(r, channel[i], won[i]->start);
	r->up = 0;
	r->by_raise = true;
	return RAISE_UP;
}

static int lowered(struct raise *r)
{
	if (-r->up < LOWER_AFTER || -r->down < LOWER_AFTER)
		return RAISE_NONE;
	r->fall_xy = r->fall_y = (struct raise_trend){ 0 };
	bool down;
	if (r->by_raise) {
		if (r->xy.valid)
			fall(r, RAISE_XY, r->rose[1] * 0.7f >= 42.0f ? 42.0f : r->rose[1] * 0.7f, 2.0f, &r->fall_xy);
		if (r->y.valid)
			fall(r, RAISE_Y, r->rose[0] * 0.7f >= 5.7f ? 5.7f : r->rose[0] * 0.7f, 0.2f, &r->fall_y);
		down = r->xy.valid == r->fall_xy.valid && r->y.valid == r->fall_y.valid;
	} else {
		fall(r, RAISE_XY, 70.0f, 2.0f, &r->fall_xy);
		fall(r, RAISE_Y, 7.5f, 0.2f, &r->fall_y);
		down = r->fall_xy.valid || r->fall_y.valid;
	}
	if (!down)
		return RAISE_NONE;
	r->down = 0;
	return RAISE_DOWN;
}

void raise_init(struct raise *r)
{
	memset(r, 0, sizeof *r);
	r->at = -1;
	r->fed = -1;
	r->up = r->down = 1;
}

/* A sample with x and y both 0 has no xy angle: 0 keeps the filter out
 * of NaN, where the vendor's would stay for good. */
int raise_feed(struct raise *r, const float a[3], bool screen_on)
{
	r->down--;
	r->up--;
	if (r->down < COUNTER_MIN || r->up < COUNTER_MIN)
		r->up = r->down = COUNTER_RESET;
	if (r->fed < INT32_MAX)
		r->fed++;
	r->at = (int16_t)((r->at + 1) % RAISE_RING);
	int n = r->at, prev = (n + RAISE_RING - 1) % RAISE_RING;
	float xy = fm_sqrt(a[0] * a[0] + a[1] * a[1]), all = fm_sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
	float v[RAISE_CHANNELS] = { a[0], a[1], a[2], xy > 0 ? fm_asin(a[1] / xy) * 180 / VENDOR_PI : 0,
				    all > 0 ? fm_asin(a[2] / all) * 180 / VENDOR_PI : 0 };
	for (int c = 0; c < RAISE_CHANNELS; c++)
		r->ring[c][n] = r->fed > 0 ? v[c] * NEW + r->ring[c][prev] * OLD : v[c];
	if (r->fed < RAISE_RING || !moving(r))
		return RAISE_NONE;
	return screen_on ? lowered(r) : raised(r);
}

void raise_screen(struct raise *r, bool on)
{
	if (on) {
		if (!r->by_raise)
			r->y = r->xy = r->z = (struct raise_trend){ 0 };
		r->up = 0;
	} else {
		r->by_raise = false;
		r->down = 0;
	}
}

unsigned raise_repeats(int32_t elapsed_ms)
{
	const int32_t p = RAISE_PERIOD_MS, slack = RAISE_PERIOD_MS * 15 / 100;
	if (elapsed_ms >= p - slack && elapsed_ms <= p + slack)
		return 1;
	if (elapsed_ms < 0)
		return 0;
	int32_t n = elapsed_ms / p;
	if (elapsed_ms - n * p + slack >= p)
		n++;
	return n > 10 ? 10 : (unsigned)n;
}
