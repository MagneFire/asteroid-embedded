/* Steps today and on the days before. The sensor hub counts from its own
 * reset, which happens at every boot, so the count is kept as a running
 * total on top of the total saved before; the day starts over at
 * midnight of the clock's local time. The totals are saved in the
 * key-value store when they changed and the watch goes to sleep, or the
 * day changes; nothing is counted or saved before steps_init has loaded
 * them. Each day that ends goes into a ring of the last STEPS_DAYS, and
 * the days the watch missed, off or with the clock set ahead, count as
 * none. */
#include "asteroid.h"
#include "hub.h"
#include "steps.h"
#include "storage/storage.h"

#define KV_STEPS	"steps"
#define KV_DAYS		"steps.days"

static struct saved {
	uint32_t total, day, day_start;
} saved, now_state;
/* steps[d % STEPS_DAYS] is day d, for the STEPS_DAYS days up to last. */
static struct days {
	uint32_t last;
	uint32_t steps[STEPS_DAYS];
} days;
static uint32_t base = HUB_NONE;
static bool ready, changed;

void steps_init(void)
{
	if (kv_get(KV_STEPS, &saved, sizeof saved) != (int)sizeof saved)
		saved = (struct saved){ 0, rtc_read() / 86400, 0 };
	now_state = saved;
	if (kv_get(KV_DAYS, &days, sizeof days) != (int)sizeof days)
		days = (struct days){ .last = saved.day - 1 };
	base = HUB_NONE;
	changed = false;
	ready = true;
	log_printf("steps: %lu today, %lu in total\n", saved.total - saved.day_start, saved.total);
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
	steps_save();
}

bool steps_poll(void)
{
	if (!ready)
		return false;
	uint32_t count = hub_steps(), day = rtc_read() / 86400, today = steps_today();
	if (count != HUB_NONE) {
		if (base == HUB_NONE)
			base = saved.total;
		if (base + count != now_state.total) {
			now_state.total = base + count;
			changed = true;
		}
	}
	if (day != now_state.day) {
		end_day(day);
		return true;
	}
	return steps_today() != today;
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

uint32_t steps_next_ms(void)
{
	uint32_t ms = (86400 - rtc_read() % 86400 + 1) * 1000;
	return ms < 3600000 ? ms : 3600000;
}
