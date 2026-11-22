/* The Compass app's dial: the state for each accuracy, the heading held
 * within COMPASS_HOLD and shown at most every COMPASS_SHOW_MS, also as
 * the accuracy changes, the page and its sensors as it opens and the
 * watch sleeps and wakes, and the real samples of beluga turned off its charger (in tests/data, or the
 * file given) through the calibration and the orientation into what the
 * page shows. The recording has no accelerometer, so gravity is turned
 * with the gyroscope from the watch lying flat. COMPASS_VERBOSE=1
 * prints what the page shows as it changes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "ui/ui.h"
#include "sensors/compass.h"
#include "sensors/fusion.h"
#include "sensors/hub.h"
#include "compass_app.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* The hub's compass and what the page was last given. */
static bool hub_on;
static float hub_degrees;
static int hub_accuracy = -1;
static uint32_t shown_degrees = 999, shown_state = 999;
void hub_compass_on(bool on) { hub_on = on; }
int hub_heading(float *degrees)
{
	*degrees = hub_degrees;
	return hub_on ? hub_accuracy : -1;
}
void ui_set_heading(uint32_t degrees, uint32_t state)
{
	shown_degrees = degrees;
	shown_state = state;
}

#define PI	3.14159265f

static float off(float a, float b)
{
	float d = fmodf(fabsf(a - b), 360);
	return d > 180 ? 360 - d : d;
}

static int state_of(int accuracy)
{
	return accuracy <= 0 ? UI_COMPASS_NO_DATA : accuracy < 3 ? UI_COMPASS_CALIBRATE : UI_COMPASS_READY;
}

static void levels(void)
{
	struct compass_view v;
	compass_view_reset(&v);
	CHECK(v.state == UI_COMPASS_NO_DATA);
	CHECK(!compass_view_update(&v, 0, -1, 0) && v.state == UI_COMPASS_NO_DATA);
	CHECK(!compass_view_update(&v, 10.2f, 0, 0) && v.state == UI_COMPASS_NO_DATA);
	/* Low and medium show the dial too, not only high, as
	 * asteroid-compass means to. */
	CHECK(compass_view_update(&v, 10.2f, 1, 10) && v.state == UI_COMPASS_CALIBRATE && v.degrees == 10);
	CHECK(!compass_view_update(&v, 10.2f, 2, 20) && v.state == UI_COMPASS_CALIBRATE);
	CHECK(compass_view_update(&v, 10.2f, 3, 30) && v.state == UI_COMPASS_READY);
	CHECK(compass_view_update(&v, 10.2f, 0, 40) && v.state == UI_COMPASS_NO_DATA);
	CHECK(compass_view_update(&v, 10.2f, 3, 50) && v.state == UI_COMPASS_READY);
	CHECK(compass_view_update(&v, 10.2f, -1, 60) && v.state == UI_COMPASS_NO_DATA);
}

static void holds(void)
{
	struct compass_view v;
	compass_view_reset(&v);
	CHECK(compass_view_update(&v, 10.2f, 3, 1000) && v.degrees == 10);
	CHECK(!compass_view_update(&v, 10.6f, 3, 1200) && v.degrees == 10);
	CHECK(compass_view_update(&v, 10.8f, 3, 1300) && v.degrees == 11);
	CHECK(!compass_view_update(&v, 11.9f, 3, 1350) && v.degrees == 11);
	CHECK(compass_view_update(&v, 11.9f, 3, 1400) && v.degrees == 12);
	CHECK(compass_view_update(&v, 359.7f, 3, 1500) && v.degrees == 0);
	CHECK(!compass_view_update(&v, 359.4f, 3, 1600) && v.degrees == 0);
	CHECK(compass_view_update(&v, 359.0f, 3, 1700) && v.degrees == 359);
	CHECK(compass_view_update(&v, 0.4f, 3, 1800) && v.degrees == 0);
	CHECK(!compass_view_update(&v, 360, 3, 1900) && v.degrees == 0);
}

/* A change of accuracy shows at once, the heading only as often as it
 * would otherwise, except as the dial first shows. */
static void accuracy_changes(void)
{
	struct compass_view v;
	compass_view_reset(&v);
	CHECK(compass_view_update(&v, 10, 3, 0) && v.degrees == 10);
	CHECK(compass_view_update(&v, 30, 2, 20) && v.state == UI_COMPASS_CALIBRATE && v.degrees == 10);
	CHECK(compass_view_update(&v, 50, 3, 40) && v.state == UI_COMPASS_READY && v.degrees == 10);
	CHECK(!compass_view_update(&v, 50, 3, 80) && v.degrees == 10);
	CHECK(compass_view_update(&v, 50, 3, 100) && v.degrees == 50);
	CHECK(compass_view_update(&v, 50.6f, 2, 300) && v.state == UI_COMPASS_CALIBRATE && v.degrees == 50);
	CHECK(!compass_view_update(&v, 50.6f, 2, 400) && v.degrees == 50);
	compass_view_reset(&v);
	CHECK(compass_view_update(&v, 359.2f, 3, 0) && v.degrees == 359);
	CHECK(compass_view_update(&v, 0.6f, 2, 20) && v.degrees == 359);
	CHECK(compass_view_update(&v, 0.6f, 2, 120) && v.degrees == 1);
	CHECK(compass_view_update(&v, 0.6f, 3, 140) && v.state == UI_COMPASS_READY && v.degrees == 1);
	CHECK(compass_view_update(&v, 0.6f, 0, 160) && v.state == UI_COMPASS_NO_DATA);
	CHECK(compass_view_update(&v, 200, 2, 170) && v.state == UI_COMPASS_CALIBRATE && v.degrees == 200);
}

/* The page shows No data on opening and on waking until the hub has a
 * heading again, and the hub's compass goes off with the page and the
 * screen at once. */
static void page(void)
{
	hub_degrees = 90, hub_accuracy = 3;
	compass_app_page(true);
	CHECK(shown_state == UI_COMPASS_NO_DATA && !hub_on);
	compass_app_pass(true, 1000);
	CHECK(hub_on && shown_state == UI_COMPASS_READY && shown_degrees == 90);
	compass_app_sleep();
	CHECK(!hub_on && shown_state == UI_COMPASS_NO_DATA);
	hub_accuracy = -1;
	compass_app_pass(true, 5000);
	CHECK(hub_on && shown_state == UI_COMPASS_NO_DATA);
	hub_accuracy = 3, hub_degrees = 180;
	compass_app_pass(true, 5020);
	CHECK(shown_state == UI_COMPASS_READY && shown_degrees == 180);
	compass_app_page(false);
	CHECK(!hub_on && shown_degrees == 180);
	compass_app_pass(false, 5040);
	CHECK(!hub_on);
	compass_app_page(true);
	CHECK(shown_state == UI_COMPASS_NO_DATA && shown_degrees == 0);
	compass_app_pass(true, 9000);
	CHECK(hub_on && shown_state == UI_COMPASS_READY && shown_degrees == 180);
	compass_app_pass(false, 9020);
	CHECK(!hub_on);
}

/* The watch's records: the field in counts of 0.15 µT and the rate in
 * counts of 70 mdps, both Android's axes, and 1 where the recording
 * lost samples; a sample counts as 20 ms on, one after lost ones as a
 * second. */
static void replay(const char *path, bool verbose)
{
	FILE *f = fopen(path, "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	static struct compass c;
	static struct fusion fu;
	struct compass_view v;
	compass_init(&c);
	fusion_init(&fu);
	compass_view_reset(&v);
	float g[3] = { 0, 0, 9.8f };
	uint32_t now = 0, shows = 0, at_state[3] = { 0 };
	int16_t r[7];
	int n = 0, first[3] = { -1, -1, -1 }, accuracy = -1;
	for (; fread(r, sizeof r, 1, f) == 1; n++) {
		float raw[3], w[3], field[3], dt = r[6] ? 1.0f : 0.02f;
		for (int i = 0; i < 3; i++) {
			raw[i] = r[i] * 0.15f;
			w[i] = r[3 + i] * (70e-3f * PI / 180);
		}
		float wg[3] = { w[1] * g[2] - w[2] * g[1], w[2] * g[0] - w[0] * g[2], w[0] * g[1] - w[1] * g[0] }, len = 0;
		for (int i = 0; i < 3; i++) {
			g[i] -= wg[i] * (r[6] ? 0 : dt);
			len += g[i] * g[i];
		}
		for (int i = 0; i < 3; i++)
			g[i] *= 9.8f / sqrtf(len);
		now += (uint32_t)(dt * 1000);
		accuracy = compass_feed(&c, raw, w, dt, field);
		fusion_update(&fu, g, w, field, accuracy, dt);
		float angles[3];
		bool have = fusion_angles(&fu, angles);
		int32_t was = v.degrees, was_state = v.state;
		uint32_t was_t = v.shown_t;
		bool changed = compass_view_update(&v, angles[0], have ? accuracy : -1, now);
		/* The page shows no more than the accuracy says, and the
		 * heading the orientation has, at most every
		 * COMPASS_SHOW_MS once the dial is up. */
		CHECK(v.state == state_of(have ? accuracy : -1));
		CHECK(v.degrees >= 0 && v.degrees < 360);
		if (v.degrees != was) {
			CHECK(off((float)v.degrees, angles[0]) <= 0.5f + 1e-3f);
			CHECK(was_state == UI_COMPASS_NO_DATA || now - was_t >= COMPASS_SHOW_MS);
		}
		if (v.state != UI_COMPASS_NO_DATA)
			CHECK(off((float)v.degrees, angles[0]) < COMPASS_HOLD + 1e-3f || now - v.shown_t < COMPASS_SHOW_MS);
		if (first[v.state] < 0)
			first[v.state] = n;
		at_state[v.state]++;
		if (changed) {
			shows++;
			if (verbose)
				printf("%5d %6u ms: %s %3d°\n", n, now,
				       v.state == UI_COMPASS_READY ? "ready" : v.state ? "calibrate" : "no data", v.degrees);
		}
	}
	fclose(f);
	CHECK(n > 1000);
	/* No data until the accuracy is low (after sample 25, as the
	 * calibration test has it), the line asking for the figure 8 on
	 * the dial after that, the dial alone by the end of the turns. */
	CHECK(first[UI_COMPASS_NO_DATA] == 0 && first[UI_COMPASS_CALIBRATE] > 25 &&
	      first[UI_COMPASS_READY] > first[UI_COMPASS_CALIBRATE] && first[UI_COMPASS_READY] < 320 + 150);
	CHECK(v.state == UI_COMPASS_READY && at_state[UI_COMPASS_CALIBRATE] > 0);
	/* As often as the dial may turn, at most. */
	CHECK(shows <= now / COMPASS_SHOW_MS + 10);
	CHECK(shows > 50);
	if (verbose)
		printf("%d samples over %u ms, %u shown; first calibrate %d, ready %d\n", n, now, shows,
		       first[UI_COMPASS_CALIBRATE], first[UI_COMPASS_READY]);
}

int main(int argc, char **argv)
{
	bool verbose = getenv("COMPASS_VERBOSE") != NULL;
	levels();
	holds();
	accuracy_changes();
	page();
	replay(argc > 1 ? argv[1] : "tests/data/beluga-compass-2026-11-22.bin", verbose);
	if (failures)
		return 1;
	printf("compass_app_test: ok\n");
	return 0;
}
