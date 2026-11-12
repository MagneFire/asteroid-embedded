/* The step history on the host: the hub's count since boot on top of the
 * saved total, the day's end into the ring, days missed, the ring
 * wrapping, a reboot, and the clock set back. */
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
static uint32_t fake_rtc, fake_steps = HUB_NONE;

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

	/* Midnight, at most an hour away. */
	fake_rtc = DAY(20000) + 86400 - 30;
	CHECK(steps_next_ms() == 31000);
	fake_rtc = DAY(20000) + 3600;
	CHECK(steps_next_ms() == 3600000);

	printf(failures ? "steps_test: %d failures\n" : "steps_test: ok\n", failures);
	return failures != 0;
}
