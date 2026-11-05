/* A run-to-completion scheduler on the one stack: interrupts post events,
 * tasks ask for a deadline, and sched_run runs each task that has either,
 * once, to its end. Nothing here sleeps; the caller idles for
 * sched_next_ms, and has to check sched_pending with interrupts masked
 * right before it does, or an event posted in between waits for the next
 * wake.
 *
 * There is no queue to fill: an event is a bit, so posts between two runs
 * become one, and a task drains whatever it has when it runs. A task that
 * could not finish returns true and runs again next pass, after the
 * others had their turn.
 *
 * Only sched_post may be called from an interrupt: deadlines belong to
 * the main context, and an interrupt that needs one posts a task that
 * sets it. Deadlines reach at most 2^31 ms ahead. All tasks in a pass see
 * the `now` it started with; one made late by a slow task runs next pass. */
#ifndef SCHED_H
#define SCHED_H
#include <stdint.h>
#include <stdbool.h>

#define SCHED_TASKS 32

struct sched_task {
	const char *name;
	bool (*run)(uint32_t now);
	uint32_t budget_us;	/* 0: no limit */
	/* A deadline that never wakes the watch: it runs on the first pass
	 * after it, so periodic work costs no wakeups asleep. */
	bool lazy;
	/* The scheduler's own. */
	uint32_t at, max_us, max_late_ms, overruns;
	uint8_t id;
	bool armed;
};

bool sched_add(struct sched_task *t);
/* From an interrupt or a task. */
void sched_post(const struct sched_task *t);
void sched_at(struct sched_task *t, uint32_t when_ms);
void sched_after(struct sched_task *t, uint32_t now, uint32_t ms);
void sched_cancel(struct sched_task *t);
/* One pass, in the order the tasks were added. Returns how many ran, 0
 * when called from inside a task: a wait in a task has to pump what it
 * needs itself, not everything. */
unsigned sched_run(uint32_t now);
/* The time until a task is due: 0 if one is already, UINT32_MAX if none
 * has a deadline. Lazy tasks do not count. */
uint32_t sched_next_ms(uint32_t now);
bool sched_pending(void);
/* A task other than a lazy one more than `limit_ms` past its deadline, or
 * NULL. */
const struct sched_task *sched_late(uint32_t now, uint32_t limit_ms);
/* Events have been waiting for a pass for more than `limit_ms`. */
bool sched_stalled(uint32_t now, uint32_t limit_ms);
void sched_clear(void);
#endif
