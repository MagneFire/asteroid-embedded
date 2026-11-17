/* The heart rate from synthetic PPG at 25 Hz: a pulse with a second
 * harmonic, noise, baseline wander, the pulse's height stepping up and
 * down and the level jumping, at 50-180 bpm; then a flat signal and a
 * clipped one, which must give no reading. */
#include <math.h>
#include <stdio.h>
#include "sensors/hr.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t seed = 1;
static double noise(void)
{
	double v = 0;
	for (int i = 0; i < 4; i++) {
		seed = seed * 1664525u + 1013904223u;
		v += (double)(seed >> 8) / (1 << 24) - 0.5;
	}
	return v * 1.732;
}

static int32_t clip(double v)
{
	return v > 2097151 ? 2097151 : v < -2097152 ? -2097152 : (int32_t)v;
}

/* Height of the pulse at second t: 2000, then 6000, then 800. */
static double height(double t) { return t < 20 ? 2000 : t < 40 ? 6000 : 800; }

static void rate(unsigned bpm)
{
	hr_reset();
	double f = bpm / 60.0, phase = 0;
	unsigned readings = 0, late = 0, worst = 0, last_seen[3] = { 0 };
	for (unsigned n = 0; n < 60 * HR_HZ; n++) {
		double t = (double)n / HR_HZ;
		phase += 2 * M_PI * f / HR_HZ;
		double v = 300000 + (t >= 30 ? 40000 : 0) + height(t) * (sin(phase) + 0.25 * sin(2 * phase + 1)) +
			   4000 * sin(2 * M_PI * 0.2 * t) + 1500 * sin(2 * M_PI * 0.07 * t + 2) + 150 * noise();
		if (!hr_add(clip(v)))
			continue;
		uint8_t q;
		unsigned got = hr_bpm(&q);
		unsigned err = got > bpm ? got - bpm : bpm - got;
		if (err > worst)
			worst = err;
		CHECK(q > 0);
		readings++;
		late += t > 50;
		last_seen[t < 20 ? 0 : t < 40 ? 1 : 2] = n;
	}
	if (worst > 3)
		printf("%u bpm: off by up to %u\n", bpm, worst);
	CHECK(worst <= 3);
	CHECK(readings > 20);
	CHECK(late > 0);
	CHECK(last_seen[0] > 15 * HR_HZ && last_seen[1] > 35 * HR_HZ && last_seen[2] > 55 * HR_HZ);
}

static unsigned feed(double (*f)(unsigned n))
{
	hr_reset();
	unsigned readings = 0;
	for (unsigned n = 0; n < 60 * HR_HZ; n++)
		readings += hr_add(clip(f(n)));
	return readings + hr_bpm(NULL);
}

static double flat(unsigned n) { return 300000; }
static double flat_noisy(unsigned n) { return 300000 + 3 * noise(); }
static double clipped(unsigned n) { return 2000000 + 200000 * sin(2 * M_PI * 1.2 * n / HR_HZ); }
static double tops_clipped(unsigned n) { return 1500000 + 700000 * sin(2 * M_PI * 1.2 * n / HR_HZ); }

int main(void)
{
	for (unsigned bpm = 50; bpm <= 180; bpm += 5)
		rate(bpm);
	CHECK(feed(flat) == 0);
	CHECK(feed(flat_noisy) == 0);
	CHECK(feed(clipped) == 0);
	CHECK(feed(tops_clipped) == 0);
	if (failures)
		printf("hr_test: %d failures\n", failures);
	else
		printf("hr_test: ok\n");
	return failures != 0;
}
