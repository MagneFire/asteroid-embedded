/* What a UI action does to the settings, the clock and Bluetooth. */
#include "asteroid.h"
#include "ui/ui.h"
#include "bt/bt.h"
#include "sensors/hub.h"
#include "power.h"
#include "present.h"
#include "app.h"
#include "storage/storage.h"
#include "sched.h"
#include "actions.h"

/* Starting the radio holds up the main loop for a while, and the face
 * comes first. */
#define BT_TAKE_MS	2000

static bool bt_lent;
static struct sched_task bt_task;

static bool bt_run(uint32_t now)
{
	action_bluetooth_start();
	return false;
}

void action_bluetooth_start(void)
{
	if (bt_lent)
		return;
	uint8_t addr[6];
	struct bt_info info;
	if (!bt_present())
		return;
	bool have_addr = !bt_addr_get(addr);
	if (bt_start(have_addr ? addr : NULL, &info)) {
		log_printf("bt: %s failed (%d)\n", info.failed, info.status);
		return;
	}
	if (info.host_later)
		return;
	int r = bt_host_start();
	if (r)
		log_printf("bt: host failed (%d)\n", r);
}

void action_bluetooth_lend(bool to_ap, uint32_t now)
{
	bt_lent = to_ap;
	if (to_ap) {
		sched_cancel(&bt_task);
		if (bt_running()) {
			bt_host_stop();
			bt_stop();
			log_printf("bt: off while the AP has the panel\n");
		}
		return;
	}
	if (!setting_get(SETTING_BLUETOOTH) || bt_running())
		return;
	if (!bt_task.run) {
		bt_task.name = "bt";
		bt_task.run = bt_run;
		sched_add(&bt_task);
	}
	sched_after(&bt_task, now, BT_TAKE_MS);
}

#ifdef SOC_RESET_WATCH
/* The watch's own values, its boot history, and what the AP set: with
 * the AP off nothing sets the mode again. */
static const char *const reset_keeps[] = { "bt.addr", "haptics.cal", "rtc_offset", "events",
					   "ap.mode", "ap.zone", "ap.take", "ap.fade" };

/* Everything else in the store goes, and the reboot starts every service
 * from what a first boot finds. Returns only if the store could not be
 * wiped. */
int action_reset_watch(void)
{
	/* reboot() writes changed settings back; none may be left over. */
	settings_flush();
	int r = kv_wipe(reset_keeps, ARRAY_SIZE(reset_keeps));
	if (r < 0) {
		log_printf("reset: store not wiped (%d)\n", r);
		return r;
	}
	log_printf("reset: store wiped%s, rebooting\n", r ? ", its second copy not" : "");
	reboot();
}
#endif

/* A setting changed from the UI: store it and put it into effect. */
void action_setting_toggled(int s)
{
	if (s == SETTING_BRIGHTNESS || s == UI_BRIGHTNESS_DOWN || s == UI_BRIGHTNESS_UP) {
		int b = setting_get(SETTING_BRIGHTNESS);
		if (s == SETTING_BRIGHTNESS)
			b = b >= 100 ? 25 : (b / 25 + 1) * 25;
		else
			b = s == UI_BRIGHTNESS_UP ? (b + 10 > 100 ? 100 : b + 10) : (b - 10 < 10 ? 10 : b - 10);
		setting_set(SETTING_BRIGHTNESS, b);
		power_set_brightness();
	} else if (s >= UI_WATCHFACE_FIRST && s < UI_WATCHFACE_FIRST + 2) {
		setting_set(SETTING_WATCHFACE, s - UI_WATCHFACE_FIRST);
	} else if (s >= UI_LAUNCHER_FIRST && s < UI_LAUNCHER_FIRST + UI_LAUNCHERS) {
		setting_set(SETTING_LAUNCHER, s - UI_LAUNCHER_FIRST);
	} else if (s == UI_LOCK) {
		power_request(true);
	} else if (s == UI_OPEN_SETTINGS) {
		app_open(UI_APP_SETTINGS);
	} else if (s == UI_CINEMA) {
		/* Cinema mode is the always-on face off, the state before it
		 * kept to go back to, as the quick panel's cinemaToggle does. */
		if (setting_get(SETTING_AOD)) {
			setting_set(SETTING_CINEMA_AOD, 1);
			setting_set(SETTING_AOD, 0);
		} else {
			setting_set(SETTING_AOD, setting_get(SETTING_CINEMA_AOD));
		}
	} else if (s == UI_TIMEPIECE) {
		log_printf("timepiece\n");
		power_timepiece();
	} else if (s == UI_REBOOT || s == UI_POWER_OFF) {
		log_printf("%s\n", s == UI_REBOOT ? "rebooting" : "powering off");
		if (s == UI_POWER_OFF) {
			hub_screen(false);
			power_off_arm();
		}
		reboot();
	} else if (s == UI_PAIR_REJECT || s == UI_PAIR_ACCEPT) {
		bt_pairing_reply(s == UI_PAIR_ACCEPT);
	} else if (s == UI_FORGET_PHONE) {
		log_printf("bt: %s\n", bt_store_clear() ? "could not forget the phone" : "phone forgotten");
		bt_host_disconnect();
		ui_set_connected(bt_host_connected(), bt_store_bonded());
#ifdef SOC_RESET_WATCH
	} else if (s == UI_RESET_WATCH) {
		action_reset_watch();
#endif
	} else if (s == SETTING_BLUETOOTH) {
		bool on = !setting_get(SETTING_BLUETOOTH);
		setting_set(SETTING_BLUETOOTH, on);
		if (on) {
			action_bluetooth_start();
		} else {
			bt_host_stop();
			bt_stop();
		}
	} else if (s == SETTING_NIGHTSTAND || s == SETTING_AUTO_BRIGHTNESS) {
		setting_set((enum setting)s, !setting_get((enum setting)s));
		power_set_brightness();
	} else if (s >= 0 && s < SETTINGS) {
		setting_set((enum setting)s, !setting_get((enum setting)s));
	}
	present_settings();
}

/* The clock set by hand: the time of day in minutes, keeping the date,
 * or a date, keeping the time. */
void action_set_clock(int kind, int v)
{
	struct civil c;
	civil_from_epoch(rtc_read(), &c);
	if (kind == UI_EVENT_TIME_SET) {
		if (v < 0 || v >= 24 * 60)
			return;
		c.hour = (uint32_t)v / 60;
		c.minute = (uint32_t)v % 60;
		c.second = 0;
	} else {
		uint32_t day = v & 31, month = v >> 5 & 15, year = 2000 + (v >> 9 & 127);
		bool leap = year % 4 == 0 && (year % 100 || year % 400 == 0);
		uint32_t days = month == 2 ? 28u + leap : 30 + ((month + month / 8) & 1);
		if (v < 0 || month < 1 || month > 12 || day < 1 || year > 2099)
			return;
		c.day = day > days ? days : day;
		c.month = month;
		c.year = year;
	}
	rtc_write(epoch_from_civil(&c));
	hub_set_time();
	log_printf("clock set to %04lu-%02lu-%02lu %02lu:%02lu\n", c.year, c.month, c.day, c.hour, c.minute);
}

/* A setting set to a value from the UI, kept to the steps it offers. */
void action_setting_value(int s, int v)
{
	if (s == SETTING_NIGHTSTAND_BRIGHTNESS && v >= 10 && v <= 100) {
		setting_set(SETTING_NIGHTSTAND_BRIGHTNESS, v / 10 * 10);
		power_set_brightness();
	} else if (s == SETTING_NIGHTSTAND_DELAY && v >= 0 && v <= 30) {
		setting_set(SETTING_NIGHTSTAND_DELAY, v / 5 * 5);
	} else if (s == SETTING_BURN_IN && v >= 0 && v <= 3) {
		setting_set(SETTING_BURN_IN, v);
	} else if (s == SETTING_TILT && v >= TILT_OFF && v <= TILT_LIGHT) {
		setting_set(SETTING_TILT, v);
	} else if (s == SETTING_BACKLIGHT_COLOR && v >= BACKLIGHT_AUTO && v <= BACKLIGHT_RED) {
		setting_set(SETTING_BACKLIGHT_COLOR, v);
	} else if (s == SETTING_STEP_GOAL && v >= 1 && v <= STEP_GOAL_MAX) {
		setting_set(SETTING_STEP_GOAL, v);
	} else if (s == SETTING_HEIGHT && v >= 100 && v <= 230) {
		setting_set(SETTING_HEIGHT, v);
	} else if (s == SETTING_WEIGHT && v >= 30 && v <= 200) {
		setting_set(SETTING_WEIGHT, v);
	} else if (s == SETTING_HR_INTERVAL && (v == 0 || v == 5 || v == 10 || v == 30 || v == 60)) {
		setting_set(SETTING_HR_INTERVAL, v);
	} else if (s == SETTING_AGE && v >= 10 && v <= 99) {
		setting_set(SETTING_AGE, v);
	}
	present_settings();
}
