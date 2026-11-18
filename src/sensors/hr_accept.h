#ifndef HR_ACCEPT_H
#define HR_ACCEPT_H
#include <stdbool.h>
#include <stdint.h>

/* Which heart rates from the catfish hub can be kept. The PixArt library
 * starts at 110 bpm and can hold a wrong rate for a while, and y, its
 * trust in the rate, is often 0 or 1 while the rate is steady and right.
 * A rate is taken once the last HR_WINDOW of them lie within HR_SPREAD
 * of each other: after HR_TRUSTED_MS if one of those was trusted (y 2 or
 * 3), or after HR_STEADY_MS without. z, a jump in the sensor's gain, sets
 * the window back. */
#define HR_WINDOW	10
#define HR_SPREAD	4
#define HR_TRUSTED_MS	20000
#define HR_STEADY_MS	40000
#define HR_AFTER_JUMP	2

struct hr_accept {
	uint8_t bpm[HR_WINDOW];
	bool trusted[HR_WINDOW];
	unsigned n, skip;
	uint32_t first_t;
	bool started;
};

void hr_accept_reset(struct hr_accept *a);
/* One rate from the hub at now: the bpm to keep, or 0. */
unsigned hr_accept_take(struct hr_accept *a, float bpm, float y, float z, uint32_t now);

#endif
