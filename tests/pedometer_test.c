/* Steps on synthetic samples in mg. A walk is an arm swinging from the
 * shoulder at half the step rate, with the body bouncing at the step
 * rate; the watch turns with the arm. Still, desk, driving and arm
 * movements without walking should give none. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sensors/pedometer.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define G	9.8
#define ARM_M	0.6

static uint32_t seed;

static double rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (double)(seed >> 8) / (1 << 24) - 0.5;
}

struct motion {
	double step_hz, swing_rad, bounce_g, noise_mg;
	/* The pace going up and down by this part every 10 s. */
	double pace_part;
	/* Single bumps of this many mg, at random times this far apart. */
	double bump_mg, bump_min_s, bump_max_s;
	/* Movement along z alone, as scrubbing. */
	double shake_hz, shake_mg;
	/* A slow raise and lower of the arm, this long each way. */
	double raise_s;
};

static double next_bump;

static void sample(const struct motion *m, double t, int16_t out[3])
{
	double s[3] = { 0, 0, G }, th = 0;
	if (m->step_hz > 0) {
		double w = M_PI * m->step_hz;
		t -= m->pace_part / (0.2 * M_PI) * cos(0.2 * M_PI * t);
		th = m->swing_rad * sin(w * t);
		double d1 = m->swing_rad * w * cos(w * t), d2 = -m->swing_rad * w * w * sin(w * t);
		s[0] += ARM_M * (cos(th) * d2 - sin(th) * d1 * d1);
		s[2] += ARM_M * (sin(th) * d2 + cos(th) * d1 * d1);
		double p = 2 * M_PI * m->step_hz * t;
		s[2] += m->bounce_g * G * (cos(p) + 0.5 * cos(2 * p + 0.5));
		s[0] += 0.1 * m->bounce_g * G * sin(p);
	}
	if (m->raise_s > 0) {
		double phase = fmod(t, 4 * m->raise_s) / m->raise_s;
		double x = phase < 1 ? phase : phase < 2 ? 1 : phase < 3 ? 3 - phase : 0;
		th = 1.2 * (1 - cos(M_PI * x)) / 2;
	}
	if (m->shake_hz > 0)
		s[2] += m->shake_mg / 1000 * G * sin(2 * M_PI * m->shake_hz * t);
	if (m->bump_mg > 0 && t >= next_bump) {
		s[2] += m->bump_mg / 1000 * G;
		next_bump = t + m->bump_min_s + (rnd() + 0.5) * (m->bump_max_s - m->bump_min_s);
	}
	double w[3] = { s[0] * cos(th) + s[2] * sin(th), s[1], -s[0] * sin(th) + s[2] * cos(th) };
	for (int i = 0; i < 3; i++)
		out[i] = (int16_t)lround(w[i] / G * 1000 + m->noise_mg * 2 * rnd());
}

/* Seconds of motion sampled every period_us, through the resampler. */
static unsigned run(struct pedometer *p, const struct motion *m, double seconds, uint32_t period_us, double *t)
{
	struct resample r;
	resample_reset(&r);
	unsigned steps = 0;
	for (double end = *t + seconds; *t < end; *t += period_us / 1e6) {
		int16_t v[3], out[RESAMPLE_MAX][3];
		sample(m, *t, v);
		unsigned n = resample_feed(&r, v, period_us, out);
		for (unsigned k = 0; k < n; k++)
			steps += pedometer_feed(p, out[k]);
	}
	return steps;
}

static unsigned scene(const char *name, const struct motion *m, double seconds, uint32_t period_us, uint32_t s)
{
	struct pedometer p;
	pedometer_init(&p);
	seed = s;
	next_bump = 0;
	double t = 0;
	unsigned n = run(&p, m, seconds, period_us, &t);
	CHECK(n == p.steps);
	printf("%-24s %3u us/sample: %4u steps, %u rejected\n", name, period_us, n, p.rejected);
	return n;
}

static bool near(unsigned got, double want, double part)
{
	return fabs(got - want) <= want * part;
}

static const struct motion still = { .noise_mg = 8 };
static const struct motion walk = { .step_hz = 1.8, .swing_rad = 0.35, .bounce_g = 0.25, .noise_mg = 10 };
static const struct motion pace = { .step_hz = 1.8, .swing_rad = 0.3, .bounce_g = 0.2, .noise_mg = 15, .pace_part = 0.2 };
static const struct motion slow = { .step_hz = 1.3, .swing_rad = 0.2, .bounce_g = 0.12, .noise_mg = 10 };
static const struct motion fast = { .step_hz = 2.6, .swing_rad = 0.6, .bounce_g = 0.5, .noise_mg = 10 };
static const struct motion desk = { .noise_mg = 6, .bump_mg = 60, .bump_min_s = 0.15, .bump_max_s = 1.5 };
static const struct motion drive = { .noise_mg = 20, .bump_mg = 200, .bump_min_s = 0.4, .bump_max_s = 3 };
static const struct motion scrub = { .noise_mg = 10, .shake_hz = 5, .shake_mg = 300 };
static const struct motion sway = { .step_hz = 1.8, .swing_rad = 0.15, .noise_mg = 8 };
static const struct motion look = { .noise_mg = 10, .raise_s = 1.5 };

static void test_scenes(void)
{
	static const uint32_t periods[] = { 40000, 64000, 80000 };
	for (unsigned i = 0; i < sizeof periods / sizeof periods[0]; i++) {
		uint32_t us = periods[i];
		CHECK(scene("still", &still, 120, us, 1) == 0);
		CHECK(near(scene("walk 1.8 Hz", &walk, 120, us, 2), 1.8 * 120, 0.05));
		CHECK(near(scene("walk, pace changing", &pace, 120, us, 13), 1.8 * 120, 0.05));
		CHECK(near(scene("slow walk 1.3 Hz", &slow, 120, us, 3), 1.3 * 120, 0.05));
		CHECK(near(scene("fast walk 2.6 Hz", &fast, 60, us, 4), 2.6 * 60, 0.05));
		CHECK(scene("desk", &desk, 120, us, 5) == 0);
		CHECK(scene("driving", &drive, 300, us, 6) == 0);
		CHECK(scene("scrubbing 5 Hz", &scrub, 60, us, 7) == 0);
		CHECK(scene("arm sway, standing", &sway, 60, us, 8) == 0);
		CHECK(scene("looking at the watch", &look, 120, us, 9) == 0);
	}
}

/* Fewer than PEDOMETER_START steps count for nothing. */
static void test_short_walk(void)
{
	struct pedometer p;
	pedometer_init(&p);
	seed = 10;
	double t = 0;
	unsigned n = run(&p, &still, 5, 40000, &t);
	n += run(&p, &walk, (PEDOMETER_START - 2) / walk.step_hz, 40000, &t);
	n += run(&p, &still, 5, 40000, &t);
	CHECK(n == 0);
	CHECK(p.rejected > 0 && p.rejected < PEDOMETER_START);
}

/* A stop of a few seconds ends the walk, and the next one counts its
 * first steps once it reaches PEDOMETER_START. */
static void test_stop_and_go(void)
{
	struct pedometer p;
	pedometer_init(&p);
	seed = 11;
	double t = 0;
	unsigned n = run(&p, &walk, 20, 40000, &t);
	n += run(&p, &still, 5, 40000, &t);
	CHECK(!p.counting);
	n += run(&p, &walk, 20, 40000, &t);
	CHECK(near(n, 1.8 * 40, 0.08));
}

/* A gap in the samples, as between two batches of the sensor, keeps a
 * walk going if it is short, and ends it if it is long. */
static void test_gap(void)
{
	struct pedometer p;
	pedometer_init(&p);
	seed = 12;
	double t = 0;
	run(&p, &walk, 10, 40000, &t);
	CHECK(p.counting);
	pedometer_gap(&p, 500);
	CHECK(p.counting);
	pedometer_gap(&p, 1500);
	CHECK(!p.counting);
}

static void test_resample(void)
{
	struct resample r;
	int16_t out[RESAMPLE_MAX][3];
	resample_reset(&r);
	int16_t a[3] = { 0, 100, -100 }, b[3] = { 80, 0, 0 };
	CHECK(resample_feed(&r, a, 40000, out) == 1 && out[0][1] == 100);
	CHECK(resample_feed(&r, b, 40000, out) == 1 && out[0][0] == 80 && out[0][1] == 0);
	resample_reset(&r);
	CHECK(resample_feed(&r, a, 80000, out) == 1);
	CHECK(resample_feed(&r, b, 80000, out) == 2 && out[0][0] == 40 && out[0][1] == 50 && out[1][0] == 80);
	resample_reset(&r);
	unsigned total = 0;
	for (int i = 0; i < 1000; i++)
		total += resample_feed(&r, a, 64000, out);
	CHECK(total == 1599);
}

int main(void)
{
	test_resample();
	test_scenes();
	test_short_walk();
	test_stop_and_go();
	test_gap();
	if (failures)
		printf("pedometer: %d failures\n", failures);
	else
		printf("pedometer: ok\n");
	return failures != 0;
}
