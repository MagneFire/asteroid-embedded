/* The magnetometer's calibration on a simulated watch at 50 Hz: the
 * Earth's field at 50° inclination turned with the watch, distorted as
 * the vendor's correction undoes, plus a hard-iron offset, the noise
 * beluga's AK09918 has and the chip's steps, with the gyroscope's rates
 * for the same turns. Then the charger's magnets: on it, taken off
 * cleanly or slowly, and put back. Then samples taken as the main loop
 * gets to them, and the samples of a real watch turned off its charger
 * (in tests/data, or the file given). COMPASS_VERBOSE=1 prints the
 * times to each accuracy, and COMPASS_DUMP=file writes the simulated
 * samples out, with gravity, for replaying them through the vendor's
 * library. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sensors/compass.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

#define DT	0.02
#define PI	3.14159265358979

static uint32_t seed = 7;
static double rnd(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (double)(seed >> 8) / (1 << 24);
}

/* Of 0.85 µT, as the watch's. */
static double noise(void) { return 1.7 * (rnd() + rnd() + rnd() - 1.5); }

/* World x east, y north, z up; the field points north and down. */
static const double earth[3] = { 0, 30, -36 };
static const double charger[3] = { 1538, -630, -566 };
static bool verbose;
static FILE *dump;

/* A record: kind (0 a sample, 1 a fresh start, 2 a name), the time in
 * µs, gravity in m/s², the rate in rad/s and the field in µT. */
static void record(int kind, double t, const double g[3], const float w[3], const float m[3], const char *name)
{
	if (!dump)
		return;
	float v[10] = { (float)kind, 0 };
	uint64_t us = (uint64_t)llround(t * 1e6);
	for (int i = 0; g && i < 3; i++) {
		v[1 + i] = (float)g[i];
		v[4 + i] = w[i];
		v[7 + i] = m[i];
	}
	fwrite(v, sizeof v, 1, dump);
	fwrite(&us, sizeof us, 1, dump);
	char text[32] = { 0 };
	if (name)
		strncpy(text, name, sizeof text - 1);
	fwrite(text, sizeof text, 1, dump);
}

static void mul(const double a[3][3], const double b[3][3], double r[3][3])
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			r[i][j] = 0;
			for (int k = 0; k < 3; k++)
				r[i][j] += a[i][k] * b[k][j];
		}
}

/* The rotation taking the watch's axes to the world's, from heading
 * (clockwise from north), pitch and roll in radians. */
static void rotation(double yaw, double pitch, double roll, double r[3][3])
{
	double a = -yaw, b = pitch, c = roll;
	double z[3][3] = { { cos(a), -sin(a), 0 }, { sin(a), cos(a), 0 }, { 0, 0, 1 } };
	double x[3][3] = { { 1, 0, 0 }, { 0, cos(b), -sin(b) }, { 0, sin(b), cos(b) } };
	double y[3][3] = { { cos(c), 0, sin(c) }, { 0, 1, 0 }, { -sin(c), 0, cos(c) } };
	double t[3][3];
	mul(x, y, t);
	mul(z, t, r);
}

/* What the chip reads for a corrected field: the correction undone. */
static double distort[3][3];

static void invert_correction(void)
{
	double m[3][3];
	for (int i = 0; i < 3; i++) {
		float e[3] = { i == 0, i == 1, i == 2 }, col[3];
		compass_correct(e, col);
		for (int j = 0; j < 3; j++)
			m[j][i] = col[j];
	}
	double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			int i1 = (j + 1) % 3, i2 = (j + 2) % 3, j1 = (i + 1) % 3, j2 = (i + 2) % 3;
			distort[i][j] = (m[i1][j1] * m[i2][j2] - m[i1][j2] * m[i2][j1]) / det;
		}
}

typedef void (*path_fn)(double t, double angles[3]);

static double k_yaw, k_pitch, k_roll, k_hz;

/* The vendor replay's motion: every way. */
static void sweep(double t, double a[3])
{
	a[0] = 2 * t;
	a[1] = 1.25 * sin(1.53 * t);
	a[2] = 1.9 * sin(1.27 * t);
}

/* A figure eight of the given reach, one loop per 1/k_hz s. */
static void eight(double t, double a[3])
{
	double p = 2 * PI * k_hz * t;
	a[0] = k_yaw * sin(p);
	a[1] = k_pitch * sin(2 * p);
	a[2] = k_roll * sin(p + 0.5);
}

static void still(double t, double a[3]) { a[0] = 0.3, a[1] = 0.1, a[2] = 0; }

/* The magnets' field on top of the Earth's, in the watch's axes: from
 * one to another over `ramp` s, as a magnet's falls with the cube of
 * the distance, then held. */
static double extra_from[3], extra_to[3], extra_t0, extra_ramp;
static bool extra_on, extra_near, gyro_off, gyro_turned, gyro_double;

static void extra_field(double t, double out[3])
{
	double s = extra_ramp > 0 ? (t - extra_t0) / extra_ramp : 1;
	s = s < 0 ? 0 : s > 1 ? 1 : s;
	s = extra_near ? s * s * s : 1 - (1 - s) * (1 - s) * (1 - s);
	for (int i = 0; i < 3; i++)
		out[i] = extra_from[i] + s * (extra_to[i] - extra_from[i]);
}

struct run {
	double t;
	int status, first[4];
	float out[3];
};

static struct compass c;
static double now;

/* The field seen by the watch at t_mag, the Earth's turned with it and
 * the offset in the chip's axes or a magnet's in place of it, and the
 * gyroscope's rate at t_gyro. */
static void sample_at(path_fn path, const double offset[3], double t_mag, double t_gyro, float raw[3], float gyro[3])
{
	double a0[3], a1[3], a2[3], r0[3][3], r1[3][3], r2[3][3];
	path(t_mag, a1);
	rotation(a1[0], a1[1], a1[2], r1);
	double f[3], d[3], w[3][3];
	for (int i = 0; i < 3; i++)
		f[i] = r1[0][i] * earth[0] + r1[1][i] * earth[1] + r1[2][i] * earth[2];
	if (extra_on) {
		double m[3];
		extra_field(t_mag, m);
		for (int i = 0; i < 3; i++)
			f[i] += m[i];
	}
	for (int i = 0; i < 3; i++)
		d[i] = distort[i][0] * f[0] + distort[i][1] * f[1] + distort[i][2] * f[2];
	for (int i = 0; i < 3; i++)
		raw[i] = (float)(round((d[i] + offset[i] + noise()) / 0.15) * 0.15);
	/* The body's rate: rᵀ·dr/dt is its skew matrix. */
	path(t_gyro - DT, a0);
	path(t_gyro, a1);
	path(t_gyro + DT, a2);
	rotation(a0[0], a0[1], a0[2], r0);
	rotation(a1[0], a1[1], a1[2], r1);
	rotation(a2[0], a2[1], a2[2], r2);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			w[i][j] = 0;
			for (int k = 0; k < 3; k++)
				w[i][j] += r1[k][i] * (r2[k][j] - r0[k][j]) / (2 * DT);
		}
	gyro[0] = (float)((w[2][1] - w[1][2]) / 2);
	gyro[1] = (float)((w[0][2] - w[2][0]) / 2);
	gyro[2] = (float)((w[1][0] - w[0][1]) / 2);
	if (gyro_off)
		gyro[0] = gyro[1] = gyro[2] = 0;
	for (int i = 0; i < 3 && gyro_double; i++)
		gyro[i] *= 2;
	if (gyro_turned) {
		float x = gyro[0];
		gyro[0] = gyro[1], gyro[1] = gyro[2], gyro[2] = x;
	}
	double g[3] = { r1[2][0] * 9.8, r1[2][1] * 9.8, r1[2][2] * 9.8 };
	record(0, t_mag, g, gyro, raw, NULL);
}

static void sample(path_fn path, const double offset[3], float raw[3], float gyro[3])
{
	sample_at(path, offset, now, now, raw, gyro);
}

static void reached(struct run *r, double t0)
{
	for (int l = 0; l <= r->status; l++)
		if (r->first[l] < 0)
			r->first[l] = (int)lround((now - t0) * 1000);
}

static void report(const char *name, const struct run *r)
{
	if (verbose)
		printf("%-28s accuracy %d, first at 1: %d ms, 2: %d ms, 3: %d ms, %u directions\n", name, r->status,
		       r->first[1], r->first[2], r->first[3], compass_bins(&c));
}

static struct run go(const char *name, path_fn path, const double offset[3], double seconds)
{
	struct run r = { .first = { -1, -1, -1, -1 } };
	double end = now + seconds, t0 = now;
	record(2, now, NULL, NULL, NULL, name);
	for (; now < end; now += DT) {
		float raw[3], gyro[3];
		sample(path, offset, raw, gyro);
		r.status = compass_feed(&c, raw, gyro, (float)DT, r.out);
		reached(&r, t0);
	}
	report(name, &r);
	return r;
}

/* As the main loop takes them: a pass 0-8 ms late, one in ten 10-40 ms,
 * the next due 20 ms after the last unless already past; the newest of
 * the magnetometer's 100 Hz samples, none if no new one, and of the
 * gyroscope's at 52 Hz; the time since the last sample. */
static struct run go_late(const char *name, path_fn path, const double offset[3], double seconds)
{
	struct run r = { .first = { -1, -1, -1, -1 } };
	double end = now + seconds, t0 = now, due = now, fed = now, last_mag = -1;
	record(2, now, NULL, NULL, NULL, name);
	unsigned passes = 0, feeds = 0;
	while (due < end) {
		now = due + (rnd() < 0.1 ? 0.01 + 0.03 * rnd() : 0.008 * rnd());
		now = round(now * 1000) / 1000;
		due += DT;
		if (due <= now)
			due = now + DT;
		passes++;
		double t_mag = floor(now / 0.01) * 0.01, t_gyro = floor(now * 52) / 52;
		if (t_mag == last_mag)
			continue;
		last_mag = t_mag;
		float raw[3], gyro[3];
		sample_at(path, offset, t_mag, t_gyro, raw, gyro);
		r.status = compass_feed(&c, raw, gyro, (float)(now - fed), r.out);
		fed = now;
		feeds++;
		reached(&r, t0);
	}
	report(name, &r);
	if (verbose)
		printf("%-28s %.1f samples a second of %.1f passes\n", "", feeds / seconds, passes / seconds);
	return r;
}

/* The watch's records: the field in counts of 0.15 µT and the rate in
 * counts of 70 mdps, both Android's axes, and 1 where the recording
 * lost samples. It was read from RAM at about 16 Hz without times, so a
 * sample counts as 20 ms on, the time the turns tell, and one after
 * lost ones as longer than a gap. */
static int replay(const char *path, int *first, float offset[3])
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	int16_t v[7];
	int n = 0, status = 0;
	for (int l = 0; l < 4; l++)
		first[l] = -1;
	for (; fread(v, sizeof v, 1, f) == 1; n++) {
		float raw[3], gyro[3], out[3];
		for (int i = 0; i < 3; i++) {
			raw[i] = v[i] * 0.15f;
			gyro[i] = v[3 + i] * (70e-3f * (float)PI / 180);
		}
		status = compass_feed(&c, raw, gyro, v[6] ? 1.0f : (float)DT, out);
		for (int l = 0; l <= status; l++)
			if (first[l] < 0)
				first[l] = n;
	}
	fclose(f);
	memcpy(offset, c.cal.offset, sizeof c.cal.offset);
	return n;
}

static void reset(void)
{
	compass_init(&c);
	now = 0;
	record(1, now, NULL, NULL, NULL, NULL);
	extra_on = gyro_off = gyro_turned = gyro_double = false;
}

static bool offset_near(const double raw_offset[3], float within)
{
	float o[3] = { (float)raw_offset[0], (float)raw_offset[1], (float)raw_offset[2] }, want[3];
	compass_correct(o, want);
	bool ok = true;
	for (int i = 0; i < 3; i++)
		ok &= fabsf(c.cal.offset[i] - want[i]) < within;
	if (!ok && verbose)
		printf("offset %.2f %.2f %.2f, want %.2f %.2f %.2f\n", c.cal.offset[0], c.cal.offset[1], c.cal.offset[2],
		       want[0], want[1], want[2]);
	return ok;
}

static void magnet(const double from[3], const double to[3], double ramp, bool nearer)
{
	extra_near = nearer;
	memcpy(extra_from, from, sizeof extra_from);
	memcpy(extra_to, to, sizeof extra_to);
	extra_t0 = now;
	extra_ramp = ramp;
	extra_on = true;
}

int main(int argc, char **argv)
{
	verbose = getenv("COMPASS_VERBOSE") != NULL;
	if (getenv("COMPASS_DUMP"))
		dump = fopen(getenv("COMPASS_DUMP"), "wb");
	invert_correction();
	static const double iron[3] = { 21, -13, 18 }, big[3] = { 250, 50, -260 };

	/* The correction is the vendor's, which its library gave for
	 * steady fields of 40 µT on each axis. */
	float x[3] = { 40, 0, 0 }, y[3] = { 0, 40, 0 }, z[3] = { 0, 0, 40 }, o[3];
	compass_correct(x, o);
	CHECK(fabsf(o[0] - 36.66f) < 0.2f && fabsf(o[1] + 4.32f) < 0.2f && fabsf(o[2] - 4.08f) < 0.2f);
	compass_correct(y, o);
	CHECK(fabsf(o[0] + 6.24f) < 0.2f && fabsf(o[1] - 39.78f) < 0.2f && fabsf(o[2] - 3.06f) < 0.2f);
	compass_correct(z, o);
	CHECK(fabsf(o[0] - 1.26f) < 0.2f && fabsf(o[1] + 0.12f) < 0.2f && fabsf(o[2] - 41.28f) < 0.2f);

	/* Held still it learns nothing. */
	reset();
	struct run r = go("still", still, iron, 10);
	CHECK(r.status == 0);

	/* Turned every way, as in the vendor replay, it gets to each
	 * accuracy about when the vendor's library does (1.2, 1.6 and
	 * 2.1 s for these samples) and finds the offset; with one far
	 * from the origin too. */
	reset();
	r = go("every way", sweep, iron, 15);
	CHECK(r.status == 3 && r.first[1] >= 800 && r.first[1] <= 1600 && r.first[3] >= 1600 && r.first[3] <= 2600 &&
	      offset_near(iron, 1));
	float field[3] = { r.out[0], r.out[1], r.out[2] };
	CHECK(fabsf(sqrtf(field[0] * field[0] + field[1] * field[1] + field[2] * field[2]) - 46.86f) < 1.5f);
	reset();
	r = go("every way, large offset", sweep, big, 15);
	CHECK(r.status == 3 && offset_near(big, 1.5f));

	/* A broad figure eight and a small one get there too, the small
	 * one slower, as with the vendor's (high at 1.5 and 3.8 s). */
	reset();
	k_yaw = 95 * PI / 180, k_pitch = 65 * PI / 180, k_roll = 95 * PI / 180, k_hz = 1 / 3.2;
	r = go("broad figure eight", eight, iron, 18);
	CHECK(r.status == 3 && r.first[3] < 3000);
	reset();
	k_yaw = 35 * PI / 180, k_pitch = 22 * PI / 180, k_roll = 12 * PI / 180;
	r = go("small figure eight", eight, iron, 18);
	CHECK(r.status == 3 && r.first[3] > 3000 && r.first[3] < 5000);

	/* A field that turns without the gyroscope seeing it is not
	 * learnt from, as with the vendor's. */
	reset();
	gyro_off = true;
	r = go("turns the gyroscope missed", sweep, iron, 15);
	CHECK(r.status == 0);

	/* On the charger: unreliable at its first sample, which keeps the
	 * last field; taken off cleanly it is high again within seconds of
	 * turning, the offset kept. */
	reset();
	go("every way", sweep, iron, 15);
	float before[3];
	memcpy(before, c.out, sizeof before);
	static const double zero[3];
	magnet(zero, charger, 0, true);
	float raw[3], gyro[3], out[3];
	record(2, now, NULL, NULL, NULL, "onto the charger");
	sample(still, iron, raw, gyro);
	CHECK(compass_feed(&c, raw, gyro, (float)DT, out) == 0 && !memcmp(out, before, sizeof out));
	now += DT;
	r = go("on the charger", still, iron, 10);
	CHECK(r.status == 0 && r.first[1] < 0 && offset_near(iron, 1));
	extra_on = false;
	r = go("taken off the charger", sweep, iron, 10);
	CHECK(r.status == 3 && r.first[1] < 1500 && r.first[3] < 2500 && offset_near(iron, 1));

	/* The captured state from the watch: fresh, on the charger, its
	 * noise in every direction around the magnets' field, which is not
	 * a calibration; then taken off. */
	reset();
	magnet(zero, charger, 0, true);
	r = go("fresh on the charger", still, iron, 20);
	CHECK(r.status == 0 && c.cal.level == 0);
	extra_on = false;
	r = go("then taken off", sweep, iron, 15);
	CHECK(r.status == 3 && offset_near(iron, 1));

	/* Taken off slowly, the last 180 µT of it on the way without a
	 * jump: those samples are left behind. */
	reset();
	magnet(zero, charger, 0, true);
	go("fresh on the charger", still, iron, 2);
	double tail[3] = { 180, 40, -60 };
	magnet(charger, tail, 2.5, false);
	go("taken off slowly", sweep, iron, 2.5);
	magnet(tail, zero, 1.5, false);
	go("the last of it", sweep, iron, 1.5);
	extra_on = false;
	r = go("then turned", sweep, iron, 15);
	CHECK(r.status == 3 && r.first[2] < 10000 && offset_near(iron, 1));

	/* Put back on the charger: unreliable before it gets there, then
	 * high again soon after taken off. */
	magnet(zero, charger, 3, true);
	double t0 = now;
	int drop_ms = -1;
	record(2, now, NULL, NULL, NULL, "put back on the charger");
	for (; now < t0 + 3; now += DT) {
		sample(still, iron, raw, gyro);
		if (compass_feed(&c, raw, gyro, (float)DT, out) == 0 && drop_ms < 0)
			drop_ms = (int)lround((now - t0) * 1000);
	}
	if (verbose)
		printf("%-28s unreliable after %d ms of 3000\n", "put back on the charger", drop_ms);
	CHECK(drop_ms >= 0 && drop_ms < 1500);
	extra_on = false;
	r = go("taken off again", sweep, iron, 10);
	CHECK(r.status == 3 && r.first[1] < 2000 && offset_near(iron, 1));

	/* A field that changes at once comes out over eight samples. */
	reset();
	float a[3] = { 0, 40, -30 }, b[3] = { -40, 0, -30 }, ra[3], rb[3];
	for (int i = 0; i < 3; i++) {
		ra[i] = (float)(distort[i][0] * a[0] + distort[i][1] * a[1] + distort[i][2] * a[2]);
		rb[i] = (float)(distort[i][0] * b[0] + distort[i][1] * b[1] + distort[i][2] * b[2]);
	}
	for (int k = 0; k < 20; k++)
		compass_feed(&c, ra, NULL, (float)DT, out);
	float xs[COMPASS_AVG];
	for (int k = 0; k < COMPASS_AVG; k++) {
		compass_feed(&c, rb, NULL, (float)DT, out);
		xs[k] = out[0];
	}
	CHECK(fabsf(xs[0] + 5) < 0.1f && fabsf(xs[3] + 20) < 0.1f && fabsf(xs[7] + 40) < 0.1f);
	/* After a gap the average starts over from the next sample. */
	for (int k = 0; k < 3; k++)
		compass_feed(&c, ra, NULL, (float)DT, out);
	compass_restart(&c);
	compass_feed(&c, rb, NULL, (float)DT, out);
	CHECK(fabsf(out[0] + 40) < 0.1f && fabsf(out[1]) < 0.1f);

	/* Taken as the main loop gets to them, about 45 a second at times
	 * apart from 1 to 48 ms, the same; with the gyroscope's axes wrong
	 * the turns explain nothing, and at twice its rate never enough
	 * for high or to stay. */
	reset();
	r = go_late("every way, late passes", sweep, iron, 15);
	CHECK(r.status == 3 && r.first[1] >= 800 && r.first[3] <= 2600 && offset_near(iron, 1));
	reset();
	k_yaw = 95 * PI / 180, k_pitch = 65 * PI / 180, k_roll = 95 * PI / 180, k_hz = 1 / 3.2;
	r = go_late("broad eight, late passes", eight, iron, 18);
	CHECK(r.status == 3 && r.first[3] < 3000);
	reset();
	gyro_off = true;
	r = go_late("missed turns, late passes", sweep, iron, 15);
	CHECK(r.status == 0);
	reset();
	gyro_turned = true;
	r = go_late("gyroscope's axes wrong", sweep, iron, 15);
	CHECK(r.status == 0);
	reset();
	gyro_double = true;
	r = go_late("gyroscope at twice the rate", sweep, iron, 15);
	CHECK(r.status <= 1 && r.first[3] < 0);

	/* beluga taken off its charger on 22 November (at sample 25), held
	 * and turned about one axis, then every way from sample 320 for
	 * 22 s; from fresh and with the offset the watch had before, 11 µT
	 * off this one: high within 3 s of turning every way, the offset
	 * within 3 µT of both the one the watch found itself later (200.4
	 * 17.8 -190.5 µT) and a sphere through all the samples. */
	const char *data = argc > 1 ? argv[1] : "tests/data/beluga-compass-2026-11-22.bin";
	static const float want[3] = { 201.5f, 17.7f, -190.2f };
	for (int seeded = 0; seeded < 2; seeded++) {
		reset();
		if (seeded)
			c.cal = (struct compass_cal){ { 206.6f, 28.1f, -188.6f }, 45.2f, 3 };
		int first[4];
		float off[3];
		int n = replay(data, first, off);
		CHECK(n > 1000);
		CHECK(c.status == 3 && first[1] > 25 && first[3] < 320 + 150);
		CHECK(fabsf(off[0] - want[0]) < 3 && fabsf(off[1] - want[1]) < 3 && fabsf(off[2] - want[2]) < 3);
		if (verbose)
			printf("%-28s accuracy %d, first at 1: %d ms, 2: %d ms, 3: %d ms, offset %.1f %.1f %.1f\n",
			       seeded ? "the watch, offset from before" : "the watch, fresh", c.status, (first[1] - 320) * 20,
			       (first[2] - 320) * 20, (first[3] - 320) * 20, off[0], off[1], off[2]);
	}

	if (dump)
		fclose(dump);
	if (failures)
		return 1;
	printf("compass_test: ok\n");
	return 0;
}
