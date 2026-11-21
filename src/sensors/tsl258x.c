/* The vendor's firmware picks one of four brightness levels from the
 * raw channel 0 count at 111x and 100 ms, stepping at 90, 1000 and
 * 4500. It never turns counts into lux itself, so the scale here is
 * set to put its top step at about 330 lux, bright indoor light, and
 * its first at about 7. Channel 1 is left out as the vendor leaves it.
 *
 * At 111x the count fills up at about 2800 lux. Above four fifths of
 * that the next reading is at 1x, and back at 111x once 1x reads what
 * 111x would see as 22200. */
#include "tsl258x.h"

#define ID_MASK		0xf0
#define ID_TSL258X	0x90

#define LUX_SCALE	300
#define HIGH_FULL	30000
#define LOW_EMPTY	200

static const int32_t gains[] = { 1, 111 };

bool tsl258x_id_ok(uint8_t id)
{
	return (id & ID_MASK) == ID_TSL258X;
}

int32_t tsl258x_lux(enum tsl258x_gain gain, uint16_t c0)
{
	return (int32_t)c0 * LUX_SCALE / (gains[gain] * TSL258X_CYCLES);
}

enum tsl258x_gain tsl258x_next_gain(enum tsl258x_gain gain, uint16_t c0)
{
	if (gain == TSL258X_GAIN_111X)
		return c0 >= HIGH_FULL ? TSL258X_GAIN_1X : gain;
	return c0 < LOW_EMPTY ? TSL258X_GAIN_111X : gain;
}
