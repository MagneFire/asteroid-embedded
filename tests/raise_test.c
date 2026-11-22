/* Wrist raises and lowers on synthetic samples, one every 40 ms, with
 * the screen coming on for a raise and going off for a lower. Gravity
 * is at an angle yaw in the watch's xy plane and el above it. The
 * expected samples are what the vendor's classifier gave for the same
 * samples; RAISE_DUMP=file writes them out for that. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sensors/raise.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define MAX_SAMPLES	1200
#define COUNT		(0.244f * 9.8f / 1000)

enum { NONE, KEY_ON, SCREEN_OFF };

struct scene {
	const char *name;
	unsigned n;
	float a[MAX_SAMPLES][3];
	uint8_t screen[MAX_SAMPLES];
};

static struct scene sc;
static uint32_t seed;

static double rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (double)(seed >> 8) / (1 << 24) - 0.5;
}

static void start(const char *name, uint32_t s)
{
	memset(&sc, 0, sizeof sc);
	sc.name = name;
	seed = s;
}

static void put(double yaw, double el, const double extra[3])
{
	double y = yaw * M_PI / 180, e = el * M_PI / 180;
	double g[3] = { 9.8 * cos(e) * cos(y), 9.8 * cos(e) * sin(y), 9.8 * sin(e) };
	for (int i = 0; i < 3; i++)
		sc.a[sc.n][i] = (float)lround((g[i] + (extra ? extra[i] : 0)) / COUNT) * COUNT;
	sc.n++;
}

static double at_yaw = 0, at_el = 0;

static void hold(double yaw, double el, unsigned n, double noise)
{
	at_yaw = yaw;
	at_el = el;
	for (unsigned i = 0; i < n; i++) {
		double x[3] = { noise * rnd(), noise * rnd(), noise * rnd() };
		put(yaw, el, x);
	}
}

/* Half-cosine from where it is, as the vendor's replay did. */
static void turn(double yaw, double el, unsigned n, double noise)
{
	for (unsigned i = 1; i <= n; i++) {
		double t = (1 - cos(M_PI * i / n)) / 2;
		double x[3] = { noise * rnd(), noise * rnd(), noise * rnd() };
		put(at_yaw + t * (yaw - at_yaw), at_el + t * (el - at_el), x);
	}
	at_yaw = yaw;
	at_el = el;
}

/* An arm swinging at the side while walking. */
static void swing(double yaw, double yaw_amp, double el, double el_amp, double hz, unsigned n, double gait)
{
	for (unsigned i = 0; i < n; i++) {
		double p = 2 * M_PI * hz * i * 0.04;
		double x[3] = { gait * sin(2 * p), 0.6 * gait * sin(2 * p + 1), 0.4 * gait * cos(2 * p) };
		put(yaw + yaw_amp * sin(p), el + el_amp * sin(p + 0.7), x);
	}
}

/* The screen on after a raise and off after a lower, else as the scene
 * says: the samples of each raise and lower in up[] and down[]. */
static void replay(unsigned *up, unsigned *nup, unsigned *down, unsigned *ndown)
{
	static struct raise r;
	raise_init(&r);
	bool on = false;
	*nup = *ndown = 0;
	for (unsigned k = 0; k < sc.n; k++) {
		if (sc.screen[k] == KEY_ON && !on)
			raise_screen(&r, on = true);
		else if (sc.screen[k] == SCREEN_OFF && on)
			raise_screen(&r, on = false);
		int e = raise_feed(&r, sc.a[k], on);
		if (e == RAISE_UP) {
			up[(*nup)++] = k;
			raise_screen(&r, on = true);
		} else if (e == RAISE_DOWN) {
			down[(*ndown)++] = k;
			raise_screen(&r, on = false);
		}
	}
}

static FILE *dump;

static void expect(const unsigned *ups, unsigned nups, const unsigned *downs, unsigned ndowns)
{
	unsigned up[64], down[64], nup, ndown;
	if (dump) {
		uint32_t n = sc.n, len = (uint32_t)strlen(sc.name);
		fwrite(&len, 4, 1, dump);
		fwrite(sc.name, 1, len, dump);
		fwrite(&n, 4, 1, dump);
		fwrite(sc.a, sizeof sc.a[0], n, dump);
		fwrite(sc.screen, 1, n, dump);
	}
	replay(up, &nup, down, &ndown);
	bool ok = nup == nups && ndown == ndowns && !memcmp(up, ups, nup * sizeof *up) &&
		  !memcmp(down, downs, ndown * sizeof *down);
	if (!ok) {
		printf("%s: up", sc.name);
		for (unsigned i = 0; i < nup; i++)
			printf(" %u", up[i]);
		printf(", down");
		for (unsigned i = 0; i < ndown; i++)
			printf(" %u", down[i]);
		printf("\n");
	}
	CHECK(ok);
}

#define EXPECT(u, d) expect(u, sizeof u / sizeof *u, d, sizeof d / sizeof *d)

int main(void)
{
	const char *path = getenv("RAISE_DUMP");
	if (path)
		dump = fopen(path, "wb");

	/* The vendor replay of the notes: up 520 ms after the lift began,
	 * down 480 ms after the arm went back. */
	start("lift and lower", 1);
	hold(-20, 20, 95, 0);
	turn(55, 45, 14, 0);
	hold(55, 45, 35, 0);
	turn(-20, 20, 14, 0);
	hold(-20, 20, 40, 0);
	{
		static const unsigned u[] = { 108 }, d[] = { 156 };
		EXPECT(u, d);
	}
	CHECK(108 - 95 == 520 / RAISE_PERIOD_MS && 156 - 144 == 480 / RAISE_PERIOD_MS);

	/* Turns of the same length that are not raises. */
	static const double not_raises[][4] = { { 0, 0, 45, 48 }, { -60, 5, 20, 55 }, { 0, -30, 40, 40 }, { 0, 60, 30, 50 } };
	for (unsigned i = 0; i < 4; i++) {
		start("not a raise", 2);
		hold(not_raises[i][0], not_raises[i][1], 95, 0);
		turn(not_raises[i][2], not_raises[i][3], 14, 0);
		hold(not_raises[i][2], not_raises[i][3], 40, 0);
		static const unsigned none[1];
		expect(none, 0, none, 0);
	}

	/* A raise over 1.6 s is too slow; one from an arm hanging down is
	 * one. */
	start("slow and hanging", 3);
	hold(-20, 20, 95, 0.2);
	turn(55, 45, 40, 0.2);
	hold(55, 45, 40, 0.2);
	sc.screen[sc.n] = SCREEN_OFF;
	hold(55, 45, 10, 0.2);
	turn(-85, 0, 20, 0.2);
	hold(-85, 0, 40, 0.2);
	turn(40, 60, 14, 0.2);
	hold(40, 60, 40, 0.2);
	{
		static const unsigned u[] = { 270 }, d[1];
		expect(u, 1, d, 0);
	}

	/* Woken by the key, the arm lowered: down by the fallback, a
	 * large fall of the xy angle or of y. */
	start("key wake", 4);
	hold(55, 45, 100, 0.1);
	sc.screen[sc.n] = KEY_ON;
	hold(55, 45, 30, 0.1);
	turn(-40, 10, 12, 0.1);
	hold(-40, 10, 40, 0.1);
	{
		static const unsigned u[1], d[] = { 141 };
		expect(u, 0, d, 1);
	}

	/* Arm swings while walking, ordinary and wide. */
	start("swings", 5);
	hold(-80, 0, 80, 0.1);
	swing(-80, 25, 0, 10, 0.93, 300, 0.9);
	swing(-60, 44, 10, 30, 0.9, 300, 1.0);
	{
		static const unsigned none[1];
		expect(none, 0, none, 0);
	}

	/* The forearm laid on a desk is taken for a raise; turned away and
	 * back while the screen is off, not. */
	start("desk", 6);
	hold(-85, -5, 100, 0.1);
	turn(-10, 70, 18, 0.1);
	hold(-10, 70, 60, 0.1);
	sc.screen[sc.n] = SCREEN_OFF;
	hold(-10, 70, 20, 0.1);
	turn(-80, -15, 14, 0.1);
	hold(-80, -15, 30, 0.1);
	turn(-10, 70, 14, 0.1);
	hold(-10, 70, 40, 0.1);
	{
		static const unsigned u[] = { 116 }, d[1];
		expect(u, 1, d, 0);
	}

	/* A raise while walking. */
	start("walking raise", 7);
	swing(-20, 10, 20, 5, 0.9, 120, 0.6);
	at_yaw = -20;
	at_el = 20;
	turn(55, 45, 14, 0.3);
	swing(55, 3, 45, 3, 0.9, 60, 0.6);
	{
		static const unsigned u[] = { 132 }, d[1];
		expect(u, 1, d, 0);
	}

	/* The elapsed time since the last sample, in samples. */
	CHECK(raise_repeats(38) == 1 && raise_repeats(34) == 1 && raise_repeats(46) == 1 && raise_repeats(33) == 0 &&
	      raise_repeats(47) == 1 && raise_repeats(74) == 2 && raise_repeats(1000) == 10 && raise_repeats(-5) == 0);

	if (dump)
		fclose(dump);
	if (failures)
		return 1;
	printf("raise_test: ok\n");
	return 0;
}
