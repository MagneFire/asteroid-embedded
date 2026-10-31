/* The timer, the stopwatch and the ringing on the host, over an
 * in-memory key-value store, with stub_ms as the clock and stub_rtc
 * as the RTC. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "alarm.h"
#include "ui/ui.h"
#include "clockapps.h"
#include "stubs.h"

static struct {
	char key[32];
	uint8_t val[512];
	unsigned len;
	bool used;
} kvs[8];

int kv_get(const char *key, void *buf, unsigned size)
{
	for (unsigned i = 0; i < 8; i++)
		if (kvs[i].used && !strcmp(kvs[i].key, key)) {
			memcpy(buf, kvs[i].val, kvs[i].len < size ? kvs[i].len : size);
			return (int)kvs[i].len;
		}
	return -2;
}

int kv_set(const char *key, const void *val, unsigned len)
{
	unsigned free = 8;
	for (unsigned i = 0; i < 8; i++) {
		if (kvs[i].used && !strcmp(kvs[i].key, key)) {
			free = i;
			break;
		}
		if (!kvs[i].used && free == 8)
			free = i;
	}
	CHECK(free < 8 && len <= sizeof kvs[0].val);
	snprintf(kvs[free].key, sizeof kvs[free].key, "%s", key);
	memcpy(kvs[free].val, val, len);
	kvs[free].len = len;
	kvs[free].used = true;
	return 0;
}

static uint32_t t_ms, t_set, sw_ms, laps_n, laps_first;
static bool t_running, sw_running, ring_on, ring_is_timer, haptic_on;
static int ring_calls, haptic_calls;
void ui_set_timer(uint32_t ms, uint32_t set_ms, bool running) { t_ms = ms; t_set = set_ms; t_running = running; }
void ui_set_stopwatch(uint32_t ms, bool running) { sw_ms = ms; sw_running = running; }
void ui_set_ringing(bool ringing, bool timer) { ring_calls++; ring_on = ringing; ring_is_timer = timer; }
void ui_set_laps(const uint32_t *ms, uint32_t n) { laps_n = n; laps_first = n ? ms[0] : 0; }
void haptics_play(const uint16_t *on_off_ms, unsigned n) { haptic_calls++; haptic_on = on_off_ms != NULL; }

static void event(uint32_t kind, int value)
{
	struct ui_event e = { .kind = kind, .value = value };
	clockapps_event(&e, stub_ms);
}

static void alarm_event(uint32_t kind, int index, uint8_t hour, uint8_t minute, uint8_t days)
{
	struct ui_event e = { .kind = kind, .value = index, .alarm = { hour, minute, days, true } };
	clockapps_event(&e, stub_ms);
}

static void test_timer(void)
{
	stub_ms = 1000;
	stub_rtc = 100000;
	clockapps_init(true, true);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 300000 && t_set == 300000 && !t_running);
	CHECK(sw_ms == 0 && !sw_running);
	CHECK(clockapps_next_ms(stub_ms) == UINT32_MAX);

	event(UI_EVENT_TIMER_SET, 10);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 10000 && t_set == 10000);
	stub_ms = 2000;
	event(UI_EVENT_CLOCK, UI_TIMER_START);
	stub_ms = 6000;
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 6000 && t_running);
	CHECK(clockapps_next_ms(stub_ms) == 6000);
	/* A second set while running is ignored. */
	event(UI_EVENT_TIMER_SET, 20);
	clockapps_refresh(stub_ms);
	CHECK(t_set == 10000);

	stub_ms = 11999;
	CHECK(!clockapps_tick(stub_ms) && !clockapps_ringing());
	stub_ms = 12000;
	CHECK(clockapps_tick(stub_ms) && clockapps_ringing());
	CHECK(ring_on && ring_is_timer && haptic_calls == 1 && haptic_on);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 10000 && !t_running);
	CHECK(clockapps_next_ms(stub_ms) == UINT32_MAX);
	event(UI_EVENT_ALARM_RING, UI_ALARM_DISMISS);
	CHECK(!clockapps_ringing());
	stub_ms = 12100;
	CHECK(!clockapps_tick(stub_ms) && !ring_on && !haptic_on);

	/* Unanswered, a timer stops ringing after 30 s. */
	stub_ms = 13000;
	event(UI_EVENT_CLOCK, UI_TIMER_START);
	stub_ms = 23000;
	CHECK(clockapps_tick(stub_ms) && clockapps_ringing());
	stub_ms = 23000 + 29999;
	CHECK(!clockapps_tick(stub_ms) && clockapps_ringing());
	stub_ms = 23000 + 30000;
	CHECK(!clockapps_tick(stub_ms) && !clockapps_ringing());

	/* Pause, resume, reset, and the +/- buttons. */
	stub_ms = 60000;
	event(UI_EVENT_CLOCK, UI_TIMER_START);
	stub_ms = 63000;
	event(UI_EVENT_CLOCK, UI_TIMER_START);
	stub_ms = 70000;
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 7000 && !t_running);
	event(UI_EVENT_CLOCK, UI_TIMER_START);
	stub_ms = 71000;
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 6000 && t_running);
	event(UI_EVENT_CLOCK, UI_TIMER_RESET);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 10000 && !t_running);
	event(UI_EVENT_CLOCK, UI_TIMER_MINUTE_UP);
	event(UI_EVENT_CLOCK, UI_TIMER_SECONDS_DOWN);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 60000 && t_set == 60000);
	for (int i = 0; i < 10; i++)
		event(UI_EVENT_CLOCK, UI_TIMER_MINUTE_DOWN);
	clockapps_refresh(stub_ms);
	CHECK(t_set == 10000);
}

static void test_stopwatch(void)
{
	stub_ms = 50000;
	event(UI_EVENT_CLOCK, UI_STOPWATCH_START);
	stub_ms = 52500;
	event(UI_EVENT_CLOCK, UI_STOPWATCH_LAP);
	CHECK(laps_n == 1 && laps_first == 2500);
	stub_ms = 53000;
	clockapps_refresh(stub_ms);
	CHECK(sw_ms == 3000 && sw_running);
	stub_ms = 55000;
	event(UI_EVENT_CLOCK, UI_STOPWATCH_START);
	stub_ms = 56000;
	clockapps_refresh(stub_ms);
	CHECK(sw_ms == 5000 && !sw_running);
	event(UI_EVENT_CLOCK, UI_STOPWATCH_LAP);
	CHECK(laps_n == 1);
	stub_ms = 60000;
	event(UI_EVENT_CLOCK, UI_STOPWATCH_START);
	stub_ms = 61000;
	clockapps_refresh(stub_ms);
	CHECK(sw_ms == 6000 && sw_running);
}

static void test_persistence(void)
{
	/* The stopwatch was saved running at 60000 ms with 5000 ms on it;
	 * at 61000 ms a 100 s timer starts. */
	stub_ms = 61000;
	event(UI_EVENT_TIMER_SET, 100);
	event(UI_EVENT_CLOCK, UI_TIMER_START);
	uint32_t rtc0 = stub_rtc;
	/* Reboot 10 s later. */
	stub_rtc = rtc0 + 10;
	stub_ms = 500;
	laps_n = 99;
	clockapps_init(true, true);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 90000 && t_set == 100000 && t_running);
	CHECK(sw_ms == 15000 && sw_running);
	CHECK(laps_n == 1 && laps_first == 2500);
	CHECK(clockapps_next_ms(stub_ms) == 90000);
	/* Reboot after the timer ended: back at its duration. */
	stub_rtc = rtc0 + 200;
	stub_ms = 500;
	clockapps_init(true, true);
	clockapps_refresh(stub_ms);
	CHECK(t_ms == 100000 && !t_running);
	CHECK(sw_ms == 205000 && sw_running);
	event(UI_EVENT_CLOCK, UI_STOPWATCH_RESET);
	CHECK(laps_n == 0);
	clockapps_refresh(stub_ms);
	CHECK(sw_ms == 0 && !sw_running);
}

static void test_alarm(void)
{
	/* Day 3 at 07:05:00; an alarm for 07:06, once. */
	stub_rtc = 86400 * 3 + 3600 * 7 + 60 * 5;
	stub_ms = 1000;
	ring_calls = 0;
	alarm_event(UI_EVENT_ALARM_EDIT, -1, 7, 6, 0);
	CHECK(alarm_count() == 1);
	CHECK(clockapps_next_ms(stub_ms) == 60000);
	stub_rtc += 30;
	stub_ms += 30000;
	CHECK(!clockapps_tick(stub_ms));
	stub_rtc += 30;
	stub_ms += 30000;
	CHECK(clockapps_tick(stub_ms) && clockapps_ringing());
	CHECK(ring_on && !ring_is_timer);
	CHECK(!alarm_get(0)->on);
	/* Snoozed: rings again 10 minutes later. */
	event(UI_EVENT_ALARM_RING, UI_ALARM_SNOOZE);
	CHECK(!clockapps_ringing());
	CHECK(clockapps_next_ms(stub_ms) == 600000);
	stub_rtc += 600;
	stub_ms += 600000;
	CHECK(clockapps_tick(stub_ms) && clockapps_ringing());
	/* Sleeping through it snoozes it too. */
	clockapps_sleep();
	CHECK(!clockapps_ringing());
	CHECK(clockapps_next_ms(stub_ms) == 600000);
	stub_rtc += 600;
	stub_ms += 600000;
	CHECK(clockapps_tick(stub_ms));
	event(UI_EVENT_ALARM_RING, UI_ALARM_DISMISS);
	CHECK(clockapps_next_ms(stub_ms) == UINT32_MAX);
	alarm_event(UI_EVENT_ALARM_DELETE, 0, 0, 0, 0);
	CHECK(alarm_count() == 0);
}

int main(void)
{
	test_timer();
	test_stopwatch();
	test_persistence();
	test_alarm();
	printf("clockapps_test: %s\n", stub_failures ? "FAILED" : "ok");
	return stub_failures != 0;
}
