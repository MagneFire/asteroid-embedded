/* What UI actions do to the settings and the clock, on the host, with a
 * plain array as the settings and a counter for each side effect. */
#include <setjmp.h>
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
#include "storage/storage.h"
#include "sched.h"
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
int bt_addr_get(uint8_t *a) { return 0; }
static bool bt_chip = true;
bool bt_present(void) { return bt_chip; }
static bool bt_on;
int bt_start(const uint8_t *a, struct bt_info *i)
{
	memset(i, 0, sizeof *i);
	bt_started++;
	bt_on = true;
	return 0;
}
void bt_stop(void)
{
	bt_stopped++;
	bt_on = false;
}
bool bt_running(void) { return bt_on; }
/* The one task the actions add, and when it is due, or 0. */
static struct sched_task *task;
static uint32_t task_at;
bool sched_add(struct sched_task *t)
{
	task = t;
	return true;
}
void sched_after(struct sched_task *t, uint32_t now, uint32_t ms) { task_at = now + ms; }
void sched_cancel(struct sched_task *t) { task_at = 0; }
int bt_host_start(void) { return 0; }
void bt_host_stop(void) {}
void power_off_arm(void) {}
/* Reset watch: what the wipe returns, and a reboot that comes back
 * here. */
static int wipe_result, wipes, flushes;
static bool reboot_ok;
static jmp_buf rebooted;
static const char *const *wipe_keep;
static unsigned wipe_keeps;
int kv_wipe(const char *const *keep, unsigned n)
{
	wipes++;
	wipe_keep = keep;
	wipe_keeps = n;
	return wipe_result;
}
void settings_flush(void) { flushes++; }
void reboot(void)
{
	if (!reboot_ok)
		abort();
	longjmp(rebooted, 1);
}
uint32_t rtc_read(void) { return rtc; }
void rtc_write(uint32_t t) { rtc = t; }
void hub_set_time(void) {}
void hands_set_time(void) {}
static int cal_on = -1, cal_hand = -1, cal_ccw = -1, cal_steps;
void app_hands_calibrate(bool on) { cal_on = on; }
void hands_calibrate(uint8_t hand, bool ccw, uint8_t steps)
{
	cal_hand = hand;
	cal_ccw = ccw;
	cal_steps += steps;
}
void hub_screen(bool on) {}
void power_timepiece(void) { abort(); }
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
	/* Without a chip the setting still changes, nothing is started. */
	bt_chip = false;
	action_setting_toggled(SETTING_BLUETOOTH);
	CHECK(values[SETTING_BLUETOOTH] == 1 && bt_started == 1);
	action_setting_toggled(SETTING_BLUETOOTH);
	bt_chip = true;
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
	action_setting_value(SETTING_TILT, TILT_LIGHT);
	CHECK(values[SETTING_TILT] == TILT_LIGHT);
	action_setting_value(SETTING_TILT, TILT_LIGHT + 1);
	CHECK(values[SETTING_TILT] == TILT_LIGHT);
	action_setting_value(SETTING_NIGHTSTAND_DELAY, 17);
	CHECK(values[SETTING_NIGHTSTAND_DELAY] == 15);
	action_setting_value(SETTING_NIGHTSTAND_DELAY, 31);
	CHECK(values[SETTING_NIGHTSTAND_DELAY] == 15);
	action_setting_value(SETTING_STEP_GOAL, 20);
	CHECK(values[SETTING_STEP_GOAL] == 20);
	action_setting_value(SETTING_HEIGHT, 181);
	action_setting_value(SETTING_HEIGHT, 99);
	CHECK(values[SETTING_HEIGHT] == 181);
	action_setting_value(SETTING_WEIGHT, 82);
	action_setting_value(SETTING_WEIGHT, 201);
	CHECK(values[SETTING_WEIGHT] == 82);
	action_setting_value(SETTING_HR_INTERVAL, 30);
	action_setting_value(SETTING_HR_INTERVAL, 20);
	CHECK(values[SETTING_HR_INTERVAL] == 30);
	action_setting_value(SETTING_HR_INTERVAL, 5);
	CHECK(values[SETTING_HR_INTERVAL] == 5);
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

static bool wipe_keeps_key(const char *key)
{
	for (unsigned i = 0; i < wipe_keeps; i++)
		if (!strcmp(wipe_keep[i], key))
			return true;
	return false;
}

/* The reboot comes once the wiped records are the ones in use, even if
 * their second copy failed. */
static void reset_watch(void)
{
	static const int results[] = { 0, 1, -4 };
	reboot_ok = true;
	for (unsigned i = 0; i < 3; i++) {
		wipe_result = results[i];
		wipes = flushes = 0;
		volatile bool back = false;
		if (!setjmp(rebooted))
			CHECK(action_reset_watch() == -4 && i == 2);
		else
			back = true;
		CHECK(back == (i < 2) && wipes == 1 && flushes == 1);
	}
	reboot_ok = false;
	/* What is not the wearer's stays: the watch's own values, the
	 * history, and the mode and the rest of what the AP set. */
	static const char *const stay[] = { "bt.addr", "haptics.cal", "events", "ap.mode", "ap.zone", "ap.take", "ap.fade" };
	for (unsigned i = 0; i < sizeof stay / sizeof stay[0]; i++)
		CHECK(wipe_keeps_key(stay[i]));
	CHECK(!wipe_keeps_key("bt.bonds") && !wipe_keeps_key("steps") && !wipe_keeps_key("settings"));
}

/* The radio is off while the AP has the panel and back a while after
 * the watch takes it, if the setting has it on. */
static void lend(void)
{
	values[SETTING_BLUETOOTH] = 1;
	bt_started = bt_stopped = 0;
	bt_on = true;
	action_bluetooth_lend(true, 1000);
	CHECK(!bt_on && bt_stopped == 1 && !task_at);
	action_bluetooth_start();
	CHECK(!bt_on && !bt_started);
	action_bluetooth_lend(false, 2000);
	CHECK(!bt_on && task && task_at == 4000);
	/* Lent again before the start: no start. */
	action_bluetooth_lend(true, 3000);
	CHECK(!task_at && bt_stopped == 1);
	action_bluetooth_lend(false, 5000);
	CHECK(task_at == 7000);
	task->run(7000);
	CHECK(bt_on && bt_started == 1);
	/* The setting off: nothing starts it when the watch takes the
	 * panel back. */
	action_bluetooth_lend(true, 8000);
	values[SETTING_BLUETOOTH] = 0;
	action_bluetooth_lend(false, 9000);
	CHECK(!bt_on && !task_at);
	/* Switched on while lent: it waits for the panel. */
	action_bluetooth_lend(true, 10000);
	action_setting_toggled(SETTING_BLUETOOTH);
	CHECK(values[SETTING_BLUETOOTH] == 1 && !bt_on && bt_started == 1);
	action_bluetooth_lend(false, 11000);
	CHECK(task_at == 13000);
	task->run(13000);
	CHECK(bt_on && bt_started == 2);
	action_bluetooth_lend(false, 14000);
	CHECK(task_at == 13000);
}

/* Each arrow turns its own hand one step its own way. */
static void hands_calibration(void)
{
	action_setting_toggled(UI_HANDS_OPEN);
	CHECK(cal_on == 1);
	const struct { int code, hand, ccw; } turns[] = {
		{ UI_HANDS_MINUTE_CW, 0, 0 }, { UI_HANDS_MINUTE_CCW, 0, 1 },
		{ UI_HANDS_HOUR_CW, 1, 0 }, { UI_HANDS_HOUR_CCW, 1, 1 },
	};
	for (unsigned i = 0; i < 4; i++) {
		action_setting_toggled(turns[i].code);
		CHECK(cal_hand == turns[i].hand && cal_ccw == turns[i].ccw && cal_steps == (int)i + 1);
	}
	action_setting_toggled(UI_HANDS_CLOSE);
	CHECK(cal_on == 0);
}

int main(void)
{
	reset_watch();
	brightness();
	cinema();
	others();
	steps();
	clock_set();
	civil_days();
	lend();
	hands_calibration();
	if (failures)
		return 1;
	puts("actions_test: ok");
	return 0;
}
