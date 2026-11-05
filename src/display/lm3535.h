/* The LM3535 backlight's brightness code for a level of 0 to 255: the
 * vendor's slider curve for the dark zone, the one its driver runs with
 * while the chip's light sensor is unused. The code steps are not linear
 * in current. Low levels are off or a fixed dim step, as the vendor's. */
#ifndef LM3535_H
#define LM3535_H
#include <stdint.h>

#define LM3535_DIM	0x26
#define LM3535_MAX	0x7f

static inline uint8_t lm3535_code(uint8_t level)
{
	if (level < 5)
		return 0;
	if (level < 10)
		return LM3535_DIM;
	int64_t v = level;
	int64_t code = (24 * v * v * v - 15295 * v * v + 4123276 * v + 634967215) / 10000000;
	return code > LM3535_MAX ? LM3535_MAX : (uint8_t)code;
}
#endif
