/* Workouts: one at a time, from start to stop, with the steps counted
 * meanwhile and what they come to and the heart rate readings it got. The running one is
 * saved when it starts, so a reboot does not end it; its heart rate is
 * kept in memory only. The last WORKOUTS that ended are kept, newest
 * first. */
#include <string.h>
#include "asteroid.h"
#include "sensors/steps.h"
#include "storage/storage.h"
#include "workout.h"

#define KV_WORKOUTS	"workouts"

static struct saved {
	uint32_t start;
	struct activity from;
	bool running;
	uint8_t count;
	uint16_t reserved;
	struct workout past[WORKOUTS];
} saved;
static uint32_t bpm_sum, bpm_n, bpm_max;

void workout_init(void)
{
	if (kv_get(KV_WORKOUTS, &saved, sizeof saved) != (int)sizeof saved)
		memset(&saved, 0, sizeof saved);
	bpm_sum = bpm_n = bpm_max = 0;
}

bool workout_running(void)
{
	return saved.running;
}

void workout_start(uint32_t rtc)
{
	if (saved.running)
		return;
	saved.running = true;
	saved.start = rtc;
	saved.from = steps_activity();
	bpm_sum = bpm_n = bpm_max = 0;
	kv_set(KV_WORKOUTS, &saved, sizeof saved);
}

void workout_heart_rate(uint32_t bpm)
{
	if (!saved.running || !bpm || bpm > 255)
		return;
	bpm_sum += bpm;
	bpm_n++;
	if (bpm > bpm_max)
		bpm_max = bpm;
}

struct workout workout_now(uint32_t rtc)
{
	struct activity done = steps_since(&saved.from);
	return (struct workout){
		.start = saved.start,
		.seconds = rtc > saved.start ? rtc - saved.start : 0,
		.steps = done.steps,
		.distance_m = done.distance_m,
		.kcal = done.kcal,
		.avg_bpm = bpm_n ? (uint8_t)((bpm_sum + bpm_n / 2) / bpm_n) : 0,
		.max_bpm = (uint8_t)bpm_max,
	};
}

void workout_stop(uint32_t rtc)
{
	if (!saved.running)
		return;
	memmove(&saved.past[1], &saved.past[0], sizeof saved.past[0] * (WORKOUTS - 1));
	saved.past[0] = workout_now(rtc);
	if (saved.count < WORKOUTS)
		saved.count++;
	saved.running = false;
	kv_set(KV_WORKOUTS, &saved, sizeof saved);
}

const struct workout *workout_past(unsigned i)
{
	return i < saved.count ? &saved.past[i] : NULL;
}
