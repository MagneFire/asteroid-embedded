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

bool crown_page_locked(struct crown_page *p, uint32_t now)
{
	if (p->locked && now - p->turned >= CROWN_PAGE_LOCK_MS)
		p->locked = false;
	return p->locked;
}

int crown_page(struct crown_page *p, int32_t counts, bool at_end, uint32_t now)
{
	bool idle = now - p->last > CROWN_IDLE_MS;
	p->last = now;
	if (crown_page_locked(p, now) || !at_end) {
		p->acc = 0;
		return 0;
	}
	if (idle)
		p->acc = 0;
	p->acc += counts;
	if (p->acc > -CROWN_PAGE_COUNTS && p->acc < CROWN_PAGE_COUNTS)
		return 0;
	int page = p->acc > 0 ? 1 : -1;
	p->acc = 0;
	p->turned = now;
	p->locked = true;
	return page;
}
