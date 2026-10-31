/* The timer and the stopwatch are kept in the key-value store across
 * reboots: the timer's duration and time left and, while it runs, the
 * RTC second it ends at; the stopwatch's time and laps and, while it
 * runs, the RTC second it started at. A timer that ended while the
 * watch was off is back at its duration. Unanswered, an alarm snoozes
 * after RING_MS, as the alarm presenter's autoSnoozeTimer does; a
 * timer just stops. */
#include <string.h>
#include "asteroid.h"
#include "storage/storage.h"
#include "alarm.h"
#include "ui/ui.h"
#include "clockapps.h"

#define RING_MS		30000
#define KV_TIMER	"timer"
#define KV_STOPWATCH	"stopwatch"
#define LAPS		99
#define TIMER_DEFAULT_MS 300000

struct saved_timer {
	uint32_t set_ms, left_ms, end_rtc;
};

struct saved_stopwatch {
	uint32_t acc_ms, start_rtc, running, laps;
	uint32_t lap_ms[LAPS];
};

static bool have_ui, have_haptics;
static bool ringing, ring_timer, sw_running;
static uint32_t ring_t, check_t, timer_set, timer_left, timer_end, sw_acc, sw_start;
static uint32_t lap_ms[LAPS];
static unsigned laps;
static uint32_t shown_timer, shown_sw;

static void save_timer(uint32_t now)
{
	struct saved_timer v = { timer_set, timer_left, timer_end ? pmic_rtc_read() + (timer_end - now + 500) / 1000 : 0 };
	kv_set(KV_TIMER, &v, sizeof v);
}

static void save_stopwatch(uint32_t now)
{
	struct saved_stopwatch v = {
		.acc_ms = sw_acc,
		.start_rtc = sw_running ? pmic_rtc_read() - (now - sw_start) / 1000 : 0,
		.running = sw_running,
		.laps = laps,
	};
	memcpy(v.lap_ms, lap_ms, laps * sizeof lap_ms[0]);
	kv_set(KV_STOPWATCH, &v, offsetof(struct saved_stopwatch, lap_ms) + laps * sizeof lap_ms[0]);
}

static void load_timer(void)
{
	struct saved_timer v;
	if (kv_get(KV_TIMER, &v, sizeof v) != sizeof v || !v.set_ms)
		return;
	uint32_t rtc = pmic_rtc_read();
	timer_set = v.set_ms;
	timer_left = v.end_rtc && v.end_rtc <= rtc ? v.set_ms : v.left_ms;
	if (v.end_rtc > rtc)
		timer_end = time_ms() + (v.end_rtc - rtc) * 1000;
}

static void load_stopwatch(void)
{
	static struct saved_stopwatch v;
	int n = kv_get(KV_STOPWATCH, &v, sizeof v);
	unsigned head = offsetof(struct saved_stopwatch, lap_ms);
	if (n < (int)head || v.laps > LAPS || n != (int)(head + v.laps * sizeof v.lap_ms[0]))
		return;
	uint32_t rtc = pmic_rtc_read();
	sw_acc = v.acc_ms + (v.running && rtc > v.start_rtc ? (rtc - v.start_rtc) * 1000 : 0);
	sw_running = v.running;
	sw_start = time_ms();
	memcpy(lap_ms, v.lap_ms, v.laps * sizeof lap_ms[0]);
	laps = v.laps;
}

void clockapps_init(bool ui, bool haptics)
{
	have_ui = ui;
	have_haptics = haptics;
	ringing = ring_timer = sw_running = false;
	ring_t = check_t = timer_end = sw_acc = sw_start = 0;
	timer_set = timer_left = TIMER_DEFAULT_MS;
	laps = 0;
	shown_timer = shown_sw = 1;
	load_timer();
	load_stopwatch();
	if (have_ui)
		ui_set_laps(lap_ms, laps);
}

void clockapps_show_alarms(void)
{
	struct ui_alarm list[ALARMS];
	unsigned n = alarm_count();
	for (unsigned i = 0; i < n; i++) {
		const struct alarm *a = alarm_get(i);
		list[i] = (struct ui_alarm){ a->hour, a->minute, a->days, a->on };
	}
	ui_set_alarms(list, n);
}

/* A change from the alarm app: an alarm edited or deleted. */
static void alarm_edited(const struct ui_event *e)
{
	if (e->kind == UI_EVENT_ALARM_DELETE) {
		if (e->value >= 0)
			alarm_delete((unsigned)e->value);
	} else if (e->alarm.hour < 24 && e->alarm.minute < 60) {
		struct alarm a = { e->alarm.hour, e->alarm.minute, (uint8_t)(e->alarm.days & 0x7f), e->alarm.on };
		if (!alarm_set(e->value >= 0 ? (unsigned)e->value : ALARMS, &a))
			log_printf("alarm: no room\n");
	}
	clockapps_show_alarms();
}

static void clock_action(int a, uint32_t now)
{
	if (a == UI_TIMER_START && timer_end) {
		timer_left = timer_end - now;
		timer_end = 0;
	} else if (a == UI_TIMER_START && timer_left) {
		timer_end = now + timer_left;
	} else if (a == UI_TIMER_RESET) {
		timer_end = 0;
		timer_left = timer_set;
	} else if (a >= UI_TIMER_MINUTE_UP && a <= UI_TIMER_SECONDS_DOWN && !timer_end) {
		int32_t step = a <= UI_TIMER_MINUTE_DOWN ? 60000 : 10000, t = (int32_t)timer_set;
		t += a == UI_TIMER_MINUTE_UP || a == UI_TIMER_SECONDS_UP ? step : -step;
		timer_set = timer_left = (uint32_t)(t < 10000 ? 10000 : t > 5940000 ? 5940000 : t);
	} else if (a == UI_STOPWATCH_START) {
		if (sw_running)
			sw_acc += now - sw_start;
		else
			sw_start = now;
		sw_running = !sw_running;
	} else if (a == UI_STOPWATCH_RESET) {
		sw_acc = 0;
		sw_running = false;
		laps = 0;
		ui_set_laps(lap_ms, 0);
	} else if (a == UI_STOPWATCH_LAP && sw_running && laps < ARRAY_SIZE(lap_ms)) {
		memmove(lap_ms + 1, lap_ms, laps++ * sizeof lap_ms[0]);
		lap_ms[0] = sw_acc + now - sw_start;
		ui_set_laps(lap_ms, laps);
	}
	if (a >= UI_STOPWATCH_START)
		save_stopwatch(now);
	else
		save_timer(now);
	shown_timer = shown_sw = UINT32_MAX;
}

bool clockapps_event(const struct ui_event *e, uint32_t now)
{
	int a = e->value;
	switch (e->kind) {
	case UI_EVENT_TIMER_SET:
		if (!timer_end && a >= 1 && a <= 86399) {
			timer_set = timer_left = (uint32_t)a * 1000;
			shown_timer = UINT32_MAX;
			save_timer(now);
		}
		return true;
	case UI_EVENT_ALARM_EDIT:
	case UI_EVENT_ALARM_DELETE:
		alarm_edited(e);
		return true;
	case UI_EVENT_ALARM_RING:
		if (!ringing)
			return true;
		if (a == UI_ALARM_SNOOZE && !ring_timer)
			alarm_snooze(pmic_rtc_read());
		log_printf("alarm: %s\n", a == UI_ALARM_SNOOZE ? "snoozed" : "dismissed");
		ringing = false;
		return true;
	case UI_EVENT_CLOCK:
		clock_action(a, now);
		return true;
	default:
		return false;
	}
}

bool clockapps_tick(uint32_t now)
{
	bool wake = false;
	if (now - check_t >= 1000) {
		check_t = now;
		if (alarm_check(pmic_rtc_read()) && have_ui) {
			log_printf("alarm: ringing\n");
			ringing = true;
			ring_timer = false;
			ring_t = now;
			ui_set_ringing(true, false);
			clockapps_show_alarms();
			wake = true;
		}
	}
	if (timer_end && (int32_t)(now - timer_end) >= 0 && have_ui) {
		log_printf("timer: done\n");
		timer_end = 0;
		timer_left = timer_set;
		ringing = ring_timer = true;
		ring_t = now;
		ui_set_ringing(true, true);
		wake = true;
	}
	if (ringing && now - ring_t >= RING_MS) {
		if (!ring_timer)
			alarm_snooze(pmic_rtc_read());
		log_printf("alarm: %s\n", ring_timer ? "stopped ringing" : "snoozed");
		ringing = false;
	}
	if (ringing) {
		static const uint16_t ring[] = { 500, 500 };
		if (have_haptics && !haptics_busy())
			haptics_play(ring, ARRAY_SIZE(ring));
	} else if (have_ui && ring_t) {
		ring_t = 0;
		ui_set_ringing(false, false);
		if (have_haptics)
			haptics_play(NULL, 0);
	}
	return wake;
}

void clockapps_refresh(uint32_t now)
{
	if (!have_ui)
		return;
	uint32_t t = timer_end ? timer_end - now : timer_left, sw = sw_acc + (sw_running ? now - sw_start : 0);
	if (t / 100 != shown_timer) {
		shown_timer = t / 100;
		ui_set_timer(t, timer_set, timer_end);
	}
	if (sw / 100 != shown_sw) {
		shown_sw = sw / 100;
		ui_set_stopwatch(sw, sw_running);
	}
}

void clockapps_sleep(void)
{
	if (!ringing)
		return;
	if (!ring_timer)
		alarm_snooze(pmic_rtc_read());
	log_printf("alarm: %s\n", ring_timer ? "dismissed" : "snoozed");
	ringing = false;
}

bool clockapps_ringing(void)
{
	return ringing;
}

uint32_t clockapps_next_ms(uint32_t now)
{
	uint32_t next = UINT32_MAX, alarm = alarm_next_s(pmic_rtc_read());
	if (alarm != UINT32_MAX)
		next = !alarm ? 1 : alarm > UINT32_MAX / 1000 ? UINT32_MAX : alarm * 1000;
	if (timer_end) {
		uint32_t left = (int32_t)(timer_end - now) > 0 ? timer_end - now : 1;
		if (left < next)
			next = left;
	}
	return next;
}
