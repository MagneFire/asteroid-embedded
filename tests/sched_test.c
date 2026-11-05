/* The scheduler on the host, with fake time and interrupts simulated by
 * posting from inside a task. */
#include <stdio.h>
#include "asteroid.h"
#include "sched.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static uint32_t fake_us;
uint32_t time_us(void) { return fake_us; }

static struct sched_task a;
static int runs[4], order[8], ordered;
static bool drain;
static uint32_t cost_us;
static struct sched_task *rearm, *post_other;

static bool count_run(int i)
{
	runs[i]++;
	if (ordered < 8)
		order[ordered++] = i;
	fake_us += cost_us;
	return false;
}

static bool run0(uint32_t now) { return count_run(0); }
static bool run1(uint32_t now) { return count_run(1); }
static bool run2(uint32_t now)
{
	count_run(2);
	if (rearm)
		sched_after(rearm, now, 100);
	if (post_other)
		sched_post(post_other);
	return false;
}
static bool nested;
static unsigned nested_ran;

static bool run_busy(uint32_t now)
{
	count_run(3);
	if (nested) {
		sched_post(&a);
		nested_ran = sched_run(now);
	}
	return drain;
}

static struct sched_task a = { .name = "a", .run = run0 }, b = { .name = "b", .run = run1 }, c = { .name = "c", .run = run2 },
			  busy = { .name = "busy", .run = run_busy };

static void setup(void)
{
	sched_clear();
	for (int i = 0; i < 4; i++)
		runs[i] = 0;
	ordered = 0;
	drain = false;
	cost_us = 0;
	rearm = post_other = NULL;
	sched_add(&a);
	sched_add(&b);
	sched_add(&c);
	sched_add(&busy);
}

static void events(void)
{
	setup();
	CHECK(!sched_pending());
	CHECK(sched_run(0) == 0);
	sched_post(&b);
	sched_post(&a);
	sched_post(&a);
	CHECK(sched_pending());
	CHECK(sched_next_ms(0) == 0);
	CHECK(sched_run(0) == 2);
	/* Posts coalesce, and tasks run in the order they were added. */
	CHECK(runs[0] == 1 && runs[1] == 1 && order[0] == 0 && order[1] == 1);
	CHECK(!sched_pending());
}

static void post_during_run(void)
{
	/* An interrupt during a pass, for a task the pass already went by:
	 * nothing is lost, and the idle check sees it. */
	setup();
	post_other = &a;
	sched_post(&c);
	sched_run(0);
	CHECK(runs[2] == 1 && runs[0] == 0);
	CHECK(sched_pending() && sched_next_ms(0) == 0);
	post_other = NULL;
	sched_run(0);
	CHECK(runs[0] == 1 && !sched_pending());
}

static void deadlines(void)
{
	setup();
	CHECK(sched_next_ms(0) == UINT32_MAX);
	sched_after(&a, 0, 100);
	sched_after(&b, 0, 40);
	CHECK(sched_next_ms(0) == 40);
	CHECK(sched_next_ms(39) == 1);
	CHECK(sched_run(39) == 0);
	CHECK(sched_run(40) == 1 && runs[1] == 1);
	CHECK(sched_next_ms(40) == 60);
	/* It fires once. */
	CHECK(sched_run(41) == 0);
	sched_cancel(&a);
	CHECK(sched_run(100) == 0 && sched_next_ms(100) == UINT32_MAX);
	/* A task can ask for its next deadline as it runs. */
	rearm = &c;
	sched_at(&c, 200);
	sched_run(200);
	CHECK(sched_next_ms(200) == 100);
	/* An event does not use up a deadline. */
	setup();
	sched_after(&a, 0, 100);
	sched_post(&a);
	sched_run(0);
	CHECK(runs[0] == 1 && sched_next_ms(0) == 100);
}

static void wrap(void)
{
	setup();
	uint32_t now = UINT32_MAX - 10;
	sched_after(&a, now, 20);
	CHECK(sched_next_ms(now) == 20);
	CHECK(sched_run(now + 19) == 0);
	CHECK(sched_run(now + 20) == 1);
	/* A far deadline stays in the future. */
	sched_after(&b, 0, UINT32_MAX);
	CHECK(sched_run(1) == 0 && sched_next_ms(0) == 0x7fffffffu);
}

static void starvation(void)
{
	/* A task with endless work gets one turn per pass, the others too. */
	setup();
	drain = true;
	sched_post(&busy);
	for (uint32_t i = 0; i < 5; i++) {
		sched_post(&a);
		CHECK(sched_pending());
		sched_run(i);
		CHECK(runs[3] == (int)i + 1 && runs[0] == (int)i + 1);
	}
	sched_after(&b, 5, 0);
	sched_run(5);
	CHECK(runs[1] == 1);
	/* Finished, it stops asking. */
	drain = false;
	sched_run(6);
	CHECK(!sched_pending());
}

static void saturation(void)
{
	static struct sched_task many[SCHED_TASKS + 1];
	sched_clear();
	for (unsigned i = 0; i < SCHED_TASKS; i++) {
		many[i].run = run0;
		CHECK(sched_add(&many[i]));
	}
	CHECK(!sched_add(&many[SCHED_TASKS]));
	runs[0] = 0;
	for (int i = 0; i < 1000; i++)
		sched_post(&many[SCHED_TASKS - 1]);
	CHECK(sched_run(0) == 1 && runs[0] == 1);
	/* The top bit is a task like the others. */
	sched_at(&many[SCHED_TASKS - 1], 10);
	CHECK(sched_run(10) == 1);
}

static void budget(void)
{
	setup();
	a.budget_us = 100;
	cost_us = 150;
	sched_post(&a);
	sched_run(0);
	cost_us = 50;
	sched_post(&a);
	sched_run(0);
	CHECK(a.overruns == 1 && a.max_us == 150);
	a.budget_us = 0;
}

static void late(void)
{
	setup();
	sched_at(&b, 100);
	CHECK(!sched_late(100, 50) && !sched_late(150, 50));
	CHECK(sched_late(151, 50) == &b);
	sched_run(151);
	CHECK(!sched_late(1000, 50));
}

static void lazy(void)
{
	/* It neither shortens a sleep nor counts as late, but runs when a
	 * pass comes by. */
	setup();
	a.lazy = true;
	sched_after(&a, 0, 10);
	CHECK(sched_next_ms(0) == UINT32_MAX);
	CHECK(sched_next_ms(100) == UINT32_MAX && !sched_late(1000, 50));
	CHECK(sched_run(1000) == 1 && runs[0] == 1);
	a.lazy = false;
}

static void reentry(void)
{
	/* A wait inside a task cannot start a pass. */
	setup();
	nested = true;
	sched_post(&busy);
	CHECK(sched_run(0) == 1 && nested_ran == 0);
	nested = false;
	CHECK(runs[0] == 0 && sched_pending());
	sched_run(1);
	CHECK(runs[0] == 1);
}

static void stalled(void)
{
	setup();
	sched_run(100);
	CHECK(!sched_stalled(1000, 50));
	sched_post(&a);
	CHECK(!sched_stalled(150, 50) && sched_stalled(151, 50));
	sched_run(151);
	CHECK(!sched_stalled(1000, 50));
	/* How late a task ran is kept after it has. */
	sched_at(&b, 200);
	sched_run(230);
	CHECK(b.max_late_ms == 30);
}

int main(void)
{
	lazy();
	reentry();
	stalled();
	events();
	post_during_run();
	deadlines();
	wrap();
	starvation();
	saturation();
	budget();
	late();
	if (failures)
		return 1;
	puts("sched_test: ok");
	return 0;
}
