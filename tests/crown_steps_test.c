/* The crown's counts as steps of a value. */
#include <stdio.h>
#include "input/crown_steps.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
	struct crown_steps s = { 0 };
	uint32_t t = 10000;

	/* A brush: too few counts, and what it left is gone after a pause. */
	CHECK(crown_steps(&s, CROWN_START_COUNTS - 1, t) == 0);
	t += CROWN_IDLE_MS + 1;
	CHECK(crown_steps(&s, 1, t) == 0);

	/* A back and forth nets nothing. */
	t += 1000;
	CHECK(crown_steps(&s, 8, t) == 0 && crown_steps(&s, -8, t + 10) == 0 && crown_steps(&s, 8, t + 20) == 0);

	/* A slow turn, a count every 100 ms: the first step at the start
	 * threshold, then one every CROWN_STEP_COUNTS. */
	t += 1000;
	int steps = 0, first = 0;
	for (int i = 1; i <= CROWN_START_COUNTS + 3 * CROWN_STEP_COUNTS; i++, t += 100) {
		steps += crown_steps(&s, 1, t);
		if (steps && !first)
			first = i;
	}
	CHECK(first == CROWN_START_COUNTS && steps == 4);

	/* Turning on without a pause needs no new start, either way. */
	CHECK(crown_steps(&s, -CROWN_STEP_COUNTS, t) == -1);
	CHECK(crown_steps(&s, -3 * CROWN_STEP_COUNTS, t + 50) == -3);

	/* A fast turn in one poll, the other way round too. */
	t += 1000;
	CHECK(crown_steps(&s, CROWN_START_COUNTS + 2 * CROWN_STEP_COUNTS + 3, t) == 3);
	t += 1000;
	CHECK(crown_steps(&s, -(CROWN_START_COUNTS + CROWN_STEP_COUNTS), t) == -2);

	/* After a pause the next turn starts over. */
	t += CROWN_IDLE_MS + 1;
	CHECK(crown_steps(&s, CROWN_STEP_COUNTS, t) == 0);

	/* Pages: nothing while what the crown scrolls has not reached its
	 * end, then one page for a turn on past it, however fast. */
	struct crown_page p = { 0 };
	t += 1000;
	CHECK(crown_page(&p, -100, false, t) == 0 && !crown_page_locked(&p, t));
	CHECK(crown_page(&p, -(CROWN_PAGE_COUNTS - 1), true, t + 10) == 0);
	CHECK(crown_page(&p, -3 * CROWN_PAGE_COUNTS, true, t + 20) == -1);

	/* The rest of the spin is dropped, and so are its counts. */
	t += 20;
	CHECK(crown_page_locked(&p, t + CROWN_PAGE_LOCK_MS - 1));
	CHECK(crown_page(&p, -3 * CROWN_PAGE_COUNTS, true, t + CROWN_PAGE_LOCK_MS - 1) == 0);
	CHECK(!crown_page_locked(&p, t + CROWN_PAGE_LOCK_MS));
	CHECK(crown_page(&p, -(CROWN_PAGE_COUNTS - 1), true, t + CROWN_PAGE_LOCK_MS) == 0);
	CHECK(crown_page(&p, -1, true, t + CROWN_PAGE_LOCK_MS + 10) == -1);

	/* A pause, a turn back or leaving the end starts over. */
	t += 5000;
	CHECK(crown_page(&p, CROWN_PAGE_COUNTS - 1, true, t) == 0);
	CHECK(crown_page(&p, 1, true, t + CROWN_IDLE_MS + 1) == 0);
	t += 1000;
	CHECK(crown_page(&p, CROWN_PAGE_COUNTS - 1, true, t) == 0);
	CHECK(crown_page(&p, -2, true, t + 10) == 0 && crown_page(&p, 2, true, t + 20) == 0);
	CHECK(crown_page(&p, 1, true, t + 30) == 1);
	t += 1000;
	CHECK(crown_page(&p, CROWN_PAGE_COUNTS - 1, true, t) == 0);
	CHECK(crown_page(&p, 1, false, t + 10) == 0 && crown_page(&p, 1, true, t + 20) == 0);

	if (failures)
		return 1;
	printf("crown_steps_test: ok\n");
	return 0;
}
