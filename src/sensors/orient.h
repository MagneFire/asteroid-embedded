/* Which way up the screen is, from accelerometer samples: orient_update
 * takes a sample (x to the right of the screen, y to its top, z out of
 * it, any scale) and returns the angle, clockwise in degrees, to turn the
 * picture by so that its top points up: 0, 90, 180 or 270. */
#ifndef ORIENT_H
#define ORIENT_H
#include <stdint.h>

#define ORIENT_HOLD_MS	400

struct orient {
	int angle, candidate;
	uint32_t since;
};

void orient_reset(struct orient *o);
int orient_update(struct orient *o, const int16_t v[3], uint32_t now);
#endif
