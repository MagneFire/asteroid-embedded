/* A crown's counts as steps of a value. A turn starts once
 * CROWN_START_COUNTS have come one way with no pause longer than
 * CROWN_IDLE_MS, so a brush moves nothing, and that is its first step;
 * each CROWN_STEP_COUNTS after it is one more, however slowly they come.
 * A pause ends the turn and drops what was left. */
#ifndef CROWN_STEPS_H
#define CROWN_STEPS_H
#include <stdint.h>
#include <stdbool.h>

#define CROWN_STEP_COUNTS	8
#define CROWN_START_COUNTS	12
#define CROWN_IDLE_MS		300

struct crown_steps {
	int32_t acc;
	uint32_t last;
	bool turning;
};

/* The steps the counts read at now make, signed as the counts are. */
int crown_steps(struct crown_steps *s, int32_t counts, uint32_t now);
#endif
