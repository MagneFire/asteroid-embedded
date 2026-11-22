/* The orientation's angles against Android's conventions, then the
 * calibration on a synthetic field: the Earth's at 50° inclination,
 * rotated with the watch, plus a hard-iron offset and noise. Turned
 * every way, the offset is found and the accuracy climbs to high;
 * turned in one plane only, it stays unreliable; a new offset after
 * that is found again; a saved one is low until it fits new samples;
 * samples from the charger's magnets do not hold it back. */
#include <math.h>
#include <stdio.h>
#include "sensors/compass.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t seed = 7;
static double rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (double)(seed >> 8) / (1 << 24);
}

static double noise(void) { return rnd() + rnd() + rnd() - 1.5; }

/* World x east, y north, z up; the field points north and down. */
static const double field[3] = { 0, 30, -36 };

/* The rotation taking the watch's axes to the world's, from heading
 * (clockwise from north), pitch and roll in degrees. */
static void rotation(double yaw, double pitch, double roll, double r[3][3])
{
	double a = -yaw * M_PI / 180, b = pitch * M_PI / 180, c = roll * M_PI / 180;
	double z[3][3] = { { cos(a), -sin(a), 0 }, { sin(a), cos(a), 0 }, { 0, 0, 1 } };
	double x[3][3] = { { 1, 0, 0 }, { 0, cos(b), -sin(b) }, { 0, sin(b), cos(b) } };
	double y[3][3] = { { cos(c), 0, sin(c) }, { 0, 1, 0 }, { -sin(c), 0, cos(c) } };
	double t[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			t[i][j] = 0;
			for (int k = 0; k < 3; k++)
				t[i][j] += x[i][k] * y[k][j];
		}
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			r[i][j] = 0;
			for (int k = 0; k < 3; k++)
				r[i][j] += z[i][k] * t[k][j];
		}
}

/* A world vector in the watch's axes. */
static void to_watch(double r[3][3], const double w[3], float out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = (float)(r[0][i] * w[0] + r[1][i] * w[1] + r[2][i] * w[2]);
}

/* Where the watch's y axis points, clockwise from north. */
static double heading(double r[3][3])
{
	double h = atan2(r[0][1], r[1][1]) * 180 / M_PI;
	return h < 0 ? h + 360 : h;
}

static double angle_off(double a, double b)
{
	double d = fmod(fabs(a - b), 360);
	return d > 180 ? 360 - d : d;
}

static void sense(double r[3][3], const double offset[3], float g[3], float raw[3])
{
	static const double up[3] = { 0, 0, 9.81 };
	to_watch(r, up, g);
	to_watch(r, field, raw);
	for (int i = 0; i < 3; i++)
		raw[i] += (float)(offset[i] + 0.3 * noise());
}

static int feed(struct compass *c, double yaw, double pitch, double roll, const double offset[3], float g[3],
		float m[3], double r[3][3])
{
	float raw[3];
	rotation(yaw, pitch, roll, r);
	sense(r, offset, g, raw);
	return compass_feed(c, raw, m);
}

static double offset_error(const struct compass *c, const double offset[3])
{
	double e = 0;
	for (int i = 0; i < 3; i++)
		e += (c->cal.offset[i] - offset[i]) * (c->cal.offset[i] - offset[i]);
	return sqrt(e);
}

static void conventions(void)
{
	float out[3], g[3] = { 0, 0, 9.81f };
	static const float north[3] = { 0, 30, -36 }, east[3] = { -30, 0, -36 }, south[3] = { 0, -30, -36 },
			   west[3] = { 30, 0, -36 };
	CHECK(compass_orientation(g, north, out) && fabsf(out[0]) < 0.01f && fabsf(out[1]) < 0.01f &&
	      fabsf(out[2]) < 0.01f);
	CHECK(compass_orientation(g, east, out) && fabsf(out[0] - 90) < 0.01f);
	CHECK(compass_orientation(g, south, out) && fabsf(out[0] - 180) < 0.01f);
	CHECK(compass_orientation(g, west, out) && fabsf(out[0] - 270) < 0.01f);
	/* Upright with the top up: pitch -90; the right edge up: roll
	 * positive; face down: pitch 180. */
	float upright[3] = { 0, 9.81f, 0 }, right_up[3] = { 4.905f, 0, 8.496f }, face_down[3] = { 0, 0, -9.81f };
	CHECK(compass_orientation(upright, north, out) && fabsf(out[1] + 90) < 0.01f);
	CHECK(compass_orientation(right_up, north, out) && fabsf(out[2] - 30) < 0.01f && fabsf(out[1]) < 0.01f);
	CHECK(compass_orientation(face_down, north, out) && fabsf(fabsf(out[1]) - 180) < 0.01f);
	/* The field along gravity gives no heading. */
	float down[3] = { 0, 0, -45 };
	CHECK(!compass_orientation(g, down, out));

	/* Tilted up to 60° any way, the heading is that of the y axis. */
	double worst = 0;
	for (int i = 0; i < 2000; i++) {
		double r[3][3];
		rotation(rnd() * 360, rnd() * 120 - 60, rnd() * 120 - 60, r);
		float gw[3], m[3];
		static const double up[3] = { 0, 0, 9.81 };
		to_watch(r, up, gw);
		to_watch(r, field, m);
		CHECK(compass_orientation(gw, m, out));
		double d = angle_off(out[0], heading(r));
		worst = d > worst ? d : worst;
	}
	CHECK(worst < 0.05);
}

static void calibration(void)
{
	struct compass c;
	compass_init(&c, NULL);
	static const double offset[3] = { 23, -41, 57 };
	float g[3], m[3], out[3];
	double r[3][3];
	int status = feed(&c, 0, 0, 0, offset, g, m, r);
	CHECK(status == 0);

	/* Turned about the face only: one plane. */
	for (int i = 0; i < 400; i++)
		status = feed(&c, i * 9.0, 0, 0, offset, g, m, r);
	CHECK(status == 0 && c.cal.status == 0);

	/* Then every way, as in a figure 8. */
	int seen_low = 0, n = 0;
	for (; n < 1500 && status < 3; n++) {
		status = feed(&c, rnd() * 360, rnd() * 160 - 80, rnd() * 360 - 180, offset, g, m, r);
		if (status == 1 && !seen_low)
			seen_low = n;
	}
	CHECK(status == 3 && seen_low > 0);
	CHECK(offset_error(&c, offset) < 1.5);
	CHECK(fabsf(c.cal.radius - (float)sqrt(30.0 * 30 + 36 * 36)) < 1.5f);
	double worst = 0;
	for (int i = 0; i < 200; i++) {
		status = feed(&c, rnd() * 360, rnd() * 100 - 50, rnd() * 100 - 50, offset, g, m, r);
		CHECK(status == 3 && compass_orientation(g, m, out));
		double d = angle_off(out[0], heading(r));
		worst = d > worst ? d : worst;
	}
	CHECK(worst < 3);
	CHECK(c.changed);
	struct compass_cal saved = c.cal;

	/* New iron next to it: unreliable, then found again. */
	static const double moved[3] = { -60, 10, 25 };
	for (int i = 0; i < 20; i++)
		status = feed(&c, rnd() * 360, rnd() * 160 - 80, rnd() * 360 - 180, moved, g, m, r);
	CHECK(status == 0);
	for (int i = 0; i < 1500 && status < 2; i++)
		status = feed(&c, rnd() * 360, rnd() * 160 - 80, rnd() * 360 - 180, moved, g, m, r);
	CHECK(status >= 2 && offset_error(&c, moved) < 2);

	/* A saved calibration is low at once, and good samples raise it. */
	compass_init(&c, &saved);
	status = feed(&c, 0, 0, 0, offset, g, m, r);
	CHECK(status == 1 && offset_error(&c, offset) < 1.5);
	for (int i = 0; i < 1500 && status < 3; i++)
		status = feed(&c, rnd() * 360, rnd() * 160 - 80, rnd() * 360 - 180, offset, g, m, r);
	CHECK(status == 3);

	/* On the charger first, its magnets far stronger than the Earth's
	 * field, then a figure 8 off it, as on the watch. */
	static const double charger[3] = { 1300, -680, -300 };
	compass_init(&c, NULL);
	for (int i = 0; i < 100; i++)
		status = feed(&c, rnd() * 20, 0, 0, charger, g, m, r);
	for (int i = 0; i < 1500 && status < 3; i++)
		status = feed(&c, rnd() * 360, rnd() * 160 - 80, rnd() * 360 - 180, offset, g, m, r);
	CHECK(status == 3 && offset_error(&c, offset) < 1.5);

	struct compass_cal bad = { { 0, 0, 0 }, 500, 3 };
	compass_init(&c, &bad);
	CHECK(c.cal.status == 0);
}

int main(void)
{
	conventions();
	calibration();
	if (failures)
		return 1;
	printf("compass_test: ok\n");
	return 0;
}
