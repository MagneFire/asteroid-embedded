/* The screen's brightness from the light around it, no hardware
 * involved: the brightness setting picks a curve from lux to brightness,
 * as AsteroidOS's mce does. */
#ifndef AUTO_BRIGHTNESS_H
#define AUTO_BRIGHTNESS_H
#include <stdbool.h>
#include <stdint.h>

#define AUTO_BRIGHTNESS_WINDOW 5
#define AUTO_BRIGHTNESS_STEPS 21

/* All zeros is the starting state: no reading yet. */
struct auto_brightness {
	int32_t window[AUTO_BRIGHTNESS_WINDOW], lux;
	unsigned n;
	bool have_lux;
	/* The curve in use, its result and the lux it holds for. */
	bool have_level;
	int profile, level;
	int32_t lo, hi;
	/* A board's own levels for the same steps, one row per curve, or
	 * NULL for smelt's. */
	const uint8_t (*levels)[AUTO_BRIGHTNESS_STEPS];
};

/* Forget the recent readings; the last lux stays until a new one. */
void auto_brightness_reset(struct auto_brightness *a);
/* A reading in lux: the median of the last few becomes the lux. */
void auto_brightness_add(struct auto_brightness *a, int32_t lux);
/* The brightness in per cent for a setting of 1 to 100 per cent: the
 * setting itself without a reading. */
int auto_brightness_percent(struct auto_brightness *a, int setting);
#endif
