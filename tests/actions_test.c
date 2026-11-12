/* What UI actions do to the settings and the clock, on the host, with a
 * plain array as the settings and a counter for each side effect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "asteroid.h"
#include "ui/ui.h"
#include "bt/bt.h"
#include "power.h"
#include "present.h"
#include "app.h"
#include "actions.h"

static int values[SETTINGS];
static int brightness_set, shown, requested_sleep, opened = -1, pairing = -1, bt_started, bt_stopped;
static uint32_t rtc;
static int failures;

int setting_get(enum setting s) { return values[s]; }
void setting_set(enum setting s, int v) { values[s] = v; }
void power_set_brightness(void) { brightness_set++; }
void power_request(bool sleep) { requested_sleep += sleep; }
void present_settings(void) { shown++; }
void app_open(int app) { opened = app; }
void bt_pairing_reply(bool accept) { pairing = accept; }
int bt_store_clear(void) { return 0; }
void bt_host_disconnect(void) {}
bool bt_host_connected(void) { return false; }
bool bt_store_bonded(void) { return false; }
void ui_set_connected(bool c, bool b) {}
int bt_addr_load(uint8_t *a) { return 0; }
int bt_start(const uint8_t *a, struct bt_info *i) { bt_started++; return 0; }
void bt_stop(void) { bt_stopped++; }
int bt_host_start(void) { return 0; }
void bt_host_stop(void) {}
void power_off_arm(void) {}
void reboot(void) { abort(); }
uint32_t rtc_read(void) { return rtc; }
void rtc_write(uint32_t t) { rtc = t; }
void hub_set_time(void) {}
void log_printf(const char *fmt, ...) {}

#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static void brightness(void)
{
	values[SETTING_BRIGHTNESS] = 50;
	action_setting_toggled(SETTING_BRIGHTNESS);
	CHECK(values[SETTING_BRIGHTNESS] == 75);
	values[SETTING_BRIGHTNESS] = 100;
	action_setting_toggled(SETTING_BRIGHTNESS);
	CHECK(values[SETTING_BRIGHTNESS] == 25);
	values[SETTING_BRIGHTNESS] = 95;
	action_setting_toggled(UI_BRIGHTNESS_UP);
	CHECK(values[SETTING_BRIGHTNESS] == 100);
	values[SETTING_BRIGHTNESS] = 15;
	action_setting_toggled(UI_BRIGHTNESS_DOWN);
	CHECK(values[SETTING_BRIGHTNESS] == 10);
	CHECK(brightness_set == 4 && shown == 4);
}

static void cinema(void)
{
	values[SETTING_AOD] = 1;
	values[SETTING_CINEMA_AOD] = 0;
	action_setting_toggled(UI_CINEMA);
	CHECK(values[SETTING_AOD] == 0 && values[SETTING_CINEMA_AOD] == 1);
	action_setting_toggled(UI_CINEMA);
	CHECK(values[SETTING_AOD] == 1);
}

static void others(void)
{
	values[SETTING_AUTO_BRIGHTNESS] = 1;
	brightness_set = 0;
	action_setting_toggled(SETTING_AUTO_BRIGHTNESS);
	CHECK(!values[SETTING_AUTO_BRIGHTNESS] && brightness_set == 1);
	action_setting_toggled(UI_WATCHFACE_FIRST + 1);
	CHECK(values[SETTING_WATCHFACE] == 1);
	action_setting_toggled(UI_LAUNCHER_FIRST + 1);
	CHECK(values[SETTING_LAUNCHER] == 1);
	action_setting_toggled(UI_LOCK);
	CHECK(requested_sleep == 1);
	action_setting_toggled(UI_OPEN_SETTINGS);
	CHECK(opened == UI_APP_SETTINGS);
	action_setting_toggled(UI_PAIR_ACCEPT);
	CHECK(pairing == 1);
	values[SETTING_BLUETOOTH] = 0;
	action_setting_toggled(SETTING_BLUETOOTH);
	CHECK(values[SETTING_BLUETOOTH] == 1 && bt_started == 1);
	action_setting_toggled(SETTING_BLUETOOTH);
	CHECK(values[SETTING_BLUETOOTH] == 0 && bt_stopped == 1);
	int b = brightness_set;
	action_setting_toggled(SETTING_NIGHTSTAND);
	CHECK(values[SETTING_NIGHTSTAND] == 1 && brightness_set == b + 1);
}

static void steps(void)
{
	action_setting_value(SETTING_NIGHTSTAND_BRIGHTNESS, 57);
	CHECK(values[SETTING_NIGHTSTAND_BRIGHTNESS] == 50);
	action_setting_value(SETTING_NIGHTSTAND_BRIGHTNESS, 5);
	CHECK(values[SETTING_NIGHTSTAND_BRIGHTNESS] == 50);
	action_setting_value(SETTING_NIGHTSTAND_DELAY, 17);
	CHECK(values[SETTING_NIGHTSTAND_DELAY] == 15);
	action_setting_value(SETTING_NIGHTSTAND_DELAY, 31);
	CHECK(values[SETTING_NIGHTSTAND_DELAY] == 15);
	action_setting_value(SETTING_STEP_GOAL, 20);
	CHECK(values[SETTING_STEP_GOAL] == 20);
	action_setting_value(SETTING_STEP_GOAL, 0);
	action_setting_value(SETTING_STEP_GOAL, STEP_GOAL_MAX + 1);
	CHECK(values[SETTING_STEP_GOAL] == 20);
	action_setting_value(SETTING_BURN_IN, 4);
	CHECK(values[SETTING_BURN_IN] == 0);
	action_setting_value(SETTING_BURN_IN, 3);
	CHECK(values[SETTING_BURN_IN] == 3);
}

static uint32_t date(uint32_t y, uint32_t m, uint32_t d) { return (y - 2000) << 9 | m << 5 | d; }

static void clock_set(void)
{
	struct civil c = { .year = 2024, .month = 1, .day = 31, .hour = 12, .minute = 30, .second = 15 };
	rtc = epoch_from_civil(&c);
	action_set_clock(UI_EVENT_DATE_SET, (int)date(2024, 2, 31));
	civil_from_epoch(rtc, &c);
	CHECK(c.month == 2 && c.day == 29 && c.hour == 12 && c.minute == 30);
	action_set_clock(UI_EVENT_DATE_SET, (int)date(2023, 2, 30));
	civil_from_epoch(rtc, &c);
	CHECK(c.year == 2023 && c.day == 28);
	uint32_t before = rtc;
	action_set_clock(UI_EVENT_DATE_SET, (int)date(2024, 13, 1));
	action_set_clock(UI_EVENT_TIME_SET, 24 * 60);
	CHECK(rtc == before);
	action_set_clock(UI_EVENT_TIME_SET, 7 * 60 + 5);
	civil_from_epoch(rtc, &c);
	CHECK(c.hour == 7 && c.minute == 5 && c.second == 0 && c.day == 28);
}

/* Every day the RTC can hold, against the host's gmtime, both ways. */
static void civil_days(void)
{
	for (uint64_t s = 43200; s <= UINT32_MAX; s += 86400) {
		time_t t = (time_t)s;
		struct tm tm;
		gmtime_r(&t, &tm);
		struct civil c;
		civil_from_epoch((uint32_t)s, &c);
		if (c.year != (uint32_t)tm.tm_year + 1900 || c.month != (uint32_t)tm.tm_mon + 1 ||
		    c.day != (uint32_t)tm.tm_mday || c.weekday != (uint32_t)tm.tm_wday || epoch_from_civil(&c) != s) {
			printf("civil: wrong at %llu s\n", (unsigned long long)s);
			failures++;
			return;
		}
	}
}

int main(void)
{
	brightness();
	cinema();
	others();
	steps();
	clock_set();
	civil_days();
	if (failures)
		return 1;
	puts("actions_test: ok");
	return 0;
}
