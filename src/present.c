/* What the UI shows of the rest of the firmware: each of these pushes
 * state that changed to the UI. */
#include <string.h>
#include "asteroid.h"
#include "ui/ui.h"
#include "ui/stack.h"
#include "bt/bt.h"
#include "notify/notify.h"
#include "media/media.h"
#include "weather/weather.h"
#include "sensors/hub.h"
#include "sensors/steps.h"
#include "calc.h"
#include "game.h"
#include "agenda.h"
#include "workout.h"
#include "hrlog.h"
#include "power.h"
#include "present.h"

static bool have_ui, have_haptics, have_hub;
static int32_t passkey = -1;
static uint32_t agenda_seen, agenda_date, agenda_today, notify_seen, shown_seq, newest_seq, media_seen, weather_seen, weather_day, last_clock;

void present_init(bool ui, bool haptics, bool hub)
{
	have_ui = ui;
	have_haptics = haptics;
	have_hub = hub;
}

void present_calc(void)
{
	ui_set_calc(calc_shown(), strlen(calc_shown()), calc_length());
}

void present_game(void)
{
	ui_set_game(game_tiles(), game_score(), game_best(), game_over());
}

static unsigned agenda_rows(struct ui_agenda *out, uint32_t date)
{
	unsigned first, n = agenda_day(2000 + (date >> 9), date >> 5 & 15, date & 31, &first);
	for (unsigned i = 0; i < n; i++) {
		const struct agenda_event *e = agenda_get(first + i);
		out[i] = (struct ui_agenda){ (int16_t)(first + i), e->start, e->end, "" };
		memcpy(out[i].title, e->title, UI_AGENDA_TITLE);
	}
	return n;
}

static uint32_t today_date(void)
{
	struct civil c;
	civil_from_epoch(rtc_read(), &c);
	return (c.year - 2000) << 9 | c.month << 5 | c.day;
}

/* After a change: the events of the day the app shows and today's. */
void present_agenda(void)
{
	_Static_assert(AGENDA_TITLE == UI_AGENDA_TITLE, "agenda title");
	struct ui_agenda rows[AGENDA_EVENTS];
	agenda_seen = agenda_serial();
	if (!have_ui)
		return;
	ui_set_agenda(rows, agenda_rows(rows, agenda_date));
	agenda_today = today_date();
	ui_set_today(rows, agenda_rows(rows, agenda_today));
}

/* The day as (year - 2000) << 9 | month << 5 | day. */
void present_agenda_day(uint32_t date)
{
	agenda_date = date;
	present_agenda();
}

void present_activity(void)
{
	uint32_t days[7];
	for (unsigned i = 0; i < ARRAY_SIZE(days); i++)
		days[i] = have_hub ? steps_day(ARRAY_SIZE(days) - 1 - i) : 0;
	struct activity today = have_hub ? steps_activity_today() : (struct activity){ 0 };
	ui_set_activity(days[6], setting_get(SETTING_STEP_GOAL) * STEP_GOAL_UNIT, today.distance_m, today.kcal, days,
			ARRAY_SIZE(days));
}

void present_workout(void)
{
	struct workout none = { 0 }, now;
	bool running = workout_running();
	if (running)
		now = workout_now(rtc_read());
	const struct workout *w = running ? &now : workout_past(0) ? workout_past(0) : &none;
	ui_set_workout(running, w->start, w->seconds, w->steps, w->distance_m, w->kcal, w->avg_bpm, w->max_bpm);
	hub_workout(running, w->seconds, w->distance_m, w->kcal);
}

void present_heart_log(void)
{
	static uint32_t t[HRLOG_SAMPLES];
	static uint16_t x[HRLOG_SAMPLES];
	static uint8_t bpm[HRLOG_SAMPLES];
	uint32_t rtc = rtc_read();
	unsigned n = hrlog_get(rtc > 86400 ? rtc - 86400 : 0, t, bpm, HRLOG_SAMPLES);
	for (unsigned i = 0; i < n; i++)
		x[i] = t[i] > rtc ? 1000 : (uint16_t)(1000 - (rtc - t[i]) * 1000 / 86400);
	ui_set_heart_log(x, bpm, n);
}

void present_settings(void)
{
	int tilt = setting_get(SETTING_TILT);
	struct ui_settings u = {
		.brightness = setting_get(SETTING_BRIGHTNESS),
		.nightstand_brightness = setting_get(SETTING_NIGHTSTAND_BRIGHTNESS),
		.nightstand_delay = setting_get(SETTING_NIGHTSTAND_DELAY),
		.burn_in = setting_get(SETTING_BURN_IN),
		.bluetooth = setting_get(SETTING_BLUETOOTH),
		.vibration = setting_get(SETTING_VIBRATION),
		.aod = setting_get(SETTING_AOD),
		.clock12 = setting_get(SETTING_CLOCK12),
		.tilt = tilt == TILT_LIGHT && !hub_has_screen() ? TILT_FACE : tilt,
		.tap = setting_get(SETTING_TAP),
		.fahrenheit = setting_get(SETTING_FAHRENHEIT),
		.nightstand_aod = setting_get(SETTING_NIGHTSTAND_AOD),
		.nightstand_rotate = setting_get(SETTING_NIGHTSTAND_ROTATE),
		.auto_brightness = setting_get(SETTING_AUTO_BRIGHTNESS),
		.height_cm = setting_get(SETTING_HEIGHT),
		.weight_kg = setting_get(SETTING_WEIGHT),
		.hr_interval = setting_get(SETTING_HR_INTERVAL),
		.age = setting_get(SETTING_AGE),
		.male = setting_get(SETTING_MALE),
	};
	ui_set_settings(&u);
	present_activity();
	ui_set_launcher(setting_get(SETTING_LAUNCHER));
	stack_set_launcher(setting_get(SETTING_LAUNCHER));
	ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), power_nightstand());
}

static void vibrate(const char *how)
{
	static const uint16_t normal[] = { 200 }, strong[] = { 250, 150, 250 },
			      ringtone[] = { 600, 400, 600, 400, 600, 400, 600 };
	if (!strcmp(how, "none"))
		return;
	if (!strcmp(how, "normal"))
		haptics_play(normal, ARRAY_SIZE(normal));
	else if (!strcmp(how, "ringtone"))
		haptics_play(ringtone, ARRAY_SIZE(ringtone));
	else
		haptics_play(strong, ARRAY_SIZE(strong));
}

static void show_weather(void)
{
	const struct weather *w = weather_get();
	int32_t ids[WEATHER_DAYS], lo[WEATHER_DAYS], hi[WEATHER_DAYS];
	uint32_t today = rtc_read() / 86400, day0 = w->time / 86400;
	unsigned n = 0;
	for (uint32_t i = today >= day0 ? today - day0 : WEATHER_DAYS; i < w->days; i++, n++) {
		ids[n] = w->id[i];
		lo[n] = (int32_t)w->min[i] - 273;
		hi[n] = (int32_t)w->max[i] - 273;
	}
	ui_set_weather(w->city, strlen(w->city), n, ids, lo, hi);
}

/* A pairing code, a new notification or new media details. */
void present_events(uint32_t now)
{
	if (bt_passkey() != passkey) {
		passkey = bt_passkey();
		if (have_ui)
			ui_set_passkey(passkey);
		if (passkey >= 0 && !power_awake())
			power_request(false);
		power_activity(now);
	}
	if (notify_serial() != notify_seen) {
		notify_seen = notify_serial();
		unsigned n = notify_count();
		if (have_ui)
			stack_pending(n > 0);
		const struct notification *top = n ? notify_get(n - 1) : NULL;
		if ((top ? top->seq : 0) != shown_seq && have_haptics)
			haptics_play(NULL, 0);
		if (have_ui) {
			ui_clear_notifications();
			for (unsigned i = n; i--;) {
				const struct notification *e = notify_get(i);
				ui_add_notification(e->id, e->app, strlen(e->app), e->summary, strlen(e->summary), e->body,
						    strlen(e->body), e->icon, strlen(e->icon), e->time);
			}
		}
		if (top && top->seq > newest_seq) {
			newest_seq = top->seq;
			if (have_ui)
				ui_show_notification(top->id);
			if (have_haptics && setting_get(SETTING_VIBRATION))
				vibrate(top->vibrate);
			if (!power_awake())
				power_request(false);
			power_activity(now);
		}
		shown_seq = top ? top->seq : 0;
	}
	if (agenda_serial() != agenda_seen)
		present_agenda();
	if (have_ui && media_serial() != media_seen) {
		media_seen = media_serial();
		const struct media *m = media_get();
		ui_set_media(m->title, strlen(m->title), m->artist, strlen(m->artist), m->album, strlen(m->album),
			     m->playing, m->volume);
	}
}

void present_weather(void)
{
	if (have_ui && weather_serial() != weather_seen) {
		weather_seen = weather_serial();
		show_weather();
	}
}

/* Once a second on screen, or at once after present_clock_soon. */
void present_clock(uint32_t now)
{
	if (have_ui && power_awake() && (!last_clock || now - last_clock >= 1000)) {
		last_clock = now;
		struct civil c;
		uint32_t rtc = rtc_read();
		civil_from_epoch(rtc, &c);
		ui_set_clock(c.hour, c.minute, c.second, c.weekday, c.day, c.month, c.year, rtc);
		if (rtc / 86400 != weather_day) {
			weather_day = rtc / 86400;
			show_weather();
		}
		/* Only once shown, as the first frames do not wait for the
		 * store. */
		if (agenda_today && ((c.year - 2000) << 9 | c.month << 5 | c.day) != agenda_today)
			present_agenda();
		ui_set_battery(batt_percent(), batt_charging());
		ui_set_connected(bt_host_connected(), bt_store_bonded());
		static char addr_text[18];
		uint8_t addr[6];
		if (!addr_text[0] && !bt_addr_get(addr))
			snprintf(addr_text, sizeof addr_text, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3],
				 addr[2], addr[1], addr[0]);
		ui_set_info(addr_text, now / 1000);
		if (have_hub && steps_poll()) {
			present_activity();
			if (workout_running())
				present_workout();
		}
		power_tick();
		ui_set_face(setting_get(SETTING_WATCHFACE), setting_get(SETTING_NIGHTSTAND), power_nightstand());
	}
}

void present_clock_soon(void) { last_clock = 0; }
