/* Steps today. The sensor hub counts from its own reset, which happens at
 * every boot, so the count is kept as a running total on top of the
 * total saved before; the day starts over at midnight of the clock's
 * local time. The totals are saved in the key-value store when they
 * changed and the watch goes to sleep, or the day changes; nothing is
 * counted or saved before steps_init has loaded them. */
#include "asteroid.h"
#include "hub.h"
#include "steps.h"
#include "storage/storage.h"

#define KV_STEPS	"steps"

static struct saved {
	uint32_t total, day, day_start;
} saved, now_state;
static uint32_t base = HUB_NONE;
static bool ready, changed;

void steps_init(void)
{
	if (kv_get(KV_STEPS, &saved, sizeof saved) != (int)sizeof saved)
		saved = (struct saved){ 0, pmic_rtc_read() / 86400, 0 };
	now_state = saved;
	ready = true;
	log_printf("steps: %lu today, %lu in total\n", saved.total - saved.day_start, saved.total);
}

void steps_poll(void)
{
	if (!ready)
		return;
	uint32_t count = hub_steps(), day = pmic_rtc_read() / 86400;
	if (count != HUB_NONE) {
		if (base == HUB_NONE)
			base = saved.total;
		if (base + count != now_state.total) {
			now_state.total = base + count;
			changed = true;
		}
	}
	if (day != now_state.day) {
		now_state.day = day;
		now_state.day_start = now_state.total;
		steps_save();
	}
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
