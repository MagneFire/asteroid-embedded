/* Steps today and on the days before. The sensor hub counts from its own
 * reset, which happens at every boot, so the count is kept as a running
 * total on top of the total saved before, and a count that goes down is
 * a hub that started over; the day starts over at midnight of the
 * clock's local time, whatever the hub makes of midnight. The totals are
 * saved in the key-value store when they changed and the watch goes to
 * sleep, or the day changes; nothing is counted or saved before
 * steps_init has loaded them. Each day that ends goes into a ring of the
 * last STEPS_DAYS, and the days the watch missed, off or with the clock
 * set ahead, count as none. A hub that measures the distance and
 * calories from the wearer's profile has them kept the same way, today's
 * alone; otherwise the distance is worked out from the steps. The
 * calories always come from the distance: the hub counts arm movement at
 * a desk as activity. */
#include <string.h>
#include "asteroid.h"
#include "fitness.h"
#include "hub.h"
#include "steps.h"
#include "storage/storage.h"

#define KV_STEPS	"steps"
#define KV_DAYS		"steps.days"

/* As the steps were first saved, then the hub's distance and calories. */
#define SAVED_V1	12

static struct saved {
	uint32_t total, day, day_start;
	uint32_t measured[2], measured_start[2];
} saved, now_state;
/* steps[d % STEPS_DAYS] is day d, for the STEPS_DAYS days up to last. */
static struct days {
	uint32_t last;
	uint32_t steps[STEPS_DAYS];
} days;
static uint32_t last = HUB_NONE, measured_base[2];
static bool measured_based;
static bool ready, changed;

/* A hub that keeps steps today itself (catfish) also counts while the
 * watch is off. Its count is taken once after a boot on the same day as
 * the last save, and only the steps it has above ours: from another day
 * it can be one the hub never saw end, and what it saved can lag ours.
 * Its LCD then shows ours, from then on and from each midnight. */
static void end_day(uint32_t day);
static uint32_t boot_day;
static bool hub_today_read;

static void take_hub_today(uint32_t day)
{
	uint32_t t = hub_steps_today();
	if (hub_today_read || t == HUB_NONE)
		return;
	hub_today_read = true;
	if (day == boot_day && day == now_state.day && t > steps_today()) {
		now_state.total += t - steps_today();
		changed = true;
	}
	if (t != steps_today())
		hub_sync_steps_today(steps_today());
}

void steps_init(void)
{
	int n = kv_get(KV_STEPS, &saved, sizeof saved);
	if (n == SAVED_V1)
		memset(saved.measured, 0, sizeof saved - SAVED_V1);
	else if (n != (int)sizeof saved)
		saved = (struct saved){ .day = rtc_read() / 86400 };
	now_state = saved;
	if (kv_get(KV_DAYS, &days, sizeof days) != (int)sizeof days)
		days = (struct days){ .last = saved.day - 1 };
	last = HUB_NONE;
	measured_based = changed = hub_today_read = false;
	ready = true;
	/* The steps since this boot are today's. */
	uint32_t day = rtc_read() / 86400;
	boot_day = day == saved.day ? day : HUB_NONE;
	if (day != saved.day)
		end_day(day);
	log_printf("steps: %lu today, %lu in total\n", steps_today(), now_state.total);
}

static void end_day(uint32_t day)
{
	uint32_t ended = now_state.day;
	if (day > ended) {
		uint32_t from = ended > days.last ? days.last + 1 : ended;
		if (day - from > STEPS_DAYS)
			from = day - STEPS_DAYS;
		for (uint32_t d = from; d < day; d++)
			days.steps[d % STEPS_DAYS] = d == ended ? now_state.total - now_state.day_start : 0;
		days.last = day - 1;
		kv_set(KV_DAYS, &days, sizeof days);
	}
	now_state.day = day;
	now_state.day_start = now_state.total;
	for (unsigned i = 0; i < 2; i++)
		now_state.measured_start[i] = now_state.measured[i];
	steps_save();
}

bool steps_poll(void)
{
	if (!ready)
		return false;
	uint32_t count = hub_steps(), day = rtc_read() / 86400, today = steps_today(), m[2];
	struct activity before = steps_activity_today();
	hub_profile(&(struct hub_profile){ (uint8_t)setting_get(SETTING_AGE), (uint8_t)setting_get(SETTING_HEIGHT),
					    (uint8_t)setting_get(SETTING_WEIGHT), setting_get(SETTING_MALE) });
	if (hub_activity(m)) {
		if (!measured_based) {
			for (unsigned i = 0; i < 2; i++)
				measured_base[i] = saved.measured[i];
			measured_based = true;
		}
		for (unsigned i = 0; i < 2; i++)
			if (measured_base[i] + m[i] != now_state.measured[i]) {
				now_state.measured[i] = measured_base[i] + m[i];
				changed = true;
			}
	}
	if (count != HUB_NONE) {
		uint32_t add = last == HUB_NONE || count < last ? count : count - last;
		last = count;
		if (add) {
			now_state.total += add;
			changed = true;
		}
	}
	take_hub_today(day);
	if (day != now_state.day) {
		end_day(day);
		hub_sync_steps_today(0);
		return true;
	}
	struct activity after = steps_activity_today();
	return steps_today() != today || after.distance_m != before.distance_m || after.kcal != before.kcal;
}

void steps_save(void)
{
	if (!ready || (!changed && now_state.day == saved.day))
		return;
	if (!kv_set(KV_STEPS, &now_state, sizeof now_state)) {
		saved = now_state;
		changed = false;
	}
}

uint32_t steps_today(void)
{
	return now_state.total - now_state.day_start;
}

struct activity steps_activity(void)
{
	return (struct activity){ now_state.total, now_state.measured[0], now_state.measured[1] };
}

struct activity steps_since(const struct activity *from)
{
	struct activity a = steps_activity();
	a.steps = a.steps > from->steps ? a.steps - from->steps : 0;
	if (hub_measures_activity())
		a.distance_m = a.distance_m > from->distance_m ? a.distance_m - from->distance_m : 0;
	else
		a.distance_m = fitness_distance_m(a.steps, setting_get(SETTING_HEIGHT));
	a.kcal = fitness_kcal(a.distance_m, setting_get(SETTING_WEIGHT));
	return a;
}

struct activity steps_activity_today(void)
{
	return steps_since(&(struct activity){ now_state.day_start, now_state.measured_start[0], now_state.measured_start[1] });
}

uint32_t steps_day(unsigned ago)
{
	if (!ago)
		return steps_today();
	if (ago > now_state.day)
		return 0;
	uint32_t d = now_state.day - ago;
	if (d > days.last || days.last - d >= STEPS_DAYS)
		return 0;
	return days.steps[d % STEPS_DAYS];
}

/* A few seconds late, as the timer and the clock drift apart. */
uint32_t steps_next_ms(void)
{
	return (86400 - rtc_read() % 86400 + 5) * 1000;
}
