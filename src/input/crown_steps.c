#include "input/crown_steps.h"

int crown_steps(struct crown_steps *s, int32_t counts, uint32_t now)
{
	if (now - s->last > CROWN_IDLE_MS) {
		s->acc = 0;
		s->turning = false;
	}
	s->last = now;
	s->acc += counts;
	int steps = 0;
	if (!s->turning) {
		if (s->acc > -CROWN_START_COUNTS && s->acc < CROWN_START_COUNTS)
			return 0;
		s->turning = true;
		steps = s->acc > 0 ? 1 : -1;
		s->acc -= steps * CROWN_START_COUNTS;
	}
	steps += s->acc / CROWN_STEP_COUNTS;
	s->acc %= CROWN_STEP_COUNTS;
	return steps;
}
