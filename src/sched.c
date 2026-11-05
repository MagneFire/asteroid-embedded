#include "asteroid.h"
#include "sched.h"

static struct sched_task *tasks[SCHED_TASKS];
static unsigned count;
static uint32_t pending, last_pass;
static bool running;

/* Deadlines compare by distance, so they survive the clock's wrap. */
#define MAX_AHEAD_MS 0x7fffffffu

static bool due(const struct sched_task *t, uint32_t now)
{
	return t->armed && (int32_t)(now - t->at) >= 0;
}

bool sched_add(struct sched_task *t)
{
	if (count == SCHED_TASKS)
		return false;
	t->id = (uint8_t)count;
	t->armed = false;
	t->max_us = t->max_late_ms = t->overruns = 0;
	tasks[count++] = t;
	return true;
}

void sched_post(const struct sched_task *t)
{
	__atomic_fetch_or(&pending, 1u << t->id, __ATOMIC_SEQ_CST);
}

void sched_at(struct sched_task *t, uint32_t when_ms)
{
	t->at = when_ms;
	t->armed = true;
}

void sched_after(struct sched_task *t, uint32_t now, uint32_t ms)
{
	sched_at(t, now + (ms > MAX_AHEAD_MS ? MAX_AHEAD_MS : ms));
}

void sched_cancel(struct sched_task *t)
{
	t->armed = false;
}

unsigned sched_run(uint32_t now)
{
	if (running)
		return 0;
	running = true;
	uint32_t ready = __atomic_exchange_n(&pending, 0, __ATOMIC_SEQ_CST);
	unsigned ran = 0;
	for (unsigned i = 0; i < count; i++) {
		struct sched_task *t = tasks[i];
		bool at = due(t, now);
		if (!at && !(ready & 1u << i))
			continue;
		/* Before it runs, so it can ask for the next deadline. */
		if (at) {
			t->armed = false;
			if (now - t->at > t->max_late_ms)
				t->max_late_ms = now - t->at;
		}
		uint32_t t0 = time_us();
		bool more = t->run(now);
		uint32_t us = time_us() - t0;
		if (us > t->max_us)
			t->max_us = us;
		if (t->budget_us && us > t->budget_us)
			t->overruns++;
		if (more)
			sched_post(t);
		ran++;
	}
	last_pass = now;
	running = false;
	return ran;
}

uint32_t sched_next_ms(uint32_t now)
{
	if (sched_pending())
		return 0;
	uint32_t next = UINT32_MAX;
	for (unsigned i = 0; i < count; i++) {
		const struct sched_task *t = tasks[i];
		if (!t->armed)
			continue;
		if (due(t, now))
			return 0;
		uint32_t left = t->at - now;
		if (left < next)
			next = left;
	}
	return next;
}

bool sched_pending(void)
{
	return __atomic_load_n(&pending, __ATOMIC_SEQ_CST) != 0;
}

const struct sched_task *sched_late(uint32_t now, uint32_t limit_ms)
{
	for (unsigned i = 0; i < count; i++) {
		const struct sched_task *t = tasks[i];
		if (due(t, now) && now - t->at > limit_ms)
			return t;
	}
	return NULL;
}

bool sched_stalled(uint32_t now, uint32_t limit_ms)
{
	return sched_pending() && now - last_pass > limit_ms;
}

void sched_clear(void)
{
	count = 0;
	last_pass = 0;
	running = false;
	__atomic_store_n(&pending, 0, __ATOMIC_SEQ_CST);
}
