/* What the watch does between its drivers and the UI: UI actions, the
 * inputs that wake it, the apps that keep it awake and what follows a
 * sleep or wake. */
#include <string.h>
#include "asteroid.h"
#include "display/display.h"
#include "input/touch.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "ui/rotation.h"
#include "bt/bt.h"
#include "notify/notify.h"
#include "sensors/hub.h"
#include "sensors/steps.h"
#include "sensors/orient.h"
#include "media/media.h"
#include "weather/weather.h"
#include "alarm.h"
#include "clockapps.h"
#include "calc.h"
#include "game.h"
#include "power.h"
#include "frame.h"
#include "present.h"
#include "actions.h"
#include "app.h"

#define BT_START_MS 2000

static struct app_caps caps;
#define have_display caps.display
#define have_ui caps.ui
#define have_touch caps.touch
#define have_haptics caps.haptics
#define have_hub caps.hub

static bool steps_awake, was_lit, was_diamonds, key_was, wake_touch;
static uint32_t shown_bpm, alarm_t, bt_start_t, key_down_t, down_t;
static int down_x, down_y;
static struct orient orient;

void app_init(const struct app_caps *c)
{
	caps = *c;
	bt_start_t = time_ms() + BT_START_MS;
	orient_reset(&orient);
}

void app_open(int app)
{
	struct ui_colors c;
	frame_app_colors(ui_app_colors(app, &c) ? &c : NULL);
	/* The calculator starts over each time, as the app does. */
	if (app == UI_APP_CALCULATOR) {
		calc_reset();
		present_calc();
	}
	ui_set_app(app);
	stack_open_app();
}

static bool console_settings;

void settings_changed(void)
{
	console_settings = true;
}

/* Some time after boot, so the first frames are not held up. */
void app_start_poll(uint32_t now)
{
	if (bt_start_t && now - bt_start_t < 0x80000000u) {
		bt_start_t = 0;
		weather_load();
		if (have_hub)
			steps_init();
		if (setting_get(SETTING_BLUETOOTH))
			action_bluetooth_start();
		if (have_display)
			power_set_brightness();
		if (have_ui) {
			present_settings();
			ui_set_build(build_id);
			clockapps_show_alarms();
		}
	}
}

void app_sensors(uint32_t now)
{
	if (have_hub) {
		hub_poll();
		if (hub_tilted() && !power_awake() && setting_get(SETTING_TILT))
			power_request(false);
		if (steps_awake && !power_awake())
			steps_save();
		steps_awake = power_awake();
		/* The heart rate sensor runs while the Heart Rate app is on
		 * screen; until its second reading the screen stays on, as
		 * asteroid-hrm keeps the display from blanking until it has
		 * a previous rate. */
		bool measure = have_ui && power_awake() && ui_app_open() == UI_APP_HEART_RATE;
		hub_heart_rate_on(measure);
		unsigned readings;
		uint32_t bpm = hub_heart_rate(NULL, &readings);
		bpm = bpm == HUB_NONE ? 0 : bpm;
		if (bpm != shown_bpm)
			ui_set_heart_rate(shown_bpm = bpm);
		if (measure && readings < 2)
			power_activity(now);
		/* In nightstand mode the screen turns with the watch, as
		 * the launcher's OrientationSensor turns it, the sensor on
		 * while the screen is; the always-on face keeps the turn. */
		bool turn = have_ui && setting_get(SETTING_NIGHTSTAND_ROTATE) && power_nightstand();
		hub_accel_on(turn && power_awake());
		int16_t g[3];
		if (hub_accel(g))
			orient_update(&orient, g, now);
		if (!turn)
			orient_reset(&orient);
		rotation_set(orient.angle, now);
	}
}

void app_flashlight(uint32_t now)
{
	/* The Flashlight app keeps the screen on and, while its light
	 * is on, at full brightness, as asteroid-flashlight sets the
	 * maximum brightness and puts the setting back when it ends. */
	bool flashlight = have_ui && ui_app_open() == UI_APP_FLASHLIGHT;
	bool lit = flashlight && ui_flashlight_on();
	if (flashlight && power_awake())
		power_activity(now);
	if (lit != was_lit && power_awake()) {
		was_lit = lit;
		if (lit)
			display_set_brightness(255);
		else if (have_display)
			power_set_brightness();
	}
}

void app_touch(uint32_t now)
{
	struct touch t;
	while (have_touch && touch_poll(&t)) {
		if (t.event == TOUCH_PALM) {
			log_printf("touch: palm\n");
			if (power_awake())
				power_request(true);
			continue;
		}
		if (!power_awake()) {
			if (t.event == TOUCH_DOWN && setting_get(SETTING_TAP)) {
				power_request(false);
				wake_touch = true;
			} else if (t.event == TOUCH_UP) {
				wake_touch = false;
			}
			continue;
		}
		/* The touch that woke the watch is not a touch on the UI. */
		if (wake_touch) {
			wake_touch = t.event != TOUCH_UP;
			continue;
		}
		power_activity(now);
		if (have_ui)
			rotation_touch(&t.x, &t.y);
		if (have_ui)
			ui_set_touch_ms(t.ms);
		if (have_ui)
			stack_touch(t.event == TOUCH_DOWN ? UI_TOUCH_DOWN : t.event == TOUCH_UP ? UI_TOUCH_UP : UI_TOUCH_MOVE, t.x,
				    t.y, now);
		if (t.event == TOUCH_DOWN) {
			down_x = t.x;
			down_y = t.y;
			down_t = now;
		} else if (t.event == TOUCH_UP) {
			int dx = t.x - down_x, dy = t.y - down_y;
			const char *g = "tap";
			if (dx * dx + dy * dy > 40 * 40)
				g = (dx * dx > dy * dy) ? (dx > 0 ? "swipe right" : "swipe left") : (dy > 0 ? "swipe down" : "swipe up");
			log_printf("gesture: %s (%d,%d -> %d,%d in %lu ms)\n", g, down_x, down_y, t.x, t.y, now - down_t);
		}
		frame_touch_dot(t.down ? t.x : -1, t.y);
	}
}

/* Only a cable plugged in raises an interrupt, so its state is also
 * checked once a second. */
void app_cable(uint32_t now)
{
	static uint32_t usb_check_t;
	if (pmic_usb_irq() && pmic_usb_present()) {
		log_printf("usb: cable plugged in\n");
		usb_cable(true, true);
		power_charger_plugged(now);
	} else if (now - usb_check_t >= 1000) {
		usb_check_t = now;
		usb_cable(pmic_usb_present(), false);
	}
}

void app_key(uint32_t now)
{
	bool key = pmic_power_key();
	if (key != key_was) {
		key_was = key;
		if (key) {
			key_down_t = now;
		} else if (now - key_down_t >= 3000) {
			log_printf("rebooting on power key\n");
			reboot();
		} else {
			power_request(power_awake());
		}
	}
}

void app_tick(uint32_t now)
{
	if (now - alarm_t >= 1000) {
		alarm_t = now;
		notify_tick(pmic_rtc_read());
	}
	if (clockapps_tick(now) && !power_awake())
		power_request(false);
	if (clockapps_ringing())
		power_activity(now);
}

void app_ui_events(uint32_t now)
{
	for (struct ui_event e; have_ui && ui_next_event(&e);) {
		int a = e.value;
		power_activity(now);
		switch (e.kind) {
		case UI_EVENT_SETTING:
			action_setting_toggled(a);
			break;
		case UI_EVENT_APP:
			app_open(a);
			break;
		case UI_EVENT_MEDIA:
			media_command((unsigned)a);
			break;
		case UI_EVENT_DISMISS:
			notify_dismiss(a);
			break;
		case UI_EVENT_DISMISS_ALL:
			notify_dismiss_all();
			break;
		case UI_EVENT_TIME_SET:
		case UI_EVENT_DATE_SET:
			action_set_clock(e.kind, a);
			present_clock_soon();
			break;
		case UI_EVENT_SETTING_VALUE:
			action_setting_value(a, e.arg);
			break;
		case UI_EVENT_CALC:
			calc_key(a);
			present_calc();
			break;
		case UI_EVENT_GAME:
			game_move(a, now);
			break;
		case UI_EVENT_SNOOZE:
			notify_snooze(a, pmic_rtc_read() + (e.alarm.hour * 60 + e.alarm.minute) * 60);
			break;
		case UI_EVENT_BRIGHTNESS:
			if (a >= 10 && a <= 100) {
				setting_set(SETTING_BRIGHTNESS, a);
				power_set_brightness();
				present_settings();
			}
			break;
		default:
			clockapps_event(&e, now);
			break;
		}
	}
}

void app_after_events(uint32_t now)
{
	if (power_awake())
		clockapps_refresh(now);
	if (game_tick(now) && have_ui)
		present_game();
	/* The game's board is saved when its app closes or the watch
	 * sleeps, not on every move. */
	bool diamonds = have_ui && ui_app_open() == UI_APP_DIAMONDS;
	if (was_diamonds && !diamonds)
		game_save();
	was_diamonds = diamonds;
}

void app_console_settings(void)
{
	if (console_settings) {
		console_settings = false;
		if (have_display)
			power_set_brightness();
		if (have_ui)
			present_settings();
	}
}

void app_power_changed(enum power_change change)
{
	switch (change) {
	case POWER_SLEPT:
		game_save();
		if (have_touch)
			touch_sleep(true);
		frame_sleep();
		break;
	case POWER_WOKE:
		if (have_touch)
			touch_sleep(false);
		frame_wake();
		present_clock_soon();
		was_lit = false;
		break;
	case POWER_NONE:
		break;
	}
}
