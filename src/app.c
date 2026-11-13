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
#include "storage/emmc.h"
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
#include "agenda.h"
#include "workout.h"
#include "hrlog.h"
#include "power.h"
#include "frame.h"
#include "present.h"
#include "sched.h"
#include "stats.h"
#include "actions.h"
#include "app.h"

#define BT_START_MS 2000

static const char wake_by_touch[] = "touch";

static struct app_caps caps;
#define have_display caps.display
#define have_ui caps.ui
#define have_touch caps.touch
#define have_haptics caps.haptics
#define have_hub caps.hub
#define have_heart_rate caps.heart_rate

static bool was_logging, steps_awake, was_lit, was_diamonds, key_was, wake_touch, down_seen;
static uint32_t shown_bpm, key_down_t, down_t;
static unsigned seen_readings;
static int down_x, down_y, last_x, last_y;
static struct orient orient;

static void tasks_init(uint32_t now);

void app_init(const struct app_caps *c)
{
	caps = *c;
	orient_reset(&orient);
	tasks_init(time_ms());
	/* The first frame already shows the chosen face. */
	if (have_ui)
		present_settings();
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
	/* It opens on today, as the app does. */
	if (app == UI_APP_AGENDA) {
		struct civil c;
		civil_from_epoch(rtc_read(), &c);
		present_agenda_day((c.year - 2000) << 9 | c.month << 5 | c.day);
	}
	if (app == UI_APP_ACTIVITY) {
		if (have_hub)
			steps_poll();
		present_activity();
		present_workout();
		present_heart_log();
	}
	ui_set_app(app);
	stack_open_app();
}

/* An event edited in the Agenda app keeps its title, as the watch cannot
 * type one. */
static void agenda_edited(const struct ui_event *e)
{
	if (e->kind == UI_EVENT_AGENDA_DELETE) {
		if (e->value >= 0)
			agenda_delete((unsigned)e->value);
	} else {
		uint32_t i = (uint32_t)e->value & 0xff, date = (uint32_t)e->value >> 8, t = (uint32_t)e->arg;
		struct agenda_event v = { (uint16_t)(2000 + (date >> 9)), (uint8_t)(date >> 5 & 15), (uint8_t)(date & 31),
					  (uint16_t)(t >> 16), (uint16_t)t, "" };
		const struct agenda_event *old = agenda_get(i);
		if (old)
			memcpy(v.title, old->title, AGENDA_TITLE);
		if (agenda_set(old ? i : AGENDA_EVENTS, &v) < 0)
			log_printf("agenda: not added, full or not a date\n");
	}
	present_agenda();
}

static bool console_settings;

void settings_changed(void)
{
	console_settings = true;
}

/* Some time after boot, so the first frames are not held up. */
static bool start_run(uint32_t now)
{
	weather_load();
	if (have_hub)
		steps_init();
	workout_init();
	hrlog_init();
	if (setting_get(SETTING_BLUETOOTH))
		action_bluetooth_start();
	if (have_display)
		power_set_brightness();
	if (have_ui) {
		present_settings();
		ui_set_build(build_id);
		clockapps_show_alarms();
		present_agenda();
	}
	return false;
}

/* Work that only has to happen now and then, lazily so it adds no
 * wakeups: asleep it runs when something else wakes the watch. The
 * charger keeps its own 1 s, so it is asked more often than that. */
static struct sched_task start_task, charger_task, notify_task, cable_task, settings_task, weather_task, steps_task;

static bool charger_run(uint32_t now)
{
	batt_poll();
	sched_after(&charger_task, now, 500);
	return false;
}

static bool notify_run(uint32_t now)
{
	notify_tick(rtc_read());
	sched_after(&notify_task, now, 1000);
	return false;
}

/* Only a cable plugged in raises an interrupt. */
static bool cable_run(uint32_t now)
{
	usb_cable(batt_usb(), false);
	sched_after(&cable_task, now, 1000);
	return false;
}

static bool settings_run(uint32_t now)
{
	settings_poll(now);
	sched_after(&settings_task, now, 1000);
	return false;
}

static bool weather_run(uint32_t now)
{
	present_weather();
	sched_after(&weather_task, now, 1000);
	return false;
}

/* Not lazy: a day that ends while the watch sleeps gets its own steps,
 * not the next morning's. */
static bool steps_run(uint32_t now)
{
	if (have_hub && steps_poll() && have_ui)
		present_activity();
	sched_after(&steps_task, now, steps_next_ms());
	return false;
}

static void tasks_init(uint32_t now)
{
	static const struct { struct sched_task *t; const char *name; bool (*run)(uint32_t); } list[] = {
		{ &charger_task, "charger", charger_run }, { &notify_task, "notify", notify_run },
		{ &cable_task, "cable", cable_run }, { &settings_task, "settings", settings_run },
		{ &weather_task, "weather", weather_run },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(list); i++) {
		list[i].t->name = list[i].name;
		list[i].t->run = list[i].run;
		list[i].t->lazy = true;
		sched_add(list[i].t);
		sched_at(list[i].t, now);
	}
	start_task.name = "start";
	start_task.run = start_run;
	sched_add(&start_task);
	sched_after(&start_task, now, BT_START_MS);
	steps_task.name = "steps";
	steps_task.run = steps_run;
	sched_add(&steps_task);
	sched_after(&steps_task, now, steps_next_ms());
}

/* The hub's tilt wake is on only while the screen sleeps. */
static void app_tilt(void)
{
	static bool tilt_on = true;
	bool want_tilt = !power_awake();
	if (want_tilt != tilt_on && hub_tilt_on(want_tilt))
		tilt_on = want_tilt;
}

void app_sensors(uint32_t now)
{
	if (have_hub) {
		hub_poll();
		app_tilt();
		if (hub_tilted() && !power_awake()) {
			int tilt = setting_get(SETTING_TILT);
			if (tilt == TILT_WAKE)
				power_wake("tilt");
			else if (tilt == TILT_FACE)
				power_peek("tilt");
		}
		if (steps_awake && !power_awake())
			steps_save();
		steps_awake = power_awake();
		/* The heart rate sensor runs while the Heart Rate app is on
		 * screen; until its second reading the screen stays on, as
		 * asteroid-hrm keeps the display from blanking until it has
		 * a previous rate. */
		bool measure = have_ui && power_awake() && ui_app_open() == UI_APP_HEART_RATE;
		unsigned readings;
		uint32_t bpm = hub_heart_rate(NULL, &readings);
		bpm = bpm == HUB_NONE ? 0 : bpm;
		/* The log keeps the second reading, as the app above waits
		 * for it too. */
		bool logging = have_heart_rate &&
			       hrlog_poll(rtc_read(), setting_get(SETTING_HR_INTERVAL), readings >= 2 ? bpm : 0);
		if (was_logging && !logging && have_ui)
			present_heart_log();
		was_logging = logging;
		hub_heart_rate_on(measure || logging || (have_heart_rate && workout_running()));
		if (readings > seen_readings && bpm)
			workout_heart_rate(bpm);
		seen_readings = readings;
		if (bpm != shown_bpm) {
			ui_set_heart_rate(shown_bpm = bpm);
			if (workout_running())
				present_workout();
		}
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
	power_light(now, lit);
}

void app_touch(uint32_t now)
{
	struct touch t;
	bool palm = false;
	while (have_touch && touch_poll(&t)) {
		if (t.event == TOUCH_PALM) {
			log_printf("touch: palm\n");
			if (power_awake()) {
				power_request(true);
			} else {
				palm = true;
				wake_touch = false;
				power_wake_cancel(wake_by_touch);
			}
			continue;
		}
		enum touch_use use = power_policy_touch(&wake_touch, power_awake(), setting_get(SETTING_TAP), t.event == TOUCH_DOWN,
							t.event == TOUCH_UP);
		if (use == TOUCH_WAKE) {
			if (palm)
				wake_touch = false;
			else
				power_wake(wake_by_touch);
		}
		if (use != TOUCH_USE)
			continue;
		power_activity(now);
		/* A move to where the finger already is keeps the UI from
		 * taking the touch as a tap. */
		if (t.event == TOUCH_MOVE && t.x == last_x && t.y == last_y)
			continue;
		last_x = t.x;
		last_y = t.y;
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
			down_seen = true;
		} else if (t.event == TOUCH_UP && down_seen) {
			down_seen = false;
			int dx = t.x - down_x, dy = t.y - down_y;
			const char *g = "tap";
			if (dx * dx + dy * dy > 40 * 40)
				g = (dx * dx > dy * dy) ? (dx > 0 ? "swipe right" : "swipe left") : (dy > 0 ? "swipe down" : "swipe up");
			log_printf("gesture: %s (%d,%d -> %d,%d in %lu ms)\n", g, down_x, down_y, t.x, t.y, now - down_t);
		}
		frame_touch_dot(t.down ? t.x : -1, t.y);
	}
}

void app_cable(uint32_t now)
{
	if (batt_cable_irq() && batt_cable()) {
		log_printf("usb: cable plugged in\n");
		usb_cable(batt_usb(), true);
		power_charger_plugged(now);
	}
}

/* A pass on the sleep clock can take longer than a short press, so a
 * press may only show as its interrupt. The interrupt of a release that
 * was already seen comes just after it. */
#define KEY_RELEASE_IRQ_MS	200

static void key_pressed(void)
{
	if (power_awake())
		power_request(true);
	else
		power_wake("key");
}

void app_key(uint32_t now, bool irq)
{
	static uint32_t key_up_t;
	bool key = power_key();
	if (key != key_was) {
		key_was = key;
		if (key) {
			key_down_t = now;
		} else {
			key_up_t = now;
			if (now - key_down_t >= 3000) {
				log_printf("rebooting on power key\n");
				reboot();
			}
			key_pressed();
		}
	} else if (irq && !key && now - key_up_t >= KEY_RELEASE_IRQ_MS) {
		key_up_t = now;
		key_pressed();
	}
}

void app_tick(uint32_t now)
{
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
		case UI_EVENT_AGENDA_DAY:
			present_agenda_day((uint32_t)a);
			break;
		case UI_EVENT_AGENDA_EDIT:
		case UI_EVENT_AGENDA_DELETE:
			agenda_edited(&e);
			break;
		case UI_EVENT_WORKOUT:
			if (have_hub)
				steps_poll();
			if (a)
				workout_start(rtc_read());
			else
				workout_stop(rtc_read());
			present_workout();
			break;
		case UI_EVENT_SNOOZE:
			notify_snooze(a, rtc_read() + (e.alarm.hour * 60 + e.alarm.minute) * 60);
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
		/* Before this pass idles: the watch can go straight into a
		 * long cluster sleep, with no pass to turn it on until then. */
		if (have_hub)
			app_tilt();
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

/* The time asleep until something here has work to do. The battery
 * monitor is left out on purpose: asleep its update follows the stats
 * line, as its own period would triple the wakeups. */
uint32_t app_next_ms(void)
{
	uint32_t next[] = {
		stats_next_ms(),
		clockapps_next_ms(time_ms()),
		have_touch ? touch_next_ms() : UINT32_MAX,
		usb_next_ms(),
		emmc_next_ms(),
		batt_next_ms(),
		power_next_ms(),
		sched_next_ms(time_ms()),
		have_heart_rate ? hrlog_next_ms(rtc_read(), setting_get(SETTING_HR_INTERVAL)) : UINT32_MAX,
	}, min = UINT32_MAX;
	for (unsigned i = 0; i < ARRAY_SIZE(next); i++)
		if (next[i] < min)
			min = next[i];
	return min;
}
