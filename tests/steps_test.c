/* The step history on the host: the hub's count since boot on top of the
 * saved total, the day's end into the ring, days missed, the ring
 * wrapping, a reboot, and the clock set back; then the distance and
 * calories, worked out from the steps or measured by the hub. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "sensors/hub.h"
#include "sensors/steps.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static struct {
	char key[32];
	uint8_t val[256];
	unsigned len;
	bool used;
} kvs[4];
static unsigned kv_writes;
static uint32_t fake_rtc, fake_steps = HUB_NONE, fake_measured[2];
static bool measures, measured_now;
static struct hub_profile profile;

int kv_get(const char *key, void *buf, unsigned size)
{
	for (unsigned i = 0; i < 4; i++)
		if (kvs[i].used && !strcmp(kvs[i].key, key)) {
			memcpy(buf, kvs[i].val, kvs[i].len < size ? kvs[i].len : size);
			return (int)kvs[i].len;
		}
	return -2;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	kv_writes++;
	for (unsigned i = 0; i < 4; i++)
		if (!kvs[i].used || !strcmp(kvs[i].key, key)) {
			strcpy(kvs[i].key, key);
			memcpy(kvs[i].val, val, len);
			kvs[i].len = len;
			kvs[i].used = true;
			return 0;
		}
	return -1;
}
uint32_t rtc_read(void) { return fake_rtc; }
uint32_t hub_steps(void) { return fake_steps; }
static uint32_t fake_today = HUB_NONE, synced = HUB_NONE;
static unsigned syncs;
uint32_t hub_steps_today(void) { return fake_today; }
void hub_sync_steps_today(uint32_t steps)
{
	synced = fake_today = steps;
	syncs++;
}
bool hub_measures_activity(void) { return measures; }
bool hub_activity(uint32_t v[2])
{
	if (!measured_now)
		return false;
	v[0] = fake_measured[0];
	v[1] = fake_measured[1];
	return true;
}
void hub_profile(const struct hub_profile *p) { profile = *p; }
int setting_get(enum setting s)
{
	return s == SETTING_HEIGHT ? 180 : s == SETTING_WEIGHT ? 80 : s == SETTING_AGE ? 41 : s == SETTING_MALE ? 1 : 0;
}
void log_printf(const char *fmt, ...) {}

#define DAY(d)	((uint32_t)(d) * 86400)

static void at(uint32_t day, uint32_t secs, uint32_t count)
{
	fake_rtc = DAY(day) + secs;
	fake_steps = count;
	steps_poll();
}

int main(void)
{
	/* Nothing before init. */
	fake_rtc = DAY(20000) + 3600;
	fake_steps = 50;
	CHECK(!steps_poll());
	CHECK(kv_writes == 0);

	steps_init();
	CHECK(steps_today() == 0 && steps_day(1) == 0);
	fake_steps = HUB_NONE;
	CHECK(!steps_poll());
	fake_steps = 120;
	CHECK(steps_poll());
	CHECK(!steps_poll());
	CHECK(steps_today() == 120);
	CHECK(kv_writes == 0);
	steps_save();
	CHECK(kv_writes == 1);
	steps_save();
	CHECK(kv_writes == 1);

	/* Midnight: the day goes into the ring, today starts at 0. */
	at(20000, 86399, 500);
	CHECK(steps_today() == 500);
	fake_rtc = DAY(20001) + 1;
	CHECK(steps_poll());
	CHECK(steps_today() == 0 && steps_day(1) == 500 && steps_day(2) == 0);
	at(20001, 7200, 800);
	CHECK(steps_today() == 300);

	/* Two days with the watch off: they count as none. */
	at(20004, 60, 900);
	CHECK(steps_today() == 0);
	CHECK(steps_day(1) == 0 && steps_day(2) == 0 && steps_day(3) == 400 && steps_day(4) == 500);
	CHECK(steps_day(STEPS_DAYS + 5) == 0);

	/* A reboot: the hub counts from 0 again, on top of the saved total. */
	at(20004, 600, 1000);
	steps_save();
	steps_init();
	at(20004, 700, 0);
	CHECK(steps_today() == 100);
	at(20004, 800, 40);
	CHECK(steps_today() == 140);
	CHECK(steps_day(3) == 400 && steps_day(4) == 500);

	/* The ring wraps: a month of days, each with its own count. */
	uint32_t count = 40;
	for (uint32_t d = 20005; d < 20005 + STEPS_DAYS + 3; d++) {
		count += d - 20000;
		at(d, 3600, count);
	}
	uint32_t today = 20004 + STEPS_DAYS + 3;
	CHECK(steps_today() == 0);
	for (unsigned ago = 1; ago <= STEPS_DAYS; ago++)
		CHECK(steps_day(ago) == today - ago - 20000 + 1);
	CHECK(steps_day(STEPS_DAYS + 1) == 0);

	/* A month and more away: the whole ring is days of none. */
	at(today + 40, 10, count + 70);
	for (unsigned ago = 1; ago <= STEPS_DAYS; ago++)
		CHECK(steps_day(ago) == 0);
	CHECK(steps_today() == 0);
	at(today + 40, 20, count + 75);
	CHECK(steps_today() == 5);

	/* The clock set back a day: today starts over, the ring keeps its
	 * days, and the next midnight puts the right day in. */
	today += 40;
	at(today + 1, 10, count + 80);
	CHECK(steps_day(1) == 10);
	at(today, 100, count + 90);
	CHECK(steps_today() == 0 && steps_day(1) == 0);
	at(today, 200, count + 95);
	at(today + 1, 300, count + 95);
	CHECK(steps_day(1) == 5 && steps_today() == 0);

	/* Just after midnight, however far away. */
	fake_rtc = DAY(20000) + 86400 - 30;
	CHECK(steps_next_ms() == 35000);
	fake_rtc = DAY(20000) + 3600;
	CHECK(steps_next_ms() == (86400 - 3600 + 5) * 1000u);

	/* Worked out from the steps: 180 cm, 80 kg. */
	at(today + 1, 400, count + 1095);
	struct activity a = steps_activity_today();
	CHECK(a.steps == 1000 && a.distance_m == 747 && a.kcal == 29);
	CHECK(profile.age == 41 && profile.male && profile.height_cm == 180 && profile.weight_kg == 80);

	/* The distance measured by the hub: from its first reading on,
	 * across a reboot and into the next day. The calories still come
	 * from the distance, not from the hub. */
	measures = true;
	struct activity from = steps_activity();
	measured_now = true;
	fake_measured[0] = 300;
	fake_measured[1] = 20;
	CHECK(steps_poll());
	a = steps_since(&from);
	CHECK(a.distance_m == 300 && a.kcal == 12);
	CHECK(steps_activity_today().distance_m == 300);
	fake_measured[0] = 500;
	CHECK(steps_poll());
	CHECK(!steps_poll());
	steps_save();
	steps_init();
	fake_measured[0] = 0;
	fake_measured[1] = 0;
	at(today + 1, 500, 0);
	CHECK(steps_activity_today().distance_m == 500 && steps_activity_today().kcal == 20);
	fake_measured[0] = 50;
	at(today + 2, 10, 5);
	a = steps_activity_today();
	CHECK(a.steps == 0 && a.distance_m == 0 && a.kcal == 0);
	CHECK(steps_since(&from).distance_m == 550);

	/* What the first version saved: the steps alone. */
	measured_now = false;
	memset(kvs, 0, sizeof kvs);
	uint32_t v1[3] = { 777, today + 2, 700 };
	kv_set("steps", v1, sizeof v1);
	steps_init();
	CHECK(steps_today() == 77 && steps_activity_today().distance_m == 0);

	/* A hub restarted while running: its count starts over, ours goes
	 * on. */
	at(today + 2, 600, 5000);
	uint32_t before_restart = steps_today();
	at(today + 2, 700, 20);
	CHECK(steps_today() == before_restart + 20);
	at(today + 2, 800, 25);
	CHECK(steps_today() == before_restart + 25);

	/* Booted just after midnight: the steps since boot are today's, the
	 * day before keeps what was saved. */
	steps_save();
	uint32_t saved_today = steps_today();
	fake_rtc = DAY(today + 3) + 60;
	fake_steps = HUB_NONE;
	steps_init();
	CHECK(steps_today() == 0 && steps_day(1) == saved_today);
	at(today + 3, 70, 30);
	CHECK(steps_today() == 30 && steps_day(1) == saved_today);

	/* A hub that keeps steps today itself (catfish), read once after a
	 * boot on the same day: the steps it counted while the watch was off
	 * come in, and nothing is written back when it agrees. */
	memset(kvs, 0, sizeof kvs);
	uint32_t d = 21000;
	fake_rtc = DAY(d) + 3600;
	fake_steps = HUB_NONE;
	steps_init();
	at(d, 3600, 400);
	steps_save();
	steps_init();
	at(d, 7200, 0);
	fake_today = 1000;
	unsigned before = syncs;
	CHECK(steps_poll());
	CHECK(steps_today() == 1000 && syncs == before);
	/* Later on its count is not taken, and its LCD gets ours. */
	fake_today = 5000;
	at(d, 7300, 10);
	CHECK(steps_today() == 1010 && syncs == before + 1 && synced == 1010);
	/* It dropped the first steps it counted: its LCD gets ours again,
	 * and nothing is written while the two agree. */
	fake_today = 1010 - 23;
	at(d, 7310, 10);
	CHECK(steps_today() == 1010 && syncs == before + 2 && synced == 1010);
	at(d, 7320, 10);
	CHECK(syncs == before + 2);

	/* What it saved lags ours: ours stays and goes to its LCD. */
	steps_save();
	steps_init();
	fake_today = 990;
	at(d, 7400, 0);
	CHECK(steps_today() == 1010 && syncs == before + 3 && synced == 1010);

	/* Booted on another day with the count of a day the hub never saw
	 * end: not taken, and its LCD gets ours. */
	steps_save();
	fake_rtc = DAY(d + 1) + 3600;
	fake_steps = HUB_NONE;
	steps_init();
	fake_today = 8000;
	at(d + 1, 3600, 12);
	CHECK(steps_today() == 12 && steps_day(1) == 1010 && synced == 12);

	/* Midnight with the hub's count held back an hour: today starts at
	 * 0 and stays its own, yesterday's count is never sent back, and
	 * three days in a row do not pile up. */
	uint32_t c = 12;
	for (uint32_t n = 0; n < 3; n++) {
		c += 900;
		at(d + 1 + n, 86399, c);
		uint32_t ended = steps_today();
		CHECK(ended == (n ? 903 : 912));
		fake_today = ended;
		at(d + 2 + n, 5, c);
		CHECK(steps_today() == 0 && steps_day(1) == ended && synced == 0);
		at(d + 2 + n, 130, c);
		/* Its held-back count of yesterday comes in: ours goes back. */
		fake_today = ended;
		at(d + 2 + n, 3600, c);
		CHECK(steps_today() == 0 && steps_day(1) == ended && synced == 0);
		c += 3;
		at(d + 2 + n, 3700, c);
		CHECK(steps_today() == 3 && synced == 3);
	}

	printf(failures ? "steps_test: %d failures\n" : "steps_test: ok\n", failures);
	return failures != 0;
}
