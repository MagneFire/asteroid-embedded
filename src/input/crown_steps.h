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

/* A crown's counts past the end of what it scrolls as a turn of the page:
 * CROWN_PAGE_COUNTS one way with no pause longer than CROWN_IDLE_MS, all
 * of them past the end, turn one page, and for CROWN_PAGE_LOCK_MS after
 * it the crown does nothing, so the rest of a spin neither turns another
 * nor scrolls the page it landed on. */
#define CROWN_PAGE_COUNTS	24
#define CROWN_PAGE_LOCK_MS	400

struct crown_page {
	int32_t acc;
	uint32_t last, turned;
	bool locked;
};

/* The page the counts read at now turn, 1 or -1 as the counts are signed,
 * or 0. at_end says what they scroll is at its end their way. */
int crown_page(struct crown_page *p, int32_t counts, bool at_end, uint32_t now);
/* The counts read at now are to be dropped. */
bool crown_page_locked(struct crown_page *p, uint32_t now);
#endif
