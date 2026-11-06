/* The vendor's sums for the sensor behind smelt's ink: the larger of two
 * linear mixes of the channels, over 128 at 1x gain and 1024 at 8x. Its
 * curve to the backlight is three parabolas in units of 1e-7 of a code,
 * capped at the ambient maximum. Its code leaves 86 and 87 to the last
 * parabola, a drop to code 8; here they belong to the ranges its comment
 * gives. */
#include "tsl2584.h"

#define LOW_TO_HIGH_LUX		200
#define HIGH_TO_LOW_LUX		100
#define HIGH_LUX_DEN		128
#define LOW_LUX_DEN		(HIGH_LUX_DEN * 8)
#define MIN_LUX			2
#define RECOVER_C0		200

#define CURVE_DEN		10000000
#define CURVE_LOW_MAX		86
#define CURVE_MID_MAX		999
#define CURVE_MAX		2330
#define AMBIENT_MAX_CODE	36

static const int32_t curve_low[] = { -44773, 6893285, 23168221 };
static const int32_t curve_mid[] = { -141, 358959, 274671182 };
static const int32_t curve_high[] = { -74, 529533, 37559974 };

int32_t tsl2584_lux(enum tsl2584_mode mode, uint16_t c0, uint16_t c1, const uint16_t coeff[4])
{
	int32_t den = mode == TSL2584_LOW_LUX ? LOW_LUX_DEN : HIGH_LUX_DEN;
	int32_t lux1 = (coeff[0] * (int32_t)c0 - coeff[1] * (int32_t)c1) / den;
	int32_t lux2 = (coeff[2] * (int32_t)c0 - coeff[3] * (int32_t)c1) / den;
	int32_t lux = lux1 < lux2 ? lux2 : lux1;
	if (lux < 0 && mode == TSL2584_LOW_LUX && c0 > RECOVER_C0)
		return TSL2584_OVERRANGE;
	return lux < MIN_LUX ? MIN_LUX : lux;
}

enum tsl2584_mode tsl2584_next_mode(enum tsl2584_mode mode, int32_t lux)
{
	if (mode == TSL2584_LOW_LUX)
		return lux == TSL2584_OVERRANGE || lux >= LOW_TO_HIGH_LUX ? TSL2584_HIGH_LUX : mode;
	return lux < HIGH_TO_LOW_LUX ? TSL2584_LOW_LUX : mode;
}

uint8_t tsl2584_backlight_code(uint32_t lux)
{
	int32_t x = lux > CURVE_MAX ? CURVE_MAX : (int32_t)lux;
	const int32_t *z = x <= CURVE_LOW_MAX ? curve_low : x <= CURVE_MID_MAX ? curve_mid : curve_high;
	int32_t code = (z[0] * x * x + z[1] * x + z[2]) / CURVE_DEN;
	return code > AMBIENT_MAX_CODE ? AMBIENT_MAX_CODE : (uint8_t)code;
}
