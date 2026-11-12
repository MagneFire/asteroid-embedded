/* Workouts on the host: start and stop, steps and heart rate in
 * between, a reboot while one runs, and the history of the last ones. */
#include <stdio.h>
#include <string.h>
#include "asteroid.h"
#include "sensors/steps.h"
#include "storage/storage.h"
#include "workout.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint8_t stored[512];
static int stored_len = -2, writes;

int kv_get(const char *key, void *buf, unsigned size)
{
	if (stored_len > 0)
		memcpy(buf, stored, (unsigned)stored_len < size ? (unsigned)stored_len : size);
	return stored_len;
}
int kv_set(const char *key, const void *val, unsigned len)
{
	memcpy(stored, val, len);
	stored_len = (int)len;
	writes++;
	return 0;
}

/* The totals so far: the distance a metre a step and a kcal every 20. */
static uint32_t total;
struct activity steps_activity(void) { return (struct activity){ total, total, total / 20 }; }
struct activity steps_since(const struct activity *from)
{
	struct activity a = steps_activity();
	a.steps = a.steps > from->steps ? a.steps - from->steps : 0;
	a.distance_m = a.distance_m > from->distance_m ? a.distance_m - from->distance_m : 0;
	a.kcal = a.kcal > from->kcal ? a.kcal - from->kcal : 0;
	return a;
}

static void start(uint32_t rtc, uint32_t steps) { total = steps; workout_start(rtc); }
static void stop(uint32_t rtc, uint32_t steps) { total = steps; workout_stop(rtc); }
static struct workout now(uint32_t rtc, uint32_t steps) { total = steps; return workout_now(rtc); }

int main(void)
{
	workout_init();
	CHECK(!workout_running() && !workout_past(0));
	stop(100, 0);
	CHECK(writes == 0 && !workout_past(0));

	start(1000, 500);
	CHECK(workout_running() && writes == 1);
	start(1010, 600);
	CHECK(now(1000, 500).start == 1000);
	workout_heart_rate(100);
	workout_heart_rate(121);
	workout_heart_rate(0);
	workout_heart_rate(300);
	struct workout w = now(1600, 1700);
	CHECK(w.seconds == 600 && w.steps == 1200 && w.avg_bpm == 111 && w.max_bpm == 121);
	CHECK(w.distance_m == 1200 && w.kcal == 60);
	stop(1900, 2000);
	CHECK(!workout_running() && writes == 2);
	CHECK(workout_past(0) && workout_past(0)->seconds == 900 && workout_past(0)->steps == 1500);
	CHECK(workout_past(0)->avg_bpm == 111 && !workout_past(1));

	/* A reboot while one runs: it goes on, without the readings before. */
	start(5000, 10);
	workout_heart_rate(150);
	workout_init();
	CHECK(workout_running());
	stop(5060, 70);
	CHECK(workout_past(0)->steps == 60 && workout_past(0)->max_bpm == 0 && workout_past(1)->start == 1000);

	/* The hub count starting over below the start: no steps, not a wrap. */
	start(6000, 900);
	CHECK(now(6010, 5).steps == 0);
	stop(6020, 5);

	for (uint32_t i = 0; i < WORKOUTS + 2; i++) {
		start(10000 + i * 100, 0);
		stop(10000 + i * 100 + 10 + i, 0);
	}
	CHECK(workout_past(0)->seconds == 10 + WORKOUTS + 1);
	CHECK(workout_past(WORKOUTS - 1) && workout_past(WORKOUTS - 1)->seconds == 12);
	CHECK(!workout_past(WORKOUTS));

	printf(failures ? "workout_test: %d failures\n" : "workout_test: ok\n", failures);
	return failures != 0;
}
