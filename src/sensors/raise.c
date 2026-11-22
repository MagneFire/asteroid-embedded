#include "sensors/raise.h"

/* At rest: two samples in a row this close, near 1 g. */
#define STEADY_MG	150
#define G_MIN_MG	750
#define G_MAX_MG	1250
/* Facing up is z of 0.5 g and more (within 60° of up), away below
 * 0.3 g; in between leaves it as it was. */
#define VIEW_MG		500
#define AWAY_MG		300
/* Samples in a row below that while moving: an arm swinging at the
 * side never comes to rest facing away. */
#define LOW_SAMPLES	3

void raise_arm(struct raise *r, uint32_t now)
{
	r->in_view = true;
	raise_check(r, now);
}

void raise_check(struct raise *r, uint32_t now)
{
	r->checking = true;
	r->have_last = false;
	r->low = 0;
	r->until = now + RAISE_CHECK_MS;
}

bool raise_checking(struct raise *r, uint32_t now)
{
	if (r->checking && (int32_t)(now - r->until) >= 0)
		r->checking = false;
	return r->checking;
}

static int dist(int a, int b) { return a > b ? a - b : b - a; }

bool raise_sample(struct raise *r, const int16_t mg[3], uint32_t now)
{
	if (!raise_checking(r, now))
		return false;
	bool steady = r->have_last;
	int32_t g2 = 0;
	for (int i = 0; i < 3; i++) {
		steady &= dist(mg[i], r->last[i]) <= STEADY_MG;
		g2 += (int32_t)mg[i] * mg[i];
		r->last[i] = mg[i];
	}
	r->have_last = true;
	r->low = mg[2] < AWAY_MG ? r->low + 1 : 0;
	if (r->low >= LOW_SAMPLES)
		r->in_view = false;
	if (!steady || g2 < G_MIN_MG * G_MIN_MG || g2 > G_MAX_MG * G_MAX_MG)
		return false;
	if (mg[2] < VIEW_MG) {
		if (mg[2] < AWAY_MG) {
			r->in_view = false;
			r->checking = false;
		}
		return false;
	}
	r->checking = false;
	bool raised = !r->in_view;
	r->in_view = true;
	return raised;
}
